// 截图标注编辑器实现
//
// 四层 DIB 渲染（均为 32bpp top-down，与 ImageBGRA 内存同构）：
//   hdcSrc    源图，只读
//   hdcBase   源图 + 已提交图形（图像坐标，提交 / 撤销 / 重做时重建）
//   hdcStill  整屏静态层：背景 + 缩放后的 base + 图片描边
//   hdcScreen 最终显示层：still + 拖拽预览 + 工具栏 + 提示
// 拖动预览时只从 still 恢复"上一帧预览占用的矩形"再重画新预览，不做全图 Blt、
// 不做全图缩放；WM_PAINT 也只贴脏区。这是唯一能做到跟手的形态。
// 缩放只发生在 base -> still 这一跳，所有图形坐标永远是图像像素，
// 因此输出尺寸与输入严格一致，也不受显示器缩放影响。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>      // gdiplus.h 需要 IStream：WIN32_LEAN_AND_MEAN 下 windows.h 不含 OLE 头
#include <gdiplus.h>
#undef min
#undef max
#include "annotate.h"
#include "core/log.h"
#include <imm.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {

// ---- 尺寸与配色（均按 96dpi 书写，绘制时按 ctx.s100 缩放）----
constexpr int kMaxUndo = 20;          // 撤销步数上限（历史快照数 = kMaxUndo + 1）

constexpr COLORREF kBgColor   = RGB(15, 17, 22);
constexpr COLORREF kPanelBg   = RGB(24, 27, 34);
constexpr COLORREF kPanelEdge = RGB(62, 68, 82);
constexpr COLORREF kBtnBg     = RGB(40, 44, 54);
constexpr COLORREF kBtnEdge   = RGB(58, 64, 78);
constexpr COLORREF kBtnHover  = RGB(52, 58, 72);
constexpr COLORREF kTextMain  = RGB(240, 242, 246);
constexpr COLORREF kTextDim   = RGB(140, 148, 162);
constexpr COLORREF kAccent    = RGB(52, 102, 252);

constexpr int kPad      = 12;
constexpr int kGap      = 8;
constexpr int kBtnH     = 34;
constexpr int kRadius   = 8;
constexpr int kMargin   = 16;
constexpr int kImgGap   = 14;
constexpr int kFontPx   = 14;
constexpr int kHintPx   = 13;
constexpr int kSwatch   = 30;
constexpr int kWidthBtn = 40;
constexpr int kMinBtnW  = 56;
constexpr int kEndBtnW  = 64;

// 调色板：固定 8 色，不做自定义取色。白/黑各留一个，深浅截图都能看见
struct Swatch
{
    COLORREF    rgb;
    const wchar_t* name;
};
const Swatch kPalette[] = {
    { RGB(229, 72, 77),   L"红" },
    { RGB(245, 165, 36),  L"橙" },
    { RGB(255, 214, 10),  L"黄" },
    { RGB(48, 164, 108),  L"绿" },
    { RGB(62, 99, 221),   L"蓝" },
    { RGB(142, 78, 198),  L"紫" },
    { RGB(255, 255, 255), L"白" },
    { RGB(30, 30, 34),    L"黑" },
};
constexpr int kPaletteCount = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

// 线宽档位：显示像素（96dpi 基准），从最细到最粗
const int kWidths[] = { 2, 4, 7, 11, 16 };
constexpr int kWidthCount = (int)(sizeof(kWidths) / sizeof(kWidths[0]));

const wchar_t kToolText[AnnotateToolCount][8] = {
    L"矩形", L"椭圆", L"直线", L"箭头", L"画笔", L"高亮"
};

const wchar_t kClassName[] = L"CaptureAnnotateClass";
const wchar_t kHint[] =
    L"拖拽绘制 · Ctrl+Z 撤销 · Ctrl+Y 重做 · 回车完成 · Esc 取消";

// Esc 兜底轮询：窗口失去焦点后收不到 WM_KEYDOWN，与框选遮罩同策略
constexpr UINT_PTR kEscTimer   = 1;
constexpr UINT     kEscTimerMs = 50;

// ---- 控件编号（工具栏命中测试的返回值）----
enum
{
    BtnNone  = -1,
    BtnTool0 = 0,                                  // [BtnTool0, BtnTool0 + AnnotateToolCount)
    BtnUndo  = BtnTool0 + AnnotateToolCount,
    BtnRedo,
    BtnClear,
    BtnCancel,
    BtnDone,
    BtnColor0,                                     // [BtnColor0, BtnColor0 + kPaletteCount)
    BtnWidth0 = BtnColor0 + kPaletteCount,         // [BtnWidth0, BtnWidth0 + kWidthCount)
};

// 一次已提交的绘制：几何全部是图像像素，输出时不需任何换算
struct Op
{
    int      tool  = AnnotateRect;
    COLORREF color = RGB(229, 72, 77);
    float    width = 2.0f;
    std::vector<POINT> pts;    // 矩形/椭圆/直线/箭头只用首尾两点；画笔/高亮用全程轨迹
};
struct Ctx
{
    HWND hwnd = nullptr;
    int  winW = 0, winH = 0;
    int  s100 = 100;                 // dpi 百分比（125% = 125）

    // 图片在整屏层上的位置与显示比例（scale <= 1，小图不放大）
    int   imgW = 0, imgH = 0;
    float scale = 1.0f;
    int   imgX = 0, imgY = 0, dispW = 0, dispH = 0;
    RECT  imgRect{};

    // 三层缓存：
    //   hdcSrc   原图（只读）
    //   hdcBase  原图 + 已提交图形（图像坐标，提交/撤销/重做时重建）
    //   hdcStill 整屏静态层：背景 + 缩放后的 base + 图片描边（不含预览与工具栏）
    //   hdcScreen 最终显示层：still + 拖拽预览 + 工具栏 + 提示
    // 拖动时只从 still 恢复"上一帧预览占用的矩形"并重画新预览，
    // 不再做全图 Blt 与全图缩放 —— 这是流畅度的关键。
    HDC     hdcSrc = nullptr, hdcBase = nullptr, hdcStill = nullptr, hdcScreen = nullptr;
    HBITMAP hbmSrc = nullptr, hbmBase = nullptr, hbmStill = nullptr, hbmScreen = nullptr;
    void*   bitsSrc  = nullptr;
    void*   bitsBase = nullptr;

