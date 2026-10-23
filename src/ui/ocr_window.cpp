#include "ocr_window.h"
#include "core/log.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <cstring>
#include <cwchar>

namespace
{

const wchar_t kOcrWndClass[] = L"CaptureOcrResultClass";
constexpr int kIdEdit  = 1001;
constexpr int kIdCopy  = 1002;
constexpr int kIdClose = 1003;
constexpr UINT_PTR kCopyFeedbackTimer = 1;

HWND   g_hwnd = nullptr;
HWND   g_edit = nullptr;
HWND   g_copyBtn = nullptr;
HWND   g_closeBtn = nullptr;
HFONT  g_font = nullptr;
HBRUSH g_bgBrush = nullptr;
int    g_dpi = 96;

LRESULT CALLBACK EditKeyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                             UINT_PTR, DWORD_PTR);

LRESULT CALLBACK ResultProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool EnsureClass()
{
    static bool ready = []() {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc   = ResultProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kOcrWndClass;
        return RegisterClassExW(&wc) != 0;
    }();
    return ready;
}

int Scale(int v)
{
    return MulDiv(v, g_dpi, 96);
}

void Layout(int w, int h)
{
    if (!g_edit || !g_copyBtn || !g_closeBtn)
        return;

    const int margin = Scale(14);
    const int gap    = Scale(10);
    const int btnH   = Scale(32);
    const int btnW   = Scale(104);

    int editH = h - margin * 2 - btnH - gap;
    if (editH < Scale(60))
        editH = Scale(60);
    MoveWindow(g_edit, margin, margin, w - margin * 2, editH, TRUE);

    const int by = h - margin - btnH;
    MoveWindow(g_closeBtn, w - margin - btnW, by, btnW, btnH, TRUE);
    MoveWindow(g_copyBtn, w - margin - btnW * 2 - gap, by, btnW, btnH, TRUE);
}

void ApplyFont()
{
    if (!g_font)
        return;
    if (g_edit)     SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_copyBtn)  SendMessageW(g_copyBtn, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_closeBtn) SendMessageW(g_closeBtn, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_edit)
        SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELONG(Scale(8), Scale(8)));
}

void RebuildFont()
{
    HFONT next = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei");
    if (!next)
        return;
    HFONT old = g_font;
    g_font = next;
    ApplyFont();
    if (old)
        DeleteObject(old);
}

bool CopyTextToClipboard(const std::wstring& text)
{
    if (text.empty())
        return false;
    if (!OpenClipboard(g_hwnd))
        return false;

    bool ok = false;
    if (EmptyClipboard())
    {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem)
        {
            void* dst = GlobalLock(mem);
            if (dst)
            {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                if (SetClipboardData(CF_UNICODETEXT, mem))
                    ok = true;
                else
                    GlobalFree(mem);
            }
            else
            {
                GlobalFree(mem);
            }
        }
    }
    CloseClipboard();
    return ok;
}

