// Capture - 屏幕截取 / 录制工具
// Win32 + DX11 + ImGui UI + WGC 捕获 + MF(H.264/AAC) 编码 + WASAPI Loopback 音频
// UI 层已拆分至 src/ui/，本文件只负责：窗口/设备初始化、消息分发、录制状态机、动作执行
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "app_state.h"
#include "graphics/graphics.h"
#include "capture/capture.h"
#include "recorder/recorder.h"
#include "audio/audio.h"
#include "audio/mixer.h"
#include "screenshot/screenshot.h"
#include "screenshot/snipping.h"
#include "settings/settings.h"
#include "core/log.h"
#include "core/util.h"
#include "core/hotkey.h"
#include "core/autostart.h"
#include "core/trayicon.h"
#include "core/appicon.h"
#include "ui/ui_app.h"
#include "ui/ui_theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

// 热键 ID 来自 core/hotkey.h
// 预览刷新间隔（20fps；录制写帧仍为捕获满帧，不受影响）
constexpr DWORD kPreviewIntervalMs = 50;
// 界面左上角标识用的图标像素尺寸（ico 内含 64x64 档位，绘制时缩放到 32px）
constexpr UINT kNavIconPx = 64;

// 主窗口类名：单实例检测要先 FindWindow 才能把已有实例唤起来，故提到文件作用域
const wchar_t kMainWndClass[] = L"CaptureWndClass";
// 第二个实例发给已有实例的唤醒消息（收到后恢复并显示主窗口）
#define WM_CAPTURE_WAKE (WM_APP + 111)

// ---------------------------------------------------------------------------
// 全局对象
// ---------------------------------------------------------------------------
static Graphics    g_gfx;
static ScreenCapture g_cap;
static Recorder    g_rec;
static AudioCapture g_sysAudio;   // 系统声音（扬声器回放 Loopback）
static AudioCapture g_micAudio;   // 麦克风（录音设备采集）
static AudioMixer  g_mixer;       // 两路同时开启时把两路合成一条 48kHz 立体声流
static TrayIcon    g_tray;
static HotkeyManager g_hotkey;
static UiApp       g_ui;
static Settings    g_cfg;
static AppState    g_st;
static HWND        g_hwnd = nullptr;

// 截图两阶段：先隐藏主窗口等新帧（否则冻结帧里会有本程序窗口），再进框选
enum class ShotPhase
{
    None,       // 空闲
    WaitFrame,  // 已隐藏主窗口，等待 WGC 送来不含本窗口的新帧
};

static ULONGLONG g_lastPreviewTick = 0;   // 预览节流
static int       g_recSeq = 0;            // 同名文件序号
static int       g_shotSeq = 0;
static bool      g_trayHintShown = false;  // 首次最小化到托盘时给气泡提示
static bool      g_quitPending = false;    // 录制中请求退出：等录制收尾后再销毁窗口
static bool      g_deviceLost = false;     // GPU 设备已丢失：保存已录内容后退出
static ShotPhase g_shotPhase = ShotPhase::None;
static ULONGLONG g_shotHideTick = 0;       // 隐藏主窗口的时刻
static long long g_shotFrameMark = 0;      // 隐藏时的帧计数，用于判断新帧是否到达
static bool      g_shotRestoreWin = false; // 框选结束后是否需要恢复主窗口

// ---------------------------------------------------------------------------
// 捕获 / 音频回调（WGC 工作线程 / 音频采集线程）
// ---------------------------------------------------------------------------
static void OnCaptureFrame(ID3D11Texture2D* tex, long long ts100ns)
{
    g_st.frameCount++;
    if (g_st.recording)
    {
        long long t = ts100ns - g_st.baseTime;
        g_rec.WriteVideoFrame(tex, t > 0 ? t : 0);
    }
}

// 音频路由：只有一路可用时该路直接写入编码器；两路都可用则先经混音器合成。
// 由 UI 线程在开始/停止录制时切换，采集线程只读，故用原子量传递
enum class AudioRoute
{
    Off,        // 无音频
    SysDirect,  // 仅系统声音，直通
    MicDirect,  // 仅麦克风，直通
    Mix         // 两路混音
};

static std::atomic<int> g_route { (int)AudioRoute::Off };

static void WriteAudioToRecorder(const BYTE* data, UINT32 bytes, long long ts100ns)
{
    if (!g_st.recording)
        return;
    const long long t = ts100ns - g_st.baseTime;
    g_rec.WriteAudio(data, bytes, t > 0 ? t : 0);
}

