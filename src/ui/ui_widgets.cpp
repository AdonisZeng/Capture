#include "ui_widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// 按下时整体下沉，给出物理按压的触感反馈
ImVec2 PressOffset(bool pressed)
{
    return ImVec2(0.0f, pressed ? Control::PushDown : 0.0f);
}

}   // namespace

ImFont* FontOf(FontSlot slot)
{
    // 走主题层的槽位表：atlas 的 Fonts 下标可能因加载失败而错位
    return ThemeFont(slot);
}

void PushFontSlot(FontSlot slot)
{
    ImGui::PushFont(FontOf(slot));
}

void PopFontSlot()
{
    ImGui::PopFont();
}

bool HasNavFocus()
{
    // 仅当所属窗口持有焦点时才画焦点环，避免窗口失焦后残留高亮
    return ImGui::IsItemFocused() &&
           ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
}

void DrawFocusRing(ImVec2 mn, ImVec2 mx, float rounding)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(ImVec2(mn.x - 2.0f, mn.y - 2.0f), ImVec2(mx.x + 2.0f, mx.y + 2.0f),
                ImGui::GetColorU32(Pal::Accent()), rounding + 2.0f, 0, 1.5f);
}

bool NavItem(const char* id, const char* label, bool active, float width, float height)
{
    ImGui::PushID(id);
    ImGui::InvisibleButton("##nav", ImVec2(width, height));

    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    bool hover = ImGui::IsItemHovered();
    bool held  = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    bool focused = HasNavFocus();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 状态强度递增：常态 < 悬停 < 按下 < 激活
    float fill = active ? 0.085f : (held ? 0.070f : (hover ? 0.040f : 0.0f));
    if (fill > 0.0f)
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(Pal::Overlay(fill)), Radius::Control);

    if (active)
        dl->AddRectFilled(ImVec2(mn.x, mn.y + Space::Xs * 1.5f),
                          ImVec2(mn.x + 3.0f, mx.y - Space::Xs * 1.5f),
                          ImGui::GetColorU32(Pal::Accent()), 3.0f);

    ImVec4 col = active ? Pal::Text() : (hover ? Pal::Text() : Pal::TextDim());
    PushFontSlot(FontBody);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(mn.x + (width - ts.x) * 0.5f,
                       mn.y + (height - ts.y) * 0.5f),
                ImGui::GetColorU32(col), label);
    PopFontSlot();

    if (focused)
        DrawFocusRing(mn, mx, Radius::Control);

    ImGui::PopID();
    return clicked;
}

bool PrimaryButton(const char* id, const char* label, ImVec2 size, ImVec4 base,
                   const char* shortcut, bool enabled)
{
    // 高度缺省时取输入框同高；InvisibleButton 会把 0 尺寸解释为"铺满剩余空间"
    if (size.y <= 0.0f)
        size.y = ImGui::GetFrameHeight();
    if (!enabled)
        ImGui::BeginDisabled();
    ImGui::InvisibleButton(id, size);
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    bool hover = enabled && ImGui::IsItemHovered();
    bool active = enabled && ImGui::IsItemActive();
    bool clicked = enabled && ImGui::IsItemClicked();
    bool focused = HasNavFocus();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 col = base;
    if (active)      col = Darken(base, 0.82f);
    else if (hover)  col = Lighten(base, 1.18f);
    if (!enabled)    col = Pal::Disabled();

    ImVec2 off = PressOffset(active);
    ImVec2 dmn(mn.x + off.x, mn.y + off.y);
    ImVec2 dmx(mx.x + off.x, mx.y + off.y);
    dl->AddRectFilled(dmn, dmx, ImGui::GetColorU32(col), Radius::Control);

    // 主文字：水平略偏左（右侧留给快捷键提示），按实际矩形垂直居中
    PushFontSlot(FontBody);
    ImVec2 ts = ImGui::CalcTextSize(label);
    float rightPad = (shortcut && *shortcut) ? 78.0f : 0.0f;
    float x = mn.x + (size.x - ts.x) * 0.5f;
    if (rightPad > 0.0f)
        x = mn.x + (size.x - rightPad - ts.x) * 0.5f + rightPad * 0.35f;
    // 前景字色按底色亮度自动取深/浅，语义色当背景时也能满足 AA 对比度
    ImVec4 textCol = enabled ? Pal::On(col) : Pal::TextDim();
    dl->AddText(ImVec2(x, (dmn.y + dmx.y - ts.y) * 0.5f),
                ImGui::GetColorU32(textCol), label);
    PopFontSlot();

    if (shortcut && *shortcut)
    {
        PushFontSlot(FontSmall);
        ImVec2 ss = ImGui::CalcTextSize(shortcut);
        ImVec4 sc = enabled ? Pal::On(col) : Pal::TextDim();
        dl->AddText(ImVec2(dmx.x - ss.x - 14.0f, (dmn.y + dmx.y - ss.y) * 0.5f),
                    ImGui::GetColorU32(ImVec4(sc.x, sc.y, sc.z, 0.62f)), shortcut);
        PopFontSlot();
    }

    if (focused)
        DrawFocusRing(mn, mx, Radius::Control);

    if (!enabled)
        ImGui::EndDisabled();
    return clicked;
}