LRESULT CALLBACK EditKeyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                             UINT_PTR, DWORD_PTR)
{
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        if (IsWindow(g_hwnd))
            DestroyWindow(g_hwnd);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ResultProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DPICHANGED:
    {
        const UINT dpi = HIWORD(wParam);
        if (dpi >= 96)
            g_dpi = (int)dpi;
        if (g_hwnd)
            RebuildFont();
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested)
        {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        Layout(rc.right, rc.bottom);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_SIZE:
        Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_ERASEBKGND:
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_bgBrush ? g_bgBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    {
        HDC dc = (HDC)wParam;
        SetTextColor(dc, RGB(232, 234, 238));
        SetBkColor(dc, RGB(30, 33, 40));
        return (LRESULT)(g_bgBrush ? g_bgBrush : (HBRUSH)GetStockObject(BLACK_BRUSH));
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kIdCopy:
        {
            const int len = GetWindowTextLengthW(g_edit);
            std::wstring text((size_t)len + 1, L'\0');
            GetWindowTextW(g_edit, text.data(), len + 1);
            text.resize((size_t)len);
            if (CopyTextToClipboard(text))
            {
                SetWindowTextW(g_copyBtn, L"已复制");
                SetTimer(hwnd, kCopyFeedbackTimer, 1500, nullptr);
            }
            else
            {
                MessageBoxW(hwnd, L"复制到剪贴板失败", L"Capture", MB_ICONWARNING);
            }
            return 0;
        }
        case kIdClose:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_TIMER:
        if (wParam == kCopyFeedbackTimer)
        {
            KillTimer(hwnd, kCopyFeedbackTimer);
            if (IsWindow(g_copyBtn))
                SetWindowTextW(g_copyBtn, L"复制文字");
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kCopyFeedbackTimer);
        if (g_edit)
            RemoveWindowSubclass(g_edit, EditKeyProc, 1);
        g_hwnd = nullptr;
        g_edit = nullptr;
        g_copyBtn = nullptr;
        g_closeBtn = nullptr;
        return 0;

    case WM_NCDESTROY:
        // 子控件已全部销毁后再释放字体/画刷，避免它们仍被控件引用
        if (g_font)
        {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        if (g_bgBrush)
        {
            DeleteObject(g_bgBrush);
            g_bgBrush = nullptr;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}   // namespace

void SetOcrResultWindowText(const std::wstring& text, bool busy, bool copyEnabled)
{
    if (!IsWindow(g_hwnd) || !IsWindow(g_edit))
        return;
    SetWindowTextW(g_edit, text.c_str());
    EnableWindow(g_copyBtn, copyEnabled && !busy);
    if (!busy)
        SetWindowTextW(g_copyBtn, L"复制文字");
}

void ShowOcrResultWindow(HWND owner, const std::wstring& text, bool busy, bool copyEnabled)
{
    (void)owner;

    if (IsWindow(g_hwnd))
    {
        SetOcrResultWindowText(text, busy, copyEnabled);
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        return;
    }

    if (!EnsureClass())
    {
        LOG_ERR(L"文字识别: 注册结果窗类失败(0x%08lX)", GetLastError());
        return;
    }
    if (!g_bgBrush)
        g_bgBrush = CreateSolidBrush(RGB(24, 27, 34));

    HDC ref = GetDC(nullptr);
    g_dpi = GetDeviceCaps(ref, LOGPIXELSY);
    ReleaseDC(nullptr, ref);
    if (g_dpi < 96)
        g_dpi = 96;

    int winW = Scale(560);
    int winH = Scale(420);

    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi))
    {
        mi.rcWork = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - winW) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - winH) / 2;

    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOcrWndClass,
                             L"文字识别", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX,
                             x, y, winW, winH, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!g_hwnd)
    {
        LOG_ERR(L"文字识别: 创建结果窗失败(0x%08lX)", GetLastError());
        return;
    }

    // 系统 DPI 只用于初始尺寸；窗口实际落在哪个显示器，以 GetDpiForWindow 为准，
    // 否则混合缩放的多屏上会按错误的 DPI 建字号与控件尺寸
    const UINT actualDpi = GetDpiForWindow(g_hwnd);
    if (actualDpi >= 96)
    {
        g_dpi = (int)actualDpi;
        winW = Scale(560);
        winH = Scale(420);
        const int nx = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - winW) / 2;
        const int ny = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - winH) / 2;
        SetWindowPos(g_hwnd, nullptr, nx, ny, winW, winH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Win10/11 深色标题栏（旧版本不支持时忽略返回值）
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                          &dark, sizeof(dark));

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    g_edit = CreateWindowExW(0, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                             ES_READONLY | ES_AUTOVSCROLL | ES_LEFT,
                             0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)kIdEdit, hinst, nullptr);
    g_copyBtn = CreateWindowExW(0, L"BUTTON", L"复制文字",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)kIdCopy, hinst, nullptr);
    g_closeBtn = CreateWindowExW(0, L"BUTTON", L"关闭",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)kIdClose, hinst, nullptr);

    RebuildFont();
    SetWindowSubclass(g_edit, EditKeyProc, 1, 0);

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    Layout(rc.right, rc.bottom);

    SetOcrResultWindowText(text, busy, copyEnabled);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    LOG_INFO(L"文字识别: 结果窗已显示");
}

bool OcrResultWindowVisible()
{
    return IsWindow(g_hwnd) && IsWindowVisible(g_hwnd);
}

void CloseOcrResultWindow()
{
    if (IsWindow(g_hwnd))
        DestroyWindow(g_hwnd);
}