    HFONT hFont = nullptr, hFontSmall = nullptr;

    AnnotateStyle style;

    // 历史：snaps 是每个状态的完整操作列表，snapIdx 指向当前状态。
    // 用快照而不是"操作 + 逆操作"是因为 Op 很小（几个 POINT），拷贝开销可忽略，
    // 换来撤销/重做/清空三者共用一套状态，不会出现逆操作对不上的情况。
    std::vector<Op> ops;
    std::vector<std::vector<Op>> snaps;
    int snapIdx = 0;

    bool  dragging = false;
    POINT ptDown{};
    POINT ptCur{};
    std::vector<POINT> stroke;       // 画笔/高亮的轨迹（图像坐标）
    RECT  previewDirty{};            // 当前预览在客户区占用的矩形（空 = 无预览）

    int  hover = BtnNone;
    RECT panel{};
    RECT hintRect{};
    RECT btnTool[AnnotateToolCount]{};
    RECT btnUndo{}, btnRedo{}, btnClear{}, btnCancel{}, btnDone{};
    RECT btnColor[kPaletteCount]{};
    RECT btnWidth[kWidthCount]{};

    bool escDown = true;             // Esc 边沿检测（进入时视为已按下，必须松开再按）
    bool done = false;
    bool canceled = false;
};

Ctx* g_ctx = nullptr;   // 同一时刻只可能有一个标注窗口

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------
int ClampI(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int Scale(const Ctx& c, int v)
{
    return v * c.s100 / 100;
}

// 自上而下的 32bpp DIB（biHeight 取负，首行即画面顶部，与 ImageBGRA 一致）
HBITMAP MakeDib(HDC ref, int w, int h, HDC& dc, void** bits)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* p = nullptr;
    HBITMAP bm = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (!bm)
        return nullptr;
    dc = CreateCompatibleDC(ref);
    if (!dc)
    {
        DeleteObject(bm);
        return nullptr;
    }
    SelectObject(dc, bm);
    if (bits)
        *bits = p;
    return bm;
}

// 先删 DC 再删位图：DC 仍选中位图时 DeleteObject 会失败
void DestroyDib(HDC& dc, HBITMAP& bm)
{
    if (dc)
    {
        DeleteDC(dc);
        dc = nullptr;
    }
    if (bm)
    {
        DeleteObject(bm);
        bm = nullptr;
    }
}

void EnsureGdiPlus()
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput si;
    if (Gdiplus::GdiplusStartup(&token, &si, nullptr) != Gdiplus::Ok)
        LOG_ERR(L"标注: GDI+ 初始化失败，图形将无法绘制");
}
// ---------------------------------------------------------------------------
// 图形绘制（GDI+：抗锯齿与半透明是画笔/高亮笔的观感前提，纯 GDI 做不到）
// ---------------------------------------------------------------------------
Gdiplus::Color MakeColor(COLORREF c, BYTE a)
{
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void SetupGraphics(Gdiplus::Graphics& g)
{
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
}

void DrawPolyline(Gdiplus::Graphics& g, const Gdiplus::Pen& pen,
                  const std::vector<POINT>& pts)
{
    std::vector<Gdiplus::PointF> gp;
    gp.reserve(pts.size());
    for (const POINT& p : pts)
        gp.push_back(Gdiplus::PointF((Gdiplus::REAL)p.x, (Gdiplus::REAL)p.y));
    g.DrawLines(&pen, gp.data(), (INT)gp.size());
}

// 箭头 = 直线 + 实心三角头（头部尺寸随线宽与长度自适应，短线不画出反向的怪头）
void DrawArrow(Gdiplus::Graphics& g, const Gdiplus::Pen& pen, POINT a, POINT b, float w)
{
    g.DrawLine(&pen, (INT)a.x, (INT)a.y, (INT)b.x, (INT)b.y);

    const float dx = (float)(b.x - a.x), dy = (float)(b.y - a.y);
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 2.0f)
        return;

    float head = w * 4.0f + 6.0f;
    if (head > len)
        head = len;

    const float ang = atan2f(dy, dx);
    const float s1  = ang + 2.55f;
    const float s2  = ang - 2.55f;
    const Gdiplus::PointF tri[3] = {
        Gdiplus::PointF((Gdiplus::REAL)b.x, (Gdiplus::REAL)b.y),
        Gdiplus::PointF((Gdiplus::REAL)(b.x + cosf(s1) * head),
                        (Gdiplus::REAL)(b.y + sinf(s1) * head)),
        Gdiplus::PointF((Gdiplus::REAL)(b.x + cosf(s2) * head),
                        (Gdiplus::REAL)(b.y + sinf(s2) * head)),
    };
    Gdiplus::Color col;
    pen.GetColor(&col);
    Gdiplus::SolidBrush br(col);
    g.FillPolygon(&br, tri, 3);
}

