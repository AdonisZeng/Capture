#pragma once
// UI 主题：多字号中文字体 + 深/浅两套配色 + 统一的圆角/间距/尺寸 token
//
// 【约定】所有 UI 绘制一律取用本文件的 token，禁止在绘制代码里写裸数值。
//   Radius   圆角尺度：容器比内部控件大一级，形成可辨识的嵌套层次
//   Space    间距音阶：4px 基准
//   Control  交互控件标准尺寸
//   Pal      语义化配色；按钮底色配 Pal::On(bg) 取前景字色，保证 WCAG AA
#include "imgui.h"
#include <cmath>

// ---------------------------------------------------------------------------
// 主题模式：深色 / 浅色（左下角椭圆开关切换，持久化于 settings.json）
// ---------------------------------------------------------------------------
bool IsDarkMode();            // 当前是否为深色模式
void SetDarkMode(bool dark);  // 切换模式（需自行调用 ApplyTheme 刷新样式）
void ToggleDarkMode();

// 字体槽位（绘制时 PushFont(ThemeFont(FontXxx))）
enum FontSlot
{
    FontBody = 0,   // 正文 / 按钮
    FontTitle,      // 页面标题
    FontSmall,      // 辅助说明
    FontCount
};

// 取指定槽位字体。由主题层内部表提供：
// 不能依赖 ImFontAtlas::Fonts 的数组下标，一旦某档字号加载失败只留下部分字体，
// 下标就会整体错位（表现为标题是正文大小等错字现象）
ImFont* ThemeFont(FontSlot slot);

// ---------------------------------------------------------------------------
// 圆角尺度：全局唯一来源
//   卡片(10) > 控件(6) > 尖锐(0)；椭圆控件走 PillRadius(h)
// ---------------------------------------------------------------------------
namespace Radius
{
constexpr float None    = 0.0f;    // 视频 / 缩略图画布：内容自带边界，圆角会切掉画面
constexpr float Control = 6.0f;    // 输入框、按钮、导航项、分段控件
constexpr float Card    = 10.0f;   // 卡片容器
constexpr float Popup   = 10.0f;   // 弹窗
}   // namespace Radius

// 椭圆控件的圆角：取高度的一半
inline float PillRadius(float h) { return h * 0.5f; }

// ---------------------------------------------------------------------------
// 间距音阶：4px 基准
// ---------------------------------------------------------------------------
namespace Space
{
constexpr float Xs = 4.0f;    // 图标与文字、控件组内的极紧间隙
constexpr float Sm = 8.0f;    // 同一分组内相邻控件
constexpr float Md = 12.0f;   // 表单项之间
constexpr float Lg = 16.0f;   // 分组之间、内容区边距
constexpr float Xl = 24.0f;   // 大区块之间
}   // namespace Space

// ---------------------------------------------------------------------------
// 交互控件标准尺寸
// ---------------------------------------------------------------------------
namespace Control
{
constexpr float NavWidth  = 104.0f;   // 左侧导航栏宽度
constexpr float StatusBar = 30.0f;    // 底部状态栏高度
constexpr float NavItem   = 44.0f;    // 导航项高度
constexpr float NavGap    = 8.0f;     // 相邻导航项的垂直间距
constexpr float Segmented = 30.0f;    // 分段控件高度
constexpr float ButtonSm  = 30.0f;    // 紧凑按钮
constexpr float Button    = 34.0f;    // 通用次要按钮
constexpr float ButtonLg  = 46.0f;    // 主操作按钮
constexpr float Row       = 26.0f;    // 开关 / 标签行高
constexpr float CardPad   = 18.0f;    // 卡片内边距
constexpr float BrowseBtn = 60.0f;    // 「浏览」按钮固定宽度
constexpr float PushDown  = 1.0f;     // 按下时的视觉下沉位移
}   // namespace Control

// ---------------------------------------------------------------------------
// 颜色工具
// ---------------------------------------------------------------------------
// 提亮 / 压暗，保留 alpha 不变
inline ImVec4 Lighten(ImVec4 c, float f)
{
    return ImVec4(c.x * f > 1.0f ? 1.0f : c.x * f,
                  c.y * f > 1.0f ? 1.0f : c.y * f,
                  c.z * f > 1.0f ? 1.0f : c.z * f, c.w);
}

inline ImVec4 Darken(ImVec4 c, float f)
{
    return ImVec4(c.x * f, c.y * f, c.z * f, c.w);
}

// ---------------------------------------------------------------------------
// 界面配色（预览区需要中性背景，整体偏冷灰）
// ---------------------------------------------------------------------------
namespace Pal
{
// 按当前模式取色
inline ImVec4 Themed(ImVec4 dark, ImVec4 light)
{
    return IsDarkMode() ? dark : light;
}

// 半透明叠加色：深色模式提亮、浅色模式压暗（悬停/选中底色）
inline ImVec4 Overlay(float a)
{
    return Themed(ImVec4(1.0f, 1.0f, 1.0f, a), ImVec4(0.0f, 0.0f, 0.0f, a * 0.55f));
}

// 与背景色配套的前景字色：按 WCAG 相对亮度自动取深/浅。
// 深色#12 1418 对饱和语义色可达 11:1，白色对深语义色可达 5:1，
// 避免「亮黄底 + 白字」这类几乎不可读的组合。
inline ImVec4 On(ImVec4 bg)
{
    auto lin = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const float L = 0.2126f * lin(bg.x) + 0.7152f * lin(bg.y) + 0.0722f * lin(bg.z);
    return L > 0.42f ? ImVec4(0.071f, 0.078f, 0.094f, 1.0f)   // 深字
                     : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);        // 白字
}

