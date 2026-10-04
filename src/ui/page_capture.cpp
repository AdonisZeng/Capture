#include "page_capture.h"
#include "ui_app.h"
#include "ui_widgets.h"
#include "app_state.h"
#include "screenshot/screenshot.h"
#include "settings/settings.h"
#include "core/util.h"
#include <cstdio>

namespace {

// 文本输入缓冲（首次进入时从配置填充，编辑后写回）
char g_shotDirBuf[1024] = {};
char g_shotNameBuf[256] = {};
bool g_bufInit = false;

// 格式下拉的选项直接取自编码器能力表（screenshot），避免界面与实际可写格式两处维护
const char* g_formatItems[ShotFormatCount] = {};

// 历史列表最多显示几行（PushRecent 最多存 8 条，这里只露最近 4 条）。
// MeasureFooterHeight 与实际绘制都要用这个数，故单独提出，避免两处写死不同值
int HistoryRowCount(size_t total)
{
    constexpr int kMaxRows = 4;
    return total < (size_t)kMaxRows ? (int)total : kMaxRows;
}

void EnsureBufs(Settings& cfg)
{
    if (g_bufInit)
        return;
    std::wstring dir = cfg.shotDir.empty() ? cfg.saveDir : cfg.shotDir;
    strncpy_s(g_shotDirBuf, WideToUtf8Str(dir).c_str(), _TRUNCATE);
    strncpy_s(g_shotNameBuf, WideToUtf8Str(cfg.shotPattern).c_str(), _TRUNCATE);
    for (int i = 0; i < ShotFormatCount; ++i)
        g_formatItems[i] = ShotFormatLabel(i);
    g_bufInit = true;
}

}   // namespace