void DrawOp(Gdiplus::Graphics& g, const Op& op)
{
    if (op.pts.empty())
        return;

    // 高亮笔用半透明：压住文字但仍能读出来
    const BYTE alpha = (op.tool == AnnotateHighlight) ? 90 : 255;
    Gdiplus::Pen pen(MakeColor(op.color, alpha), op.width);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    const POINT a = op.pts.front();
    const POINT b = op.pts.back();
    const int x = a.x < b.x ? a.x : b.x;
    const int y = a.y < b.y ? a.y : b.y;
    const int w = a.x < b.x ? b.x - a.x : a.x - b.x;
    const int h = a.y < b.y ? b.y - a.y : a.y - b.y;

    switch (op.tool)
    {
    case AnnotateRect:
        if (w > 0 && h > 0)
            g.DrawRectangle(&pen, x, y, w, h);
        break;
    case AnnotateEllipse:
        if (w > 0 && h > 0)
            g.DrawEllipse(&pen, x, y, w, h);
        break;
    case AnnotateLine:
        g.DrawLine(&pen, (INT)a.x, (INT)a.y, (INT)b.x, (INT)b.y);
        break;
    case AnnotateArrow:
        DrawArrow(g, pen, a, b, op.width);
        break;
    default:   // 画笔 / 高亮
        if (op.pts.size() >= 2)
            DrawPolyline(g, pen, op.pts);
        else
        {
            Gdiplus::Color col;
            pen.GetColor(&col);
            Gdiplus::SolidBrush br(col);
            const Gdiplus::REAL r = op.width * 0.5f;
            g.FillEllipse(&br, (Gdiplus::REAL)a.x - r, (Gdiplus::REAL)a.y - r, r * 2, r * 2);
        }
        break;
    }
}
// ---------------------------------------------------------------------------
// 当前样式 -> 落笔参数
// ---------------------------------------------------------------------------
// 线宽按"屏幕显示像素"定义（用户看到多粗，输出就多粗），落到图像坐标要除 scale：
// 大图缩小显示时输出图的线会相应变粗，与 100% 查看时的观感保持一致
float WidthInImage(const Ctx& c, int tool)
{
    float w = (float)AnnotateWidthPx(c.style.width) * (float)c.s100 / 100.0f;
    w /= c.scale;

    if (tool == AnnotateHighlight)
    {
        w *= 2.5f;
        const float minW = 14.0f * (float)c.s100 / 100.0f / c.scale;
        if (w < minW)
            w = minW;
    }
    if (w < 1.0f)
        w = 1.0f;
    return w;
}

Op BuildOp(const Ctx& c)
{
    Op op;
    op.tool  = c.style.tool;
    op.color = AnnotateColorAt(c.style.color);
    op.width = WidthInImage(c, c.style.tool);
    if (c.style.tool == AnnotatePen || c.style.tool == AnnotateHighlight)
        op.pts = c.stroke;
    else
    {
        op.pts.push_back(c.ptDown);
        op.pts.push_back(c.ptCur);
    }
    return op;
}

// 过滤误点：零位移的拖拽不产生图形；画笔允许单点（落一个圆点）
bool OpHasExtent(const Op& op)
{
    if (op.tool == AnnotatePen || op.tool == AnnotateHighlight)
        return op.pts.size() >= 2;
    if (op.pts.size() < 2)
        return false;
    return op.pts.front().x != op.pts.back().x || op.pts.front().y != op.pts.back().y;
}

// ---------------------------------------------------------------------------
// 历史与重绘
// ---------------------------------------------------------------------------
void ComposeAll(Ctx& c);
void ComposeChrome(Ctx& c);
void ClearPreview(Ctx& c);
void RebuildStill(Ctx& c);
void UpdateStillRegion(Ctx& c, const RECT& imgDirty);

void RebuildBase(Ctx& c)
{
    if (!c.hdcBase || !c.hdcSrc)
        return;
    BitBlt(c.hdcBase, 0, 0, c.imgW, c.imgH, c.hdcSrc, 0, 0, SRCCOPY);

    Gdiplus::Graphics g(c.hdcBase);
    SetupGraphics(g);
    for (const Op& op : c.ops)
        DrawOp(g, op);
}

// 图形的图像坐标包围盒（含线宽与箭头头部余量）。
// 脏区宁可略大也不能小：漏掉边缘会在屏幕上留下一条擦不掉的旧像素
RECT OpImageBounds(const Op& op, int extra)
{
    if (op.pts.empty())
        return RECT{ 0, 0, 0, 0 };

    RECT r{ op.pts.front().x, op.pts.front().y, op.pts.front().x, op.pts.front().y };
    for (const POINT& p : op.pts)
    {
        if (p.x < r.left)   r.left   = p.x;
        if (p.y < r.top)    r.top    = p.y;
        if (p.x > r.right)  r.right  = p.x;
        if (p.y > r.bottom) r.bottom = p.y;
    }
    const int pad = (int)(op.width * 5.0f) + extra;
    r.left   -= pad;
    r.top    -= pad;
    r.right  += pad;
    r.bottom += pad;
    return r;
}

// 把 next 设为当前状态并压入历史。超过 kMaxUndo 步时丢最旧的快照
// （图形仍保留在 ops 里，只是不能再撤销到那一步）
// dirty 非空时只刷新该图像区域（提交单个图形），空则整图重建（撤销/重做/清空）
void PushSnapshot(Ctx& c, std::vector<Op> next, const RECT* dirty)
{
    c.ops = std::move(next);
    c.snaps.resize((size_t)c.snapIdx + 1);   // 丢弃撤销后新提交所覆盖的重做分支
    c.snaps.push_back(c.ops);
    c.snapIdx = (int)c.snaps.size() - 1;
    if ((int)c.snaps.size() > kMaxUndo + 1)
    {
        c.snaps.erase(c.snaps.begin());
        --c.snapIdx;
    }
    RebuildBase(c);
    if (dirty)
        UpdateStillRegion(c, *dirty);
    else
        RebuildStill(c);
    ComposeAll(c);
}

void Commit(Ctx& c, Op&& op)
{
    const RECT dirty = OpImageBounds(op, 2);
    c.ops.push_back(std::move(op));
    PushSnapshot(c, std::move(c.ops), &dirty);
}

void Undo(Ctx& c)
{
    if (c.snapIdx <= 0)
        return;
    --c.snapIdx;
    PushSnapshot(c, c.snaps[c.snapIdx], nullptr);
}

void Redo(Ctx& c)
{
    if (c.snapIdx + 1 >= (int)c.snaps.size())
        return;
    ++c.snapIdx;
    PushSnapshot(c, c.snaps[c.snapIdx], nullptr);
}

void ClearAll(Ctx& c)
{
    if (c.ops.empty())
        return;
    PushSnapshot(c, {}, nullptr);   // 清空同样进历史，可撤销
}
// ---------------------------------------------------------------------------
// 工具栏绘制（GDI：文字用系统字体直出，比 GDI+ 的字体渲染更清晰且无需额外字体对象）
// ---------------------------------------------------------------------------
void DrawRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH br  = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

