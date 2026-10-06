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
#include "core/update.h"
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

// 热键编辑缓冲（与设置页共用，SetHotkeyText 会同步实际生效值）
constexpr int kHotkeyCount = 3;
char g_hkEdit[kHotkeyCount][64] = {};
// 正在捕获按键的热键行（-1 = 无）。点「更改」后进入，捕获成功自动应用，
// Esc/切页/再点取消。主循环据此吞掉 WM_HOTKEY，避免按到旧组合误触
int g_hkCapturing = -1;

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
    if (page != page_)
        g_hkCapturing = -1;   // 切页作废未完成的按键捕获（否则热键被一直吞掉）
    page_ = page;
    if (cfg_)
        cfg_->lastPage = page;
}

bool UiApp::IsCapturingHotkey() const
{
    return g_hkCapturing != -1;
}

// ---------------------------------------------------------------------------
// 设置页：版本与更新
// ---------------------------------------------------------------------------
// 用户主动点过检查/下载/重试/安装后才允许失败弹模态框：
// 开机自动检查在无网络时失败是常态，那种只能写状态行，否则每次启动都弹框.
// 注意 armed 是进程内一次性的武装：成功落地（UpToDate）或用户明确放弃
// （忽略版本/稍后）时必须解除，否则手动成功一次就会让之后某次自动失败误弹。
// Available/Ready 成功态不自动解除——下载/安装的武装靠下一步用户动作重新置位，
// 若在此清零，StartDownload 工作线程尚未把状态切到 Downloading 的窗口期内
// 武装会被误清（仍停在 Available 的 1~3 帧），下载失败就弹不出来了。
// 文件作用域 static：匿名命名空间内的卡片/弹窗与对外的 ArmUpdateFailurePopup
// （托盘手动入口用）共用同一份，避免内外两份标记漂移。
static bool g_updateFailedArmed = false;

// 对外：托盘等 UI 之外的手动入口先武装再 CheckNow（见 ui_app.h 注释）
void ArmUpdateFailurePopup()
{
    g_updateFailedArmed = true;
}

namespace {

// Release 正文是 Markdown 原文，直接塞进弹窗会很长；这里截断到 kNotesMaxChars
constexpr size_t kNotesMaxChars = 700;

// 去掉 CR，并截断超长正文（弹窗高度不可控，正文必须自己收口）
std::string ClipNotes(const std::string& notes)
{
    std::string s;
    s.reserve(notes.size());
    for (char c : notes)
        if (c != '\r')
            s += c;
    // 去掉首尾空行
    size_t b = s.find_first_not_of(" \n");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \n");
    s = s.substr(b, e - b + 1);
    if (s.size() > kNotesMaxChars)
    {
        // 截到 UTF-8 字符边界再截断：直接 resize 会拦腰砍断多字节序列，
        // 留下的半个汉字会被 ImGui 渲染成乱码方块
        size_t cut = kNotesMaxChars;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
            --cut;   // 落在续字节上：回退到该字符的首字节
        s.resize(cut);
        s += "…";
    }
    return s;
}

// 「版本与更新」卡片。高度按内容实际结束位置反算（与上方热键卡片同一手法），
// 不写死数值：状态行在「检查中 / 有新版本 / 下载进度 / 已就绪」之间切换，
// 行数会变，写死必然溢出（武装标记见文件作用域 g_updateFailedArmed）

// 确认替换：先武装（Apply 的三条 SetError 失败都会进 Failed 弹窗），再拉起助手。
// Apply 失败已由 Update 置 Failed 并写 ErrorText，失败弹窗会展示，不再叠 errPopup
// （两个模态框会叠在一起）；仅当状态未进 Failed（Apply 前置拒绝且未写错误，
// 理论上 UI 已按 Ready 设防、走不到）才 fallback 到 errPopup，避免静默无反应。
bool TryApplyUpdate(AppState& st)
{
    g_updateFailedArmed = true;
    if (Update::Apply())
    {
        st.wantApplyUpdate = true;
        return true;
    }
    if (Update::CurrentState() != Update::State::Failed)
    {
        const std::string e = Update::ErrorText();
        st.errPopup = e.empty() ? "无法启动更新助手" : e;
    }
    return false;
}

void DrawUpdateCard(const UiContext& ctx, float cardW)
{
    Settings& cfg = *ctx.cfg;
    AppState& st = *ctx.st;
    const Update::State us = Update::CurrentState();
    const Update::Info info = Update::LatestInfo();
    const bool busy = st.IsBusy();   // 录制/保存中不允许换文件

    const float pad = Control::CardPad;
    ImVec2 cardMin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(cardMin.x + pad, cardMin.y + pad));
    ImGui::Indent(pad);
    SectionTitle("版本与更新");