void DrawCapturePage(const UiContext& ctx)
{
    Settings& cfg = *ctx.cfg;
    AppState& st = *ctx.st;
    EnsureBufs(cfg);

    const float pad = Control::CardPad;
    const float gap = pad;
    const float fullW = ImGui::GetContentRegionAvail().x;

    PageTitle("截屏", "截图后可保存为 PNG / JPEG / BMP / TIFF / GIF，或直接复制到剪贴板");

    const float fullH = ImGui::GetContentRegionAvail().y;
    const float leftW = (fullW - gap) * 0.55f;
    const bool hasFrame = st.previewW > 0;

    // 表单标签列宽由最长标签实测得出，改文案或字号后不会错位
    static const char* const kFormLabels[] = { "目录", "文件名", "格式" };
    const float labelW = MeasureLabelColumn(kFormLabels, 3);

    // ================= 左栏：操作 + 输出配置 =================
    // ImGui 1.92+ 必须显式传 ImGuiChildFlags_Borders，否则 child 被视为无边框，
    // PushCardStyle 推入的 ChildBorderSize 与 WindowPadding(卡片内边距) 都会被清零
    PushCardStyle(pad);
    ImGui::BeginChild("cap_left", ImVec2(leftW, fullH), ImGuiChildFlags_Borders);

    SectionTitle("操作");
    float availW = ImGui::GetContentRegionAvail().x;
    const float btnH = Control::ButtonLg;

    // 单一入口：点击后进入全屏框选，拖拽出区域就截区域，直接单击则截整屏
    char hkText[48] = {};
    strncpy_s(hkText, WideToUtf8Str(ctx.hotkeyCapture).c_str(), _TRUNCATE);
    if (PrimaryButton("shot", "截屏", ImVec2(availW, btnH), Pal::Accent(), hkText, hasFrame))
        st.wantShot = true;

    // 说明行行数恒定：要么讲用法，要么解释按钮为何不可用
    if (hasFrame)
        HintText("点击后进入全屏框选：按住左键拖出区域；直接单击则截取整个屏幕");
    else
        HintTextColored("尚未捕获到画面，无法截图，请等预览出现后再试", Pal::Warning());

    SectionDivider();
    SectionTitle("输出");

    // 目录行：浏览按钮由 PathRow 统一布局，右缘与下一行的输入框对齐
    bool browse = false;
    if (PathRow("shotdir", "目录", labelW, g_shotDirBuf, sizeof(g_shotDirBuf), true, &browse))
        cfg.shotDir = Utf8ToWide(g_shotDirBuf);
    if (browse)
    {
        std::wstring dir = cfg.shotDir.empty() ? cfg.saveDir : cfg.shotDir;
        if (PickFolderDialog(ctx.ownerHwnd, dir))
        {
            cfg.shotDir = dir;
            strncpy_s(g_shotDirBuf, WideToUtf8Str(dir).c_str(), _TRUNCATE);
            SaveSettings(cfg);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    if (PathRow("shotname", "文件名", labelW, g_shotNameBuf, sizeof(g_shotNameBuf)))
        cfg.shotPattern = Utf8ToWide(g_shotNameBuf);
    HintText("支持占位符：{date}=20261002   {time}=153218   {n}=序号；扩展名按所选格式自动追加");

    ImGui::Dummy(ImVec2(0.0f, Space::Xs));
    if (ComboRow("shot_fmt", "格式", labelW, g_formatItems, ShotFormatCount, &cfg.shotFormat))
        SaveSettings(cfg);

    ImGui::Dummy(ImVec2(0.0f, Space::Xs));
    if (ToggleRow("shot_save", "保存为文件", &cfg.shotSaveFile))
        SaveSettings(cfg);
    if (ToggleRow("shot_clip", "复制到剪贴板", &cfg.shotCopyClip))
        SaveSettings(cfg);
    // 双输出全关是可达的死局，必须当场说明后果而不是静默无反应
    if (!cfg.shotSaveFile && !cfg.shotCopyClip)
        HintTextColored("两种输出都已关闭，截图不会产生任何结果", Pal::Warning());

    ImGui::EndChild();
    PopCardStyle();

    ImGui::SameLine(0.0f, gap);

    // ================= 右栏：上次截图 =================
    PushCardStyle(pad);
    ImGui::BeginChild("cap_right", ImVec2(0.0f, fullH), ImGuiChildFlags_Borders);

    SectionTitle("上次截图");

    const bool has = st.lastShot.Valid();
    const bool hasPath = !st.shotPath.empty();

    // 底部为固定区（信息两行 + 按钮 + 历史列表），缩略图吃掉剩余高度。
    // 高度由 MeasureFooterHeight 按实际控件序列得出，不写死数值。
    // 历史列表必须计入（fm.historyRows），否则内容超出卡片高度，
    // child 会长出滚动条、缩略图与底部区的比例关系也不再成立
    FooterMetrics fm;
    fm.hasSingleBtn = hasPath;
    fm.historyRows = HistoryRowCount(st.recentShots.size());
    ImVec2 avail = ImGui::GetContentRegionAvail();
    constexpr float kMinThumbH = 100.0f;
    float thumbH = avail.y - MeasureFooterHeight(fm);
    if (thumbH < kMinThumbH)
        thumbH = kMinThumbH;

    ImageFit((ImTextureID)st.shotSRV.Get(), ImVec2((float)st.shotW, (float)st.shotH),
             ImVec2(avail.x, thumbH), "还没有截图");

    ImGui::Dummy(ImVec2(0.0f, Space::Xs + 2.0f));
    if (has)
    {
        char info[96];
        ULONGLONG ago = (GetTickCount64() - st.shotTick) / 1000;
        const char* when = ago < 5 ? "刚刚"
                        : (ago < 60 ? "不到 1 分钟前" : (ago < 3600 ? "几分钟前" : "更早"));
        snprintf(info, sizeof(info), "%ux%u · %s", st.lastShot.w, st.lastShot.h, when);
        ImGui::TextUnformatted(info);
        if (hasPath)
        {
            std::string pathText = WideToUtf8Str(EllipsizePath(st.shotPath, 44));
            HintText(pathText.c_str());
        }
        else if (st.shotCopied)
        {
            HintText("已复制到剪贴板（未保存文件）");
        }
    }
    else
    {
        HintText("点击上方按钮或按快捷键开始截图");
    }

    ImGui::Dummy(ImVec2(0.0f, Space::Xs));
    float bw = (avail.x - Space::Md) * 0.5f;
    if (SecondaryButton("shot_copy", "再次复制", ImVec2(bw, Control::Button), nullptr) && has)
    {
        if (CopyToClipboard(st.lastShot))
            st.SetToast(L"已复制到剪贴板");
        else
            st.errPopup = "复制到剪贴板失败";
    }
    ImGui::SameLine(0.0f, Space::Md);
    if (SecondaryButton("shot_retry", "重新截图", ImVec2(bw, Control::Button), nullptr) && has)
    {
        st.wantShot = true;
    }
    if (hasPath)
    {
        ImGui::Dummy(ImVec2(0.0f, Space::Md));
        if (SecondaryButton("shot_open", "打开文件位置",
                            ImVec2(ImGui::GetContentRegionAvail().x, Control::ButtonSm), nullptr))
        {
            if (!RevealInExplorer(st.shotPath))
                st.errPopup = "无法定位文件，可能已被移动或删除";
        }
    }

    const int histRows = HistoryRowCount(st.recentShots.size());
    if (histRows > 0)
    {
        ImGui::Dummy(ImVec2(0.0f, Space::Md));
        SectionTitle("最近截图");
        int shown = 0;
        for (const std::wstring& p : st.recentShots)
        {
            if (shown >= histRows)
                break;
            std::string name = WideToUtf8Str(p);
            const size_t pos = name.find_last_of("\\/");
            std::string base = (pos == std::string::npos) ? name : name.substr(pos + 1);
            if (base.size() > 40)
                base = base.substr(0, 40) + "…";
            char rid[32] = {};
            snprintf(rid, sizeof(rid), "recent_shot_%d", shown);
            if (SecondaryButton(rid, base.c_str(),
                                ImVec2(ImGui::GetContentRegionAvail().x, Control::ButtonSm),
                                nullptr))
            {
                if (!RevealInExplorer(p))
                    st.errPopup = "无法定位文件，可能已被移动或删除";
            }
            ++shown;
        }
    }

    ImGui::EndChild();
    PopCardStyle();
}