void DrawTextIn(HDC dc, HFONT font, const RECT& r, const wchar_t* text,
                COLORREF color, UINT fmt)
{
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT tr = r;
    DrawTextW(dc, text, -1, &tr, fmt | DT_SINGLELINE | DT_VCENTER);
    if (old)
        SelectObject(dc, old);
}

void DrawButton(Ctx& c, HDC dc, const RECT& r, const wchar_t* text,
                bool active, bool hovered, bool enabled = true)
{
    COLORREF fill = active ? kAccent : (hovered && enabled ? kBtnHover : kBtnBg);
    COLORREF edge = active ? kAccent : kBtnEdge;
    DrawRoundRect(dc, r, Scale(c, 6), fill, edge);
    DrawTextIn(dc, c.hFont, r, text,
               !enabled ? kTextDim : (active ? RGB(255, 255, 255) : kTextMain),
               DT_CENTER);
}

void DrawSwatch(Ctx& c, HDC dc, const RECT& r, COLORREF rgb, bool active, bool hovered)
{
    if (active)
    {
        RECT outer = r;
        InflateRect(&outer, Scale(c, 2), Scale(c, 2));
        DrawRoundRect(dc, outer, Scale(c, 7), kTextMain, kTextMain);
    }
    DrawRoundRect(dc, r, Scale(c, 5), rgb,
                  (hovered && !active) ? kTextMain : kBtnEdge);
}

void DrawWidthButton(Ctx& c, HDC dc, const RECT& r, int px, bool active, bool hovered)
{
    DrawRoundRect(dc, r, Scale(c, 5),
                  active ? kAccent : (hovered ? kBtnHover : kBtnBg),
                  active ? kAccent : kBtnEdge);

    int th = px * c.s100 / 100;
    const int innerH = (r.bottom - r.top) - Scale(c, 12);
    if (th > innerH)
        th = innerH;
    if (th < 1)
        th = 1;
    const int cy = (r.top + r.bottom) / 2;
    RECT bar{ r.left + Scale(c, 8), cy - th / 2, r.right - Scale(c, 8), cy - th / 2 + th };
    HBRUSH br = CreateSolidBrush(kTextMain);
    FillRect(dc, &bar, br);
    DeleteObject(br);
}

void DrawHint(Ctx& c)
{
    DrawTextIn(c.hdcScreen, c.hFontSmall, c.hintRect, kHint, kTextDim, DT_CENTER);
}

void DrawPanel(Ctx& c)
{
    HDC dc = c.hdcScreen;
    DrawRoundRect(dc, c.panel, Scale(c, kRadius), kPanelBg, kPanelEdge);

    for (int i = 0; i < AnnotateToolCount; ++i)
        DrawButton(c, dc, c.btnTool[i], kToolText[i],
                   c.style.tool == i, c.hover == BtnTool0 + i);

    const bool canUndo = c.snapIdx > 0;
    const bool canRedo = c.snapIdx + 1 < (int)c.snaps.size();
    DrawButton(c, dc, c.btnUndo,  L"撤销", false, c.hover == BtnUndo,  canUndo);
    DrawButton(c, dc, c.btnRedo,  L"重做", false, c.hover == BtnRedo,  canRedo);
    DrawButton(c, dc, c.btnClear, L"清空", false, c.hover == BtnClear, !c.ops.empty());

    DrawButton(c, dc, c.btnCancel, L"取消", false, c.hover == BtnCancel);
    DrawButton(c, dc, c.btnDone,   L"完成", true,  c.hover == BtnDone);

    for (int i = 0; i < kPaletteCount; ++i)
        DrawSwatch(c, dc, c.btnColor[i], kPalette[i].rgb,
                   c.style.color == i, c.hover == BtnColor0 + i);

    for (int i = 0; i < kWidthCount; ++i)
        DrawWidthButton(c, dc, c.btnWidth[i], kWidths[i],
                        c.style.width == i, c.hover == BtnWidth0 + i);
}
// ---------------------------------------------------------------------------
// 布局：工具栏尺寸与图片显示区域
// ---------------------------------------------------------------------------
int TextWidth(HDC dc, HFONT font, const wchar_t* s)
{
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    if (old)
        SelectObject(dc, old);
    return sz.cx;
}