bool SecondaryButton(const char* id, const char* label, ImVec2 size, const char* shortcut)
{
    // 高度缺省时取输入框同高；InvisibleButton 会把 0 尺寸解释为"铺满剩余空间"，
    // 导致按钮变成贯穿卡片的大边框、文字飘到按钮外（"浏览"按钮即此问题）
    if (size.y <= 0.0f)
        size.y = ImGui::GetFrameHeight();
    ImGui::InvisibleButton(id, size);
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    bool hover = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    bool focused = HasNavFocus();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 off = PressOffset(active);
    ImVec2 dmn(mn.x + off.x, mn.y + off.y);
    ImVec2 dmx(mx.x + off.x, mx.y + off.y);

    // 状态强度递增：常态 0.02 < 悬停 0.05 < 按下 0.08
    ImVec4 bg = active ? Pal::Overlay(0.080f)
                       : (hover ? Pal::Overlay(0.050f) : Pal::Overlay(0.020f));
    dl->AddRectFilled(dmn, dmx, ImGui::GetColorU32(bg), Radius::Control);
    dl->AddRect(dmn, dmx,
                ImGui::GetColorU32((hover || active) ? Pal::Accent() : Pal::Border()),
                Radius::Control);

    PushFontSlot(FontBody);
    ImVec2 ts = ImGui::CalcTextSize(label);
    float rightPad = (shortcut && *shortcut) ? 78.0f : 0.0f;
    float x = mn.x + (size.x - ts.x) * 0.5f;
    if (rightPad > 0.0f)
        x = mn.x + (size.x - rightPad - ts.x) * 0.5f + rightPad * 0.35f;
    dl->AddText(ImVec2(x, (dmn.y + dmx.y - ts.y) * 0.5f),
                ImGui::GetColorU32(Pal::Text()), label);
    PopFontSlot();

    if (shortcut && *shortcut)
    {
        PushFontSlot(FontSmall);
        ImVec2 ss = ImGui::CalcTextSize(shortcut);
        dl->AddText(ImVec2(dmx.x - ss.x - 12.0f, (dmn.y + dmx.y - ss.y) * 0.5f),
                    ImGui::GetColorU32(Pal::TextDim()), shortcut);
        PopFontSlot();
    }

    if (focused)
        DrawFocusRing(mn, mx, Radius::Control);

    return clicked;
}

void PageTitle(const char* title, const char* subtitle)
{
    PushFontSlot(FontTitle);
    ImGui::TextUnformatted(title);
    PopFontSlot();
    if (subtitle && *subtitle)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Pal::TextDim());
        PushFontSlot(FontSmall);
        ImGui::TextUnformatted(subtitle);
        PopFontSlot();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, Space::Md));
}

void SectionTitle(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Pal::TextDim());
    PushFontSlot(FontSmall);
    ImGui::TextUnformatted(text);
    PopFontSlot();
    ImGui::PopStyleColor();
}

void SectionDivider()
{
    ImGui::Dummy(ImVec2(0.0f, Space::Md));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, Space::Sm));
}

void FieldLabel(const char* label, float labelW)
{
    // SameLine(offset) 的偏移量相对窗口外沿而非内容区，跨内边距使用会错位；
    // 这里以行首绝对坐标定位，仅覆写 x，y 沿用 SameLine 的行对齐结果
    ImVec2 rowPos = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(rowPos.x + labelW, cur.y));
}

