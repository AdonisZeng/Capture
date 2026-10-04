#include "page_record.h"
#include "ui_app.h"
#include "ui_widgets.h"
#include "app_state.h"
#include "settings/settings.h"
#include "recorder/recorder.h"
#include "audio/audio.h"
#include "core/util.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

char g_recDirBuf[1024] = {};
char g_recNameBuf[256] = {};
bool g_recBufInit = false;

// ---- 下拉项与实际值的映射表（配置里存实际值，界面按下拉索引操作） ----

const char* const kFpsItems[] = { "15 fps", "24 fps", "25 fps", "30 fps",
                                  "50 fps", "60 fps", "120 fps" };
const int kFpsValues[] = { 15, 24, 25, 30, 50, 60, 120 };

// 显示顺序按清晰度从高到低，原画（不缩放）放在最后
const char* const kResItems[] = { "3840×2160", "2560×1440", "1920×1080",
                                  "1280×720", "原画" };
const int kResValues[] = { RecRes2160P, RecRes1440P, RecRes1080P,
                           RecRes720P, RecResNative };

const char* const kBrItems[] = { "自动（按分辨率）", "8 Mbps", "12 Mbps", "16 Mbps",
                                 "24 Mbps", "32 Mbps", "48 Mbps" };
const int kBrValues[] = { 0, 8, 12, 16, 24, 32, 48 };

const char* const kAudioBrItems[] = { "96 kbps", "128 kbps", "192 kbps", "256 kbps" };
const int kAudioBrValues[] = { 96, 128, 192, 256 };

// 数组元素个数，避免 items/values 两张表长度对不上
template <typename T, size_t N>
constexpr int CountOf(const T (&)[N]) { return (int)N; }

// 实际值 -> 下拉索引；不在表内（如手改配置写了个表中没有的值）时取 fallback
int ValueToIndex(const int* table, int count, int value, int fallback)
{
    for (int i = 0; i < count; ++i)
        if (table[i] == value)
            return i;
    return fallback;
}

// 硬件编码器名探测要枚举系统 MFT（数十毫秒），只做一次并缓存
const std::wstring& HardwareEncoderName()
{
    static std::wstring name;
    static bool probed = false;
    if (!probed)
    {
        probed = true;
        name = ProbeHardwareEncoderName();
    }
    return name;
}

void EnsureBufs(Settings& cfg)
{
    if (g_recBufInit)
        return;
    strncpy_s(g_recDirBuf, WideToUtf8Str(cfg.saveDir).c_str(), _TRUNCATE);
    strncpy_s(g_recNameBuf, WideToUtf8Str(cfg.recordPattern).c_str(), _TRUNCATE);
    g_recBufInit = true;
}

// 录制时长 mm:ss（超过 1 小时用 h:mm:ss）
std::string FormatElapsed(long long sec)
{
    char buf[24];
    if (sec >= 3600)
        snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
    else
        snprintf(buf, sizeof(buf), "%02lld:%02lld", sec / 60, sec % 60);
    return buf;
}

// ---- 音频设备选择器 ----
// 设备由 COM 枚举得到（每帧绘制不能枚举），这里缓存 wstring 设备 ID 与 UTF-8 文案。
// 第 0 项固定为「系统默认」（ID 为空），其余为实际设备
struct DevicePicker
{
    std::vector<AudioDeviceInfo> devs;
    std::vector<std::string>     labels;
    std::vector<const char*>     items;
    bool valid = false;