void LayoutUI(Ctx& c)
{
    HDC dc = c.hdcScreen;
    const int pad    = Scale(c, kPad);
    const int gap    = Scale(c, kGap);
    const int btnH   = Scale(c, kBtnH);
    const int swatch = Scale(c, kSwatch);
    const int wbtn   = Scale(c, kWidthBtn);
    const int margin = Scale(c, kMargin);

    auto btnW = [&](const wchar_t* s, int minW)
    {
        const int w = TextWidth(dc, c.hFont, s) + pad * 2;
        return w < minW ? minW : w;
    };

    const int toolW = btnW(L"矩形", Scale(c, kMinBtnW));
    const int actW  = btnW(L"撤销", Scale(c, kMinBtnW));
    const int endW  = btnW(L"取消", Scale(c, kEndBtnW));

    // 行 1：工具 | 撤销/重做/清空 | 取消/完成
    int row1W = AnnotateToolCount * toolW + (AnnotateToolCount - 1) * gap;
    row1W += gap * 2 + actW * 3 + gap * 2;
    row1W += gap * 2 + endW * 2 + gap;
    // 行 2：8 个色块 | 5 个线宽档
    int row2W = kPaletteCount * swatch + (kPaletteCount - 1) * gap;
    row2W += gap * 2 + kWidthCount * wbtn + (kWidthCount - 1) * gap;

    const int panelW = (row1W > row2W ? row1W : row2W) + pad * 2;
    const int panelH = pad * 2 + btnH + gap + swatch;

    c.panel.left   = (c.winW - panelW) / 2;
    c.panel.top    = c.winH - margin - panelH;
    c.panel.right  = c.panel.left + panelW;
    c.panel.bottom = c.panel.top + panelH;

    const int x0 = c.panel.left + pad;
    const int y1 = c.panel.top + pad;

    int x = x0;
    for (int i = 0; i < AnnotateToolCount; ++i)
    {
        c.btnTool[i] = RECT{ x, y1, x + toolW, y1 + btnH };
        x += toolW + gap;
    }
    x += gap;
    c.btnUndo  = RECT{ x, y1, x + actW, y1 + btnH };  x += actW + gap;
    c.btnRedo  = RECT{ x, y1, x + actW, y1 + btnH };  x += actW + gap;
    c.btnClear = RECT{ x, y1, x + actW, y1 + btnH };

    // 取消/完成贴右（面板宽度取两行最大值时，右对齐才不会留一段空当）
    c.btnDone   = RECT{ c.panel.right - pad - endW, y1, c.panel.right - pad, y1 + btnH };
    c.btnCancel = RECT{ c.btnDone.left - gap - endW, y1, c.btnDone.left - gap, y1 + btnH };

    const int y2 = y1 + btnH + gap;
    x = x0;
    for (int i = 0; i < kPaletteCount; ++i)
    {
        c.btnColor[i] = RECT{ x, y2, x + swatch, y2 + swatch };
        x += swatch + gap;
    }
    x += gap;
    for (int i = 0; i < kWidthCount; ++i)
    {
        c.btnWidth[i] = RECT{ x, y2, x + wbtn, y2 + swatch };
        x += wbtn + gap;
    }

    // ---- 图片显示区域 ----
    const int hintH = Scale(c, kHintPx) + Scale(c, 10);
    c.hintRect = RECT{ margin, Scale(c, kMargin), c.winW - margin, Scale(c, kMargin) + hintH };

    const int top    = c.hintRect.bottom + Scale(c, 6);
    const int availW = c.winW - margin * 2;
    int       availH = c.panel.top - Scale(c, kImgGap) - top;
    if (availH < 1)
        availH = 1;

    // 只缩不放：小截图 1:1 显示，大截图等比适配。用户没有缩放/平移操作
    float sc = 1.0f;
    if (c.imgW > availW || c.imgH > availH)
    {
        const float sx = (float)availW / (float)c.imgW;
        const float sy = (float)availH / (float)c.imgH;
        sc = sx < sy ? sx : sy;
        if (sc < 0.02f)
            sc = 0.02f;
    }
    c.scale = sc;
    c.dispW = (int)(c.imgW * sc + 0.5f);
    c.dispH = (int)(c.imgH * sc + 0.5f);
    if (c.dispW < 1) c.dispW = 1;
    if (c.dispH < 1) c.dispH = 1;

    c.imgX = (c.winW - c.dispW) / 2;
    c.imgY = top + (availH - c.dispH) / 2;
    if (c.imgY < top)
        c.imgY = top;
    c.imgRect = RECT{ c.imgX, c.imgY, c.imgX + c.dispW, c.imgY + c.dispH };
}
// ---------------------------------------------------------------------------
// 合成
// ---------------------------------------------------------------------------
// 图像坐标 -> 客户区坐标（与 UpdateStillRegion 用同一套取整，避免累积偏差）
int ScreenX(const Ctx& c, int x)
{
    return c.imgX + (int)floorf((float)x * c.scale + 0.5f);
}

int ScreenY(const Ctx& c, int y)
{
    return c.imgY + (int)floorf((float)y * c.scale + 0.5f);
}

// 静态层 = 背景 + 缩放后的 base + 图片描边。只在初始化与撤销/重做/清空时整体重建
void RebuildStill(Ctx& c)
{
    if (!c.hdcStill)
        return;

    RECT full{ 0, 0, c.winW, c.winH };
    HBRUSH bg = CreateSolidBrush(kBgColor);
    FillRect(c.hdcStill, &full, bg);
    DeleteObject(bg);

    if (c.hdcBase)
    {
        if (c.scale >= 0.999f)
            BitBlt(c.hdcStill, c.imgX, c.imgY, c.dispW, c.dispH, c.hdcBase, 0, 0, SRCCOPY);
        else
        {
            SetStretchBltMode(c.hdcStill, HALFTONE);
            SetBrushOrgEx(c.hdcStill, c.imgX, c.imgY, nullptr);
            StretchBlt(c.hdcStill, c.imgX, c.imgY, c.dispW, c.dispH,
                       c.hdcBase, 0, 0, c.imgW, c.imgH, SRCCOPY);
        }
    }

    // 1px 描边：浅色截图与深色背景之间需要一条分界线
    HBRUSH edge = CreateSolidBrush(kPanelEdge);
    FrameRect(c.hdcStill, &c.imgRect, edge);
    DeleteObject(edge);
}

// 只把 base 的一块图像区域刷进静态层（提交单个图形时用）。
// 整图 HALFTONE 缩放是几十毫秒级的开销，不能放在每次落笔的路径上
void UpdateStillRegion(Ctx& c, const RECT& imgDirty)
{
    if (!c.hdcStill || !c.hdcBase)
        return;

    RECT ir;
    ir.left   = ClampI(imgDirty.left,   0, c.imgW);
    ir.top    = ClampI(imgDirty.top,    0, c.imgH);
    ir.right  = ClampI(imgDirty.right,  0, c.imgW);
    ir.bottom = ClampI(imgDirty.bottom, 0, c.imgH);
    if (ir.right <= ir.left || ir.bottom <= ir.top)
        return;

    const int dx = ScreenX(c, ir.left);
    const int dy = ScreenY(c, ir.top);
    const int dw = ScreenX(c, ir.right) - dx;
    const int dh = ScreenY(c, ir.bottom) - dy;
    if (dw <= 0 || dh <= 0)
        return;

    if (c.scale >= 0.999f)
    {
        BitBlt(c.hdcStill, dx, dy, dw, dh, c.hdcBase, ir.left, ir.top, SRCCOPY);
    }
    else
    {
        SetStretchBltMode(c.hdcStill, HALFTONE);
        SetBrushOrgEx(c.hdcStill, dx, dy, nullptr);
        StretchBlt(c.hdcStill, dx, dy, dw, dh, c.hdcBase,
                   ir.left, ir.top, ir.right - ir.left, ir.bottom - ir.top, SRCCOPY);
    }
}