static void OnSysAudioData(const BYTE* data, UINT32 bytes, long long ts100ns)
{
    switch ((AudioRoute)g_route.load())
    {
    case AudioRoute::Mix:       g_mixer.Push(0, data, bytes, ts100ns); break;
    case AudioRoute::SysDirect: WriteAudioToRecorder(data, bytes, ts100ns); break;
    default: break;
    }
}

static void OnMicAudioData(const BYTE* data, UINT32 bytes, long long ts100ns)
{
    switch ((AudioRoute)g_route.load())
    {
    case AudioRoute::Mix:       g_mixer.Push(1, data, bytes, ts100ns); break;
    case AudioRoute::MicDirect: WriteAudioToRecorder(data, bytes, ts100ns); break;
    default: break;
    }
}

// 停止全部音频采集：先停采集线程，再复位路由，避免线程还在推数据时改路由
static void StopAudio()
{
    g_sysAudio.Stop();
    g_micAudio.Stop();
    g_route = (int)AudioRoute::Off;
    g_mixer.Reset();
}

// ---------------------------------------------------------------------------
// 路径生成（同名文件自动追加序号，避免覆盖已有录制）
// ---------------------------------------------------------------------------
static std::wstring UniquePath(const std::wstring& dir, const std::wstring& pattern,
                               const wchar_t* ext, int& seq)
{
    for (int i = 0; i < 1000; ++i)
    {
        std::wstring path = BuildFilePath(dir, pattern, ext, seq);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return path;
        ++seq;
    }
    return BuildFilePath(dir, pattern, ext, seq);
}

// ---------------------------------------------------------------------------
// 预览（节流到 20fps：录制写帧仍为捕获满帧，不受影响）
// ---------------------------------------------------------------------------
static void UpdatePreview()
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastPreviewTick < kPreviewIntervalMs)
        return;
    g_lastPreviewTick = now;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    UINT w = 0, h = 0;
    g_cap.GetLatestFrame(tex, w, h);

    if (tex && (tex.Get() != g_st.previewTex || w != g_st.previewW || h != g_st.previewH))
    {
        g_st.previewSRV.Reset();
        if (FAILED(g_gfx.Device()->CreateShaderResourceView(tex.Get(), nullptr,
                       g_st.previewSRV.ReleaseAndGetAddressOf())))
            return;
        g_st.previewTex = tex.Get();
        g_st.previewW = w;
        g_st.previewH = h;
    }
}

// ---------------------------------------------------------------------------
// 截屏
// ---------------------------------------------------------------------------
static void DoShot();   // 定义在下方；托盘场景下 BeginShot 会直接调用

// 框选结束后恢复主窗口：只有截图前确实可见（非最小化到托盘）才需要恢复
static void RestoreWindowAfterShot()
{
    if (!g_shotRestoreWin)
        return;
    g_shotRestoreWin = false;
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

// 发起一次截图：先隐藏主窗口，等新帧到达后再进框选（见主循环的 WaitFrame）
static void BeginShot()
{
    if (!g_cfg.shotSaveFile && !g_cfg.shotCopyClip)
    {
        g_st.errPopup = "截图的两种输出方式（存盘 / 复制到剪贴板）都已关闭，请在截屏页开启至少一种";
        return;
    }
    if (g_shotPhase != ShotPhase::None)
        return;   // 上一次框选还没结束（理论上不会，框选是模态的）

    // 托盘/最小化状态下窗口本就不可见，画面里没有本程序窗口，可直接截
    g_shotRestoreWin = IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd);
    if (!g_shotRestoreWin)
    {
        DoShot();
        return;
    }

    g_shotFrameMark = g_st.frameCount.load();
    g_shotHideTick = GetTickCount64();
    ShowWindow(g_hwnd, SW_HIDE);
    g_shotPhase = ShotPhase::WaitFrame;
}

