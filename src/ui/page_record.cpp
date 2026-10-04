#include "page_record.h"
#include "ui_app.h"
#include "ui_widgets.h"
#include "app_state.h"
#include "settings/settings.h"
#include "recorder/recorder.h"
#include "audio/audio.h"
#include "capture/capture.h"
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

const char* const kSrcItems[] = { "显示器", "窗口", "区域" };
const int kSrcValues[] = { CapMonitor, CapWindow, CapRegion };

const char* const kDelayItems[] = { "立即开始", "延时 3 秒", "延时 5 秒", "延时 10 秒" };
const int kDelayValues[] = { 0, 3, 5, 10 };

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

// ---- 显示器选择器（与音频设备选择器同构：缓存 + 刷新按钮）----
struct MonitorPicker
{
    std::vector<MonitorInfo> devs;
    std::vector<std::string> labels;
    std::vector<const char*> items;
    bool valid = false;

    void Refresh()
    {
        devs.clear();
        EnumerateMonitors(devs);
        labels.clear();
        items.clear();
        for (const MonitorInfo& m : devs)
            labels.push_back(WideToUtf8Str(m.label));
        for (const std::string& s : labels)
            items.push_back(s.c_str());
        valid = true;
    }
    // 配置里的显示器已拔出时回落到主显示器
    int IndexOf(const std::wstring& device) const
    {
        for (size_t i = 0; i < devs.size(); ++i)
            if (devs[i].device == device)
                return (int)i;
        for (size_t i = 0; i < devs.size(); ++i)
            if (devs[i].primary)
                return (int)i;
        return 0;
    }
    int Count() const { return (int)devs.size(); }
};

MonitorPicker g_monPicker;

// ---- 窗口选择器（HWND 会话内有效，不进配置文件）----
struct WindowPicker
{
    std::vector<WindowInfo> wins;
    std::vector<std::string> labels;
    std::vector<const char*> items;
    int sel = 0;

    void Refresh(HWND exclude)
    {
        wins.clear();
        EnumerateWindows(wins, exclude);
        labels.clear();
        items.clear();
        for (const WindowInfo& w : wins)
            labels.push_back(WideToUtf8Str(w.title));
        for (const std::string& s : labels)
            items.push_back(s.c_str());
        sel = 0;
    }
};

WindowPicker g_winPicker;