    // ---- 当前版本 ----
    ImGui::TextUnformatted("当前版本");
    ImGui::SameLine(120.0f);
    ImGui::TextColored(Pal::Text(), "%s（%s）", Update::VersionDisplay(), Update::ArchName());

    // ---- 状态行 ----
    const std::string status = Update::StatusText();
    if (us == Update::State::Failed)
        HintTextColored(status.c_str(), Pal::Danger());
    else if (us == Update::State::Available || us == Update::State::Ready)
        HintTextColored(status.c_str(), Pal::Accent());
    else
        HintText(status.c_str());

    // ---- 下载进度 ----
    if (us == Update::State::Downloading)
    {
        // 内缩必须写进 size_arg，不能用 SetNextItemWidth：
        // ProgressBar 走 CalcItemSize(size_arg, CalcItemWidth(), ...)，
        // 而 CalcItemSize 只在 size.x == 0 时才取 CalcItemWidth 的值；
        // 传 -1 之类的负值是「相对内容区宽度」语义，SetNextItemWidth 完全不起作用
        // （表现为右缘仍压在卡片描边上，少扣了 CardPad）
        // 本卡片内容区右缘即卡片描边，扣 CardPad 留白、再扣 EdgeInset 防裁剪线削掉半像素
        ImGui::ProgressBar((float)Update::Progress(),
                           ImVec2(-(pad + Control::EdgeInset), 0.0f));
    }

    // ---- 操作按钮 ----
    // 录制/保存期间一律禁用：换文件必须等程序完全空闲，否则助手要等主进程退出，
    // 而主进程此时还在等 Finalize，用户看到的是「点了没反应」
    ImGui::BeginDisabled(busy || Update::Busy());
    if (us == Update::State::Available)
    {
        if (SecondaryButton("up_dl", "下载更新", ImVec2(110.0f, Control::Button)))
        {
            g_updateFailedArmed = true;
            // StartDownload 起线程失败时保持 Available（调用方据返回值决定提示），
            // 不关任何东西、不误报“已开始”，用户可直接再点一次
            if (Update::StartDownload())
                st.SetToast(L"开始下载新版本，下载完成后请点击「重启安装」");
            else
                st.SetToast(L"下载任务启动失败，请重试");
        }
        ImGui::SameLine();
        if (SecondaryButton("up_ignore", "忽略此版本", ImVec2(120.0f, Control::Button)))
        {
            Update::SkipVersion();
            // 用户明确放弃本轮更新：解除武装，避免之后某次自动失败误弹
            g_updateFailedArmed = false;
            st.SetToast(L"已忽略该版本，可在 24 小时后或手动再次检查");
        }
    }
    else if (us == Update::State::Ready)
    {
        if (PrimaryButton("up_install", "重启安装", ImVec2(130.0f, Control::Button),
                          Pal::Accent()))
        {
            // Apply() 只是拉起替换助手并返回 true，真正的换文件与重启由助手完成，
            // 故此处必须让主循环退出（wantApplyUpdate）；失败进 Failed 弹窗，
            // 不再叠 errPopup（见 TryApplyUpdate）
            TryApplyUpdate(st);
        }
        ImGui::SameLine();
        if (SecondaryButton("up_later", "稍后", ImVec2(90.0f, Control::Button)))
        {
            // 暂不安装：解除武装（安装按钮会重新武装），之后自动失败不打扰
            g_updateFailedArmed = false;
            st.SetToast(L"已下载，可在设置页随时点击「重启安装」");
        }
    }
    else
    {
        // 检查中时 Update::Busy() 为真，上面那个 BeginDisabled 已经把按钮禁掉了，
        // 文案同步改成「检查中…」让禁用原因可见
        const bool checking = (us == Update::State::Checking);
        if (SecondaryButton("up_check", checking ? "检查中…" : "检查更新",
                            ImVec2(120.0f, Control::Button)))
        {
            g_updateFailedArmed = true;
            // 起线程失败（上一轮线程未回收）时保持原状态并提示，不静默吞掉点击
            if (!Update::CheckNow())
                st.SetToast(L"已有更新任务在进行中，请稍后再试");
        }
        ImGui::SameLine();
        if (SecondaryButton("up_web", "打开发布页", ImVec2(120.0f, Control::Button)))
            Update::OpenReleasePage();
    }
    ImGui::EndDisabled();