// 真正执行：抓帧 -> 框选 -> 裁剪 -> 输出
static void DoShot()
{
    g_shotPhase = ShotPhase::None;

    ImageBGRA img;
    if (!GrabLatestFrame(g_cap, img))
    {
        RestoreWindowAfterShot();
        g_st.errPopup = "尚未捕获到画面，无法截图";
        LOG_ERR(L"截图失败: 无可用帧");
        return;
    }

    SnipSelection sel;
    bool ok = RunSnipping(GetModuleHandleW(nullptr), img, sel);
    RestoreWindowAfterShot();
    if (!ok)
    {
        // 取消是用户主动选择，给一次可见反馈，否则会以为程序没反应
        g_st.SetToast(L"已取消截图");
        return;
    }

    ImageBGRA cropped;
    if (!CropImage(img, sel.px, cropped))
    {
        g_st.errPopup = "截图区域无效，请重新框选";
        LOG_ERR(L"截图失败: 裁剪区域无效");
        return;
    }
    img = std::move(cropped);

    bool saved = false, copied = false;
    const int fmt = g_cfg.shotFormat;
    const std::wstring fmtName = Utf8ToWide(ShotFormatShortName(fmt));
    if (g_cfg.shotSaveFile)
    {
        std::wstring dir = g_cfg.shotDir.empty() ? g_cfg.saveDir : g_cfg.shotDir;
        // 模板里手打的扩展名要先去掉，否则与所选格式冲突（如 JPEG + "截图.png"）
        std::wstring pattern = StripImageExtension(g_cfg.shotPattern);
        std::wstring path = UniquePath(dir, pattern, ShotFormatExt(fmt), g_shotSeq);
        if (SaveImage(path, img, fmt))
        {
            saved = true;
            g_st.shotPath = path;
        }
        else
        {
            // errPopup 是 UTF-8 的 std::string（与 ImGui 文本一致），不要走 Utf8ToWide
            g_st.errPopup = std::string("保存 ") + ShotFormatShortName(fmt) +
                            " 失败，请检查目录权限";
        }
    }
    if (g_cfg.shotCopyClip)
        copied = CopyToClipboard(img);

    // 保留像素供"再次复制"与缩略图显示
    g_st.lastShot = std::move(img);
    g_st.shotW = g_st.lastShot.w;
    g_st.shotH = g_st.lastShot.h;
    g_st.shotCopied = copied;
    g_st.shotTick = GetTickCount64();
    g_st.shotSRV = g_gfx.CreateMemorySRV(g_st.lastShot.pixels.data(),
                                         g_st.lastShot.w, g_st.lastShot.h);
    if (!saved)
        g_st.shotPath.clear();

    if (saved && copied)
        g_st.SetToast(L"已保存 " + fmtName + L" 并复制到剪贴板");
    else if (saved)
        g_st.SetToast(L"已保存 " + fmtName + L": " + EllipsizePath(g_st.shotPath, 40));
    else if (copied)
        g_st.SetToast(L"已复制到剪贴板");
    else
        g_st.SetToast(L"截图失败，请查看日志");

    LOG_INFO(L"截图完成: %ux%u, 格式=%hs, 存盘=%d, 剪贴板=%d", g_st.lastShot.w, g_st.lastShot.h,
             ShotFormatShortName(fmt), saved ? 1 : 0, copied ? 1 : 0);   // %hs = char*
}