float MeasureLabelColumn(const char* const* labels, int count)
{
    PushFontSlot(FontBody);
    float maxW = 0.0f;
    for (int i = 0; i < count; ++i)
        maxW = std::max(maxW, ImGui::CalcTextSize(labels[i]).x);
    PopFontSlot();
    return maxW + Space::Sm;   // 标签与右侧控件之间留一列间距
}

bool PathRow(const char* id, const char* label, float labelW,
             char* buf, size_t bufCap, bool withBrowse, bool* browseHit)
{
    ImGui::PushID(id);
    FieldLabel(label, labelW);

    // 输入框宽度按实际剩余空间扣除按钮与间距，右缘与同行控件对齐
    float inputW = ImGui::GetContentRegionAvail().x;
    if (withBrowse)
        inputW -= Space::Sm + Control::BrowseBtn;
    ImGui::SetNextItemWidth(inputW);
    bool edited = ImGui::InputText("##path", buf, bufCap);

    if (withBrowse)
    {
        ImGui::SameLine(0.0f, Space::Sm);
        bool hit = SecondaryButton("browse", "浏览",
                                   ImVec2(Control::BrowseBtn, ImGui::GetFrameHeight()));
        if (browseHit)
            *browseHit = hit;
    }
    else if (browseHit)
    {
        *browseHit = false;
    }

    ImGui::PopID();
    return edited;
}

bool ToggleRow(const char* id, const char* label, bool* value)
{
    ImGui::PushID(id);
    float rowH = 26.0f;
    float knobW = 40.0f, knobH = 22.0f;
    ImVec2 start = ImGui::GetCursorScreenPos();
    float rowW = ImGui::GetContentRegionAvail().x;   // 行首处测量：行内可用宽度

    // 标签行高与开关轨道垂直居中对齐（FramePadding 使 FrameHeight 大于行高，不可用其对齐）
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (knobH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted(label);

    // 右侧开关：右缘对齐容器内容区右缘（与输入框等控件的右缘一致）。
    // 不可用 GetWindowPos（窗口外沿）混合内容区宽度，否则会偏移一个内边距
    ImGui::SetCursorScreenPos(ImVec2(start.x + rowW - knobW, start.y));
    ImGui::InvisibleButton("##toggle", ImVec2(knobW, knobH));
    bool clicked = ImGui::IsItemClicked();
    bool hover = ImGui::IsItemHovered();
    bool focused = HasNavFocus();

    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = PillRadius(knobH);
    ImVec4 track = *value ? Pal::Accent() : Pal::Overlay(hover ? 0.16f : 0.12f);
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(track), r);
    if (!*value)
        dl->AddRect(mn, mx, ImGui::GetColorU32(Pal::Border()), r);

    constexpr float inset = 2.0f;   // 滑块与轨道内壁的间隙
    float kx = *value ? mx.x - knobH + inset : mn.x + inset;
    dl->AddCircleFilled(ImVec2(kx + knobH * 0.5f - inset, (mn.y + mx.y) * 0.5f),
                        knobH * 0.5f - 4.0f,
                        ImGui::GetColorU32(*value ? ImVec4(1, 1, 1, 1) : Pal::TextDim()));

    if (focused)
        DrawFocusRing(mn, mx, r);

    if (clicked)
        *value = !*value;

    ImGui::PopID();
    return clicked;
}

bool VolumeRow(const char* id, const char* label, float labelW, int* percent)
{
    if (!percent)
        return false;
    if (*percent < 0)   *percent = 0;
    if (*percent > 200) *percent = 200;

    ImGui::PushID(id);
    FieldLabel(label, labelW);

    // 右侧预留百分比文本的宽度，使滑块右缘与上方下拉框一致
    PushFontSlot(FontSmall);
    const float pctW = ImGui::CalcTextSize("200%").x;
    PopFontSlot();

    float sliderW = ImGui::GetContentRegionAvail().x - pctW - Space::Sm;
    if (sliderW < Control::Button)
        sliderW = Control::Button;

    float v = (float)*percent;
    ImGui::SetNextItemWidth(sliderW);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Pal::Accent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Pal::Accent());
    PushFontSlot(FontBody);
    const bool changed = ImGui::SliderFloat("##vol", &v, 0.0f, 200.0f, "");
    PopFontSlot();
    ImGui::PopStyleColor(2);
    if (changed)
        *percent = (int)(v + 0.5f);

    ImGui::SameLine(0.0f, Space::Sm);
    PushFontSlot(FontSmall);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", *percent);
    ImGui::AlignTextToFramePadding();   // 与滑块垂直居中
    ImGui::TextUnformatted(buf);
    PopFontSlot();

    ImGui::PopID();
    return changed;
}

