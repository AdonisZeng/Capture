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
const wchar_t   kHintText[]  = L"按住左键拖出截图区域 · 单击截取整个屏幕 · Esc 取消";

// Esc 兜底轮询定时器：遮罩失焦后 WM_KEYDOWN 收不到 Esc，靠它保证任何焦点状态下都能取消
constexpr UINT_PTR kEscTimerId = 1;
constexpr UINT     kEscTimerMs = 50;
// 非主显示器没有底图（App 只捕获主显示器），其区域填这个色，观感与压暗一致
constexpr COLORREF kOffscreenBg = RGB(14, 16, 20);

constexpr COLORREF kAccent  = RGB(52, 102, 252);   // 与 UI 主题 Accent 同色
constexpr COLORREF kLabelBg = RGB(16, 18, 22);

struct SnipCtx
{
    HWND  hwnd = nullptr;
    int   winW = 0, winH = 0;      // 遮罩窗口尺寸（覆盖整个虚拟屏的物理像素）
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

    bool  dragging = false;
    bool  hasSel   = false;
    POINT ptDown{}, ptNow{};
    RECT  sel{};

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

// 选区转成帧像素矩形，并保证至少 1x1（缩放取整可能把宽度压成 0）
RECT FrameRectOf(const SnipCtx& c)
{
    RECT r{};
    r.left   = ToFrameX(c, c.sel.left);
    r.top    = ToFrameY(c, c.sel.top);
    r.right  = ToFrameX(c, c.sel.right);
    r.bottom = ToFrameY(c, c.sel.bottom);
    if (r.right  <= r.left)   r.right  = (r.left   + 1 <= (int)c.frameW) ? r.left   + 1 : (int)c.frameW;
    if (r.bottom <= r.top)    r.bottom = (r.top    + 1 <= (int)c.frameH) ? r.top    + 1 : (int)c.frameH;
    if (r.left   >= r.right)  r.left   = r.right - 1;
    if (r.top    >= r.bottom) r.top    = r.bottom - 1;
    return r;
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

void Compose(SnipCtx& c)
{
    HDC dc = c.hdcWork;

    // 1) 压暗后的整屏
    BitBlt(dc, 0, 0, c.winW, c.winH, c.hdcDim, 0, 0, SRCCOPY);

    if (c.hasSel)
    {
        const RECT& s = c.sel;
        const int dw = s.right - s.left;
        const int dh = s.bottom - s.top;
        if (dw > 0 && dh > 0)
        {
            // 2) 选区内还原原始亮度：按同一比例回到帧坐标取样，避免缩放累积偏差
            const int fx = ToFrameX(c, s.left);
            const int fy = ToFrameY(c, s.top);
            int fw = ToFrameX(c, s.right) - fx;
            int fh = ToFrameY(c, s.bottom) - fy;
            if (fw < 1) fw = 1;
            if (fh < 1) fh = 1;
            StretchBlt(dc, s.left, s.top, dw, dh, c.hdcFrame, fx, fy, fw, fh, SRCCOPY);
        }

        // 3) 边框
        HBRUSH hollow = (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN pen = CreatePen(PS_SOLID, kBorderPx, kAccent);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBr = SelectObject(dc, hollow);
        Rectangle(dc, s.left, s.top, s.right, s.bottom);
        SelectObject(dc, oldBr);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

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
    }
    else
    {
        // 尚未框选：顶部给出操作说明，避免用户面对全屏遮罩不知所措。
        // 提示画在有底图的主显示器顶部，多屏时才不会飘到没画面那一侧
        DrawTextBox(dc, c.hFont, kHintText, 0, c.monY + kHintTop * c.dpi / 96, c, true);
    }

    POINT ptSrc{ 0, 0 };
    SIZE  winSize{ c.winW, c.winH };
    BLENDFUNCTION bf{};
    // ULW_OPAQUE：忽略每像素 alpha（GDI 绘制不保证 alpha 字节），整窗不透明
    UpdateLayeredWindow(c.hwnd, nullptr, nullptr, &winSize, c.hdcWork, &ptSrc,
                        0, &bf, ULW_OPAQUE);
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
        SetCapture(hWnd);
        c->dragging = true;
        c->hasSel   = false;
        c->ptDown   = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        c->ptNow    = c->ptDown;
        Compose(*c);
        return 0;

    case WM_MOUSEMOVE:
        if (c->dragging)
        {
            c->ptNow = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            UpdateSel(*c);
            c->hasSel = true;
            Compose(*c);
        }
        return 0;

    case WM_LBUTTONUP:
        if (!c->dragging)
            return 0;
        ReleaseCapture();
        c->dragging = false;
        c->ptNow = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        UpdateSel(*c);
        {
            const int dx = c->ptNow.x - c->ptDown.x;
            const int dy = c->ptNow.y - c->ptDown.y;
            const bool tiny = (dx < 0 ? -dx : dx) <= kClickSlopPx &&
                              (dy < 0 ? -dy : dy) <= kClickSlopPx;
            c->full   = tiny;    // 单击 = 整屏
            c->hasSel = !tiny;
        }
        c->done = true;
        return 0;

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
bool RunSnipping(HINSTANCE hinst, const ImageBGRA& frame, SnipSelection& out)
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

    SnipCtx c;
    c.hwnd   = hwnd;
    c.winW   = vw;
    c.winH   = vh;
    c.frameW = frame.w;
    c.frameH = frame.h;
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
    Compose(c);
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
    if (c.full)
        out.px = RECT{ 0, 0, (LONG)c.frameW, (LONG)c.frameH };
    else
        out.px = FrameRectOf(c);

    LOG_INFO(L"框选: 结果 %ld,%ld %ldx%ld (整屏=%d)",
             out.px.left, out.px.top, out.px.right - out.px.left,
             out.px.bottom - out.px.top, c.full ? 1 : 0);
    return true;
}