// ---------------------------------------------------------------------------
// 录制控制（主循环线程调用）
// ---------------------------------------------------------------------------
static void StartRecording()
{
    if (g_st.IsBusy())
        return;
    LOG_INFO(L"用户请求开始录制");

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    UINT w = 0, h = 0;
    g_cap.GetLatestFrame(tex, w, h);
    if (!tex)
    {
        g_st.errPopup = "尚未捕获到任何画面，无法开始录制";
        LOG_ERR(L"开始录制失败: 尚未捕获到任何画面");
        return;
    }
    if (g_cfg.saveDir.empty() || g_cfg.recordPattern.empty())
    {
        g_st.errPopup = "请先在录屏页设置输出目录与文件名";
        return;
    }
    // 目录可能已被删除或手输错误。先建好再去初始化编码器，
    // 否则失败时只有一句"创建 SinkWriter 失败"，真正原因不明
    if (!EnsureDir(g_cfg.saveDir))
    {
        LOG_ERR(L"开始录制失败: 输出目录不可用 %s", g_cfg.saveDir.c_str());
        g_st.errPopup = "输出目录不可用且无法创建：" + WideToUtf8Str(g_cfg.saveDir);
        return;
    }

    // 指针设置在本次录制生效
    g_cap.SetCursorCapture(g_cfg.includeCursor);

    // ---- 音频：先探测两路可用性，再决定要不要给 MP4 建音轨 ----
    // 只在开启时打开设备，避免生成空的 AAC 轨；两路各自失败互不影响
    g_st.audioSysInfo.clear();
    g_st.audioMicInfo.clear();
    bool sysOk = false, micOk = false;
    {
        std::wstring err;
        if (g_cfg.withSystemAudio)
        {
            sysOk = g_sysAudio.Open(AudioRole::System, g_cfg.sysAudioDevice, err);
            if (!sysOk)
            {
                g_st.audioSysInfo = err;
                LOG_WARN(L"系统声音不可用, 该路不录制: %s", err.c_str());
            }
        }
        if (g_cfg.withMic)
        {
            err.clear();
            micOk = g_micAudio.Open(AudioRole::Mic, g_cfg.micDevice, err);
            if (!micOk)
            {
                g_st.audioMicInfo = err;
                LOG_WARN(L"麦克风不可用, 该路不录制: %s", err.c_str());
            }
        }
    }
    const bool withAudio = sysOk || micOk;

    // 分辨率档位 -> 实际编码尺寸（等比缩放、只缩不放）；码率取手动值或按分辨率自动
    UINT recW = 0, recH = 0;
    RecordOutputSize(w, h, g_cfg.recResolution, recW, recH);
    if (recW == 0 || recH == 0)
    {
        recW = w & ~1u;
        recH = h & ~1u;
    }
    const int bitrate = ResolveRecordBitrateMbps(g_cfg, recW, recH);

    std::wstring path = UniquePath(g_cfg.saveDir, g_cfg.recordPattern, L".mp4", g_recSeq);
    std::wstring err;
    if (!g_rec.Init(g_gfx.Device(), path.c_str(), w, h, recW, recH,
                    (UINT)g_cfg.fps, (UINT)bitrate, withAudio,
                    g_cfg.hwEncode, g_cfg.audioBitrate, err))
    {
        StopAudio();
        LOG_ERR(L"开始录制失败: %s", err.c_str());
        g_st.errPopup = WideToUtf8Str(err);
        return;
    }

    g_st.baseTime = MFGetSystemTime();
    g_st.recordStartTick = GetTickCount64();

    // 混音器：每次录制前复位时间轴；界面百分比换成线性增益系数
    g_mixer.Reset();
    g_mixer.SetChannels(sysOk, micOk);
    g_mixer.Configure((float)g_cfg.sysVolume / 100.0f, (float)g_cfg.micVolume / 100.0f);
    g_mixer.SetOutput(WriteAudioToRecorder);

    if (sysOk && !g_sysAudio.Start(OnSysAudioData))
    {
        sysOk = false;
        g_st.audioSysInfo = L"音频流启动失败";
    }
    if (micOk && !g_micAudio.Start(OnMicAudioData))
    {
        micOk = false;
        g_st.audioMicInfo = L"麦克风音频流启动失败";
    }
    if (sysOk && micOk)
        g_route = (int)AudioRoute::Mix;
    else if (sysOk)
        g_route = (int)AudioRoute::SysDirect;
    else if (micOk)
        g_route = (int)AudioRoute::MicDirect;
    else
        g_route = (int)AudioRoute::Off;

    g_st.recording = true;
    const wchar_t* audioMode = !sysOk && !micOk ? L"无"
                         : (sysOk && micOk)          ? L"系统声音+麦克风（混音）"
                         : sysOk                     ? L"系统声音"
                                                    : L"麦克风";
    g_st.SetToast(L"开始录制: " + EllipsizePath(path, 36));
    LOG_INFO(L"录制开始: 文件=%s, 输出分辨率=%ux%u(源%ux%u), fps=%d, 码率=%dMbps, 音频=%ls",
             path.c_str(), recW, recH, w, h, g_cfg.fps, bitrate, audioMode);
}

static void StopRecordingPhase1()   // 快速路径：停止写入，UI 本帧显示"正在保存"
{
    if (!g_st.recording)
        return;
    LOG_INFO(L"用户请求停止录制, 停止写入进入保存阶段");
    g_st.recording = false;
    StopAudio();
    g_st.saving = true;
}

static void StopRecordingPhase2()    // 帧末执行：Finalize 阻塞数秒
{
    g_rec.Stop();
    g_st.saving = false;
    g_st.SetToast(L"录制已保存");
}