bool ThemeToggle(const char* id, float width, float height)
{
    ImGui::PushID(id);
    ImGui::InvisibleButton("##theme", ImVec2(width, height));
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    bool hover = ImGui::IsItemHovered();
    bool held  = ImGui::IsItemActive();
    bool focused = HasNavFocus();
    bool dark  = IsDarkMode();

    // 单击即切换：深色 ↔ 浅色（圆点位置仅在两套配色间做缓动动画）
    bool changed = false;
    if (ImGui::IsItemClicked())
    {
        SetDarkMode(!dark);
        ApplyTheme();
        changed = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = PillRadius(height);
    ImVec2 center((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);

    // 椭圆轨道
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(Pal::TrackBg()), r);
    dl->AddRect(mn, mx, ImGui::GetColorU32((hover || held) ? Pal::Accent() : Pal::Border()), r);

    // 两端状态标记（左=浅色，右=深色）
    float markR = 2.0f;
    ImU32 mark = ImGui::GetColorU32(Pal::TextDim());
    dl->AddCircleFilled(ImVec2(mn.x + height * 0.5f, center.y), markR, mark);
    dl->AddCircleFilled(ImVec2(mx.x - height * 0.5f, center.y), markR, mark);

    // 圆点：位置做缓动动画（0=左端，1=右端）
    float dt = ImGui::GetIO().DeltaTime;
    ImGuiID animId = ImGui::GetID("##knob");
    ImGuiStorage* store = ImGui::GetStateStorage();
    float target = dark ? 1.0f : 0.0f;
    float anim = store->GetFloat(animId, target);
    anim += (target - anim) * std::min(1.0f, dt * 16.0f);
    if (std::fabs(target - anim) < 0.001f)
        anim = target;
    store->SetFloat(animId, anim);

    float travel = (width - height) * 0.5f;
    float knobR = r - Space::Xs;
    ImVec2 knob(center.x + travel * (anim * 2.0f - 1.0f), center.y);
    dl->AddCircleFilled(knob, knobR, ImGui::GetColorU32(Pal::Accent()));

    if (focused)
        DrawFocusRing(mn, mx, r);

    if (hover)
        ImGui::SetTooltip(dark ? "深色模式：点击切换为浅色"
                               : "浅色模式：点击切换为深色");
    ImGui::PopID();
    return changed;
}

bool Segmented(const char* id, const char* const* items, int count, int* current,
               float width)
{
    if (count <= 0)
        return false;
    if (width <= 0.0f)
        width = ImGui::GetContentRegionAvail().x;

    const float gap = Space::Xs;
    const float h = Control::Segmented;
    const float itemW = (width - gap * (count - 1)) / count;
    ImVec2 base = ImGui::GetCursorScreenPos();
    bool changed = false;

    ImGui::PushID(id);
    for (int i = 0; i < count; ++i)
    {
        ImVec2 pos(base.x + (itemW + gap) * i, base.y);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton(items[i], ImVec2(itemW, h));

        bool sel = (*current == i);
        bool hover = ImGui::IsItemHovered();
        bool held = ImGui::IsItemActive();
        bool focused = HasNavFocus();
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        // 状态强度递增：常态 < 悬停 < 按下 < 选中
        ImVec4 bg = sel ? Pal::Overlay(0.10f)
                        : (held ? Pal::Overlay(0.075f)
                                : (hover ? Pal::Overlay(0.05f) : Pal::Overlay(0.02f)));
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(bg), Radius::Control);
        if (sel)
            dl->AddRect(mn, mx, ImGui::GetColorU32(Pal::Accent()), Radius::Control);

        PushFontSlot(FontBody);
        ImVec2 ts = ImGui::CalcTextSize(items[i]);
        dl->AddText(ImVec2(mn.x + (itemW - ts.x) * 0.5f, mn.y + (h - ts.y) * 0.5f),
                    ImGui::GetColorU32(sel ? Pal::Text() : Pal::TextDim()), items[i]);
        PopFontSlot();

        if (focused)
            DrawFocusRing(mn, mx, Radius::Control);

        if (ImGui::IsItemClicked() && !sel)
        {
            *current = i;
            changed = true;
        }
    }
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + h));
    return changed;
}

