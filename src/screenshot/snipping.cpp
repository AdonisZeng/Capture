// 区域框选遮罩实现
//
// 渲染链路：帧 DIB（原尺寸，仅在初始化时填充一次）
//            -> 压暗 DIB（窗口尺寸，初始化时就地压暗一次）
//            -> 合成 DIB（每次重绘：压暗底 + 选区还原 + 边框/角标/文字）
//            -> UpdateLayeredWindow(ULW_OPAQUE)
// 这样鼠标移动时只有两次 Blt，压暗这类逐像素运算不会跟着鼠标走。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "snipping.h"
#include "core/log.h"
#include <windowsx.h>
#include <imm.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{

// 按下到抬起的位移小于该值（物理像素）即判定为"单击" -> 整屏
constexpr int   kClickSlopPx = 6;
// 选区外的压暗系数（越小越暗）
constexpr float kDimFactor   = 0.42f;
constexpr int   kBorderPx    = 2;
constexpr int   kHandlePx    = 10;
constexpr int   kLabelPadX   = 8;
constexpr int   kLabelPadY   = 5;
constexpr int   kFontPx      = 14;
constexpr int   kHintTop     = 28;

const wchar_t   kSnipClass[] = L"CaptureSnipClass";
// 启用窗口识别时：单击截高亮窗口，拖拽才是框选
const wchar_t   kHintText[]  = L"移动鼠标选中窗口 · 单击截取窗口 · Ctrl+单击截整屏 · 按住左键拖出区域 · 方向键微调(Shift=10px) · 回车确认 · Esc 取消";
// 区域录制等不启用窗口识别的场景保持原行为：单击截全屏
const wchar_t   kHintTextLegacy[] = L"按住左键拖出区域 · 单击截全屏 · 方向键微调(Shift=10px) · 回车确认 · Esc 取消";
// 选区确认、快捷操作栏出现后：遮罩仍覆盖全屏，必须给出明确出口与键位，
// 否则用户容易以为程序卡住（此时上一条操作说明已被选区内容覆盖）
const wchar_t   kHintTextConfirm[] = L"选择下方操作：复制 / 文字识别 / 分享 · 回车 = 复制 · Esc = 取消";

// Esc 兜底轮询定时器：遮罩失焦后 WM_KEYDOWN 收不到 Esc，靠它保证任何焦点状态下都能取消
constexpr UINT_PTR kEscTimerId = 1;
constexpr UINT     kEscTimerMs = 50;
// 非主显示器没有底图（App 只捕获主显示器），其区域填这个色，观感与压暗一致
constexpr COLORREF kOffscreenBg = RGB(14, 16, 20);

constexpr COLORREF kAccent  = RGB(52, 102, 252);   // 与 UI 主题 Accent 同色
constexpr COLORREF kLabelBg = RGB(16, 18, 22);

// 快捷操作栏（尺寸均按 96dpi 书写，绘制时按 c.dpi 缩放）
constexpr int kBarPad     = 10;
constexpr int kBarGap     = 6;
constexpr int kBarBtnH    = 36;
constexpr int kBarMargin  = 10;
constexpr int kBarRadius  = 8;
constexpr COLORREF kBarBg     = RGB(24, 27, 34);
constexpr COLORREF kBarBorder = RGB(62, 68, 82);
constexpr COLORREF kBarBtnBg  = RGB(40, 44, 54);

struct SnipCtx
{
    HWND  hwnd = nullptr;
    int   winW = 0, winH = 0;      // 遮罩窗口尺寸（覆盖整个虚拟屏的物理像素）
    // 遮罩窗口在屏幕上的原点（虚拟屏左上角）：客户区坐标 + 该原点 = 屏幕坐标
    int   originX = 0, originY = 0;
    // 主显示器在「窗口坐标系」中的位置与尺寸：只有这块区域有帧底图，
    // 选区必须夹在它内部，坐标换算也只在这块区域内做
    int   monX = 0, monY = 0, monW = 0, monH = 0;
    UINT  frameW = 0, frameH = 0;  // 底图尺寸
    int   dpi = 96;

    HDC     hdcFrame = nullptr, hdcDim = nullptr, hdcWork = nullptr;
    HBITMAP hbmFrame = nullptr, hbmDim = nullptr, hbmWork = nullptr;
    HGDIOBJ oldFrame = nullptr, oldDim = nullptr, oldWork = nullptr;
    BYTE*   dimBits = nullptr;
    HFONT   hFont = nullptr;

    bool  pickWindow = false;       // 是否启用「悬停识别窗口」（截屏启用，区域录制保持原行为）
    bool  hasHover   = false;       // 鼠标下是否有可截取的窗口
    RECT  hover{};                  // 高亮窗口矩形（遮罩坐标）
    HWND  hoverHwnd  = nullptr;     // 上次命中的窗口：鼠标未移出它时免去全量枚举

    bool  dragging = false;
    bool  hasSel   = false;
    bool  movedBeyondSlop = false;  // 本次按下后是否已超过单击阈值（只影响拖动中的视觉状态）
    bool  clickHadHover   = false;  // 按下瞬间是否有高亮窗口
    bool  ctrlDown        = false;  // 按下瞬间 Ctrl 是否按住（Ctrl+单击 = 截整屏）
    RECT  clickRect{};              // 按下瞬间的高亮窗口矩形
    POINT ptDown{}, ptNow{};
    RECT  sel{};

    // ---- 快捷操作栏 ----
    // 松手后不立即结束：保留遮罩与选区，在选区右下角显示一排按钮，
    // 用户点按钮才返回对应动作（× = 取消）。区域录制不启用。
    bool actionBar = false;
    bool confirmed = false;          // 选区已确认，正在操作快捷栏
    SnipAction action = SnipAction::None;
    bool shareOpen = false;          // “分享”子菜单是否展开
    int  barHover  = -1;             // 当前悬停按钮（-1 = 无）
    struct BarLayout
    {
        RECT panel{};
        RECT copy{}, ocr{}, share{}, close{};
        RECT sharePanel{};
        RECT wechat{}, qq{};
        bool valid = false;
    } bar;