    // ---- 自动检查开关 ----
    // 必须显式传 rightInset：本卡片是手动布局（背景走通道 0，内容画在页面 child 里），
    // 自身没有 WindowPadding，内容区右缘即卡片描边；不内缩开关会紧贴卡片右缘，
    // 与上方 BeginChild 卡片里的开关对不齐
    bool autoChk = cfg.updateAutoCheck;
    if (ToggleRow("up_auto", "启动时自动检查更新", &autoChk, pad))
    {
        cfg.updateAutoCheck = autoChk;
        Update::SetAutoCheck(autoChk);
        SaveSettings(cfg);
        st.SetToast(autoChk ? L"已开启启动自动检查（最短间隔 24 小时）"
                            : L"已关闭启动自动检查");
    }
    HintText("更新包来自本项目的 GitHub Releases（HTTPS 下载）；"
             "程序未做代码签名，请只从官方 Release 获取");

    ImGui::Unindent(pad);   // 与开头 Indent 配对

    ImVec2 cardMax(cardMin.x + cardW,
                   ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y + pad);
    dl->ChannelsSetCurrent(0);
    DrawCard(cardMin, cardMax, Radius::Card, Pal::CardBg(), pad);
    dl->ChannelsMerge();
}

// 发现新版本的模态弹窗。latch 保证同一次发现只弹一次，
// 「稍后」关闭后要等状态离开 Available（下载/忽略/重新检查）才允许再弹
bool g_updatePopupLatch = false;
// 「已就绪」保证同一结果只提示一次，离开 Ready 即复位；
// 「失败」按错误文本去重（新的失败原因才再弹），离开 Failed 即清空——
// 否则重试后报同样的错弹不出来
bool g_updateReadyLatch = false;
std::string g_updateFailedSeen;

// 下载中的全页面浮层进度。设置页卡片里也有进度条，但用户若停在其他页
// （尤其是从发现弹窗一点就关的路径），没有任何可见反馈，会以为“点了没反应”
// 标题全阶段统一为一个；NoFocusOnAppearing 避免录制中突然抢走录制按钮的焦点；
// NoFocusOnAppearing 避免录制中突然抢走录制按钮的焦点；位置用 Appearing 而非
// FirstUseEver——主窗口移动/缩放后下一次下载能重新居中，而不是沿用首次的老坐标
void DrawUpdateProgressOverlay()
{
    const Update::State us = Update::CurrentState();
    if (us != Update::State::Downloading)
        return;
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("正在更新", nullptr,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoFocusOnAppearing))
    {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", Update::StatusText().c_str());
    if (us == Update::State::Downloading)
        ImGui::ProgressBar((float)Update::Progress(), ImVec2(-1.0f, 0.0f));
    HintText("下载在后台进行，完成后会提示安装（窗口可拖开，不影响录制按钮）");
    ImGui::End();
}

