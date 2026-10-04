#include "ui_app.h"
#include "page_capture.h"
#include "page_record.h"
#include "ui_widgets.h"
#include "app_state.h"
#include "settings/settings.h"
#include "core/util.h"
#include "core/log.h"
#include "core/hotkey.h"
#include "core/autostart.h"
#include <windows.h>
#include <mfapi.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>

#include <cstdio>

namespace {

constexpr float kNavWidth  = Control::NavWidth;
constexpr float kStatusH   = Control::StatusBar;
constexpr int   kPageCount = 3;
const char* const kPageNames[kPageCount] = { "截屏", "录屏", "设置" };
constexpr const char* kVersion = "v1.0";

// 热键编辑缓冲（与设置页共用，SetHotkeyText 会同步实际生效值）
constexpr int kHotkeyCount = 3;
char g_hkEdit[kHotkeyCount][64] = {};

void CopyToBuf(char* dst, size_t cap, const std::wstring& s)
{
    std::string utf8 = WideToUtf8Str(s);
    strncpy_s(dst, cap, utf8.c_str(), _TRUNCATE);
}

}   // namespace

// ---------------------------------------------------------------------------
// 目录选择对话框
// ---------------------------------------------------------------------------
bool PickFolderDialog(HWND owner, std::wstring& dir)
{
    std::wstring init = dir.empty() ? DesktopDir() : dir;

    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择输出目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    bi.lParam = (LPARAM)init.c_str();

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return false;
    wchar_t chosen[MAX_PATH] = {};
    bool ok = SUCCEEDED(SHGetPathFromIDListW(pidl, chosen));
    CoTaskMemFree(pidl);
    if (ok)
        dir = chosen;
    return ok;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void UiApp::Init(AppState* st, Settings* cfg, HWND ownerHwnd)
{
    st_ = st;
    cfg_ = cfg;
    page_ = cfg ? cfg->lastPage : 0;
    if (page_ < 0 || page_ >= kPageCount)
        page_ = 0;

    ctx_.st = st;
    ctx_.cfg = cfg;
    ctx_.ownerHwnd = ownerHwnd;
    ctx_.hotkeyDirty = &hotkeyDirty_;

    for (int i = 0; i < kHotkeyCount; ++i)
        g_hkEdit[i][0] = '\0';   // 首次进入设置页时再从配置填充
}

void UiApp::Shutdown()
{
    st_ = nullptr;
    cfg_ = nullptr;
    ctx_ = UiContext();
}

void UiApp::SetHotkeyText(const std::wstring& capture, const std::wstring& record,
                          const std::wstring& show)
{
    ctx_.hotkeyCapture = capture;
    ctx_.hotkeyRecord = record;
    ctx_.hotkeyShow = show;
    CopyToBuf(g_hkEdit[0], sizeof(g_hkEdit[0]), capture);
    CopyToBuf(g_hkEdit[1], sizeof(g_hkEdit[1]), record);
    CopyToBuf(g_hkEdit[2], sizeof(g_hkEdit[2]), show);
}

bool UiApp::ConsumeHotkeyDirty()
{
    bool v = hotkeyDirty_;
    hotkeyDirty_ = false;
    return v;
}

void UiApp::SetPage(int page)
{
    if (page < 0 || page >= kPageCount)
        return;
    page_ = page;
    if (cfg_)
        cfg_->lastPage = page;
}

// ---------------------------------------------------------------------------
// 设置页（热键）
// ---------------------------------------------------------------------------
namespace {

void DrawSettingsPage(const UiContext& ctx)
{
    Settings& cfg = *ctx.cfg;
    AppState& st = *ctx.st;

    if (g_hkEdit[0][0] == '\0')
    {
        CopyToBuf(g_hkEdit[0], sizeof(g_hkEdit[0]), cfg.hotkeyCapture);
        CopyToBuf(g_hkEdit[1], sizeof(g_hkEdit[1]), cfg.hotkeyRecord);
        CopyToBuf(g_hkEdit[2], sizeof(g_hkEdit[2]), cfg.hotkeyShow);
    }

    PageTitle("设置", "全局快捷键在窗口隐藏时依然生效；与应用冲突时会自动回退");

    float pad = Control::CardPad;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cardMin = ImGui::GetCursorScreenPos();
    float cardW = ImGui::GetContentRegionAvail().x;

    // 卡片高度随内容自适应：内容画在通道 1，背景/边框画在通道 0 垫底。
    // 固定高度过小时内容会溢出卡片，下方按钮再按固定底缘定位就会压在溢出文字上。
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(cardMin.x + pad, cardMin.y + pad));
    // 卡片不是独立 child，没有 WindowPadding：换行后光标会退回内容区左缘（贴卡片边）。
    // 缩进一个 pad，让循环内每组的标题/输入框/说明、以及底部格式说明都保持同一左缘留白
    ImGui::Indent(pad);

    // 右侧需留出「实际生效」文本的位置，输入宽度据此反推
    float fieldW = cardW - pad * 2.0f - 150.0f;
    constexpr size_t kHotkeyBufSize = sizeof(g_hkEdit[0]);
    struct Row
    {
        const char* label;
        const char* desc;
        const wchar_t* eff;
        char* buf;
    };
    const Row rows[kHotkeyCount] = {
        { "区域截图", "全屏遮罩上拖拽选择区域", ctx.hotkeyCapture.c_str(), g_hkEdit[0] },
        { "开始 / 停止录制", "再次按下则结束当前录制", ctx.hotkeyRecord.c_str(),  g_hkEdit[1] },
        { "显示主窗口", "从托盘恢复窗口",           ctx.hotkeyShow.c_str(),    g_hkEdit[2] },
    };

    bool anyInvalid = false;
    for (int i = 0; i < kHotkeyCount; ++i)
    {
        ImGui::PushID(i);
        SectionTitle(rows[i].label);

        bool valid = HotkeyManager::IsValidText(Utf8ToWide(rows[i].buf));
        if (!valid)
            anyInvalid = true;

        if (!valid)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Pal::ErrorFieldBg());
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("##hk", rows[i].buf, kHotkeyBufSize);
        if (!valid)
            ImGui::PopStyleColor();

        std::string effText = rows[i].eff[0] ? WideToUtf8Str(rows[i].eff) : std::string("未生效");
        ImGui::SameLine();
        if (rows[i].eff[0])
            ImGui::TextUnformatted(effText.c_str());
        else
            ImGui::TextColored(Pal::Warning(), "%s", effText.c_str());
        ImGui::PopID();

        // 错误不能只靠底色传达：色觉障碍用户无法分辨红底与普通底，
        // 故底色之外必须给出文字说明，且占用与说明同一行以保持行高稳定
        if (valid)
            HintText(rows[i].desc);
        else
            HintTextColored("格式无法识别，请参照下方说明修正", Pal::Danger());
    }

    ImGui::NewLine();
    ImGui::TextDisabled("格式：修饰键(Ctrl/Alt/Shift/Win) + 键(A-Z / 0-9 / F1-F12)，如 Ctrl+Alt+S");
    ImGui::TextDisabled("配置文件：%s",
                        WideToUtf8Str(SettingsFilePath()).c_str());
    ImGui::Unindent(pad);   // 与卡片开头的 Indent 配对，避免缩进泄漏到下方按钮行

    // 内容终点扣除行尾 ItemSpacing 即卡片底缘，补上 padding 后垫背景
    ImVec2 cardMax(cardMin.x + cardW,
                   ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y + pad);
    dl->ChannelsSetCurrent(0);
    DrawCard(cardMin, cardMax, Radius::Card, Pal::CardBg(), pad);
    dl->ChannelsMerge();

    ImGui::SetCursorScreenPos(ImVec2(cardMin.x + pad, cardMax.y + Space::Md));
    if (SecondaryButton("apply_hk", "应用并保存", ImVec2(140.0f, Control::Button)))
    {
        if (anyInvalid)
        {
            st.errPopup = "热键格式无法识别，请参照下方说明";
        }
        else
        {
            cfg.hotkeyCapture = Utf8ToWide(g_hkEdit[0]);
            cfg.hotkeyRecord  = Utf8ToWide(g_hkEdit[1]);
            cfg.hotkeyShow    = Utf8ToWide(g_hkEdit[2]);
            *ctx.hotkeyDirty = true;
            SaveSettings(cfg);
            st.SetToast(L"热键设置已保存，正在重新注册…");
        }
    }
    ImGui::SameLine();
    if (SecondaryButton("open_cfg_dir", "打开配置目录", ImVec2(140.0f, Control::Button)))
    {
        std::wstring dir, file;
        SplitPath(SettingsFilePath(), dir, file);
        if (!EnsureDir(dir))
            st.errPopup = "无法创建配置目录";
        else if (!RevealInExplorer(dir + L"\\" + file))
            st.errPopup = "无法打开配置目录";
    }

    // ================= 通用 =================
    // 卡片高度按内容行数累加（标题 + 开关行 + 说明），避免写死被 DPI/字号放大撑爆
    {
        float gpad = Control::CardPad;
        float cardH = gpad * 2.0f
                    + TextLineH(FontSmall)                            // 「通用」小标题
                    + Control::Row + ImGui::GetStyle().ItemSpacing.y  // 开关行
                    + TextLineH(FontSmall);                           // 说明文字
        PushCardStyle(gpad);
        ImGui::BeginChild("general_card", ImVec2(0.0f, cardH),
                          ImGuiChildFlags_Borders);
        PopCardStyle();
        SectionTitle("通用");

        // 开关态每帧读注册表实际状态：外部（任务管理器/注册表编辑器）改动后 UI 也能如实显示；
        // 切换失败时不改配置，下一帧查询结果自动把开关弹回
        bool on = AutostartIsEnabled();
        if (ToggleRow("autostart", "开机自动启动", &on))
        {
            bool ok = on ? AutostartEnable() : AutostartDisable();
            if (ok)
            {
                cfg.autoStart = on;
                SaveSettings(cfg);
                st.SetToast(on ? L"已开启开机自动启动" : L"已关闭开机自动启动");
            }
            else
            {
                st.errPopup = on ? "开启自启动失败：无法写入注册表"
                                 : "关闭自启动失败：无法删除注册表值";
            }
        }
        HintText("登录 Windows 后自动启动并显示托盘图标");
        ImGui::EndChild();
    }
}

}   // namespace

