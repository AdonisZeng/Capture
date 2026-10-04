#include "trayicon.h"
#include "core/log.h"
#include "core/appicon.h"
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <cmath>
#include <cstring>

namespace {

constexpr UINT IDM_SHOW     = 40001;
constexpr UINT IDM_CAPTURE  = 40002;
constexpr UINT IDM_RECORD   = 40003;
constexpr UINT IDM_PAUSE    = 40006;
constexpr UINT IDM_OPENDIR  = 40007;
constexpr UINT IDM_SETTINGS = 40004;
constexpr UINT IDM_QUIT     = 40005;
constexpr UINT IDM_CHECKUPD = 40008;

// 兜底图标（仅在 exe 资源中的应用图标加载失败时使用）
// 32bpp BGRA 图标：录制态为红色实心圆 + 白心，空闲态为灰色圆环
HICON BuildFallbackIcon(UINT cx, UINT cy, bool recording)
{
    size_t pitch = (size_t)cx * 4;
    std::vector<BYTE> pixels(pitch * cy, 0);

    float fcx = cx / 2.0f, fcy = cy / 2.0f;
    float outer = cx * 0.46f;
    float inner = recording ? 0.0f : outer * 0.60f;   // 空闲态圆环内径
    float dot   = cx * 0.14f;                          // 录制态中心白点

    for (int y = 0; y < (int)cy; ++y)
    {
        for (int x = 0; x < (int)cx; ++x)
        {
            float dx = x + 0.5f - fcx, dy = y + 0.5f - fcy;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > outer)
                continue;

            BYTE r = recording ? 235 : 150;
            BYTE g = recording ? 64 : 158;
            BYTE b = recording ? 64 : 170;
            if (recording && d <= dot)
            {
                r = 255; g = 255; b = 255;
            }
            BYTE* p = &pixels[(size_t)y * pitch + (size_t)x * 4];
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
        }
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)cx;
    bi.bmiHeader.biHeight = -(LONG)cy;   // 负值 = 自上而下（ICO 期望方向）
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HICON icon = nullptr;
    if (color && bits)
    {
        memcpy(bits, pixels.data(), pixels.size());
        HBITMAP mask = CreateBitmap((int)cx, (int)cy, 1, 1, nullptr);
        ICONINFO ii = {};
        ii.fIcon = TRUE;
        ii.hbmColor = color;
        ii.hbmMask = mask;
        icon = CreateIconIndirect(&ii);
        if (mask)
            DeleteObject(mask);
        DeleteObject(color);
    }
    else if (color)
    {
        DeleteObject(color);
    }
    DeleteDC(dc);
    return icon;
}

}   // namespace

TrayIcon::~TrayIcon()
{
    Destroy();
}

HICON TrayIcon::MakeIcon(UINT cx, UINT cy, bool recording) const
{
    // 与任务栏/标题栏/界面左上角保持同一套图标：优先取 exe 资源里的应用图标，
    // 录制态在右下角叠加红色徽标；资源加载失败才回退到程序内绘制的圆形图标。
    HICON icon = recording ? AppIcon::LoadRecording(cx, cy) : AppIcon::Load(cx, cy);
    if (icon)
        return icon;
    return BuildFallbackIcon(cx, cy, recording);
}

bool TrayIcon::Create(HWND hwnd)
{
    hwnd_ = hwnd;
    UINT cx = GetSystemMetrics(SM_CXSMICON);
    UINT cy = GetSystemMetrics(SM_CYSMICON);
    if (cx == 0) cx = 16;
    if (cy == 0) cy = 16;
    icon_ = MakeIcon(cx, cy, false);
    iconRecording_ = MakeIcon(cx, cy, true);
    if (!icon_)
    {
        LOG_ERR(L"托盘图标创建失败");
        return false;
    }
    tip_ = L"Capture - 就绪";

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = msgId_;
    nid.hIcon = icon_;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        LOG_ERR(L"Shell_NotifyIcon(NIM_ADD) 失败(0x%08lX)", GetLastError());
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    added_ = true;
    LOG_INFO(L"托盘图标已创建 (%ux%u)", cx, cy);
    return true;
}

void TrayIcon::Destroy()
{
    if (hwnd_ && added_)
    {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
    if (iconRecording_) { DestroyIcon(iconRecording_); iconRecording_ = nullptr; }
    hwnd_ = nullptr;
}

void TrayIcon::Update(const std::wstring& tooltip, bool recording)
{
    if (!hwnd_ || !added_)
        return;
    tip_ = tooltip;
    if (tip_.size() >= 128)
        tip_ = tip_.substr(0, 127);

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = (recording && iconRecording_) ? iconRecording_ : icon_;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    if (recording != recording_)
    {
        recording_ = recording;
        LOG_INFO(L"托盘图标切换为%s", recording ? L"录制中" : L"空闲");
    }
}

void TrayIcon::Notify(const std::wstring& title, const std::wstring& msg)
{
    if (!hwnd_ || !added_)
        return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, msg.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
        LOG_WARN(L"托盘气泡通知失败(0x%08lX)", GetLastError());
}

void TrayIcon::ShowContextMenu()
{
    if (!hwnd_)
        return;
    POINT pt{};
    GetCursorPos(&pt);
    // 必须先置前台，否则菜单无法正确关闭
    SetForegroundWindow(hwnd_);

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, IDM_SHOW,     L"显示主窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CAPTURE,  L"截图");
    AppendMenuW(menu, MF_STRING, IDM_RECORD,   L"开始 / 停止录制");
    AppendMenuW(menu, MF_STRING, IDM_PAUSE, paused_ ? L"继续录制" : L"暂停录制");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPENDIR,  L"打开输出目录");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(menu, MF_STRING, IDM_CHECKUPD, L"检查更新…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT,     L"退出");

    UINT cmd = (UINT)TrackPopupMenuEx(menu,
                                     TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);

    switch (cmd)
    {
    case IDM_SHOW:     PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::Show, 0);     break;
    case IDM_CAPTURE:  PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::Capture, 0);  break;
    case IDM_RECORD:   PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::Record, 0);   break;
    case IDM_PAUSE:    PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::Pause, 0);    break;
    case IDM_OPENDIR:  PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::OpenDir, 0);  break;
    case IDM_SETTINGS: PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::Settings, 0); break;
    case IDM_CHECKUPD: PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::CheckUpdate, 0); break;
    case IDM_QUIT:     PostMessageW(hwnd_, msgId_, (WPARAM)TrayAction::Quit, 0);     break;
    default: break;
    }
}

bool TrayIcon::OnMessage(WPARAM wParam, LPARAM lParam, TrayAction& action)
{
    action = TrayAction::None;
    if (!hwnd_)
        return false;

    // 菜单选择结果或主动投递的动作。
    // 上界必须跟着 TrayAction 的最后一个成员走（见 trayicon.h 里的说明）：
    // 用区间判断而不是 switch，新增成员忘了改这里就会变成「点了没反应」
    if (wParam >= (WPARAM)TrayAction::Show && wParam <= (WPARAM)TrayAction::CheckUpdate)
    {
        action = (TrayAction)wParam;
        return true;
    }

    // 鼠标事件（NOTIFYICON_VERSION_4：LOWORD(lParam) 为事件码，HIWORD 为图标 ID）
    switch ((UINT)LOWORD(lParam))
    {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        action = TrayAction::Show;
        return true;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowContextMenu();
        return true;
    default:
        return false;
    }
}