void DrawUpdatePopup(const UiContext& ctx)
{
    AppState& st = *ctx.st;

    DrawUpdateProgressOverlay();

    const Update::State us = Update::CurrentState();

    // 成功落地即解除武装：本轮手动操作已有结果，之后若出现自动失败不应误弹。
    // 只清 UpToDate——Available/Ready 不清：下载/安装的武装靠下一步用户动作重建，
    // 若在此清零，StartDownload 工作线程尚未切到 Downloading 的窗口期内武装会被误清。
    if (us == Update::State::UpToDate)
        g_updateFailedArmed = false;

    // ---- 已就绪：之前只在设置页出现按钮，不切过去就永远不知道下完了 ----
    // 注意该弹窗是一次性的（latch）：点「稍后」关闭后不再自动重开，
    // 用户可到设置页随时「重启安装」——避免每帧重开骚扰用户
    if (us == Update::State::Ready)
    {
        if (!g_updateReadyLatch)
        {
            g_updateReadyLatch = true;
            ImGui::OpenPopup("更新已就绪");
            st.SetToast(L"新版本已下载完成，可随时重启安装");
        }
        if (!ImGui::BeginPopupModal("更新已就绪", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        const Update::Info info = Update::LatestInfo();
        ImGui::TextColored(Pal::Accent(), "%s", info.tag.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("当前 %s", Update::VersionDisplay());
        ImGui::Dummy(ImVec2(0.0f, Space::Sm));
        ImGui::TextWrapped("%s 已下载完成，重启后生效", info.tag.c_str());
        ImGui::Dummy(ImVec2(0.0f, Space::Md));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, Space::Sm));
        // 录制/保存中不允许换文件：与设置页卡片一致，主循环里 wantApplyUpdate 也有兜底
        constexpr float kBW = 118.0f, kBH = Control::Button;
        ImGui::BeginDisabled(st.IsBusy());
        if (PrimaryButton("upr_install", "重启安装", ImVec2(kBW, kBH), Pal::Accent()))
        {
            // 失败进 Failed 弹窗，不叠 errPopup（见 TryApplyUpdate）；成功才关本弹窗
            if (TryApplyUpdate(st))
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (SecondaryButton("upr_later", "稍后", ImVec2(70.0f, kBH)))
        {
            g_updateFailedArmed = false;
            ImGui::CloseCurrentPopup();
        }
        if (st.IsBusy())
            HintText("录制/保存期间暂不能替换，可稍后再来安装");
        ImGui::EndPopup();
        return;
    }
    g_updateReadyLatch = false;

    // ---- 失败：之前只写设置页状态行，不在设置页就完全看不到 ----
    if (us == Update::State::Failed)
    {
        // ErrorText 理论上非空（所有 SetError 都带文案），但防御性给 fallback：
        // 空串会与 g_updateFailedSeen 初始值撞车导致永不弹框，且弹窗正文不能空
        const std::string rawErr = Update::ErrorText();
        const std::string err = rawErr.empty() ? "更新失败（未知错误）" : rawErr;
        if (g_updateFailedSeen != err)
        {
            g_updateFailedSeen = err;
            // 开机自动检查的失败不打扰：无网络时每次启动都弹框比不弹更糟
            if (g_updateFailedArmed)
            {
                g_updateFailedArmed = false;
                ImGui::OpenPopup("更新失败");
            }
        }
        if (!ImGui::BeginPopupModal("更新失败", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::PushStyleColor(ImGuiCol_Text, Pal::Danger());
        ImGui::TextWrapped("%s", err.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, Space::Md));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, Space::Sm));
        constexpr float kBW = 118.0f, kBH = Control::Button;
        // 下载阶段的失败可按原发布信息直接重下（省一次检查往返）；
        // 检查阶段的失败只能重新检查（旧信息不可信）。按钮文案据此区分
        const bool canDl = Update::CanRetryDownload();
        if (SecondaryButton("upf_retry", canDl ? "重试下载" : "重试", ImVec2(kBW, kBH)))
        {
            g_updateFailedArmed = true;
            // 返回值决定关不关弹窗：起线程失败（上一轮线程未回收）时保持 Failed，
            // 此时关了弹窗就再也弹不回来了，必须留着让用户再点一次
            const bool started = canDl ? Update::RetryDownload() : Update::CheckNow();
            if (started)
                ImGui::CloseCurrentPopup();
            else
                st.SetToast(L"已有更新任务在进行中，请稍后再试");
        }
        ImGui::SameLine();
        if (SecondaryButton("upf_web", "打开发布页", ImVec2(kBW, kBH)))
        {
            Update::OpenReleasePage();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (SecondaryButton("upf_close", "关闭", ImVec2(70.0f, kBH)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    g_updateFailedSeen.clear();

    if (us != Update::State::Available)
    {
        g_updatePopupLatch = false;
        return;
    }
    if (!g_updatePopupLatch)
    {
        ImGui::OpenPopup("发现新版本");
        g_updatePopupLatch = true;
    }
    if (!ImGui::BeginPopupModal("发现新版本", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const Update::Info info = Update::LatestInfo();
    ImGui::TextColored(Pal::Accent(), "%s", info.tag.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("当前 %s", Update::VersionDisplay());
    ImGui::Dummy(ImVec2(0.0f, Space::Sm));

    // 发布说明：可能为空（没写 Release 正文），此时不占位
    const std::string notes = ClipNotes(info.notes);
    if (!notes.empty())
        ImGui::TextWrapped("%s", notes.c_str());
    if (!info.assetName.empty())
    {
        ImGui::Dummy(ImVec2(0.0f, Space::Xs));
        ImGui::TextDisabled("安装包：%s", info.assetName.c_str());
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Md));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, Space::Sm));

    constexpr float kBW = 118.0f, kBH = Control::Button;
    if (SecondaryButton("upd_dl", "下载更新", ImVec2(kBW, kBH)))
    {
        // 之前这里点了就关弹窗且无任何提示，下载进度又只在设置页看得见——
        // “点了没反应”的观感就从这来的
        g_updateFailedArmed = true;
        // 起线程失败时不关弹窗不报开始，用户可直接再点（见卡片同名按钮注释）
        if (Update::StartDownload())
        {
            st.SetToast(L"开始下载新版本，完成后会提示安装");
            ImGui::CloseCurrentPopup();
        }
        else
            st.SetToast(L"下载任务启动失败，请重试");
    }
    ImGui::SameLine();
    if (SecondaryButton("upd_web", "打开发布页", ImVec2(kBW, kBH)))
    {
        Update::OpenReleasePage();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (SecondaryButton("upd_ignore", "忽略此版本", ImVec2(kBW, kBH)))
    {
        Update::SkipVersion();
        g_updateFailedArmed = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (SecondaryButton("upd_later", "稍后", ImVec2(70.0f, kBH)))
    {
        g_updateFailedArmed = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

}   // namespace

// ---------------------------------------------------------------------------
// 设置页（热键）
// ---------------------------------------------------------------------------
namespace {

// 三行热键统一应用：校验 -> 写配置 -> 通知主循环重注册 -> 落盘。
// 手动输入的「应用并保存」与按键捕获的自动应用走同一入口
bool ApplyAllHotkeys(const UiContext& ctx)
{
    Settings& cfg = *ctx.cfg;
    AppState& st = *ctx.st;
    std::wstring texts[kHotkeyCount] = {
        Utf8ToWide(g_hkEdit[0]),
        Utf8ToWide(g_hkEdit[1]),
        Utf8ToWide(g_hkEdit[2]),
    };
    for (int i = 0; i < kHotkeyCount; ++i)
    {
        if (!HotkeyManager::IsValidText(texts[i]))
        {
            st.errPopup = "热键格式无法识别，请参照下方说明";
            return false;
        }
    }
    cfg.hotkeyCapture = texts[0];
    cfg.hotkeyRecord  = texts[1];
    cfg.hotkeyShow    = texts[2];
    *ctx.hotkeyDirty = true;
    SaveSettings(cfg);
    st.SetToast(L"热键设置已保存，正在重新注册…");
    return true;
}

// 捕获行每帧轮询：Esc 取消；主键按下 + 修饰键 -> 组装 -> 填入 -> 自动应用。
// 注意先按住修饰键再按主键：主键先落下那一刻修饰键还没就位，会提示重按一次
void PollHotkeyCapture(const UiContext& ctx, int row)
{
    AppState& st = *ctx.st;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        g_hkCapturing = -1;
        st.SetToast(L"已取消更改热键");
        return;
    }

    UINT hitVk = 0;
    for (int c = 0; c < 26 && !hitVk; ++c)   // A-Z（枚举连续，见 imgui.h）
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + c), false))
            hitVk = (UINT)('A' + c);
    for (int c = 0; c < 10 && !hitVk; ++c)   // 0-9
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + c), false))
            hitVk = (UINT)('0' + c);
    for (int c = 0; c < 24 && !hitVk; ++c)   // F1-F24
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_F1 + c), false))
            hitVk = (UINT)(VK_F1 + c);
    if (!hitVk)
    {
        // 其余主键只收 Parse 支持集内的（与 KeyFromName 同源），无关按键直接忽略
        static const struct { ImGuiKey key; UINT vk; } kSpecial[] = {
            { ImGuiKey_Space,       VK_SPACE    },
            { ImGuiKey_Insert,      VK_INSERT   },
            { ImGuiKey_Delete,      VK_DELETE   },
            { ImGuiKey_Pause,       VK_PAUSE    },
            { ImGuiKey_Tab,         VK_TAB      },
            { ImGuiKey_Home,        VK_HOME     },
            { ImGuiKey_End,         VK_END       },
            { ImGuiKey_PrintScreen, VK_SNAPSHOT },
        };
        for (const auto& s : kSpecial)
        {
            if (ImGui::IsKeyPressed(s.key, false))
            {
                hitVk = s.vk;
                break;
            }
        }
    }
    if (!hitVk)
        return;   // 纯修饰键或无关按键：继续等

    const ImGuiIO& io = ImGui::GetIO();
    std::wstring composed;
    if (!HotkeyManager::Compose(io.KeyCtrl, io.KeyAlt, io.KeyShift, io.KeySuper,
                                hitVk, composed))
    {
        st.SetToast(L"组合键至少需要一个修饰键（Ctrl/Alt/Shift/Win），请重新按键");
        return;
    }
    CopyToBuf(g_hkEdit[row], sizeof(g_hkEdit[row]), composed);
    g_hkCapturing = -1;
    ApplyAllHotkeys(ctx);   // 成功则 toast「已保存…」，失败（别行非法）弹格式说明
}

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

    // 右侧需留出「更改按钮 + 实际生效文本」的位置，输入宽度据此反推。
    // 按钮高传 0：SecondaryButton 会回退到 GetFrameHeight()，与输入框同高
    float fieldW = cardW - pad * 2.0f - 150.0f;
    constexpr float kChangeBtnW = 76.0f;
    const float hkInputW = fieldW - kChangeBtnW - ImGui::GetStyle().ItemSpacing.x;
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

    for (int i = 0; i < kHotkeyCount; ++i)
    {
        ImGui::PushID(i);
        SectionTitle(rows[i].label);

        const bool capturing = (g_hkCapturing == i);
        bool valid = true;
        if (capturing)
        {
            // 捕获态：输入框只做提示展示（禁用防编辑），按键轮询见 PollHotkeyCapture
            char prompt[96] = {};
            strncpy_s(prompt, "请按下组合键…（Esc 取消）", _TRUNCATE);
            ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(hkInputW);
            ImGui::InputText("##hkcap", prompt, sizeof(prompt),
                             ImGuiInputTextFlags_ReadOnly);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (SecondaryButton("hk_cancel", "取消", ImVec2(kChangeBtnW, 0.0f)))
            {
                g_hkCapturing = -1;
                st.SetToast(L"已取消更改热键");
            }
            PollHotkeyCapture(ctx, i);
        }
        else
        {
            valid = HotkeyManager::IsValidText(Utf8ToWide(rows[i].buf));
            // 别行捕获中：本行输入框禁用（按键只进捕获行），「更改」可点（切换捕获目标）
            const bool locked = (g_hkCapturing != -1);
            if (locked)
                ImGui::BeginDisabled();
            if (!valid)
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Pal::ErrorFieldBg());
            ImGui::SetNextItemWidth(hkInputW);
            ImGui::InputText("##hk", rows[i].buf, kHotkeyBufSize);
            if (!valid)
                ImGui::PopStyleColor();
            if (locked)
                ImGui::EndDisabled();
            ImGui::SameLine();
            if (SecondaryButton("hk_change", "更改", ImVec2(kChangeBtnW, 0.0f)))
            {
                // 点下按钮的同时输入框已被逐出焦点（点击即换 ActiveId），
                // 且捕获期间三行输入框全是禁用态，按键落不进任何框里
                g_hkCapturing = i;
            }
        }

        std::string effText = rows[i].eff[0] ? WideToUtf8Str(rows[i].eff) : std::string("未生效");
        ImGui::SameLine();
        if (rows[i].eff[0])
            ImGui::TextUnformatted(effText.c_str());
        else
            ImGui::TextColored(Pal::Warning(), "%s", effText.c_str());
        ImGui::PopID();

        // 错误不能只靠底色传达：色觉障碍用户无法分辨红底与普通底，
        // 故底色之外必须给出文字说明，且占用与说明同一行以保持行高稳定
        if (capturing)
            HintText("先按住修饰键（Ctrl/Alt/Shift/Win），再按主键（A-Z / 0-9 / F1-F12 等）");
        else if (valid)
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
        ApplyAllHotkeys(ctx);   // 手动输入入口；按键捕获成功后同样走这里自动应用
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

    // ================= 版本与更新 =================
    ImGui::Dummy(ImVec2(0.0f, Space::Sm));
    DrawUpdateCard(ctx, ImGui::GetContentRegionAvail().x);
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

    // 底部版本号（取自 core/version.h，与 exe 属性、Release tag 同源；
    // 主题开关在下方状态栏左下角，见 DrawStatusBar）
    PushFontSlot(FontSmall);
    const char* kVersion = Update::VersionDisplay();
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
    DrawUpdatePopup(ctx_);
    ImGui::End();
}