    bool done = false;
    bool canceled = false;
    bool full = false;
};

SnipCtx* g_ctx = nullptr;   // 同一时刻只可能有一个框选窗口

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
int ClampI(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 自上而下的 32bpp DIB（biHeight 取负，首行即画面顶部，与 ImageBGRA 一致）
HBITMAP CreateTopDownDib(HDC ref, int w, int h, void** bits)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(ref, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// ---------------------------------------------------------------------------
// 鼠标下的窗口识别
// ---------------------------------------------------------------------------

struct FindWindowData
{
    POINT pt{};          // 屏幕坐标
    DWORD pid = 0;       // 本进程 ID：跳过遮罩自身与主窗口
    HWND  found = nullptr;
};

// 该窗口是否为点下可截取的候选（Z 序枚举与缓存命中后的向上回溯共用同一套规则）
bool IsPickableWindowAt(HWND hwnd, POINT pt, DWORD selfPid)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == selfPid)
        return false;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;

    // 点击穿透的浮层（各种游戏/录屏覆盖层）不该挡住下面的真实窗口
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT)
        return false;
    if (ex & WS_EX_LAYERED)
    {
        // 全透明分层窗口同样视为不可见（UpdateLayeredWindow 的逐像素 alpha 查询不到，保持原样）
        BYTE alpha = 255;
        DWORD flags = 0;
        COLORREF key = 0;
        if (GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) &&
            (flags & LWA_ALPHA) && alpha == 0)
            return false;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc))
        return false;
    if (rc.right <= rc.left || rc.bottom <= rc.top)
        return false;
    if (!PtInRect(&rc, pt))
        return false;

    // 桌面窗口不算可截取目标：鼠标在桌面空白处时仍按「单击=整屏」处理
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (lstrcmpiW(cls, L"Progman") == 0 || lstrcmpiW(cls, L"WorkerW") == 0)
        return false;

    // 被 DWM 隐藏的窗口（UWP 挂起、其他虚拟桌面等）不可见
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    return true;
}

// EnumWindows 按 Z 序（从上到下）枚举顶层窗口，第一个命中的即为最上层窗口
BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lp)
{
    FindWindowData* d = reinterpret_cast<FindWindowData*>(lp);
    if (!IsPickableWindowAt(hwnd, d->pt, d->pid))
        return TRUE;
    d->found = hwnd;
    return FALSE;
}

// 缓存窗口仍包含光标时，沿 Z 序向上回溯它上面的窗口，取最上层的命中者
// （典型场景：小窗口盖在大窗口上，鼠标移回小窗口区域后必须切回小窗口）。
// 必须扫完整个上方链：中途多个窗口都包含该点时，越靠上的越优先。
HWND FindHigherPickableWindow(HWND cached, POINT pt, DWORD selfPid)
{
    HWND best = cached;
    for (HWND h = GetWindow(cached, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV))
    {
        if (IsPickableWindowAt(h, pt, selfPid))
            best = h;
    }
    return best;
}

// 取窗口的可见边界：优先用 DWM 的扩展边框（GetWindowRect 含透明外扩，
// Win10 起约 7px 阴影边），失败时回退 GetWindowRect
bool GetVisibleWindowRect(HWND hwnd, RECT& rect)
{
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &rect, sizeof(rect))) &&
        rect.right > rect.left && rect.bottom > rect.top)
        return true;
    return GetWindowRect(hwnd, &rect) &&
           rect.right > rect.left && rect.bottom > rect.top;
}

// 返回屏幕点下最上层的可截取窗口；成功时 rect 为屏幕坐标
HWND TopWindowAtPoint(POINT pt, RECT& rect)
{
    FindWindowData d;
    d.pt  = pt;
    d.pid = GetCurrentProcessId();
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&d));
    if (!d.found || !GetVisibleWindowRect(d.found, rect))
        return nullptr;
    return d.found;
}

// 遮罩坐标 -> 帧像素坐标。
// 底图只对应主显示器那块矩形（单显示器时 monX/monY=0 且 monW/monH=winW/winH，与原逻辑等价），
// 故先减掉该矩形原点，再按该矩形的比例换算
int ToFrameX(const SnipCtx& c, int x)
{
    if (c.monW <= 0) return 0;
    return ClampI((int)((double)(x - c.monX) * c.frameW / c.monW + 0.5), 0, (int)c.frameW);
}

int ToFrameY(const SnipCtx& c, int y)
{
    if (c.monH <= 0) return 0;
    return ClampI((int)((double)(y - c.monY) * c.frameH / c.monH + 0.5), 0, (int)c.frameH);
}

// 遮罩坐标矩形转成帧像素矩形，并保证至少 1x1（缩放取整可能把宽度压成 0）
RECT FrameRectOfRect(const SnipCtx& c, const RECT& s)
{
    RECT r{};
    r.left   = ToFrameX(c, s.left);
    r.top    = ToFrameY(c, s.top);
    r.right  = ToFrameX(c, s.right);
    r.bottom = ToFrameY(c, s.bottom);
    if (r.right  <= r.left)   r.right  = (r.left   + 1 <= (int)c.frameW) ? r.left   + 1 : (int)c.frameW;
    if (r.bottom <= r.top)    r.bottom = (r.top    + 1 <= (int)c.frameH) ? r.top    + 1 : (int)c.frameH;
    if (r.left   >= r.right)  r.left   = r.right - 1;
    if (r.top    >= r.bottom) r.top    = r.bottom - 1;
    return r;
}

RECT FrameRectOf(const SnipCtx& c)
{
    return FrameRectOfRect(c, c.sel);
}

