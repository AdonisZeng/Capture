#include "share.h"
#include "core/log.h"
#include <shellapi.h>
#include <string>
#include <cwctype>

namespace
{

std::wstring LowerBaseName(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
    for (wchar_t& ch : name)
        ch = (wchar_t)towlower(ch);
    return name;
}

// 进程可执行文件名是否属于目标应用（WeChat 4.x 的主进程名有 WeChat.exe / Weixin.exe 两种）
bool ProcessMatches(HWND hwnd, bool weChat)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId())
        return false;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return false;

    wchar_t path[1024] = {};
    DWORD size = (DWORD)std::size(path);
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &size);
    CloseHandle(proc);
    if (!ok)
        return false;

    const std::wstring exe = LowerBaseName(path);
    if (weChat)
        return exe == L"wechat.exe" || exe == L"weixin.exe";
    return exe == L"qq.exe";
}

struct AppWindowSearch
{
    bool   weChat = false;
    HWND   normal = nullptr;    // 可见且未最小化
    HWND   minimized = nullptr; // 可见但最小化
};

BOOL CALLBACK FindAppWindowProc(HWND hwnd, LPARAM lp)
{
    AppWindowSearch* s = reinterpret_cast<AppWindowSearch*>(lp);
    if (!IsWindowVisible(hwnd) || !ProcessMatches(hwnd, s->weChat))
        return TRUE;
    if (IsIconic(hwnd))
    {
        if (!s->minimized)
            s->minimized = hwnd;
    }
    else
    {
        s->normal = hwnd;
        return FALSE;   // 优先普通可见窗口，找到即停
    }
    return TRUE;
}

HWND FindAppWindow(bool weChat)
{
    AppWindowSearch s;
    s.weChat = weChat;
    EnumWindows(FindAppWindowProc, reinterpret_cast<LPARAM>(&s));
    return s.normal ? s.normal : s.minimized;
}

void BringToForeground(HWND hwnd)
{
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    else
        ShowWindow(hwnd, SW_SHOW);

    if (GetForegroundWindow() != hwnd && !SetForegroundWindow(hwnd))
    {
        // 前台锁（用户刚点过别的窗口）下 SetForegroundWindow 可能失败；
        // SwitchToThisWindow 是系统提供的兜底切换方式，不注入按键、不改焦点链
        SwitchToThisWindow(hwnd, TRUE);
    }
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

// 从注册表 App Paths 读安装路径（用户级优先，再机器级；32/64 视图都试）
bool ReadAppPath(const wchar_t* exeName, std::wstring& out)
{
    wchar_t sub[512];
    swprintf_s(sub, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s", exeName);

    const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    const DWORD views[] = { RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY };
    for (HKEY root : roots)
    {
        for (DWORD view : views)
        {
            wchar_t value[1024] = {};
            DWORD size = sizeof(value);
            DWORD type = 0;
            // App Paths 也可能是 REG_EXPAND_SZ（路径里带环境变量），
            // 只接受 REG_SZ 会把已安装的应用误判为未安装
            const LSTATUS st = RegGetValueW(root, sub, nullptr,
                                            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view,
                                            &type, value, &size);
            if (st == ERROR_SUCCESS && value[0])
            {
                if (type == REG_EXPAND_SZ)
                {
                    wchar_t expanded[1024] = {};
                    const DWORD n = ExpandEnvironmentStringsW(value, expanded,
                                                              (DWORD)std::size(expanded));
                    out = (n > 0 && n <= std::size(expanded)) ? expanded : value;
                }
                else
                {
                    out = value;
                }
                return true;
            }
        }
    }
    return false;
}

bool LaunchApp(const wchar_t* exeName, const wchar_t* fallbackProtocol)
{
    std::wstring path;
    if (ReadAppPath(exeName, path))
    {
        const HINSTANCE h = ShellExecuteW(nullptr, L"open", path.c_str(),
                                          nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)h > 32)
        {
            LOG_INFO(L"分享: 已启动 %s (%s)", exeName, path.c_str());
            return true;
        }
        LOG_WARN(L"分享: 启动 %s 失败(%lld)", exeName, (long long)(INT_PTR)h);
    }

    const HINSTANCE h = ShellExecuteW(nullptr, L"open", fallbackProtocol,
                                      nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h > 32)
    {
        LOG_INFO(L"分享: 已通过协议拉起 %s", fallbackProtocol);
        return true;
    }
    LOG_WARN(L"分享: 协议拉起 %s 失败(%lld)", fallbackProtocol, (long long)(INT_PTR)h);
    return false;
}

}   // namespace

bool ActivateShareApp(ShareApp app)
{
    const bool weChat = (app == ShareApp::WeChat);
    const wchar_t* name = weChat ? L"微信" : L"QQ";

    if (HWND hwnd = FindAppWindow(weChat))
    {
        BringToForeground(hwnd);
        LOG_INFO(L"分享: 已唤起%s窗口", name);
        return true;
    }

    // 微信 4.x 的主程序名有 WeChat.exe / Weixin.exe 两种，App Paths 只注册其中一个
    bool launched = false;
    if (weChat)
        launched = LaunchApp(L"WeChat.exe", L"weixin://") ||
                   LaunchApp(L"Weixin.exe", L"weixin://");
    else
        launched = LaunchApp(L"QQ.exe", L"tencent://");
    if (!launched)
        LOG_WARN(L"分享: 未能唤起%s", name);
    return launched;
}