// 预览的脏区：把图形的图像包围盒映射到客户区
RECT PreviewDirty(const Ctx& c, const Op& op)
{
    const RECT r = OpImageBounds(op, 2);
    if (r.right <= r.left || r.bottom <= r.top)
        return RECT{ 0, 0, 0, 0 };

    RECT d;
    d.left   = ScreenX(c, r.left);
    d.top    = ScreenY(c, r.top);
    d.right  = ScreenX(c, r.right);
    d.bottom = ScreenY(c, r.bottom);

    RECT clip = c.imgRect;
    InflateRect(&clip, 2, 2);

    RECT out;
    if (!IntersectRect(&out, &d, &clip))
        SetRectEmpty(&out);
    return out;
}

// 预览用世界变换直接画到显示层：坐标仍是图像坐标，线宽随 scale 缩放，
// 因此与最终烧进输出的样子一致（不必再维护一层"图像尺寸的合成图"）
void DrawPreview(Ctx& c, const Op& op)
{
    Gdiplus::Graphics g(c.hdcScreen);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.TranslateTransform((Gdiplus::REAL)c.imgX, (Gdiplus::REAL)c.imgY);
    g.ScaleTransform(c.scale, c.scale);
    g.SetClip(Gdiplus::RectF(0.0f, 0.0f, (Gdiplus::REAL)c.imgW, (Gdiplus::REAL)c.imgH));
    DrawOp(g, op);

    // 刚按下还没位移：画一个起点圆点，给"已经开始"的即时反馈
    const int dx = c.ptCur.x - c.ptDown.x;
    const int dy = c.ptCur.y - c.ptDown.y;
    if (c.dragging && dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2)
    {
        Gdiplus::SolidBrush br(MakeColor(AnnotateColorAt(c.style.color), 255));
        const Gdiplus::REAL r = op.width;
        g.FillEllipse(&br, (Gdiplus::REAL)c.ptDown.x - r, (Gdiplus::REAL)c.ptDown.y - r,
                      r * 2.0f, r * 2.0f);
    }
}

// 拖动中的增量刷新：恢复上一帧预览占用的矩形，再画新预览。
// 这是流畅度的关键 —— 每次鼠标移动只有两块小区域参与合成
void UpdatePreview(Ctx& c)
{
    if (!c.hdcScreen || !c.hdcStill)
        return;

    const Op op = BuildOp(c);   // 一次构建，脏区与绘制共用
    const RECT nd = PreviewDirty(c, op);
    RECT rd = nd;
    if (!IsRectEmpty(&c.previewDirty))
    {
        RECT u;
        if (UnionRect(&u, &c.previewDirty, &nd))
            rd = u;
    }
    if (IsRectEmpty(&rd))
        return;

    BitBlt(c.hdcScreen, rd.left, rd.top, rd.right - rd.left, rd.bottom - rd.top,
           c.hdcStill, rd.left, rd.top, SRCCOPY);
    DrawPreview(c, op);
    c.previewDirty = nd;

    InvalidateRect(c.hwnd, &rd, FALSE);
    // 鼠标消息比 WM_PAINT 优先投递，不强制在这里落地的话预览会肉眼可见地滞后
    UpdateWindow(c.hwnd);
}

void ClearPreview(Ctx& c)
{
    if (IsRectEmpty(&c.previewDirty))
        return;
    const RECT rd = c.previewDirty;
    BitBlt(c.hdcScreen, rd.left, rd.top, rd.right - rd.left, rd.bottom - rd.top,
           c.hdcStill, rd.left, rd.top, SRCCOPY);
    SetRectEmpty(&c.previewDirty);
    InvalidateRect(c.hwnd, &rd, FALSE);
}

void ComposeAll(Ctx& c)
{
    if (!c.hdcScreen || !c.hdcStill)
        return;

    BitBlt(c.hdcScreen, 0, 0, c.winW, c.winH, c.hdcStill, 0, 0, SRCCOPY);
    SetRectEmpty(&c.previewDirty);
    if (c.dragging)
        UpdatePreview(c);   // 从空脏区开始，等价于直接画预览

    DrawPanel(c);
    DrawHint(c);

    InvalidateRect(c.hwnd, nullptr, FALSE);
    UpdateWindow(c.hwnd);
}

// 只重画工具栏（悬停变化）：面板区域从静态层恢复后重绘，不动图片与预览
void ComposeChrome(Ctx& c)
{
    if (!c.hdcScreen || !c.hdcStill)
        return;

    RECT pr = c.panel;
    InflateRect(&pr, Scale(c, 4), Scale(c, 4));
    RECT clip{ 0, 0, c.winW, c.winH };
    if (!IntersectRect(&pr, &pr, &clip))
        return;

    BitBlt(c.hdcScreen, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top,
           c.hdcStill, pr.left, pr.top, SRCCOPY);
    DrawPanel(c);

    InvalidateRect(c.hwnd, &pr, FALSE);
    UpdateWindow(c.hwnd);
}
// ---------------------------------------------------------------------------
// 命中测试与按钮动作
// ---------------------------------------------------------------------------
int HitTest(const Ctx& c, POINT pt)
{
    if (!PtInRect(&c.panel, pt))
        return BtnNone;

    for (int i = 0; i < AnnotateToolCount; ++i)
        if (PtInRect(&c.btnTool[i], pt))
            return BtnTool0 + i;
    if (PtInRect(&c.btnUndo, pt))   return BtnUndo;
    if (PtInRect(&c.btnRedo, pt))   return BtnRedo;
    if (PtInRect(&c.btnClear, pt))  return BtnClear;
    if (PtInRect(&c.btnCancel, pt)) return BtnCancel;
    if (PtInRect(&c.btnDone, pt))   return BtnDone;
    for (int i = 0; i < kPaletteCount; ++i)
        if (PtInRect(&c.btnColor[i], pt))
            return BtnColor0 + i;
    for (int i = 0; i < kWidthCount; ++i)
        if (PtInRect(&c.btnWidth[i], pt))
            return BtnWidth0 + i;
    return BtnNone;
}