// ---------------------------------------------------------------------------
// 窗口显示 / 托盘
// ---------------------------------------------------------------------------
static void ShowMainWindow()
{
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    // 从托盘恢复时让任务栏按钮闪一下，帮助用户定位窗口
    FLASHWINFO fi = { sizeof(fi), g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
    FlashWindowEx(&fi);
}

static void UpdateTray()
{
    if (!g_tray.Valid())
        return;
    wchar_t buf[128] = {};
    if (g_st.State() == RunState::Recording)
    {
        long long sec = (long long)((GetTickCount64() - g_st.recordStartTick) / 1000);
        if (sec < 0) sec = 0;
        swprintf(buf, 128, L"Capture - 录制中 %lld:%02lld:%02lld",
                 sec / 3600, (sec / 60) % 60, sec % 60);
    }
    else if (g_st.State() == RunState::Saving)
    {
        wcscpy_s(buf, L"Capture - 正在保存文件…");
    }
    else
    {
        std::string last = g_st.shotPath.empty() ? std::string()
                                                 : WideToUtf8Str(EllipsizePath(g_st.shotPath, 24));
        if (!last.empty())
            swprintf(buf, 128, L"Capture - 就绪（上次截图 %ls）", Utf8ToWide(last.c_str()).c_str());
        else
            wcscpy_s(buf, L"Capture - 就绪");
    }
    g_tray.Update(buf, g_st.State() == RunState::Recording);
}

static void ApplyHotkeys()
{
    g_hotkey.Apply(g_hwnd, g_cfg);
    g_ui.SetHotkeyText(g_cfg.hotkeyCapture, g_cfg.hotkeyRecord, g_cfg.hotkeyShow);
    SaveSettings(g_cfg);
}

// ---------------------------------------------------------------------------
// GPU 设备丢失后的进程内恢复
// ---------------------------------------------------------------------------
// 旧设备已死（TDR / 驱动重置），但窗口/托盘/热键/ImGui 上下文（含字体图集）
// 都与设备无关，可以保留。按依赖顺序重建：WGC 停止 -> ImGui DX11 后端 ->
// UI 纹理 -> 旧设备释放 -> 新设备/交换链 -> 后端重绑 -> UI 纹理重建 -> WGC 重启。
// 返回 false = 恢复失败，此时只能退出程序。
static bool RecoverFromDeviceLost()
{
    LOG_INFO(L"GPU 设备恢复: 开始重建设备链");

    // 1. 先停 WGC：帧回调线程引用的纹理全部属于旧设备，避免恢复期间继续报错
    g_cap.Stop();

    // 2. 释放 ImGui DX11 后端与所有旧设备上的 UI 纹理（字体图集在 ImGui
    //    上下文里，随重新 Init 自动重建，LoadThemeFonts 不需要重跑）
    ImGui_ImplDX11_Shutdown();
    g_st.previewSRV.Reset();
    g_st.previewTex = nullptr;
    g_st.previewW = g_st.previewH = 0;
    g_st.appIconSRV.Reset();
    g_st.shotSRV.Reset();

    // 3. 释放 Recorder 缓存的旧设备资源（暂存/缩放/着色器等）
    g_rec.ReleaseDeviceResources();

    // 4. 旧设备/交换链彻底释放后重建设备
    g_gfx.Shutdown();
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)   // 最小化/异常态兜底
    {
        w = g_cfg.windowW;
        h = g_cfg.windowH;
    }
    if (!g_gfx.Init(g_hwnd, (int)w, (int)h))
    {
        LOG_ERR(L"GPU 设备恢复: 设备/交换链重建失败");
        return false;
    }

    // 5. ImGui 后端重新绑定新设备
    if (!ImGui_ImplDX11_Init(g_gfx.Device(), g_gfx.Context()))
    {
        LOG_ERR(L"GPU 设备恢复: ImGui 后端初始化失败");
        return false;
    }

    // 6. UI 图像纹理按需重建（导航图标、截图缩略图）
    {
        std::vector<BYTE> iconPx;
        if (AppIcon::LoadBGRA(kNavIconPx, iconPx))
            g_st.appIconSRV = g_gfx.CreateMemorySRV(iconPx.data(), kNavIconPx, kNavIconPx);
        if (g_st.lastShot.w > 0 && g_st.lastShot.h > 0 && !g_st.lastShot.pixels.empty())
            g_st.shotSRV = g_gfx.CreateMemorySRV(g_st.lastShot.pixels.data(),
                                                 g_st.lastShot.w, g_st.lastShot.h);
    }

    // 7. WGC 用新设备重启（预览恢复常开）；失败不阻断程序，仅提示
    {
        std::wstring err;
        if (!g_cap.Start(g_gfx.Device(), g_cfg.includeCursor, OnCaptureFrame, err))
        {
            LOG_ERR(L"GPU 设备恢复: WGC 捕获重启失败: %s", err.c_str());
            g_st.errPopup = "图形设备已恢复，但屏幕捕获重启失败：" + WideToUtf8Str(err);
        }
    }

    LOG_INFO(L"GPU 设备恢复: 完成");
    return true;
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_gfx.Device() != nullptr && wParam != SIZE_MINIMIZED)
            g_gfx.Resize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;   // 屏蔽 Alt 菜单键闪烁
        break;

    case WM_HOTKEY:
    {
        // lParam 最高位为 1 表示热键被按住不放，忽略以避免长按连发
        if (lParam & 0x8000)
            return 0;
        switch ((UINT)wParam)
        {
        case HK_CAPTURE_ID:
            g_st.wantShot = true;   // 进入全屏框选（单击 = 整屏）
            break;
        case HK_RECORD_ID:
            if (g_st.recording)    g_st.wantStopRecord = true;
            else if (!g_st.saving) g_st.wantStartRecord = true;
            break;
        case HK_SHOW_ID:
            g_st.wantShowWindow = true;
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_CAPTURE_WAKE:
        g_st.wantShowWindow = true;   // 第二个实例请求把本机主窗口唤到前台
        return 0;

    case WM_TRAYICON:
    {
        TrayAction act = TrayAction::None;
        if (g_tray.OnMessage(wParam, lParam, act))
        {
            switch (act)
            {
            case TrayAction::Show:     g_st.wantShowWindow = true;  break;
            case TrayAction::Capture:  g_st.wantShot = true;         break;
            case TrayAction::Record:
                if (g_st.recording)        g_st.wantStopRecord = true;
                else if (!g_st.saving)     g_st.wantStartRecord = true;
                break;
            case TrayAction::Settings:  g_st.wantSettingsPage = true; break;
            case TrayAction::Quit:      g_st.wantQuit = true;        break;
            default: break;
            }
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        // 有托盘时不退出，改为最小化到托盘
        if (g_tray.Valid())
        {
            ShowWindow(hWnd, SW_HIDE);
            if (!g_trayHintShown)
            {
                g_trayHintShown = true;
                g_tray.Notify(L"Capture 仍在后台运行",
                              L"点击托盘图标可再次打开，双击恢复窗口。");
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        LOG_INFO(L"收到 WM_DESTROY, 窗口销毁");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    (void)hPrevInstance; (void)lpCmdLine;

    // ---- 单实例 ----
    // 必须在 LogInit 之前判定：日志名按启动秒级时间戳生成，若第二个实例先初始化日志，
    // 同一秒启动会往同一份文件里追加；两份配置也会互相覆盖。
    HANDLE hSingleInstance = CreateMutexW(nullptr, FALSE, L"Local\\Capture.SingleInstance");
    if (hSingleInstance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 已有实例在跑：把它唤到前台后直接退出，全程不碰日志与配置
        if (HWND existing = FindWindowW(kMainWndClass, nullptr))
            PostMessageW(existing, WM_CAPTURE_WAKE, 0, 0);
        CloseHandle(hSingleInstance);
        return 0;
    }

    LogInit();
    LOG_INFO(L"程序入口 wWinMain");
    ImGui_ImplWin32_EnableDpiAwareness();

    // ---- 设置 ----
    LoadSettings(g_cfg);   // 内部已补全默认值
    FillDefaults(g_cfg);

    // ---- 开机自启动 ----
    // 配置意愿为开但注册表缺失或 exe 位置已变时自动修复写回；
    // 意愿为关则不动注册表（尊重用户在系统侧的手动调整）
    if (g_cfg.autoStart && !AutostartIsEnabled())
    {
        if (AutostartEnable())
            LOG_INFO(L"开机自启动注册表已按配置修复");
        else
            LOG_WARN(L"开机自启动注册表修复失败，可在设置页重新开启");
    }

    // ---- 窗口 ----
    // 应用图标统一来自 exe 资源 IDI_CAPTURE（Capture.rc + ico/Capture.ico）
    HICON iconBig   = AppIcon::Load((UINT)GetSystemMetrics(SM_CXICON),
                                    (UINT)GetSystemMetrics(SM_CYICON));
    HICON iconSmall = AppIcon::Load((UINT)GetSystemMetrics(SM_CXSMICON),
                                    (UINT)GetSystemMetrics(SM_CYSMICON));
    if (!iconSmall)
        iconSmall = AppIcon::Load(16, 16);
    if (!iconBig)
        iconBig = iconSmall;   // 大图标缺失时用小图标兜底

    const wchar_t* const CLASS_NAME = kMainWndClass;
    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, WndProc };
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = iconBig;        // 标题栏左上角 / 任务栏（大图标）
    wc.hIconSm = iconSmall;    // 标题栏左上角（小图标）
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, CLASS_NAME, L"Capture - 屏幕截取与录制",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             g_cfg.windowW, g_cfg.windowH, nullptr, nullptr,
                             hInstance, nullptr);
    if (!g_hwnd)
    {
        LOG_ERR(L"窗口创建失败");
        MessageBoxW(nullptr, L"窗口创建失败", L"Capture", MB_ICONERROR);
        return 1;
    }

    // 再显式设置一次：类图标在某些 shell 场景下不会自动刷新到任务栏按钮
    if (iconBig)
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)iconBig);
    if (iconSmall)
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSmall);

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    LOG_INFO(L"主窗口创建成功: %ldx%ld", rc.right - rc.left, rc.bottom - rc.top);
    if (!g_gfx.Init(g_hwnd, rc.right - rc.left, rc.bottom - rc.top))
    {
        LOG_ERR(L"Direct3D 11 初始化失败");
        MessageBoxW(nullptr, L"Direct3D 11 初始化失败", L"Capture", MB_ICONERROR);
        return 1;
    }
    LOG_INFO(L"Direct3D 11 初始化成功");

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // 布局不持久化（设置由 settings.json 负责）
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    LoadThemeFonts();
    SetDarkMode(g_cfg.darkMode);
    ApplyTheme();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_gfx.Device(), g_gfx.Context());
    LOG_INFO(L"ImGui 初始化完成");

    // ---- 界面左上角标识用的图标纹理（与标题栏/任务栏/托盘同一图标）----
    {
        std::vector<BYTE> iconPx;
        if (AppIcon::LoadBGRA(kNavIconPx, iconPx))
            g_st.appIconSRV = g_gfx.CreateMemorySRV(iconPx.data(), kNavIconPx, kNavIconPx);
        else
            LOG_WARN(L"界面标识图标纹理创建失败(导航栏将只显示文字)");
    }

    // ---- 托盘 + 全局热键 + UI ----
    g_tray.Create(g_hwnd);
    g_ui.Init(&g_st, &g_cfg, g_hwnd);
    ApplyHotkeys();

    // ---- 屏幕捕获（预览常开）----
    {
        std::wstring err;
        if (!g_cap.Start(g_gfx.Device(), g_cfg.includeCursor, OnCaptureFrame, err))
        {
            LOG_ERR(L"屏幕捕获启动失败: %s", err.c_str());
            g_st.errPopup = WideToUtf8Str(err);
        }
    }

    ShowWindow(g_hwnd, nShowCmd);
    LOG_INFO(L"进入消息循环");

    // ---- 消息循环 ----
    MSG msg{};
    bool done = false;
    ULONGLONG lastTrayTick = 0;
    while (!done)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // ---- 消费动作请求 ----
        if (g_st.wantQuit)
        {
            g_st.wantQuit = false;
            if (g_st.recording)
            {
                // 录制中退出：先走正常停止流程，避免 MP4 尾部未写导致文件损坏
                g_st.wantStopRecord = true;
                g_quitPending = true;
                g_st.SetToast(L"正在结束录制并保存文件，请稍候…");
            }
            else if (g_st.saving)
            {
                // 保存阶段不能 DestroyWindow（MP4 尾部还没写完）。此前这里两个分支都不成立，
                // wantQuit 已被清零，用户的退出请求被静默吞掉。改为登记退出意图，
                // 由下方 g_quitPending 检查在 Finalize 完成后执行真正的销毁
                g_quitPending = true;
                g_st.SetToast(L"正在保存文件，保存完成后自动退出…");
            }
            else
            {
                DestroyWindow(g_hwnd);   // 触发 WM_DESTROY -> PostQuitMessage
            }
            continue;
        }
        if (g_quitPending && !g_st.IsBusy())
        {
            g_quitPending = false;
            DestroyWindow(g_hwnd);
            continue;
        }

        // ---- GPU 设备丢失（TDR / 驱动重置）----
        // WGC / MF / UI 共用同一设备，一丢全挂：视频帧全部失败、画面冻结、WGC 不再出帧。
        // 处理：先走标准两阶段停止把手头已写入的帧 Finalize 进文件；保存完成后
        // 不再退出程序，而是弹窗告知并进程内重建设备链（RecoverFromDeviceLost），
        // 让程序回到可用状态继续运行
        if (!g_deviceLost && (g_gfx.IsDeviceLost() || g_rec.IsDeviceLost()))
        {
            g_deviceLost = true;
            LOG_ERR(L"检测到 GPU 设备丢失, 保存已写入内容后将尝试恢复");
            if (g_st.recording && !g_st.wantStopRecord)
                g_st.wantStopRecord = true;
        }
        if (g_deviceLost && !g_st.IsBusy())
        {
            // 保存已完成（或本就空闲）。先停 WGC 让死设备安静下来，再告知用户
            g_cap.Stop();
            MessageBoxW(nullptr,
                        L"显卡设备被系统重置（驱动崩溃 / TDR），本次录制已中止。\n"
                        L"已写入的部分已保存到输出目录。\n\n"
                        L"点击「确定」后程序会自动恢复运行，无需重启。\n"
                        L"若频繁出现，建议更新或回退显卡驱动。",
                        L"Capture", MB_ICONWARNING | MB_TOPMOST);
            if (RecoverFromDeviceLost())
            {
                g_deviceLost = false;
                g_st.SetToast(L"图形设备已恢复，可继续使用");
            }
            else
            {
                MessageBoxW(nullptr,
                            L"图形设备自动恢复失败，程序即将退出。\n建议重启电脑后再试。",
                            L"Capture", MB_ICONERROR | MB_TOPMOST);
                DestroyWindow(g_hwnd);
            }
            continue;
        }
        if (g_st.wantShowWindow)
        {
            g_st.wantShowWindow = false;
            // 截图前隐藏主窗口的那几十毫秒内忽略恢复请求：提前显示会把窗口
            // 重新带回画面，框选底图里就会出现本程序窗口（框选结束后会自行恢复）
            if (g_shotPhase == ShotPhase::None)
                ShowMainWindow();
        }
        if (g_st.wantSettingsPage)
        {
            g_st.wantSettingsPage = false;
            g_ui.SetPage(2);
            ShowMainWindow();
        }
        if (g_st.wantShot)
        {
            g_st.wantShot = false;
            BeginShot();
        }
        if (g_shotPhase == ShotPhase::WaitFrame)
        {
            // 至少等两帧：一帧可能仍是隐藏前抓取的，两帧后才必然不含本窗口。
            // 捕获异常（显卡/权限导致帧停更）时按 400ms 超时兜底，不能一直卡住
            const ULONGLONG waited = GetTickCount64() - g_shotHideTick;
            if (g_st.frameCount.load() >= g_shotFrameMark + 2 || waited >= 400)
                DoShot();
        }
        if (g_st.wantStartRecord)
        {
            g_st.wantStartRecord = false;
            StartRecording();
        }
        // 录制过程中画面源尺寸变了（切分辨率 / 插拔显示器 / 旋转屏幕）：
        // 编码器已按旧尺寸配置，继续写会拉伸且每帧重建暂存纹理，故就地中止
        if (g_st.recording && g_rec.IsSrcSizeBroken())
        {
            g_st.wantStopRecord = true;
            g_st.errPopup = "录制过程中画面尺寸发生变化（切换分辨率或旋转屏幕），本次录制已中止";
        }
        if (g_st.wantStopRecord)
        {
            g_st.wantStopRecord = false;
            StopRecordingPhase1();
        }
        if (g_ui.ConsumeHotkeyDirty())
            ApplyHotkeys();

        // ---- 捕获帧率统计（每 500ms）----
        ULONGLONG now = GetTickCount64();
        if (now - g_st.lastFpsTick >= 500)
        {
            double dt = (now - g_st.lastFpsTick) / 1000.0;
            long long cnt = g_st.frameCount;
            g_st.capFps = (cnt - g_st.lastFrameCount) / dt;
            g_st.lastFrameCount = cnt;
            g_st.lastFpsTick = now;
        }

        // ---- 托盘状态（每 500ms）----
        if (now - lastTrayTick >= 500)
        {
            lastTrayTick = now;
            UpdateTray();
        }

        // ---- UI 帧 ----
        // 设备已丢失时跳过整段渲染：死设备上绘制无意义，且 ImGui DX11 后端
        // 在 Map 失败时的行为不保证；保存流程（帧末 Finalize）照常推进
        if (!g_deviceLost)
        {
            UpdatePreview();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            g_ui.Draw();
            ImGui::Render();

            g_gfx.BeginFrame();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_gfx.Present();
        }

        // Finalize 放在帧末：用户先看到"正在保存…"再阻塞
        if (g_st.saving)
            StopRecordingPhase2();
    }

    // ---- 清理 ----
    LOG_INFO(L"消息循环结束, 开始清理资源");
    SaveSettings(g_cfg);
    if (hSingleInstance) { CloseHandle(hSingleInstance); hSingleInstance = nullptr; }
    g_hotkey.Unregister();
    g_tray.Destroy();
    // 窗口已随 WM_DESTROY 销毁，图标句柄不再被 shell 引用，可以释放
    if (iconSmall && iconSmall != iconBig)
        DestroyIcon(iconSmall);
    if (iconBig)
        DestroyIcon(iconBig);
    g_ui.Shutdown();
    g_cap.Stop();
    if (g_rec.IsRecording())
        g_rec.Stop();
    StopAudio();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_gfx.Shutdown();
    LogShutdown();
    return 0;
}