// ---- 基础层 ----
inline ImVec4 WindowBg()  { return Themed(ImVec4(0.043f, 0.047f, 0.055f, 1.0f), ImVec4(0.945f, 0.953f, 0.965f, 1.0f)); }
inline ImVec4 NavBg()     { return Themed(ImVec4(0.067f, 0.071f, 0.082f, 1.0f), ImVec4(0.898f, 0.910f, 0.929f, 1.0f)); }
inline ImVec4 CardBg()    { return Themed(ImVec4(0.086f, 0.090f, 0.102f, 1.0f), ImVec4(1.000f, 1.000f, 1.000f, 1.0f)); }
inline ImVec4 Border()    { return Themed(ImVec4(0.145f, 0.153f, 0.169f, 1.0f), ImVec4(0.816f, 0.835f, 0.867f, 1.0f)); }
inline ImVec4 PreviewBg() { return Themed(ImVec4(0.031f, 0.035f, 0.043f, 1.0f), ImVec4(0.898f, 0.910f, 0.929f, 1.0f)); }

// ---- 文字 / 语义色 ----
inline ImVec4 Text()      { return Themed(ImVec4(0.910f, 0.918f, 0.933f, 1.0f), ImVec4(0.106f, 0.118f, 0.137f, 1.0f)); }
inline ImVec4 TextDim()   { return Themed(ImVec4(0.549f, 0.576f, 0.616f, 1.0f), ImVec4(0.404f, 0.427f, 0.463f, 1.0f)); }
inline ImVec4 Accent()    { return Themed(ImVec4(0.204f, 0.400f, 0.988f, 1.0f), ImVec4(0.176f, 0.365f, 0.929f, 1.0f)); }
inline ImVec4 Success()   { return Themed(ImVec4(0.180f, 0.490f, 0.310f, 1.0f), ImVec4(0.133f, 0.463f, 0.286f, 1.0f)); }
inline ImVec4 Danger()    { return Themed(ImVec4(0.750f, 0.250f, 0.250f, 1.0f), ImVec4(0.757f, 0.224f, 0.224f, 1.0f)); }
inline ImVec4 Warning()   { return Themed(ImVec4(1.000f, 0.750f, 0.300f, 1.0f), ImVec4(0.816f, 0.573f, 0.043f, 1.0f)); }

// ---- 控件底色 ----
inline ImVec4 FrameBg()            { return Themed(ImVec4(0.106f, 0.114f, 0.129f, 1.0f), ImVec4(1.000f, 1.000f, 1.000f, 1.0f)); }
inline ImVec4 FrameBgHovered()     { return Themed(ImVec4(0.145f, 0.157f, 0.176f, 1.0f), ImVec4(0.961f, 0.969f, 0.980f, 1.0f)); }
inline ImVec4 FrameBgActive()      { return Themed(ImVec4(0.180f, 0.196f, 0.220f, 1.0f), ImVec4(0.925f, 0.937f, 0.953f, 1.0f)); }
inline ImVec4 ScrollGrab()        { return Themed(ImVec4(0.192f, 0.208f, 0.231f, 1.0f), ImVec4(0.788f, 0.808f, 0.843f, 1.0f)); }
inline ImVec4 ScrollGrabHovered() { return Themed(ImVec4(0.259f, 0.278f, 0.310f, 1.0f), ImVec4(0.671f, 0.698f, 0.741f, 1.0f)); }
inline ImVec4 HeaderBg()          { return Themed(ImVec4(0.145f, 0.157f, 0.176f, 0.65f), ImVec4(0.902f, 0.910f, 0.929f, 0.75f)); }
inline ImVec4 HeaderHovered()      { return Themed(ImVec4(0.204f, 0.220f, 0.247f, 0.80f), ImVec4(0.855f, 0.875f, 0.906f, 0.85f)); }
inline ImVec4 Disabled()          { return Themed(ImVec4(0.157f, 0.169f, 0.188f, 1.0f), ImVec4(0.878f, 0.894f, 0.918f, 1.0f)); }
inline ImVec4 ErrorFieldBg()      { return Themed(ImVec4(0.280f, 0.140f, 0.160f, 1.0f), ImVec4(0.988f, 0.914f, 0.914f, 1.0f)); }
inline ImVec4 TrackBg()           { return Themed(ImVec4(0.106f, 0.114f, 0.129f, 1.0f), ImVec4(0.859f, 0.878f, 0.906f, 1.0f)); }
}   // namespace Pal

// 加载字体（微软雅黑优先，逐个备选；全部失败回退默认字体，中文可能为方块）
void LoadThemeFonts();

// 应用配色与控件样式（切换深/浅色后需重新调用）
void ApplyTheme();
