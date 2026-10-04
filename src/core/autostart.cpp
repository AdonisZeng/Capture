#include "autostart.h"
#include "core/log.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace {

constexpr const wchar_t* kRunKey    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kValueName = L"Capture";

// 当前进程 exe 完整路径（GetModuleFileNameW 截断时翻倍缓冲重试）
std::wstring ExePath()
{
    std::wstring exe(MAX_PATH, L'\0');
    DWORD n = 0;
    while ((n = GetModuleFileNameW(nullptr, &exe[0], (DWORD)exe.size())) == exe.size() &&
           exe.size() < 32768)
        exe.resize(exe.size() * 2);
    exe.resize(n);
    return exe;
}

// 注册表里存的启动命令：带引号包裹，路径含空格也能被 shell 正确解析
std::wstring AutostartCommand()
{
    return L"\"" + ExePath() + L"\"";
}

// 忽略大小写的路径比较（仅 ASCII 字母折叠，宽字符直接比较）
bool PathEqualI(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        wchar_t ca = a[i], cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca += L'a' - L'A';
        if (cb >= L'A' && cb <= L'Z') cb += L'a' - L'A';
        if (ca != cb)
            return false;
    }
    return true;
}

// 读取 Run 键下的自启动值；不存在或非字符串类型返回 false
bool ReadRunValue(std::wstring& out)
{
    HKEY key = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key);
    if (st != ERROR_SUCCESS)
        return false;

    // 先探大小再按需分配，避免固定缓冲截断超长路径
    DWORD size = 0, type = 0;
    st = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
    if (st != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0 || size > 4096)
    {
        RegCloseKey(key);
        return false;
    }
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    st = RegQueryValueExW(key, kValueName, nullptr, &type, (LPBYTE)&buf[0], &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS)
        return false;
    // RegQueryValueExW 不保证结尾 NUL，按返回字节数收敛
    buf.resize(wcsnlen(buf.c_str(), size / sizeof(wchar_t)));
    out = buf;
    return true;
}

}   // namespace

bool AutostartIsEnabled()
{
    std::wstring cmd;
    if (!ReadRunValue(cmd))
        return false;
    // 去掉两侧引号后与当前 exe 忽略大小写比较，识别 exe 位置变化
    if (cmd.size() >= 2 && cmd.front() == L'"' && cmd.back() == L'"')
        cmd = cmd.substr(1, cmd.size() - 2);
    return PathEqualI(cmd, ExePath());
}

bool AutostartEnable()
{
    HKEY key = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (st != ERROR_SUCCESS)
    {
        LOG_ERR(L"打开自启动注册表键失败(0x%08lX)", st);
        return false;
    }
    std::wstring cmd = AutostartCommand();
    st = RegSetValueExW(key, kValueName, 0, REG_SZ,
                        (const BYTE*)cmd.c_str(),
                        (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (st != ERROR_SUCCESS)
    {
        LOG_ERR(L"写入自启动注册表值失败(0x%08lX)", st);
        return false;
    }
    LOG_INFO(L"已启用开机自启动: %s", cmd.c_str());
    return true;
}

bool AutostartDisable()
{
    HKEY key = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (st == ERROR_FILE_NOT_FOUND)
        return true;   // Run 键不存在 = 本来就没有自启动项
    if (st != ERROR_SUCCESS)
    {
        LOG_ERR(L"打开自启动注册表键失败(0x%08lX)", st);
        return false;
    }
    st = RegDeleteValueW(key, kValueName);
    RegCloseKey(key);
    if (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND)
    {
        LOG_INFO(L"已禁用开机自启动");
        return true;
    }
    LOG_ERR(L"删除自启动注册表值失败(0x%08lX)", st);
    return false;
}