void DrawWindowPickerModal(const UiContext& ctx)
{
    AppState& st = *ctx.st;
    if (ImGui::BeginPopupModal("选择窗口", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (g_winPicker.wins.empty())
        {
            ImGui::TextDisabled("没有可捕获的窗口");
        }
        else
        {
            ImGui::ListBox("##winlist", &g_winPicker.sel,
                           g_winPicker.items.data(), (int)g_winPicker.items.size(), 8);
        }
        if (SecondaryButton("win_ok", "确定", ImVec2(96.0f, Control::ButtonSm), nullptr)
            && !g_winPicker.wins.empty())
        {
            int s = g_winPicker.sel;
            if (s < 0 || s >= (int)g_winPicker.wins.size())
                s = 0;
            const WindowInfo& w = g_winPicker.wins[s];
            st.capWindow = w.hwnd;
            st.capWindowTitle = w.title;
            st.wantCaptureRestart = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (SecondaryButton("win_cancel", "取消", ImVec2(96.0f, Control::ButtonSm), nullptr))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}   // namespace

void DrawRecordPage(const UiContext& ctx)
{
    Settings& cfg = *ctx.cfg;
    AppState& st = *ctx.st;
    EnsureBufs(cfg);

    const bool recording = st.State() == RunState::Recording;
    const bool saving    = st.State() == RunState::Saving;
    const bool busy      = st.IsBusy();
    // 区域录制按所选区域原分辨率录，分辨率档位不生效：相关控件置灰并说明原因，
    // 否则界面显示的档位与实际文件不符（预估与实际必须同源）
    const bool regionMode = cfg.captureSource == CapRegion;

    const float pad = Control::CardPad;
    const float gap = pad;
    const float fullW = ImGui::GetContentRegionAvail().x;

    PageTitle("录屏",
              recording ? "正在录制，配置项已锁定以保证文件与界面显示一致"
                        : "输出 MP4（H.264 + AAC），可同时录制系统声音与麦克风");

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
        const long long sec = RecElapsedSec(st);
        if (st.paused)
        {
            snprintf(pvInfo, sizeof(pvInfo), "已暂停   %s   %ux%u",
                     FormatElapsed(sec).c_str(), st.previewW, st.previewH);
            ImGui::TextColored(Pal::Warning(), pvInfo);
        }
        else
        {
            snprintf(pvInfo, sizeof(pvInfo), "● REC   %s   %ux%u @ %d fps",
                     FormatElapsed(sec).c_str(), st.previewW, st.previewH, cfg.fps);
            ImGui::TextColored(Pal::Danger(), pvInfo);
        }
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

    // ---- 画面来源 ----
    SectionTitle("画面来源");
    {
        int idx = ValueToIndex(kSrcValues, CountOf(kSrcValues), cfg.captureSource, 0);
        if (ComboRow("src", "来源", labelW, kSrcItems, CountOf(kSrcItems), &idx))
        {
            cfg.captureSource = kSrcValues[idx];
            SaveSettings(cfg);
            st.wantCaptureRestart = true;
        }
        if (cfg.captureSource == CapMonitor)
        {
            if (ImGui::IsWindowAppearing() || !g_monPicker.valid)
                g_monPicker.Refresh();
            if (g_monPicker.Count() > 0)
            {
                int midx = g_monPicker.IndexOf(cfg.captureMonitorDevice);
                if (ComboRow("mondev", "显示器", labelW, g_monPicker.items.data(),
                             g_monPicker.Count(), &midx))
                {
                    cfg.captureMonitorDevice = g_monPicker.devs[midx].device;
                    SaveSettings(cfg);
                    st.wantCaptureRestart = true;
                }
            }
            else
            {
                HintTextColored("未枚举到显示器", Pal::Warning());
            }
        }
        else if (cfg.captureSource == CapWindow)
        {
            const bool winAlive = IsWindow(st.capWindow);
            std::string cur = st.capWindowTitle.empty()
                ? std::string("尚未选择")
                : WideToUtf8Str(st.capWindowTitle);
            if (cur.size() > 42)
                cur = cur.substr(0, 42) + "…";
            FieldLabel("窗口", labelW);
            ImGui::TextUnformatted(cur.c_str());
            ImGui::SameLine();
            if (SecondaryButton("pickwin", "选择窗口…",
                                ImVec2(Control::BrowseBtn + 40.0f, Control::ButtonSm), nullptr))
            {
                g_winPicker.Refresh(ctx.ownerHwnd);
                ImGui::OpenPopup("选择窗口");
            }
            DrawWindowPickerModal(ctx);
            // 窗口没了必须说清楚：此刻主循环尚未跑到回退分支（下一帧才处理），
            // 预览仍是冻结的最后一帧，说「已回退」不成立，只说后果
            if (!winAlive && !st.capWindowTitle.empty())
                HintTextColored("所选窗口已关闭，画面来源将回退到显示器", Pal::Warning());
            else
                HintText("录制所选窗口；窗口被关闭时录制会自动中止");
        }
        else
        {
            HintText("开始录制时拖选屏幕区域，按所选区域原分辨率录制");
        }
    }

    SectionDivider();
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

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    {
        int idx = ValueToIndex(kDelayValues, CountOf(kDelayValues), cfg.recDelaySec, 0);
        if (ComboRow("recdelay", "开始延时", labelW, kDelayItems, CountOf(kDelayItems), &idx))
        {
            cfg.recDelaySec = kDelayValues[idx];
            SaveSettings(cfg);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    if (ToggleRow("openfolder", "完成后打开文件位置", &cfg.openFolderAfterRec))
        SaveSettings(cfg);

    // ---- 画质 ----
    SectionDivider();
    SectionTitle("画质");
    // 一键预设：填充下方三项，可再微调。区域模式下分辨率不生效，
    // 预设仍可用（帧率与码率都生效）
    {
        // 三个按钮按内容区宽度精确铺满，最左/最右按钮的竖边描边正好压在滚动容器
        // 的裁剪边上被裁掉半像素，看上去就是没有边框。左右各内缩 EdgeInset 解决。
        const float inset = Control::EdgeInset;
        const float pw = (ImGui::GetContentRegionAvail().x
                          - inset * 2.0f - Space::Md * 2.0f) / 3.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inset);
        if (SecondaryButton("preset_hi", "清晰优先", ImVec2(pw, Control::ButtonSm), nullptr))
        {
            cfg.recResolution = RecResNative;
            cfg.fps = 60;
            cfg.bitrateMbps = 0;
            SaveSettings(cfg);
        }
        ImGui::SameLine(0.0f, Space::Md);
        if (SecondaryButton("preset_mid", "均衡", ImVec2(pw, Control::ButtonSm), nullptr))
        {
            cfg.recResolution = RecRes1080P;
            cfg.fps = 30;
            cfg.bitrateMbps = 0;
            SaveSettings(cfg);
        }
        ImGui::SameLine(0.0f, Space::Md);
        if (SecondaryButton("preset_lo", "小体积", ImVec2(pw, Control::ButtonSm), nullptr))
        {
            cfg.recResolution = RecRes720P;
            cfg.fps = 30;
            cfg.bitrateMbps = 8;
            SaveSettings(cfg);
        }
        HintText("预设：清晰=原画60fps自动码率；均衡=1080P30fps自动；小体积=720P30fps8Mbps");
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs));

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
        ImGui::BeginDisabled(regionMode);
        int idx = ValueToIndex(kResValues, CountOf(kResValues), cfg.recResolution, 4);
        if (ComboRow("res", "分辨率", labelW, kResItems, CountOf(kResItems), &idx))
        {
            cfg.recResolution = kResValues[idx];
            SaveSettings(cfg);
        }
        ImGui::EndDisabled();

        char resHint[160];
        if (regionMode)
        {
            // 区域模式的实际尺寸由开始录制时的框选决定，此处无法预知，只能说明规则
            snprintf(resHint, sizeof(resHint),
                     "实际录制：所选区域的原始尺寸（分辨率档位在区域模式下不生效）");
        }
        else
        {
            UINT rw = 0, rh = 0;
            RecordOutputSize(st.previewW, st.previewH, cfg.recResolution, rw, rh);
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
                snprintf(resHint, sizeof(resHint),
                         "实际录制：跟随画面源分辨率（所选档位更大时不放大）");
            }
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
        ImGui::ProgressBar(st.sysLevel.load() / 100.0f, ImVec2(-1.0f, 6.0f), "");
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
        ImGui::ProgressBar(st.micLevel.load() / 100.0f, ImVec2(-1.0f, 6.0f), "");
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
                        ImVec2(Control::BrowseBtn + 40.0f, Control::ButtonSm), nullptr))
    {
        g_sysPicker.Refresh(false);
        g_micPicker.Refresh(true);
        g_monPicker.valid = false;   // 显示器列表下次绘制时重枚举
        st.SetToast(L"已重新枚举音频设备与显示器");
    }
    if (cfg.withMic)
    {
        ImGui::SameLine();
        if (SecondaryButton("mictest", "试音 5 秒",
                            ImVec2(Control::BrowseBtn + 40.0f, Control::ButtonSm), nullptr))
            st.wantMicTest = true;
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
        st.wantCursorRefresh = true;
        st.SetToast(L"鼠标指针设置已实时生效");
    }

    if (busy)
        ImGui::EndDisabled();

    // 最近录制：放在 EndDisabled 之外，录制/保存中也能点开上一次的���件
    if (!st.recentRecs.empty())
    {
        SectionDivider();
        SectionTitle("最近录制");
        int shown = 0;
        for (const std::wstring& p : st.recentRecs)
        {
            if (shown >= 4)
                break;
            std::string name = WideToUtf8Str(p);
            const size_t pos = name.find_last_of("\\/");
            std::string base = (pos == std::string::npos) ? name : name.substr(pos + 1);
            if (base.size() > 40)
                base = base.substr(0, 40) + "…";
            char rid[32] = {};
            snprintf(rid, sizeof(rid), "recent_rec_%d", shown);
            if (SecondaryButton(rid, base.c_str(),
                                ImVec2(ImGui::GetContentRegionAvail().x, Control::ButtonSm),
                                nullptr))
            {
                if (!RevealInExplorer(p))
                    st.errPopup = "无法定位文件，可能已被移动或删除";
            }
            ++shown;
        }
        HintText("点击定位到文件");
    }

    ImGui::EndChild();   // 配置区结束

    // ---- 输出概要 ----
    char summary[256];
    std::string pathText = WideToUtf8Str(EllipsizePath(PreviewRecordPath(cfg), 38));
    snprintf(summary, sizeof(summary), "输出：%s", pathText.c_str());
    HintText(summary);
    // 区域模式的输出尺寸由框选决定，无法预知，故不给具体数字；
    // 码率仍按分辨率推导，这里只说明会落在哪个量级，避免出现与文件不符的估计
    if (regionMode)
    {
        snprintf(summary, sizeof(summary),
                 "体积预估：按所选区域尺寸与实际码率计算（码率随区域分辨率自动调整）");
        HintText(summary);
    }
    else
    {
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
    }

    // ---- 主控制按钮 ----
    char hkText[48] = {};
    strncpy_s(hkText, WideToUtf8Str(ctx.hotkeyRecord).c_str(), _TRUNCATE);
    float bw = ImGui::GetContentRegionAvail().x;

    if (st.countdownActive)
    {
        const ULONGLONG nowCd = GetTickCount64();
        const long long remain = st.countdownEnd > nowCd
            ? (long long)(st.countdownEnd - nowCd + 999) / 1000 : 0;
        if (PrimaryButton("cancelcd", "取消开始", ImVec2(bw, btnH), Pal::Warning(), nullptr))
            st.wantCancelCountdown = true;
        char cd[64] = {};
        snprintf(cd, sizeof(cd), "%lld 秒后开始录制…", remain > 0 ? remain : 0);
        HintText(cd);
    }
    else if (saving)
    {
        // 保存期间不可交互。禁用态不应带「危险」语义，且 PrimaryButton 在
        // enabled=false 时底色会被覆写为 Disabled，这里直接传 Disabled 以免误导读参者
        PrimaryButton("stop", "正在保存…", ImVec2(bw, btnH), Pal::Disabled(), nullptr, false);
        HintText("正在后台写入文件尾部，可继续使用界面（不要退出程序）");
    }
    else if (recording)
    {
        const float bw2 = (bw - Space::Md) * 0.5f;
        if (st.paused)
        {
            if (PrimaryButton("resume", "继续", ImVec2(bw2, btnH), Pal::Success(), hkText))
                st.wantResume = true;
        }
        else
        {
            if (SecondaryButton("pause", "暂停", ImVec2(bw2, btnH), nullptr))
                st.wantPause = true;
        }
        ImGui::SameLine(0.0f, Space::Md);
        if (PrimaryButton("stop", "停止", ImVec2(bw2, btnH), Pal::Danger(), hkText))
            st.wantStopRecord = true;
        if (st.paused)
            HintText("已暂停：画面与声音都不再写入文件");
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