void UpdateSel(SnipCtx& c)
{
    int x0 = c.ptDown.x < c.ptNow.x ? c.ptDown.x : c.ptNow.x;
    int x1 = c.ptDown.x > c.ptNow.x ? c.ptDown.x : c.ptNow.x;
    int y0 = c.ptDown.y < c.ptNow.y ? c.ptDown.y : c.ptNow.y;
    int y1 = c.ptDown.y > c.ptNow.y ? c.ptDown.y : c.ptNow.y;
    // 夹到主显示器矩形内：落在其他显示器上的部分没有底图，不能进选区
    x0 = ClampI(x0, c.monX, c.monX + c.monW);
    x1 = ClampI(x1, c.monX, c.monX + c.monW);
    y0 = ClampI(y0, c.monY, c.monY + c.monH);
    y1 = ClampI(y1, c.monY, c.monY + c.monH);
    if (x1 <= x0) x1 = (x0 + 1 <= c.monX + c.monW) ? x0 + 1 : c.monX + c.monW;
    if (y1 <= y0) y1 = (y0 + 1 <= c.monY + c.monH) ? y0 + 1 : c.monY + c.monH;
    if (x0 >= x1) x0 = x1 - 1;
    if (y0 >= y1) y0 = y1 - 1;
    c.sel = RECT{ x0, y0, x1, y1 };
}

// ---------------------------------------------------------------------------
// 合成一次画面并推给分层窗口
// ---------------------------------------------------------------------------
void DrawTextBox(HDC dc, HFONT font, const wchar_t* text, int boxX, int boxY,
                 const SnipCtx& c, bool centerX = false)
{
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    RECT tr{};
    DrawTextW(dc, text, -1, &tr, DT_CALCRECT | DT_SINGLELINE);
    int bw = (tr.right - tr.left) + kLabelPadX * 2;
    int bh = (tr.bottom - tr.top) + kLabelPadY * 2;
    // 居中基准取有底图的主显示器矩形：多屏时虚拟屏中心可能落在没画面的那一屏
    if (centerX)
        boxX = c.monX + (c.monW - bw) / 2;
    // boxY 仍按窗口绝对坐标传入，只把结果夹进主显示器矩形
    int bx = ClampI(boxX, c.monX + 4,
                    (c.monW - 4 - bw > 4) ? c.monX + c.monW - 4 - bw : c.monX + 4);
    int by = ClampI(boxY, c.monY + 4,
                    (c.monH - 4 - bh > 4) ? c.monY + c.monH - 4 - bh : c.monY + 4);

    RECT box{ bx, by, bx + bw, by + bh };
    HBRUSH br = CreateSolidBrush(kLabelBg);
    FillRect(dc, &box, br);
    DeleteObject(br);
    DrawTextW(dc, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

// 区域内还原原始亮度：按同一比例回到帧坐标取样，避免缩放累积偏差
void RestoreRegion(HDC dc, const SnipCtx& c, const RECT& r)
{
    const int dw = r.right - r.left;
    const int dh = r.bottom - r.top;
    if (dw <= 0 || dh <= 0)
        return;
    const int fx = ToFrameX(c, r.left);
    const int fy = ToFrameY(c, r.top);
    int fw = ToFrameX(c, r.right) - fx;
    int fh = ToFrameY(c, r.bottom) - fy;
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    StretchBlt(dc, r.left, r.top, dw, dh, c.hdcFrame, fx, fy, fw, fh, SRCCOPY);
}

void DrawAccentBorder(HDC dc, const RECT& r)
{
    HPEN pen = CreatePen(PS_SOLID, kBorderPx, kAccent);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// ---------------------------------------------------------------------------
// 快捷操作栏（松手后浮在选区右下角）
// ---------------------------------------------------------------------------
enum BarHit
{
    BarNone = -1,
    BarCopy = 0,
    BarOcr,
    BarShare,
    BarClose,
    BarWeChat,
    BarQQ,
};

int BarScale(const SnipCtx& c, int v)
{
    return MulDiv(v, c.dpi, 96);
}

int MeasureBarText(SnipCtx& c, const wchar_t* text)
{
    if (!c.hdcWork || !c.hFont)
        return 0;
    HGDIOBJ oldFont = SelectObject(c.hdcWork, c.hFont);
    RECT tr{};
    DrawTextW(c.hdcWork, text, -1, &tr, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(c.hdcWork, oldFont);
    return tr.right - tr.left;
}

void LayoutActionBar(SnipCtx& c)
{
    c.bar.valid = false;
    if (!c.hdcWork)
        return;   // 字体创建失败时仍按最小宽度布局，保证工具栏可点

    const int pad    = BarScale(c, kBarPad);
    const int gap    = BarScale(c, kBarGap);
    const int btnH   = BarScale(c, kBarBtnH);
    const int margin = BarScale(c, kBarMargin);
    const int monR   = c.monX + c.monW;
    const int monB   = c.monY + c.monH;

    auto textW = [&](const wchar_t* s) { return MeasureBarText(c, s); };
    auto btnW  = [&](const wchar_t* s, int minW) {
        const int w = textW(s) + pad * 2;
        return w < minW ? minW : w;
    };

    const int wCopy  = btnW(L"复制",     BarScale(c, 56));
    const int wOcr   = btnW(L"文字识别", BarScale(c, 84));
    const int wShare = btnW(L"分享",     BarScale(c, 56));
    const int wClose = btnW(L"×",        BarScale(c, 36));
    const int barW   = pad * 2 + wCopy + gap + wOcr + gap + wShare + gap + wClose;
    const int barH   = pad * 2 + btnH;

    // 锚点：拖拽/窗口选区用选区本身；整屏截图用主显示器矩形
    RECT anchor{ c.monX, c.monY, c.monX + c.monW, c.monY + c.monH };
    if (!c.full)
        anchor = c.sel;

    int x = anchor.right - barW - margin;
    int y = anchor.bottom + margin;
    if (y + barH > monB - margin)
    {
        const int above = anchor.top - barH - margin;
        y = (above >= c.monY + margin) ? above : (monB - margin - barH);
    }
    if (y < c.monY + margin)
        y = c.monY + margin;
    const int minX = c.monX + margin;
    const int maxX = monR - margin - barW;
    // 左右都要夹：锚点取选区右边缘，靠左的小选区会把整个工具栏推到显示器外
    x = ClampI(x, minX, maxX < minX ? minX : maxX);

    c.bar.panel = RECT{ x, y, x + barW, y + barH };
    int bx = x + pad;
    c.bar.copy  = RECT{ bx, y + pad, bx + wCopy, y + pad + btnH }; bx += wCopy + gap;
    c.bar.ocr   = RECT{ bx, y + pad, bx + wOcr,  y + pad + btnH }; bx += wOcr + gap;
    c.bar.share = RECT{ bx, y + pad, bx + wShare, y + pad + btnH }; bx += wShare + gap;
    c.bar.close = RECT{ bx, y + pad, bx + wClose, y + pad + btnH };

    // “分享”子菜单：展开时与分享按钮左对齐，下方放不下就翻到上方
    const int wWx   = btnW(L"微信", BarScale(c, 52));
    const int wQq   = btnW(L"QQ",   BarScale(c, 52));
    const int subW  = pad * 2 + wWx + gap + wQq;
    const int subH  = pad * 2 + btnH;
    int sx = c.bar.share.left;
    if (sx + subW > monR - margin)
        sx = monR - margin - subW;
    if (sx < minX)
        sx = minX;
    int sy = c.bar.panel.bottom + gap;
    if (sy + subH > monB - margin)
        sy = c.bar.panel.top - gap - subH;
    if (sy < c.monY + margin)
        sy = c.monY + margin;
    c.bar.sharePanel = RECT{ sx, sy, sx + subW, sy + subH };
    c.bar.wechat = RECT{ sx + pad, sy + pad, sx + pad + wWx, sy + pad + btnH };
    c.bar.qq     = RECT{ sx + pad + wWx + gap, sy + pad,
                         sx + pad + wWx + gap + wQq, sy + pad + btnH };
    c.bar.valid = true;
}

void DrawRoundPanel(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr  = SelectObject(dc, br);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBr);
    DeleteObject(pen);
    DeleteObject(br);
}

void DrawBarButton(SnipCtx& c, HDC dc, const RECT& r, const wchar_t* text, bool hovered)
{
    DrawRoundPanel(dc, r, BarScale(c, 6),
                   hovered ? kAccent : kBarBtnBg,
                   hovered ? kAccent : RGB(58, 64, 78));
    HGDIOBJ oldFont = c.hFont ? SelectObject(dc, c.hFont) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 242, 246));
    RECT tr = r;
    DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont)
        SelectObject(dc, oldFont);
}