    void Refresh(bool capture)
    {
        devs.clear();
        EnumerateAudioDevices(capture, devs);
        devs.insert(devs.begin(), AudioDeviceInfo{ L"", L"系统默认", false });
        RebuildLabels();
        valid = true;
    }
    void RebuildLabels()
    {
        labels.clear();
        items.clear();
        labels.reserve(devs.size());
        for (const AudioDeviceInfo& d : devs)
        {
            std::string name = WideToUtf8Str(d.name);
            if (d.isDefault)
                name += "（默认）";
            labels.push_back(std::move(name));
        }
        for (const std::string& s : labels)
            items.push_back(s.c_str());
    }
    // 配置里的设备已拔出时回落到「系统默认」
    int IndexOf(const std::wstring& id) const
    {
        for (size_t i = 0; i < devs.size(); ++i)
            if (devs[i].id == id)
                return (int)i;
        return 0;
    }
    int Count() const { return (int)devs.size(); }
};

DevicePicker g_sysPicker;
DevicePicker g_micPicker;

}   // namespace

void DrawRecordPage(const UiContext& ctx)
{
    Settings& cfg = *ctx.cfg;
    AppState& st = *ctx.st;
    EnsureBufs(cfg);

    const bool recording = st.State() == RunState::Recording;
    const bool saving    = st.State() == RunState::Saving;
    const bool busy      = st.IsBusy();

    const float pad = Control::CardPad;
    const float gap = pad;
    const float fullW = ImGui::GetContentRegionAvail().x;

    PageTitle("录屏",
              recording ? "正在录制，配置项已锁定以保证文件与界面显示一致"
                        : "捕获主显示器画面，可同时录制系统声音与麦克风，输出 MP4（H.264 + AAC）");

    const float fullH = ImGui::GetContentRegionAvail().y;
    const float leftW = (fullW - gap) * 0.58f;
    // 标签列宽由最长标签实测得出，改文案或字号后不会错位
    static const char* const kFormLabels[] = { "目录", "文件名", "帧率", "分辨率", "码率",
                                               "输出设备", "麦克风", "音量", "音频码率" };
    const float labelW = MeasureLabelColumn(kFormLabels, 9);
    const float btnH = Control::ButtonLg;
    // 底部固定区：输出概要两行小字 + 主按钮
    const float sumH = TextLineH(FontSmall) * 2.0f;

    // ================= 左栏：实时预览 =================
    // ImGui 1.92+ 必须显式传 ImGuiChildFlags_Borders，否则 child 被视为无边框，
    // PushCardStyle 推入的 ChildBorderSize 与 WindowPadding(卡片内边距) 都会被清零
    PushCardStyle(pad);
    ImGui::BeginChild("rec_left", ImVec2(leftW, fullH), ImGuiChildFlags_Borders);

    SectionTitle(recording ? "录制中" : "实时预览");
    ImVec2 avail = ImGui::GetContentRegionAvail();
    constexpr float kMinPreviewH = 100.0f;
    // 底部固定一行状态文字，其余高度全给预览画面
    float pvH = avail.y - Space::Sm - TextLineH(FontBody);
    if (pvH < kMinPreviewH)
        pvH = kMinPreviewH;

    ImageFit((ImTextureID)st.previewSRV.Get(), ImVec2((float)st.previewW, (float)st.previewH),
             ImVec2(avail.x, pvH), "等待捕获画面…");

    char pvInfo[128];
    if (recording)
    {
        long long sec = (long long)((GetTickCount64() - st.recordStartTick) / 1000);
        if (sec < 0) sec = 0;
        snprintf(pvInfo, sizeof(pvInfo), "● REC   %s   %ux%u @ %d fps",
                 FormatElapsed(sec).c_str(), st.previewW, st.previewH, cfg.fps);
        ImGui::TextColored(Pal::Danger(), pvInfo);
    }
    else
    {
        snprintf(pvInfo, sizeof(pvInfo), "捕获 %.0f FPS   画面 %ux%u",
                 st.capFps, st.previewW, st.previewH);
        ImGui::TextDisabled("%s", pvInfo);
    }
    ImGui::EndChild();
    PopCardStyle();

    ImGui::SameLine(0.0f, gap);

    // ================= 右栏：输出配置 + 控制 =================
    PushCardStyle(pad);
    ImGui::BeginChild("rec_right", ImVec2(0.0f, fullH), ImGuiChildFlags_Borders);

    // 底部固定概要 + 主按钮，故配置区放入内层可滚动 child。
    // 该内层 child 无边框（不传 Borders），ImGui 会将其 WindowPadding 清零，
    // 这里再显式清零一次以明确意图，保证内容与卡片下方概要/按钮左缘对齐
    ImVec2 rightAvail = ImGui::GetContentRegionAvail();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("rec_cfg", ImVec2(rightAvail.x, rightAvail.y - btnH - sumH - 10.0f),
                      ImGuiChildFlags_None);
    ImGui::PopStyleVar();

    // 录制中锁定配置，避免文件实际参数与界面显示不一致
    if (busy)
        ImGui::BeginDisabled();

    // ---- 输出位置 ----
    SectionTitle("输出位置");

    bool browse = false;
    if (PathRow("recdir", "目录", labelW, g_recDirBuf, sizeof(g_recDirBuf), true, &browse))
        cfg.saveDir = Utf8ToWide(g_recDirBuf);
    if (browse)
    {
        std::wstring dir = cfg.saveDir;
        if (PickFolderDialog(ctx.ownerHwnd, dir))
        {
            cfg.saveDir = dir;
            strncpy_s(g_recDirBuf, WideToUtf8Str(dir).c_str(), _TRUNCATE);
            SaveSettings(cfg);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    if (PathRow("recname", "文件名", labelW, g_recNameBuf, sizeof(g_recNameBuf)))
        cfg.recordPattern = Utf8ToWide(g_recNameBuf);

    // ---- 画质 ----
    SectionDivider();
    SectionTitle("画质");

    // 选项较多的项一律用下拉框：帧率 7 档、分辨率 5 档，
    // 放进分段控件会把每项压到不足 60px，中文标签必然截断
    {
        int idx = ValueToIndex(kFpsValues, CountOf(kFpsValues), cfg.fps, 5);
        if (ComboRow("fps", "帧率", labelW, kFpsItems, CountOf(kFpsItems), &idx))
        {
            cfg.fps = kFpsValues[idx];
            SaveSettings(cfg);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    {
        int idx = ValueToIndex(kResValues, CountOf(kResValues), cfg.recResolution, 4);
        if (ComboRow("res", "分辨率", labelW, kResItems, CountOf(kResItems), &idx))
        {
            cfg.recResolution = kResValues[idx];
            SaveSettings(cfg);
        }

        UINT rw = 0, rh = 0;
        RecordOutputSize(st.previewW, st.previewH, cfg.recResolution, rw, rh);
        char resHint[128];
        if (rw && rh)
        {
            if (rw != (st.previewW & ~1u) || rh != (st.previewH & ~1u))
                snprintf(resHint, sizeof(resHint), "实际录制：%u×%u（画面源 %u×%u，等比缩放）",
                         rw, rh, st.previewW, st.previewH);
            else
                snprintf(resHint, sizeof(resHint), "实际录制：%u×%u（原画，不做缩放）", rw, rh);
        }
        else
        {
            snprintf(resHint, sizeof(resHint), "实际录制：跟随画面源分辨率（所选档位更大时不放大）");
        }
        HintText(resHint);
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    {
        int idx = ValueToIndex(kBrValues, CountOf(kBrValues), cfg.bitrateMbps, 0);
        if (ComboRow("bitrate", "码率", labelW, kBrItems, CountOf(kBrItems), &idx))
        {
            cfg.bitrateMbps = kBrValues[idx];
            SaveSettings(cfg);
        }
    }

    // ---- 音频与画面 ----
    SectionDivider();
    SectionTitle("音频与画面");

    // 进页/热插拔后重新枚举设备（枚举不能放在每帧路径上）
    if (ImGui::IsWindowAppearing() || !g_sysPicker.valid || !g_micPicker.valid)
    {
        g_sysPicker.Refresh(false);
        g_micPicker.Refresh(true);
    }

    // ---- 系统声音（扬声器回放）----
    if (ToggleRow("sysaud", "录制系统声音", &cfg.withSystemAudio))
    {
        SaveSettings(cfg);
        st.SetToast(L"声音来源设置将在下次开始录制时生效");
    }
    if (cfg.withSystemAudio)
    {
        ImGui::Dummy(ImVec2(0.0f, Space::Xs));
        int idx = g_sysPicker.IndexOf(cfg.sysAudioDevice);
        if (ComboRow("sysdev", "输出设备", labelW, g_sysPicker.items.data(),
                     g_sysPicker.Count(), &idx))
        {
            cfg.sysAudioDevice = g_sysPicker.devs[idx].id;
            SaveSettings(cfg);
        }
        ImGui::Dummy(ImVec2(0.0f, Space::Xs));
        if (VolumeRow("sysvol", "音量", labelW, &cfg.sysVolume))
            SaveSettings(cfg);
    }

    // ---- 麦克风 ----
    ImGui::Dummy(ImVec2(0.0f, Space::Sm));
    const bool micAvailable = g_micPicker.Count() > 1;
    ImGui::BeginDisabled(!micAvailable);
    if (ToggleRow("mic", "录制麦克风", &cfg.withMic))
    {
        SaveSettings(cfg);
        st.SetToast(L"声音来源设置将在下次开始录制时生效");
    }
    ImGui::EndDisabled();
    if (!micAvailable)
        HintTextColored("未检测到录音设备，无法录制麦克风", Pal::Warning());

    if (cfg.withMic)
    {
        ImGui::Dummy(ImVec2(0.0f, Space::Xs));
        int idx = g_micPicker.IndexOf(cfg.micDevice);
        if (ComboRow("micdev", "麦克风", labelW, g_micPicker.items.data(),
                     g_micPicker.Count(), &idx))
        {
            cfg.micDevice = g_micPicker.devs[idx].id;
            SaveSettings(cfg);
        }
        ImGui::Dummy(ImVec2(0.0f, Space::Xs));
        if (VolumeRow("micvol", "音量", labelW, &cfg.micVolume))
            SaveSettings(cfg);
        HintText("两路同时开启会自动混音并压到同一条音轨；麦克风未做回声消除，外放时可能有回声");
    }

    // 上一次录制的音频异常提示（当次录制结束后仍可见，便于换设备重试）
    if (!st.audioSysInfo.empty())
        HintTextColored(("系统声音不可用: " + WideToUtf8Str(st.audioSysInfo)).c_str(),
                        Pal::Warning());
    if (!st.audioMicInfo.empty())
        HintTextColored(("麦克风不可用: " + WideToUtf8Str(st.audioMicInfo)).c_str(),
                        Pal::Warning());

    ImGui::Dummy(ImVec2(0.0f, Space::Xs));
    if (SecondaryButton("audiorefresh", "刷新设备列表",
                        ImVec2(Control::BrowseBtn + 40.0f, Control::ButtonSm)))
    {
        g_sysPicker.Refresh(false);
        g_micPicker.Refresh(true);
        st.SetToast(L"已重新枚举音频设备");
    }

    // ---- 音频码率（任一路开启才有意义）----
    if (cfg.withSystemAudio || cfg.withMic)
    {
        ImGui::Dummy(ImVec2(0.0f, Space::Sm));
        int idx = ValueToIndex(kAudioBrValues, CountOf(kAudioBrValues), cfg.audioBitrate, 2);
        if (ComboRow("abr", "音频码率", labelW, kAudioBrItems, CountOf(kAudioBrItems), &idx))
        {
            cfg.audioBitrate = kAudioBrValues[idx];
            SaveSettings(cfg);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    if (ToggleRow("hwenc", "硬件加速编码", &cfg.hwEncode))
    {
        SaveSettings(cfg);
        st.SetToast(L"编码方式将在下次开始录制时生效");
    }
    {
        // 探测结果只取一次；同时说明「关闭」的实际含义，避免用户以为能提升画质
        const std::wstring& hw = HardwareEncoderName();
        std::string encHint;
        if (hw.empty())
            encHint = "未检测到硬件编码器，录制将使用软件编码（关闭开关无影响）";
        else if (cfg.hwEncode)
            encHint = "编码器：" + WideToUtf8Str(hw) + "；关闭后改用软件编码，CPU 占用会明显上升";
        else
            encHint = "当前使用软件编码；该机器可用硬件编码器：" + WideToUtf8Str(hw);
        HintText(encHint.c_str());
    }

    if (ToggleRow("cursor", "画面包含鼠标指针", &cfg.includeCursor))
    {
        SaveSettings(cfg);
        st.SetToast(L"鼠标指针设置将在下次开始录制时生效");
    }

    if (busy)
        ImGui::EndDisabled();
    ImGui::EndChild();   // 配置区结束

    // ---- 输出概要 ----
    char summary[256];
    std::string pathText = WideToUtf8Str(EllipsizePath(PreviewRecordPath(cfg), 38));
    snprintf(summary, sizeof(summary), "输出：%s", pathText.c_str());
    HintText(summary);
    UINT estW = 0, estH = 0;
    RecordOutputSize(st.previewW, st.previewH, cfg.recResolution, estW, estH);
    if (!estW || !estH)   // 画面尺寸未知时按档位标称尺寸估算
    {
        // 标称尺寸与 RecordOutputSize 的档位表保持一致，避免预估与实际不符
        static const unsigned kNominal[RecResCount][2] = {
            { 0, 0 }, { 1920, 1080 }, { 1280, 720 }, { 2560, 1440 }, { 3840, 2160 }
        };
        const int idx = (cfg.recResolution >= 0 && cfg.recResolution < RecResCount)
                            ? cfg.recResolution : RecResNative;
        estW = kNominal[idx][0];
        estH = kNominal[idx][1];
        if (!estW || !estH)   // 原画档无标称尺寸，画面源未知时按 1080p 粗估
        {
            estW = 1920;
            estH = 1080;
        }
    }
    // 码率与录制入口共用 ResolveRecordBitrateMbps，否则预估体积会与实际文件不符
    const int estBr = ResolveRecordBitrateMbps(cfg, estW, estH);
    snprintf(summary, sizeof(summary), "预计约 %lld MB / 小时（%u×%u @ %d fps，%d Mbps）",
             (long long)(estBr * 1000LL * 3600 / 8 / 1024 / 1024), estW, estH, cfg.fps, estBr);
    HintText(summary);

    // ---- 主控制按钮 ----
    char hkText[48] = {};
    strncpy_s(hkText, WideToUtf8Str(ctx.hotkeyRecord).c_str(), _TRUNCATE);
    float bw = ImGui::GetContentRegionAvail().x;

    if (saving)
    {
        // 保存期间不可交互。禁用态不应带「危险」语义，且 PrimaryButton 在
        // enabled=false 时底色会被覆写为 Disabled，这里直接传 Disabled 以免误导读参者
        PrimaryButton("stop", "正在保存…", ImVec2(bw, btnH), Pal::Disabled(), nullptr, false);
        HintText("正在写入文件尾部，请勿关闭程序");
    }
    else if (recording)
    {
        if (PrimaryButton("stop", "停止录制", ImVec2(bw, btnH), Pal::Danger(), hkText))
            st.wantStopRecord = true;
    }
    else
    {
        bool ready = st.previewW > 0;
        if (PrimaryButton("start", "开始录制", ImVec2(bw, btnH), Pal::Success(),
                          hkText, ready))
        {
            st.wantStartRecord = true;
        }
        if (!ready)
            HintTextColored("尚未捕获到画面，无法开始录制", Pal::Warning());
    }

    ImGui::EndChild();
    PopCardStyle();
}