// ---------------------------------------------------------------------------
// 导航栏
// ---------------------------------------------------------------------------
void UiApp::DrawNav(float height)
{
    ImVec2 mn = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(mn, ImVec2(mn.x + kNavWidth, mn.y + height),
                      ImGui::GetColorU32(Pal::NavBg()));
    dl->AddLine(ImVec2(mn.x + kNavWidth, mn.y),
                ImVec2(mn.x + kNavWidth, mn.y + height),
                ImGui::GetColorU32(Pal::Border()));

    // 顶部标识：应用图标（与标题栏/任务栏/托盘同一图标，纹理由 main 预先生成）
    const float iconPx = Space::Xl + Space::Sm;
    float titleY = mn.y + Space::Xl - Space::Xs;
    if (st_ && st_->appIconSRV)
    {
        const float ix = mn.x + (kNavWidth - iconPx) * 0.5f;
        const float iy = mn.y + Space::Sm;
        dl->AddImage((ImTextureID)st_->appIconSRV.Get(), ImVec2(ix, iy),
                     ImVec2(ix + iconPx, iy + iconPx));
        titleY = iy + iconPx + 4.0f;
    }
    PushFontSlot(FontTitle);
    ImVec2 ts = ImGui::CalcTextSize("Capture");
    dl->AddText(ImVec2(mn.x + (kNavWidth - ts.x) * 0.5f, titleY),
                ImGui::GetColorU32(Pal::Text()), "Capture");
    PopFontSlot();

    // 导航项：首项自 kNavTop 起，之后按 项高 + 音阶间距 递推
    constexpr float kNavTop = 80.0f;   // 让出顶部图标 + 名称占用的高度
    float y = mn.y + kNavTop;
    for (int i = 0; i < kPageCount; ++i)
    {
        ImGui::SetCursorScreenPos(ImVec2(mn.x + Space::Sm, y));
        if (NavItem(kPageNames[i], kPageNames[i], page_ == i,
                    kNavWidth - Space::Sm * 2.0f, Control::NavItem))
        {
            if (page_ != i)
            {
                page_ = i;
                if (cfg_)
                    cfg_->lastPage = i;
            }
        }
        y += Control::NavItem + Control::NavGap;
    }

    // 底部版本号（主题开关在下方状态栏左下角，见 DrawStatusBar）
    PushFontSlot(FontSmall);
    ImVec2 vs = ImGui::CalcTextSize(kVersion);
    dl->AddText(ImVec2(mn.x + (kNavWidth - vs.x) * 0.5f,
                       mn.y + height - Space::Md - Control::Row * 0.5f),
                ImGui::GetColorU32(Pal::TextDim()), kVersion);
    PopFontSlot();
}

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------
void UiApp::DrawStatusBar(float width)
{
    AppState& st = *st_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = ImGui::GetWindowHeight() - kStatusH;
    ImVec2 mn(kNavWidth, y);
    ImVec2 mx(width, y + kStatusH);
    // 导航栏底部的左下角空区一并铺底色，作为主题开关的落点（与导航栏连成一体）
    dl->AddRectFilled(ImVec2(0.0f, y), mx, ImGui::GetColorU32(Pal::NavBg()));
    dl->AddLine(ImVec2(0.0f, mn.y), ImVec2(mx.x, mn.y), ImGui::GetColorU32(Pal::Border()));

    // 左下角：主题开关（点击切换浅/深色），位于版本号下方、状态文字左侧
    const float tgW = 64.0f, tgH = Control::Row;
    ImGui::SetCursorScreenPos(ImVec2((kNavWidth - tgW) * 0.5f,
                                     (mn.y + mx.y) * 0.5f - tgH * 0.5f));
    if (ThemeToggle("theme_mode", tgW, tgH) && cfg_)
    {
        cfg_->darkMode = IsDarkMode();
        SaveSettings(*cfg_);
        st_->SetToast(IsDarkMode() ? L"已切换到深色模式" : L"已切换到浅色模式");
    }

    // 状态点 + 文本
    ImVec4 dot = Pal::TextDim();
    const char* text = "就绪";
    char timer[16] = {};
    if (st.State() == RunState::Recording)
    {
        dot = st.paused ? Pal::Warning() : Pal::Danger();
        const long long sec = RecElapsedSec(st);
        snprintf(timer, sizeof(timer), "%02lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
        text = timer;
    }
    else if (st.State() == RunState::Saving)
    {
        dot = Pal::Warning();
        text = "正在保存文件…";
    }

    ImVec2 c = ImVec2(mn.x + Space::Lg, (mn.y + mx.y) * 0.5f);
    dl->AddCircleFilled(c, Space::Xs, ImGui::GetColorU32(dot));
    PushFontSlot(FontSmall);
    dl->AddText(ImVec2(c.x + Space::Md, c.y - ImGui::CalcTextSize(text).y * 0.5f),
                ImGui::GetColorU32(Pal::Text()), text);
    if (st.State() == RunState::Recording)
    {
        // 录制中紧跟计时器之后，间距按计时器文本宽度推算，避免压字
        const char* kRecLabel = st.paused ? "已暂停" : "录制中";
        dl->AddText(ImVec2(c.x + Space::Md + ImGui::CalcTextSize(text).x + Space::Sm,
                           c.y - ImGui::CalcTextSize(kRecLabel).y * 0.5f),
                    ImGui::GetColorU32(st.paused ? Pal::Warning() : Pal::Danger()), kRecLabel);
    }

    // 右侧：捕获帧率 + 音频异常提示（两路各自的失败原因合并展示，详细说明在录屏页）
    std::string right;
    ImVec4 rightCol = Pal::TextDim();
    if (!st.audioMicInfo.empty())
    {
        right = "麦克风不可用: " + WideToUtf8Str(st.audioMicInfo);
        rightCol = Pal::Warning();
    }
    else if (!st.audioSysInfo.empty())
    {
        right = "音频不可用: " + WideToUtf8Str(st.audioSysInfo);
        rightCol = Pal::Warning();
    }
    else
    {
        char fps[64];
        snprintf(fps, sizeof(fps), "捕获 %.0f FPS   %ux%u",
                 st.capFps, st.previewW, st.previewH);
        right = fps;
    }
    dl->AddText(ImVec2(mx.x - ImGui::CalcTextSize(right.c_str()).x - Space::Lg,
                       c.y - ImGui::CalcTextSize(right.c_str()).y * 0.5f),
                ImGui::GetColorU32(rightCol), right.c_str());

    // 临时提示：状态栏居中，5 秒后自动消失
    if (!st.toast.empty() && GetTickCount64() - st.toastTick < 5000)
    {
        std::string t = WideToUtf8Str(st.toast);
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        dl->AddText(ImVec2((mn.x + mx.x) * 0.5f - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    ImGui::GetColorU32(Pal::Accent()), t.c_str());
    }
    PopFontSlot();
}

// ---------------------------------------------------------------------------
// 错误弹窗
// ---------------------------------------------------------------------------
void UiApp::DrawErrorPopup()
{
    AppState& st = *st_;
    if (!st.errPopup.empty())
        ImGui::OpenPopup("错误");
    if (ImGui::BeginPopupModal("错误", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // 正文带语义色，使弹窗性质一眼可辨
        ImGui::PushStyleColor(ImGuiCol_Text, Pal::Danger());
        ImGui::TextUnformatted(st.errPopup.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, Space::Md));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, Space::Sm));
        // 走自绘按钮，避免弹窗内混入 ImGui 原生按钮造成风格割裂
        constexpr float kOkW = 96.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - kOkW) * 0.5f);
        if (SecondaryButton("err_ok", "确定", ImVec2(kOkW, Control::Button)))
        {
            st.errPopup.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// 主绘制
// ---------------------------------------------------------------------------
void UiApp::Draw()
{
    if (!st_ || !cfg_)
        return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("CaptureTool", nullptr, flags);
    ImGui::PopStyleVar(2);

    ctx_.st = st_;
    ctx_.cfg = cfg_;

    float contentH = ImGui::GetContentRegionAvail().y - kStatusH;
    DrawNav(contentH);

    // 内容区：页面统一留白，标题不再贴住窗口上缘与导航分隔线。
    // 注意：ImGui 1.90+ 会把无边框 child 的 WindowPadding 强制清零，
    // 必须加 ImGuiChildFlags_AlwaysUseWindowPadding 留白才能真正生效。
    ImGui::SetCursorScreenPos(ImVec2(kNavWidth, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Control::CardPad, Space::Md));
    ImGui::BeginChild("content", ImVec2(0.0f, contentH),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    switch (page_)
    {
    case 0:  DrawCapturePage(ctx_);  break;
    case 1:  DrawRecordPage(ctx_);   break;
    default: DrawSettingsPage(ctx_); break;
    }
    ImGui::EndChild();

    DrawStatusBar(io.DisplaySize.x);
    DrawErrorPopup();
    ImGui::End();
}