bool ComboRow(const char* id, const char* label, float labelW,
              const char* const* items, int count, int* current, float width)
{
    if (count <= 0 || !items || !current)
        return false;
    // 配置文件里的值越界时先夹回合法区间，避免 Combo 预览读到数组外
    if (*current < 0 || *current >= count)
        *current = 0;
    if (width <= 0.0f)
        width = ImGui::GetContentRegionAvail().x;

    FieldLabel(label, labelW);
    // Combo 以控件 id 缓存预览值，必须给每个下拉独立作用域，否则会串值
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(width);
    PushFontSlot(FontBody);
    const bool changed = ImGui::Combo("##combo", current, items, count);
    PopFontSlot();
    ImGui::PopID();
    return changed;
}

void ImageFit(ImTextureID tex, ImVec2 srcSize, ImVec2 box, const char* placeholder)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mn = ImGui::GetCursorScreenPos();
    ImVec2 mx(mn.x + box.x, mn.y + box.y);
    // 画布保持直角：圆角会把视频/截图内容切掉
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(Pal::PreviewBg()), Radius::None);

    if (tex && srcSize.x > 0.0f && srcSize.y > 0.0f)
    {
        float sc = std::min((box.x - 2.0f) / srcSize.x, (box.y - 2.0f) / srcSize.y);
        if (sc > 0.0f)
        {
            ImVec2 imgSize(srcSize.x * sc, srcSize.y * sc);
            ImGui::SetCursorScreenPos(ImVec2(mn.x + (box.x - imgSize.x) * 0.5f,
                                             mn.y + (box.y - imgSize.y) * 0.5f));
            ImGui::Image(tex, imgSize);
        }
    }
    else if (placeholder && *placeholder)
    {
        // 空状态：画面缺席时要说清为什么没有、以及怎么才有
        PushFontSlot(FontSmall);
        ImVec2 ts = ImGui::CalcTextSize(placeholder);
        ImGui::SetCursorScreenPos(ImVec2(mn.x + (box.x - ts.x) * 0.5f,
                                         mn.y + (box.y - ts.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, Pal::TextDim());
        ImGui::TextUnformatted(placeholder);
        ImGui::PopStyleColor();
        PopFontSlot();
    }

    dl->AddRect(mn, mx, ImGui::GetColorU32(Pal::Border()), Radius::None);
    ImGui::SetCursorScreenPos(ImVec2(mn.x, mx.y));
}

ImVec2 DrawCard(ImVec2 min, ImVec2 max, float rounding, ImVec4 bg, float padding)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, ImGui::GetColorU32(bg), rounding);
    dl->AddRect(min, max, ImGui::GetColorU32(Pal::Border()), rounding);
    return ImVec2(min.x + padding, min.y + padding);
}

void PushCardStyle(float padding)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Pal::CardBg());
    ImGui::PushStyleColor(ImGuiCol_Border, Pal::Border());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Radius::Card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
}

void PopCardStyle()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

float TextLineH(FontSlot slot)
{
    PushFontSlot(slot);
    float h = ImGui::GetTextLineHeightWithSpacing();
    PopFontSlot();
    return h;
}

float MeasureFooterHeight(const FooterMetrics& m)
{
    // ImGui 只能自上而下排版，无法先画后测，故按已知控件序列累加。
    // 各项行高从对应字号的字体度量取，字号或 spacing token 调整后自动跟随
    float h = Space::Sm;   // 图片/缩略图下方的留白
    if (m.hasPrimaryLine)
        h += TextLineH(FontBody);
    if (m.hasHintLine)
        h += TextLineH(FontSmall);
    if (m.hasDoubleBtn)
        h += Space::Sm + Control::Button;
    if (m.hasSingleBtn)
        h += Space::Md + Control::ButtonSm;
    if (m.historyRows > 0)
    {
        // 与 page_capture 的画法逐项对齐：段前留白 + 小标题行 + 每行（按钮 + 行距）
        h += Space::Md + TextLineH(FontSmall);
        h += m.historyRows * (Control::ButtonSm + ImGui::GetStyle().ItemSpacing.y);
    }
    return h;
}

void HintText(const char* text)
{
    HintTextColored(text, Pal::TextDim());
}

void HintTextColored(const char* text, ImVec4 color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    PushFontSlot(FontSmall);
    ImGui::TextUnformatted(text);
    PopFontSlot();
    ImGui::PopStyleColor();
}