void DrawActionBar(SnipCtx& c, HDC dc)
{
    LayoutActionBar(c);
    if (!c.bar.valid)
        return;

    DrawRoundPanel(dc, c.bar.panel, BarScale(c, kBarRadius), kBarBg, kBarBorder);
    DrawBarButton(c, dc, c.bar.copy,  L"复制",     c.barHover == BarCopy);
    DrawBarButton(c, dc, c.bar.ocr,   L"文字识别", c.barHover == BarOcr);
    DrawBarButton(c, dc, c.bar.share, L"分享",     c.barHover == BarShare || c.shareOpen);
    DrawBarButton(c, dc, c.bar.close, L"×",        c.barHover == BarClose);

    if (c.shareOpen)
    {
        DrawRoundPanel(dc, c.bar.sharePanel, BarScale(c, kBarRadius), kBarBg, kBarBorder);
        DrawBarButton(c, dc, c.bar.wechat, L"微信", c.barHover == BarWeChat);
        DrawBarButton(c, dc, c.bar.qq,     L"QQ",   c.barHover == BarQQ);
    }
}

int HitActionBar(const SnipCtx& c, POINT pt)
{
    if (!c.bar.valid)
        return BarNone;
    if (PtInRect(&c.bar.copy, pt))  return BarCopy;
    if (PtInRect(&c.bar.ocr, pt))   return BarOcr;
    if (PtInRect(&c.bar.share, pt)) return BarShare;
    if (PtInRect(&c.bar.close, pt)) return BarClose;
    if (c.shareOpen)
    {
        if (PtInRect(&c.bar.wechat, pt)) return BarWeChat;
        if (PtInRect(&c.bar.qq, pt))     return BarQQ;
    }
    return BarNone;
}

bool UpdateBarHover(SnipCtx& c, POINT pt)
{
    const int hit = HitActionBar(c, pt);
    if (hit == c.barHover)
        return false;
    c.barHover = hit;
    return true;
}

void Compose(SnipCtx& c);   // 定义在下方；点击“分享”展开子菜单后需要立即重绘

// 结束一次拖拽/单击：确定选区，然后按是否启用快捷操作栏决定立即返回还是停在操作栏。
// 鼠标松开与（快捷操作栏模式下的）回车确认共用，避免两条入口出现行为差异。
void FinishSelection(SnipCtx& c)
{
    c.dragging = false;
    const int dx = c.ptNow.x - c.ptDown.x;
    const int dy = c.ptNow.y - c.ptDown.y;
    const bool tiny = (dx < 0 ? -dx : dx) <= kClickSlopPx &&
                      (dy < 0 ? -dy : dy) <= kClickSlopPx;
    if (tiny)
    {
        // Ctrl+单击 = 截整屏；否则截按下时高亮的窗口；无高亮窗口（桌面空白）也截整屏
        if (c.pickWindow && !c.ctrlDown && c.clickHadHover)
        {
            c.sel    = c.clickRect;
            c.hasSel = true;
            c.full   = false;
        }
        else
        {
            c.hasSel = false;
            c.full   = true;
        }
    }
    else
    {
        UpdateSel(c);
        c.hasSel = true;
        c.full   = false;
    }

    if (c.actionBar)
    {
        // 松手后保留遮罩与选区，改为显示快捷操作栏。
        // 整屏也把选区设为整个主显示器，这样画面恢复原亮度、工具栏有明确锚点。
        if (c.full)
        {
            c.sel = RECT{ c.monX, c.monY, c.monX + c.monW, c.monY + c.monH };
            c.hasSel = true;
        }
        c.confirmed = true;
        c.hasHover  = false;
        c.shareOpen = false;
        c.barHover  = BarNone;
        Compose(c);
    }
    else
    {
        c.done = true;
    }
}

