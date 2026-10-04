#include "ui_theme.h"
#include "core/log.h"
#include "core/util.h"
#include <windows.h>

namespace {

ImFont* g_fonts[FontCount] = {};
const float kFontSizes[FontCount] = { 16.0f, 21.0f, 13.0f };
bool    g_darkMode = true;   // 默认深色

bool FileExists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

}   // namespace

// ---------------------------------------------------------------------------
// 主题模式
// ---------------------------------------------------------------------------
bool IsDarkMode()
{
    return g_darkMode;
}

void SetDarkMode(bool dark)
{
    g_darkMode = dark;
}

void ToggleDarkMode()
{
    g_darkMode = !g_darkMode;
}

ImFont* ThemeFont(FontSlot slot)
{
    const int i = static_cast<int>(slot);
    // 走主题层自己的槽位表，而非 ImFontAtlas::Fonts 的下标：
    // atlas 里可能混有加载失败留下的残留字体，下标与槽位并不等价
    if (i >= 0 && i < FontCount && g_fonts[i])
        return g_fonts[i];
    ImGuiIO& io = ImGui::GetIO();
    return io.FontDefault ? io.FontDefault : ImGui::GetFont();
}

void LoadThemeFonts()
{
    ImGuiIO& io = ImGui::GetIO();

    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);

    // 优先微软雅黑（自带中英混排），逐级降级
    const wchar_t* candidates[] = {
        L"\\Fonts\\msyh.ttc",      // 微软雅黑
        L"\\Fonts\\msyh.ttf",
        L"\\Fonts\\simhei.ttf",    // 黑体
        L"\\Fonts\\simsun.ttc",    // 宋体
        L"\\Fonts\\segoeui.ttf",   // 无中文字形，仅保底
    };
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();

    for (const wchar_t* rel : candidates)
    {
        std::wstring full = std::wstring(winDir) + rel;
        if (!FileExists(full.c_str()))
            continue;

        bool all = true;
        for (int i = 0; i < FontCount; ++i)
        {
            ImFontConfig cfg;
            cfg.PixelSnapH = true;     // 文字不糊
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            std::string utf8 = WideToUtf8Str(full);
            ImFont* f = io.Fonts->AddFontFromFileTTF(utf8.c_str(), kFontSizes[i], &cfg, ranges);
            if (!f)
            {
                all = false;
                break;
            }
            g_fonts[i] = f;
        }
        if (all)
        {
            io.FontDefault = g_fonts[FontBody];
            LOG_INFO(L"UI 字体已加载: %ls", full.c_str());
            return;
        }
        // 该字号之后全部放弃：已 Add 成功的字体无法从 atlas 移除，留在里面不用；
        // g_fonts 中未被覆盖的槽位保持空值，ThemeFont 会自动回退
        LOG_WARN(L"字体加载不完整, 尝试下一个: %ls", full.c_str());
    }

    // 全部候选失败：并入默认字体（无中文字形，中文会显示为方块）
    ImFont* fallback = io.Fonts->AddFontDefault();
    for (int i = 0; i < FontCount; ++i)
        g_fonts[i] = fallback;
    io.FontDefault = fallback;
    LOG_WARN(L"未找到可用中文字体, 回退默认字体(中文可能显示为方块)");
}

void ApplyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // 圆角一律取 Radius::，容器比内嵌控件大一级形成嵌套层次。
    // 中文行高比英文高，默认 8px 间距过挤，间距走 Space:: 音阶
    s.WindowRounding = Radius::None;              // 主窗口贴边铺满
    s.ChildRounding = Radius::Control;
    s.FrameRounding = Radius::Control;
    s.PopupRounding = Radius::Popup;
    s.GrabRounding = Radius::Control;
    s.ScrollbarRounding = Radius::Control;
    s.TabRounding = Radius::Control;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;   // 卡片边框由 DrawCard / PushCardStyle 接管
    s.FrameBorderSize = IsDarkMode() ? 0.0f : 1.0f;   // 浅色下输入框需要描边
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(Space::Md, Space::Md);   // 供弹窗/Tooltip 使用；主窗口与各页面均显式排版，不受此值影响
    s.FramePadding = ImVec2(10.0f, Space::Xs + 2.0f);
    s.ItemSpacing = ImVec2(10.0f, 10.0f);
    s.ItemInnerSpacing = ImVec2(Space::Sm, Space::Xs + 2.0f);
    s.ScrollbarSize = 10.0f;
    s.GrabMinSize = 10.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;

    c[ImGuiCol_Text]                = Pal::Text();
    c[ImGuiCol_TextDisabled]        = Pal::TextDim();
    c[ImGuiCol_WindowBg]            = Pal::WindowBg();
    c[ImGuiCol_ChildBg]             = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]             = Pal::CardBg();
    c[ImGuiCol_Border]              = Pal::Border();
    c[ImGuiCol_BorderShadow]        = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]             = Pal::FrameBg();
    c[ImGuiCol_FrameBgHovered]      = Pal::FrameBgHovered();
    c[ImGuiCol_FrameBgActive]       = Pal::FrameBgActive();
    c[ImGuiCol_TitleBg]             = Pal::PreviewBg();
    c[ImGuiCol_TitleBgActive]       = Pal::NavBg();
    c[ImGuiCol_TitleBgCollapsed]    = Pal::PreviewBg();
    c[ImGuiCol_MenuBarBg]           = Pal::CardBg();
    c[ImGuiCol_ScrollbarBg]         = Pal::WindowBg();
    c[ImGuiCol_ScrollbarGrab]       = Pal::ScrollGrab();
    c[ImGuiCol_ScrollbarGrabHovered]= Pal::ScrollGrabHovered();
    c[ImGuiCol_ScrollbarGrabActive] = Pal::Accent();
    c[ImGuiCol_CheckMark]           = Pal::Accent();
    c[ImGuiCol_SliderGrab]          = Pal::Accent();
    c[ImGuiCol_SliderGrabActive]    = Lighten(Pal::Accent(), 1.20f);
    c[ImGuiCol_Button]              = Pal::FrameBg();
    c[ImGuiCol_ButtonHovered]       = Pal::FrameBgHovered();
    c[ImGuiCol_ButtonActive]        = Pal::FrameBgActive();
    c[ImGuiCol_Header]              = Pal::HeaderBg();
    c[ImGuiCol_HeaderHovered]       = Pal::HeaderHovered();
    c[ImGuiCol_HeaderActive]        = ImVec4(Pal::Accent().x, Pal::Accent().y, Pal::Accent().z, 0.55f);
    c[ImGuiCol_Separator]           = Pal::Border();
    c[ImGuiCol_SeparatorHovered]    = Pal::ScrollGrabHovered();
    c[ImGuiCol_SeparatorActive]     = Pal::Accent();
    c[ImGuiCol_PlotLines]           = Pal::Accent();
    // ProgressBar 的填充取 PlotHistogram（不是 PlotLines）。不设的话会用 ImGui
    // 默认色，深浅两套主题下都和 Accent 不协调
    c[ImGuiCol_PlotHistogram]       = Pal::Accent();
    c[ImGuiCol_PlotHistogramHovered]= Pal::Accent();
    c[ImGuiCol_Tab]                 = Pal::FrameBg();
    c[ImGuiCol_TabHovered]          = Pal::FrameBgHovered();
    c[ImGuiCol_TabSelected]         = Pal::HeaderBg();
    // 模态遮罩：压暗但不全黑，保持主窗口内容仍隐约可辨
    c[ImGuiCol_ModalWindowDimBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}