// 客户区坐标 -> 图像像素坐标（夹在图片内：图形不会画到裁剪区之外）
POINT ClientToImage(const Ctx& c, POINT pt)
{
    const float fx = ((float)pt.x - (float)c.imgX) / c.scale;
    const float fy = ((float)pt.y - (float)c.imgY) / c.scale;
    POINT out;
    out.x = ClampI((int)lroundf(fx), 0, c.imgW - 1);
    out.y = ClampI((int)lroundf(fy), 0, c.imgH - 1);
    return out;
}

void Finish(Ctx& c)
{
    if (c.done)
        return;
    if (c.dragging)
    {
        c.dragging = false;
        ReleaseCapture();
        Op op = BuildOp(c);
        if (OpHasExtent(op))
        {
            c.ops.push_back(std::move(op));
            RebuildBase(c);   // 正要输出，不必再进历史
        }
    }
    c.done = true;
    DestroyWindow(c.hwnd);
}

void Cancel(Ctx& c)
{
    if (c.done)
        return;
    if (c.dragging)
    {
        c.dragging = false;
        ReleaseCapture();
    }
    c.canceled = true;
    c.done = true;
    DestroyWindow(c.hwnd);
}

void OnButton(Ctx& c, int hit)
{
    if (hit >= BtnTool0 && hit < BtnTool0 + AnnotateToolCount)
        c.style.tool = hit - BtnTool0;
    else if (hit >= BtnColor0 && hit < BtnColor0 + kPaletteCount)
        c.style.color = hit - BtnColor0;
    else if (hit >= BtnWidth0 && hit < BtnWidth0 + kWidthCount)
        c.style.width = hit - BtnWidth0;
    else if (hit == BtnUndo)  { Undo(c);  return; }
    else if (hit == BtnRedo)  { Redo(c);  return; }
    else if (hit == BtnClear) { ClearAll(c); return; }
    else if (hit == BtnCancel) { Cancel(c); return; }
    else if (hit == BtnDone)   { Finish(c); return; }
    else return;

    ComposeAll(c);
}
// ---------------------------------------------------------------------------
// 资源初始化
// ---------------------------------------------------------------------------
bool InitCtx(Ctx& c, const ImageBGRA& src)
{
    c.imgW = (int)src.w;
    c.imgH = (int)src.h;

    HDC ref = GetDC(nullptr);
    if (!ref)
        return false;
    c.hbmScreen = MakeDib(ref, c.winW, c.winH, c.hdcScreen, nullptr);
    c.hbmStill  = MakeDib(ref, c.winW, c.winH, c.hdcStill, nullptr);
    c.hbmSrc    = MakeDib(ref, c.imgW, c.imgH, c.hdcSrc, &c.bitsSrc);
    c.hbmBase   = MakeDib(ref, c.imgW, c.imgH, c.hdcBase, &c.bitsBase);
    ReleaseDC(nullptr, ref);

    if (!c.hbmScreen || !c.hbmStill || !c.hbmSrc || !c.hbmBase || !c.bitsSrc)
    {
        LOG_ERR(L"标注: DIB 创建失败（图片 %dx%d）", c.imgW, c.imgH);
        return false;
    }

    // ImageBGRA 与 32bpp top-down DIB 逐字节同构，直接拷
    memcpy(c.bitsSrc, src.pixels.data(), (size_t)c.imgW * c.imgH * 4);
    BitBlt(c.hdcBase, 0, 0, c.imgW, c.imgH, c.hdcSrc, 0, 0, SRCCOPY);

    // 中文字体：与主界面同源（微软雅黑），失败时用默认字体仍能显示工具文案
    c.hFont = CreateFontW(-Scale(c, kFontPx), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    c.hFontSmall = CreateFontW(-Scale(c, kHintPx), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");

    c.snaps.clear();
    c.snaps.push_back(c.ops);
    c.snapIdx = 0;
    return true;
}

void FreeCtx(Ctx& c)
{
    DestroyDib(c.hdcSrc,    c.hbmSrc);
    DestroyDib(c.hdcBase,   c.hbmBase);
    DestroyDib(c.hdcStill,  c.hbmStill);
    DestroyDib(c.hdcScreen, c.hbmScreen);
    if (c.hFont)      { DeleteObject(c.hFont);      c.hFont = nullptr; }
    if (c.hFontSmall) { DeleteObject(c.hFontSmall); c.hFontSmall = nullptr; }
}
// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Ctx* c = (Ctx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_CREATE)
    {
        c = (Ctx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        c->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
        return 0;
    }
    if (!c)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;   // 整屏已在内存 DIB 里合成，擦背景纯属多余

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        // 只贴脏区：整屏 2560x1440 的 Blt 是 14MB 级拷贝，每帧都做等于白扔几毫秒
        if (c->hdcScreen)
        {
            const RECT& r = ps.rcPaint;
            if (r.right > r.left && r.bottom > r.top)
                BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                       c->hdcScreen, r.left, r.top, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == kEscTimer)
        {
            const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (down && !c->escDown)
                Cancel(*c);
            c->escDown = down;
        }
        return 0;

    case WM_KEYDOWN:
    {
        const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wp == VK_RETURN)   { Finish(*c); return 0; }
        if (wp == VK_ESCAPE)   { Cancel(*c); return 0; }
        if (ctrl && (wp == 'Z' || wp == 'Y'))
        {
            if (wp == 'Y' || shift) Redo(*c);
            else                    Undo(*c);
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const int hit = HitTest(*c, pt);
        if (hit != BtnNone)
        {
            OnButton(*c, hit);
            return 0;
        }
        if (!PtInRect(&c->imgRect, pt))
            return 0;

        const POINT ip = ClientToImage(*c, pt);
        c->dragging = true;
        c->ptDown   = ip;
        c->ptCur    = ip;
        c->stroke.clear();
        c->stroke.push_back(ip);
        SetCapture(hwnd);
        UpdatePreview(*c);   // 按下即画起点圆点，不必等到第一次移动
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (c->dragging)
        {
            const POINT ip = ClientToImage(*c, pt);
            if (c->style.tool == AnnotatePen || c->style.tool == AnnotateHighlight)
            {
                // 采样：相邻点至少隔 2 图像像素才记录，避免轨迹堆积重合点
                const POINT& last = c->stroke.back();
                const int dx = ip.x - last.x, dy = ip.y - last.y;
                if (dx * dx + dy * dy >= 4)
                    c->stroke.push_back(ip);
            }
            c->ptCur = ip;
            UpdatePreview(*c);
            return 0;
        }

        const int hit = HitTest(*c, pt);
        if (hit != c->hover)
        {
            c->hover = hit;
            ComposeChrome(*c);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        if (!c->dragging)
            return 0;
        c->dragging = false;
        ReleaseCapture();

        Op op = BuildOp(*c);
        if (OpHasExtent(op))
            Commit(*c, std::move(op));
        else
        {
            ClearPreview(*c);
            UpdateWindow(hwnd);
        }
        return 0;
    }

    case WM_SETCURSOR:
    {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const bool overImg = PtInRect(&c->imgRect, pt) != 0;
        const bool overBtn = HitTest(*c, pt) != BtnNone;
        SetCursor(LoadCursorW(nullptr, (overImg && !overBtn) ? IDC_CROSS : IDC_ARROW));
        return TRUE;
    }

    case WM_DESTROY:
        // 不能 PostQuitMessage：这里只是模态子窗口，退出标志一旦留在主线程队列，
        // 主循环的下一次 GetMessage 会立刻返回 0，整个程序跟着退出
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
}   // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
int AnnotateColorCount()
{
    return kPaletteCount;
}

COLORREF AnnotateColorAt(int index)
{
    if (index < 0 || index >= kPaletteCount)
        index = 0;
    return kPalette[index].rgb;
}

const wchar_t* AnnotateColorName(int index)
{
    if (index < 0 || index >= kPaletteCount)
        index = 0;
    return kPalette[index].name;
}

int AnnotateWidthCount()
{
    return kWidthCount;
}

int AnnotateWidthPx(int index)
{
    if (index < 0 || index >= kWidthCount)
        index = kWidthCount / 2;
    return kWidths[index];
}

bool RunAnnotator(HINSTANCE hinst, const ImageBGRA& src, ImageBGRA& out,
                  AnnotateStyle& style)
{
    if (!src.Valid())
        return false;
    EnsureGdiPlus();

    Ctx c;
    c.style = style;
    // 配置里的旧值越界时收敛，否则会取到 nullptr / 空表
    if (c.style.tool  < 0 || c.style.tool  >= AnnotateToolCount) c.style.tool  = AnnotateRect;
    if (c.style.color < 0 || c.style.color >= kPaletteCount)     c.style.color = 0;
    if (c.style.width < 0 || c.style.width >= kWidthCount)       c.style.width = 1;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    c.winW = mi.rcMonitor.right - mi.rcMonitor.left;
    c.winH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (c.winW <= 0 || c.winH <= 0)
        return false;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);   // 重复注册返回 0，无害

    g_ctx = &c;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName,
                                L"标注截图", WS_POPUP,
                                mi.rcMonitor.left, mi.rcMonitor.top, c.winW, c.winH,
                                nullptr, nullptr, hinst, &c);
    if (!hwnd)
    {
        g_ctx = nullptr;
        LOG_ERR(L"标注: 窗口创建失败");
        return false;
    }

    // 标注窗口只需鼠标与少量快捷键，没有任何文字输入需求：必须解绑 IME。
    // 不解绑时窗口取焦点会让第三方输入法（实测豆包 tsf-oime-core.dll）在 TSF 侧
    // 崩溃，且崩的是本进程。仅缓解而非根治，与框选遮罩同一策略
    ImmAssociateContext(hwnd, nullptr);

    const UINT dpi = GetDpiForWindow(hwnd);
    c.s100 = dpi ? (int)(dpi * 100 / 96) : 100;

    if (!InitCtx(c, src))
    {
        DestroyWindow(hwnd);
        style = c.style;
        FreeCtx(c);
        g_ctx = nullptr;
        UnregisterClassW(kClassName, hinst);
        return false;
    }

    LayoutUI(c);
    RebuildBase(c);
    RebuildStill(c);
    ComposeAll(c);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    SetTimer(hwnd, kEscTimer, kEscTimerMs, nullptr);

    // 只分发本窗口的消息：主窗口此时只是隐藏，热键/托盘消息若在这里被派发，
    // 会重入截图流程（表现为标注窗口上又叠一层框选）。WM_QUIT 必须放行
    MSG msg{};
    while (!c.done)
    {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                c.canceled = true;
                c.done = true;
                PostQuitMessage((int)msg.wParam);
                break;
            }
            if (msg.hwnd == hwnd)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        else
        {
            WaitMessage();
        }
    }

    KillTimer(hwnd, kEscTimer);

    bool ok = false;
    if (c.done && !c.canceled && c.bitsBase)
    {
        out.w = (UINT)c.imgW;
        out.h = (UINT)c.imgH;
        out.pixels.resize((size_t)c.imgW * (size_t)c.imgH * 4);
        memcpy(out.pixels.data(), c.bitsBase, out.pixels.size());
        // 关键：通过 HDC 合成时 GDI/GDI+ 不保证写 alpha 通道。不强制置 255 的话，
        // 原图 alpha 一旦被写成 0，存 PNG 会整张透明（剪贴板 CF_DIB 恰好不看 alpha，
        // 于是"复制正常、保存全透明"，很容易被误判成编码器问题）。
        for (size_t i = 3; i < out.pixels.size(); i += 4)
            out.pixels[i] = 255;
        ok = true;
    }

    if (IsWindow(hwnd))
        DestroyWindow(hwnd);

    style = c.style;
    FreeCtx(c);
    g_ctx = nullptr;
    UnregisterClassW(kClassName, hinst);
    return ok;
}