void HandleBarClick(SnipCtx& c, int hit)
{
    switch (hit)
    {
    case BarCopy:
        c.action = SnipAction::Copy;
        c.done   = true;
        break;
    case BarOcr:
        c.action = SnipAction::Ocr;
        c.done   = true;
        break;
    case BarShare:
        c.shareOpen = !c.shareOpen;
        c.barHover  = BarShare;
        Compose(c);
        break;
    case BarWeChat:
        c.action = SnipAction::ShareWeChat;
        c.done   = true;
        break;
    case BarQQ:
        c.action = SnipAction::ShareQQ;
        c.done   = true;
        break;
    case BarClose:
        c.canceled = true;
        c.done     = true;
        break;
    default:
        break;
    }
}

void Compose(SnipCtx& c)
{
    HDC dc = c.hdcWork;

    // 1) 压暗后的整屏
    BitBlt(dc, 0, 0, c.winW, c.winH, c.hdcDim, 0, 0, SRCCOPY);

    if (c.hasSel)
    {
        const RECT& s = c.sel;

        // 2) 选区内还原原始亮度
        RestoreRegion(dc, c, s);

        // 3) 边框
        DrawAccentBorder(dc, s);

        // 4) 四角标记：仅靠 2px 边框在大屏上容易被忽略，角标提供第二重视觉线索
        HBRUSH solid = CreateSolidBrush(kAccent);
        const int hs = kHandlePx * c.dpi / 96 + 1;
        const RECT corners[4] = {
            RECT{ s.left, s.top, s.left + hs, s.top + hs },
            RECT{ s.right - hs, s.top, s.right, s.top + hs },
            RECT{ s.left, s.bottom - hs, s.left + hs, s.bottom },
            RECT{ s.right - hs, s.bottom - hs, s.right, s.bottom },
        };
        for (int i = 0; i < 4; ++i)
            FillRect(dc, &corners[i], solid);
        DeleteObject(solid);

        // 5) 尺寸标签（显示最终输出的像素尺寸）
        const RECT fr = FrameRectOf(c);
        wchar_t text[48];
        swprintf_s(text, L"%ld × %ld", fr.right - fr.left, fr.bottom - fr.top);
        int ly = s.top - 34 * c.dpi / 96;
        if (ly < c.monY + 4)
            ly = (s.bottom + 8 * c.dpi / 96);
        DrawTextBox(dc, c.hFont, text, s.left, ly, c);

        // 6) 放大镜：光标附近 8 倍像素，便于把边框对齐到窗口边缘。
        // 选区确认后不再需要，且快捷操作栏会占用光标附近的空间
        if (!c.confirmed)
        {
            const int scale = c.dpi / 96;
            const int magPx = 144 * (scale > 0 ? scale : 1);
            const int zoom = 8;
            const int srcPx = magPx / zoom > 0 ? magPx / zoom : 1;
            int fx = ToFrameX(c, c.ptNow.x);
            int fy = ToFrameY(c, c.ptNow.y);
            int sx = fx - srcPx / 2;
            int sy = fy - srcPx / 2;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx + srcPx > (int)c.frameW) sx = (int)c.frameW - srcPx;
            if (sy + srcPx > (int)c.frameH) sy = (int)c.frameH - srcPx;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            int dx = c.ptNow.x + 24 * (scale > 0 ? scale : 1);
            int dy = c.ptNow.y + 24 * (scale > 0 ? scale : 1);
            if (dx + magPx > c.monX + c.monW) dx = c.ptNow.x - magPx - 12;
            if (dy + magPx > c.monY + c.monH) dy = c.ptNow.y - magPx - 12;
            if (dx < c.monX) dx = c.monX;
            if (dy < c.monY) dy = c.monY;
            const int prevMode = SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, dx, dy, magPx, magPx,
                       c.hdcFrame, sx, sy, srcPx, srcPx, SRCCOPY);
            SetStretchBltMode(dc, prevMode);
            HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, dx, dy, dx + magPx, dy + magPx);
            MoveToEx(dc, dx + magPx / 2, dy, nullptr);
            LineTo(dc, dx + magPx / 2, dy + magPx);
            MoveToEx(dc, dx, dy + magPx / 2, nullptr);
            LineTo(dc, dx + magPx, dy + magPx / 2);
            SelectObject(dc, oldBr);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
    }
    else
    {
        // 2) 鼠标下的窗口：还原亮度 + 边框 + 尺寸标签（提示单击即可截取）
        if (c.hasHover)
        {
            const RECT& h = c.hover;
            RestoreRegion(dc, c, h);
            DrawAccentBorder(dc, h);

            const RECT fr = FrameRectOfRect(c, h);
            wchar_t text[64];
            swprintf_s(text, L"窗口 %ld × %ld", fr.right - fr.left, fr.bottom - fr.top);
            int ly = h.top - 34 * c.dpi / 96;
            if (ly < c.monY + 4)
                ly = h.bottom + 8 * c.dpi / 96;
            DrawTextBox(dc, c.hFont, text, h.left, ly, c);
        }

        // 尚未框选：顶部给出操作说明，避免用户面对全屏遮罩不知所措。
        // 提示画在有底图的主显示器顶部，多屏时才不会飘到没画面那一侧
        DrawTextBox(dc, c.hFont, c.pickWindow ? kHintText : kHintTextLegacy,
                    0, c.monY + kHintTop * c.dpi / 96, c, true);
    }

    // 选区确认后顶部操作说明不再走上面的 else 分支（hasSel = true），
    // 而操作栏只有按钮文字；补一条与当前键位对应的提示，避免用户卡在模态遮罩里
    if (c.confirmed && c.actionBar)
        DrawTextBox(dc, c.hFont, kHintTextConfirm,
                    0, c.monY + kHintTop * c.dpi / 96, c, true);

    // 7) 快捷操作栏：画在选区/整屏的最上层，命中测试与绘制使用同一份矩形
    if (c.confirmed && c.actionBar)
        DrawActionBar(c, dc);

    POINT ptSrc{ 0, 0 };
    SIZE  winSize{ c.winW, c.winH };
    BLENDFUNCTION bf{};
    // ULW_OPAQUE：忽略每像素 alpha（GDI 绘制不保证 alpha 字节），整窗不透明
    UpdateLayeredWindow(c.hwnd, nullptr, nullptr, &winSize, c.hdcWork, &ptSrc,
                        0, &bf, ULW_OPAQUE);
}

