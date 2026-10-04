#pragma once
// 自绘控件集：导航项、主/次按钮、开关、分段控件、等比图片、卡片、表单构件
// 全部基于 ImGui 的 InvisibleButton + DrawList，视觉不依赖默认主题
//
// 【交互规约】每个可点击控件都必须同时表达四种状态：
//   常态 / 悬停 / 按下(含下沉位移) / 键盘焦点(焦点环)，
// 缺失任何一种都会让用户无法确认操作是否被接收。
#include "imgui.h"
#include "ui_theme.h"

// 取指定槽位字体（未加载时返回默认字体）
ImFont* FontOf(FontSlot slot);
void PushFontSlot(FontSlot slot);
void PopFontSlot();

// 当前项是否处于键盘导航焦点（自绘控件用它补充 ImGui 未画的焦点指示）
bool HasNavFocus();

// 键盘焦点环：画在控件矩形外扩一圈，避免与控件自身描边重叠
void DrawFocusRing(ImVec2 min, ImVec2 max, float rounding);

// 左侧导航项：激活项左侧 3px 强调条 + 浅色底 + 居中文字
bool NavItem(const char* id, const char* label, bool active, float width, float height);

// 主操作大按钮：实心圆角，右侧可带小字快捷键提示
bool PrimaryButton(const char* id, const char* label, ImVec2 size, ImVec4 base,
                   const char* shortcut = nullptr, bool enabled = true);

// 次要按钮：描边 + 透明底
bool SecondaryButton(const char* id, const char* label, ImVec2 size,
                     const char* shortcut = nullptr);

// 页面标题 + 副标题
void PageTitle(const char* title, const char* subtitle);

// 分组小标题
void SectionTitle(const char* text);

// 分组之间的分隔（含上下留白），替代散落的 Separator + Dummy 组合
void SectionDivider();

// 表单标签：绘制标签后把光标移到行首起 labelW 的固定列上，
// 使同一卡片内"目录/文件名"等各行的输入控件左缘对齐
void FieldLabel(const char* label, float labelW);

// 按一组标签的最大文本宽度推导标签列宽（页面初始化时算一次即可），
// 避免手填常量在字号或文案变化后错位
float MeasureLabelColumn(const char* const* labels, int count);

// 路径输入行：标签 + 输入框（+ 可选"浏览"按钮）。
// 返回文本是否被编辑；withBrowse 为真时 browseHit 输出浏览按钮是否被点击
bool PathRow(const char* id, const char* label, float labelW,
             char* buf, size_t bufCap, bool withBrowse = false,
             bool* browseHit = nullptr);

// 开关行：左标签 + 右侧开关（返回是否被切换）
bool ToggleRow(const char* id, const char* label, bool* value);

// 增益滑块行：标签 + 滑块 + 右侧百分比文本。percent 为 0..200 的整数百分比，
// 100 表示原始音量；超过 100 的部分靠后端混音时限幅保护
bool VolumeRow(const char* id, const char* label, float labelW, int* percent);

// 深/浅色主题开关：椭圆轨道 + 圆点，圆点偏左=浅色、偏右=深色
// 单击即切换（圆点位置做缓动动画）；内部直接切换主题并刷新样式，
// 返回模式是否发生变化
bool ThemeToggle(const char* id, float width, float height);

// 分段单选控件；width <= 0 时铺满可用宽度
bool Segmented(const char* id, const char* const* items, int count, int* current,
               float width = 0.0f);

// 下拉单选行：标签 + 下拉框。选项超过 3 个时用它替代 Segmented，省下纵向空间
// （分段控件的每项宽度会随选项数摊薄，中文标签容易挤到换行或截断）。
// 弹层复用 ImGui 原生 popup：主题已配置 PopupBg/FrameBg/Rounding，
// 观感与自绘控件一致，且键盘导航、选项过多时的滚动都无需自己实现
bool ComboRow(const char* id, const char* label, float labelW,
              const char* const* items, int count, int* current,
              float width = 0.0f);

// 在 box 内等比居中绘制纹理；无纹理时画 bg 底 + 占位文字
void ImageFit(ImTextureID tex, ImVec2 srcSize, ImVec2 box, const char* placeholder = nullptr);

// 画卡片背景（返回内容区，已扣除 padding）
ImVec2 DrawCard(ImVec2 min, ImVec2 max, float rounding, ImVec4 bg, float padding);

// 卡片式子窗口的样式（配合 BeginChild 使用，最后调 PopCardStyle）
// 注意：ImGui 1.92+ 起 child 的边框/内边距由 ImGuiChildFlags_Borders 决定，
// 使用本样式的 BeginChild 必须传 ImGuiChildFlags_Borders，否则留白与描边都被 ImGui 清零
void PushCardStyle(float padding);
void PopCardStyle();

// 灰色小字说明
void HintText(const char* text);
void HintTextColored(const char* text, ImVec4 color);

// 指定字号槽位下一行文字的高度（含行距）。
// 布局需要预先累加行高时必须用它，不能假设当前字体已切换过
float TextLineH(FontSlot slot);

// 底部固定区高度估算：ImGui 只能自上而下排版，无法先测后画，
// 故按已知的控件序列累加，给上方的图片/缩略图留出正确高度
struct FooterMetrics
{
    bool  hasPrimaryLine = true;   // 正文尺寸行
    bool  hasHintLine    = true;   // 小字路径/提示行
    bool  hasDoubleBtn   = true;   // 一行两个按钮
    bool  hasSingleBtn   = false;  // 追加的一行整宽按钮
    // 追加的历史列表：标题 + extraRows 个整宽小按钮。
    // 这类内容画在固定区之后，必须一并计入，否则会溢出卡片底部
    int   historyRows    = 0;
};

float MeasureFooterHeight(const FooterMetrics& m);