// 更新鼠标下的窗口高亮（pt 为遮罩客户区坐标）；返回状态是否变化，由调用方决定是否重绘
bool UpdateHover(SnipCtx& c, POINT pt)
{
    if (!c.pickWindow)
        return false;

    // 只有主显示器区域有底图，其他显示器不做窗口识别
    const RECT mon{ c.monX, c.monY, c.monX + c.monW, c.monY + c.monH };
    const POINT scrPt{ pt.x + c.originX, pt.y + c.originY };
    bool found = false;
    RECT hover{};
    if (PtInRect(&mon, pt))
    {
        // 鼠标仍落在上次命中的窗口内时复用缓存，省掉一次全量 EnumWindows；
        // 但必须沿 Z 序向上回溯，否则大窗口盖住小窗口时会一直复用大窗口。
        // 鼠标按下前会清空缓存，保证单击目标始终按按下时刻的 Z 序重新判定。
        const DWORD selfPid = GetCurrentProcessId();
        HWND hwnd = nullptr;
        RECT scr{};
        if (c.hoverHwnd && IsWindow(c.hoverHwnd) &&
            IsWindowVisible(c.hoverHwnd) && !IsIconic(c.hoverHwnd))
        {
            RECT hit{};
            if (GetWindowRect(c.hoverHwnd, &hit) && PtInRect(&hit, scrPt))
            {
                const HWND top = FindHigherPickableWindow(c.hoverHwnd, scrPt, selfPid);
                if (GetVisibleWindowRect(top, scr))
                    hwnd = top;
            }
        }
        if (!hwnd)
            hwnd = TopWindowAtPoint(scrPt, scr);
        c.hoverHwnd = hwnd;

        if (hwnd)
        {
            const RECT rc{ scr.left - c.originX, scr.top - c.originY,
                           scr.right - c.originX, scr.bottom - c.originY };
            // 窗口可能跨屏或超出显示器，夹到有底图的范围内
            if (IntersectRect(&hover, &rc, &mon) &&
                hover.right > hover.left && hover.bottom > hover.top)
                found = true;
        }
    }

    if (found == c.hasHover && (!found || EqualRect(&hover, &c.hover)))
        return false;
    c.hasHover = found;
    c.hover    = found ? hover : RECT{};
    return true;
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK SnipProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SnipCtx* c = g_ctx;
    if (!c)
        return DefWindowProcW(hWnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
        if (c->confirmed)
        {
            const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int hit = HitActionBar(*c, pt);
            if (hit != BarNone)
            {
                HandleBarClick(*c, hit);
            }
            else if (c->shareOpen)
            {
                // 点工具栏外面只收起分享子菜单，不取消截图（避免误触丢失结果）
                c->shareOpen = false;
                c->barHover  = BarNone;
                Compose(*c);
            }
            return 0;
        }
        SetCapture(hWnd);
        c->dragging         = true;
        c->hasSel           = false;
        c->movedBeyondSlop  = false;
        c->ctrlDown         = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        c->ptDown           = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        c->ptNow            = c->ptDown;
        // 按下瞬间强制重查一次 Z 序（丢弃悬停缓存），避免单击目标受旧缓存影响
        c->hoverHwnd = nullptr;
        if (UpdateHover(*c, c->ptDown))
            Compose(*c);
        c->clickHadHover = c->hasHover;
        c->clickRect     = c->hover;
        return 0;

    case WM_MOUSEMOVE:
    {
        const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (c->confirmed)
        {
            if (UpdateBarHover(*c, pt))
                Compose(*c);
            return 0;
        }
        if (c->dragging)
        {
            if (!c->movedBeyondSlop)
            {
                const int dx = pt.x - c->ptDown.x;
                const int dy = pt.y - c->ptDown.y;
                if ((dx < 0 ? -dx : dx) <= kClickSlopPx &&
                    (dy < 0 ? -dy : dy) <= kClickSlopPx)
                    return 0;   // 仍视为单击：窗口高亮保持不变
                // 超过阈值即切换为框选：隐藏窗口高亮，进入拖拽选区。
                // 注意它只影响拖动中的视觉状态，最终是否算单击仍看松开时的位移
                c->movedBeyondSlop = true;
                c->hasHover        = false;
            }
            c->ptNow = pt;
            UpdateSel(*c);
            c->hasSel = true;
            Compose(*c);
        }
        else
        {
            if (UpdateHover(*c, pt))
                Compose(*c);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        if (c->confirmed)
            return 0;
        if (!c->dragging)
            return 0;
        ReleaseCapture();
        c->ptNow = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // 最终结果只由「松开时的位移」决定：拖出去又拖回来仍算单击，
        // 否则会退化成 1~2px 的窄条（movedBeyondSlop 只负责拖动中的视觉切换）
        FinishSelection(*c);
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (c->dragging)
            ReleaseCapture();
        c->dragging = false;
        c->canceled = true;
        c->done = true;
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (c->dragging)
                ReleaseCapture();
            c->dragging = false;
            c->canceled = true;
            c->done = true;
            return 0;
        }
        if (c->confirmed)
        {
            // 操作栏显示后：回车 = 复制（最常用），方向键不再调整已确认的选区
            if (wParam == VK_RETURN)
            {
                c->action = SnipAction::Copy;
                c->done   = true;
            }
            return 0;
        }
        // 方向键微调选区角点（Shift 步进 10px），回车直接确认
        if (c->dragging || c->hasSel)
        {
            const int step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            bool moved = false;
            switch (wParam)
            {
            case VK_LEFT:  c->ptNow.x -= step; moved = true; break;
            case VK_RIGHT: c->ptNow.x += step; moved = true; break;
            case VK_UP:    c->ptNow.y -= step; moved = true; break;
            case VK_DOWN:  c->ptNow.y += step; moved = true; break;
            case VK_RETURN:
                if (c->actionBar)
                {
                    // 快捷操作栏模式：回车与松开鼠标同义，先确认选区再显示操作栏
                    if (c->dragging)
                        ReleaseCapture();
                    FinishSelection(*c);
                }
                else if (c->hasSel && !c->dragging)
                {
                    c->full = false;
                    c->done = true;
                }
                return 0;
            default: break;
            }
            if (moved)
            {
                UpdateSel(*c);
                c->hasSel = true;
                Compose(*c);
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == kEscTimerId)
        {
            // 兜底：遮罩失焦后（用户点了其他窗口）WM_KEYDOWN 收不到 Esc，
            // 遮罩会卡住只能右键退出。用异步按键状态轮询，保证任何焦点下都能取消。
            // 不用 WH_KEYBOARD_LL：那会全局拦截键盘，风险与权限要求都高得多。
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                if (c->dragging)
                    ReleaseCapture();
                c->dragging = false;
                c->canceled = true;
                c->done = true;
            }
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && c->confirmed)
        {
            POINT pt{};
            if (GetCursorPos(&pt))
            {
                ScreenToClient(hWnd, &pt);
                if (HitActionBar(*c, pt) != BarNone)
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;

    case WM_IME_SETCONTEXT:
    case WM_IME_NOTIFY:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        // 遮罩永不输入文字：直接吞掉，不建 IME 组字/候选窗口。
        // 豆包输入法 tsf-oime-core.dll 曾在本机窗口取焦点时崩溃（0xc0000005@0x1df48），
        // 解绑后可降低其介入概率（缓解，非根治；根治需输入法方修复或用户侧切换输入法）。
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        BeginPaint(hWnd, &ps);
        Compose(*c);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ReleaseGdi(SnipCtx& c)
{
    if (c.hFont)    { DeleteObject(c.hFont);    c.hFont = nullptr; }
    if (c.hdcWork)  { SelectObject(c.hdcWork, c.oldWork);  DeleteDC(c.hdcWork);  c.hdcWork = nullptr; }
    if (c.hdcDim)   { SelectObject(c.hdcDim, c.oldDim);    DeleteDC(c.hdcDim);   c.hdcDim = nullptr; }
    if (c.hdcFrame) { SelectObject(c.hdcFrame, c.oldFrame); DeleteDC(c.hdcFrame); c.hdcFrame = nullptr; }
    if (c.hbmWork)  { DeleteObject(c.hbmWork);  c.hbmWork = nullptr; }
    if (c.hbmDim)   { DeleteObject(c.hbmDim);   c.hbmDim = nullptr; }
    if (c.hbmFrame) { DeleteObject(c.hbmFrame); c.hbmFrame = nullptr; }
}

}   // namespace

// ---------------------------------------------------------------------------
// 对外入口
// ---------------------------------------------------------------------------
bool RunSnipping(HINSTANCE hinst, const ImageBGRA& frame, SnipSelection& out,
                 bool windowPick, bool actionBar)
{
    out = SnipSelection{};
    if (!frame.Valid())
    {
        LOG_ERR(L"框选: 底图无效, 无法进入框选");
        return false;
    }

    // 遮罩铺满整个虚拟屏，让所有显示器一起压暗（原先只盖主显示器，多屏时其他屏毫无变化，
    // 观感割裂）。底图仍来自与捕获模块同一台显示器（capture 用 MonitorFromPoint({0,0})）
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR hmon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(hmon, &mi))
        mi.rcMonitor = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                GetSystemMetrics(SM_CYSCREEN) };
    const RECT mon = mi.rcMonitor;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0)   // 取不到虚拟屏参数时退回主显示器：单显示器环境下完全等价
    {
        vx = mon.left;
        vy = mon.top;
        vw = mon.right - mon.left;
        vh = mon.bottom - mon.top;
    }
    if (vw <= 0 || vh <= 0)
    {
        LOG_ERR(L"框选: 显示器矩形无效 %dx%d", vw, vh);
        return false;
    }

    static bool clsReady = false;
    if (!clsReady)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = SnipProc;
        wc.hInstance     = hinst;
        wc.hCursor       = LoadCursor(nullptr, IDC_CROSS);
        wc.lpszClassName = kSnipClass;
        if (!RegisterClassExW(&wc))
        {
            LOG_ERR(L"框选: 注册窗口类失败(0x%08lX)", GetLastError());
            return false;
        }
        clsReady = true;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                kSnipClass, L"Capture - 框选截图",
                                WS_POPUP, vx, vy, vw, vh,
                                nullptr, nullptr, hinst, nullptr);
    if (!hwnd)
    {
        LOG_ERR(L"框选: 创建遮罩窗口失败(0x%08lX)", GetLastError());
        return false;
    }

    // 遮罩不需要任何文字输入：解绑 IME（IMM），避免第三方输入法在取焦点时介入。
    // 仅缓解：豆包 tsf-oime-core.dll 的崩溃发生在 TSF 侧，IMM 解绑不能保证掐断；
    // 线程级 ImmDisableTextFrameService 已实测不可用（本机恒返回 FALSE），故未采用。
    ImmAssociateContext(hwnd, nullptr);

    SnipCtx c;
    c.hwnd       = hwnd;
    c.winW       = vw;
    c.winH       = vh;
    c.originX    = vx;
    c.originY    = vy;
    c.frameW     = frame.w;
    c.frameH     = frame.h;
    c.pickWindow = windowPick;
    c.actionBar  = actionBar;
    // 主显示器矩形换算到窗口坐标系：单显示器时为 (0,0,winW,winH)，与原逻辑等价
    c.monX   = mon.left - vx;
    c.monY   = mon.top  - vy;
    c.monW   = mon.right - mon.left;
    c.monH   = mon.bottom - mon.top;

    HDC ref = GetDC(nullptr);
    c.dpi = GetDeviceCaps(ref, LOGPIXELSY);
    if (c.dpi < 96)
        c.dpi = 96;

    void* bits = nullptr;
    c.hbmFrame = CreateTopDownDib(ref, (int)frame.w, (int)frame.h, &bits);
    if (bits && frame.Stride() == (size_t)frame.w * 4)
        memcpy(bits, frame.pixels.data(), frame.pixels.size());

    void* bitsWork = nullptr;
    c.hbmDim  = CreateTopDownDib(ref, vw, vh, (void**)&c.dimBits);
    c.hbmWork = CreateTopDownDib(ref, vw, vh, &bitsWork);

    if (c.hbmFrame) { c.hdcFrame = CreateCompatibleDC(ref); c.oldFrame = SelectObject(c.hdcFrame, c.hbmFrame); }
    if (c.hbmDim)   { c.hdcDim   = CreateCompatibleDC(ref); c.oldDim   = SelectObject(c.hdcDim,   c.hbmDim); }
    if (c.hbmWork)  { c.hdcWork  = CreateCompatibleDC(ref); c.oldWork  = SelectObject(c.hdcWork,  c.hbmWork); }
    ReleaseDC(nullptr, ref);

    if (!c.hdcFrame || !c.hdcDim || !c.hdcWork)
    {
        LOG_ERR(L"框选: 创建位图/DC 失败");
        DestroyWindow(hwnd);
        ReleaseGdi(c);
        return false;
    }

    // 压暗底图只在初始化时算一次：先把帧拉伸到「主显示器矩形」（唯一有底图的区域），
    // 其余显示器所在的区域填同色深底，再只对有底图的区域逐通道乘系数。
    // 这样鼠标移动时只有两次 Blt，压暗这类逐像素运算不会跟着鼠标走。
    {
        HBRUSH bg = CreateSolidBrush(kOffscreenBg);
        RECT whole{ 0, 0, vw, vh };
        FillRect(c.hdcDim, &whole, bg);
        DeleteObject(bg);
    }
    SetStretchBltMode(c.hdcDim, COLORONCOLOR);
    StretchBlt(c.hdcDim, c.monX, c.monY, c.monW, c.monH,
               c.hdcFrame, 0, 0, (int)frame.w, (int)frame.h, SRCCOPY);
    if (c.dimBits)
    {
        const size_t stride = (size_t)c.winW * 4;
        const size_t xBegin = (size_t)c.monX * 4;
        const size_t xEnd   = std::min<size_t>((size_t)(c.monX + c.monW) * 4, stride);
        for (int y = c.monY; y < c.monY + c.monH; ++y)
        {
            BYTE* row = c.dimBits + (size_t)y * stride;
            for (size_t i = xBegin; i < xEnd; ++i)
                row[i] = (BYTE)(row[i] * kDimFactor);
        }
    }

    const int fontPx = -MulDiv(kFontPx, c.dpi, 96);
    c.hFont = CreateFontW(fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei");

    g_ctx = &c;
    // 遮罩一出现就识别光标下的窗口，用户不动鼠标也能看到高亮目标
    POINT cur{};
    if (GetCursorPos(&cur))
        UpdateHover(c, POINT{ cur.x - c.originX, cur.y - c.originY });
    Compose(c);   // UpdateHover 只更新状态，这里保证首帧一定推送
    ShowWindow(hwnd, SW_SHOW);
    // Esc 兜底轮询：遮罩失焦后 WM_KEYDOWN 收不到 Esc，靠定时器里的异步按键状态取消
    SetTimer(hwnd, kEscTimerId, kEscTimerMs, nullptr);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    LOG_INFO(L"框选: 遮罩已显示 %dx%d (底图 %ux%u)", vw, vh, frame.w, frame.h);

    // ---- 模态消息循环 ----
    // 只处理遮罩窗口自身的消息：主窗口已隐藏，且要避免嵌套分发主窗口的
    // 热键/托盘消息导致重入（WM_QUIT 必须放行，否则退出会被吞掉）
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

    KillTimer(hwnd, kEscTimerId);
    DestroyWindow(hwnd);
    g_ctx = nullptr;
    ReleaseGdi(c);

    if (c.canceled)
    {
        LOG_INFO(L"框选: 用户取消");
        return false;
    }

    out.ok = true;
    out.fullScreen = c.full;
    out.action = c.action;
    if (c.full)
        out.px = RECT{ 0, 0, (LONG)c.frameW, (LONG)c.frameH };
    else
        out.px = FrameRectOf(c);

    LOG_INFO(L"框选: 结果 %ld,%ld %ldx%ld (整屏=%d)",
             out.px.left, out.px.top, out.px.right - out.px.left,
             out.px.bottom - out.px.top, c.full ? 1 : 0);
    return true;
}
