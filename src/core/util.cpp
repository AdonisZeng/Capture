#include "util.h"
#include <windows.h>
#include <shlobj_core.h>
#include <shellapi.h>
#include <cstdio>

// ---------------------------------------------------------------------------
// 编码转换
// ---------------------------------------------------------------------------
std::string WideToUtf8(const wchar_t* s)
{
    std::string u;
    if (!s || !*s) return u;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n > 0)
    {
        u.resize(n - 1);
        WideCharToMultiByte(CP_UTF8, 0, s, -1, u.data(), n, nullptr, nullptr);
    }
    return u;
}

std::wstring Utf8ToWide(const char* s)
{
    std::wstring w;
    if (!s || !*s) return w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n > 0)
    {
        w.resize(n - 1);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    }
    return w;
}

std::string WideToUtf8Str(const std::wstring& s)
{
    return WideToUtf8(s.c_str());
}

// ---------------------------------------------------------------------------
// 目录
// ---------------------------------------------------------------------------
std::wstring ExeDir()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : dir.substr(0, pos);
}

std::wstring FindDataDir(const wchar_t* name)
{
    std::wstring base = ExeDir();
    std::wstring dir = base;
    for (int i = 0; i < 5; ++i)
    {
        std::wstring candidate = dir + L"\\" + name;
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return candidate;

        size_t pos = dir.find_last_of(L"\\/");
        if (pos == std::wstring::npos || dir.size() <= 3)
            break;   // 已到盘符根
        dir = dir.substr(0, pos);
    }
    return base + L"\\" + name;
}

std::wstring DesktopDir()
{
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &known)))
    {
        std::wstring dir(known);
        CoTaskMemFree(known);
        return dir;
    }
    return L".";
}

bool EnsureDir(const std::wstring& dir)
{
    if (dir.empty())
        return false;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;
    // 从最长前缀开始逐级创建
    std::wstring acc;
    size_t i = 0;
    if (dir.size() >= 2 && dir[1] == L':')
    {
        acc = dir.substr(0, 2);
        i = 2;
    }
    while (i < dir.size())
    {
        size_t next = dir.find_first_of(L"\\/", i);
        if (next == std::wstring::npos)
            next = dir.size();
        acc = dir.substr(0, next);
        if (!acc.empty() && acc != L"\\")
            CreateDirectoryW(acc.c_str(), nullptr);   // 已存在时失败无害
        i = next + 1;
    }
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool RevealInExplorer(const std::wstring& file)
{
    if (file.empty())
        return false;
    std::wstring dir, name;
    SplitPath(file, dir, name);
    if (name.empty())
        return false;
    std::wstring params = L"/select,\"" + file + L"\"";
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return (INT_PTR)rc > 32;
}

bool GetFreeDiskBytes(const std::wstring& path, ULONGLONG& freeBytes)
{
    if (path.empty())
        return false;
    ULARGE_INTEGER freeAvail{}, total{}, totalFree{};
    if (!GetDiskFreeSpaceExW(path.c_str(), &freeAvail, &total, &totalFree))
        return false;
    freeBytes = freeAvail.QuadPart;
    return true;
}

// ---------------------------------------------------------------------------
// 文件名
// ---------------------------------------------------------------------------
void LocalTimeParts(int& y, int& mo, int& d, int& h, int& mi, int& s)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    y = st.wYear; mo = st.wMonth; d = st.wDay;
    h = st.wHour; mi = st.wMinute; s = st.wSecond;
}

namespace {
void ReplaceAll(std::wstring& s, const wchar_t* from, const wchar_t* to)
{
    size_t len = wcslen(from);
    if (len == 0) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos)
    {
        s.replace(pos, len, to);
        pos += wcslen(to);
    }
}
}   // namespace

std::wstring ExpandPattern(const std::wstring& pattern, int seq)
{
    int y, mo, d, h, mi, s;
    LocalTimeParts(y, mo, d, h, mi, s);

    wchar_t date[16], time[16], num[24];
    swprintf(date, 16, L"%04d%02d%02d", y, mo, d);
    swprintf(time, 16, L"%02d%02d%02d", h, mi, s);
    swprintf(num, 24, L"%03d", seq);

    std::wstring out = pattern;
    ReplaceAll(out, L"{date}", date);
    ReplaceAll(out, L"{time}", time);
    ReplaceAll(out, L"{n}", num);
    return out;
}

std::wstring BuildFilePath(const std::wstring& dir, const std::wstring& pattern,
                           const wchar_t* ext, int seq)
{
    std::wstring name = ExpandPattern(pattern, seq);
    if (name.empty())
        name = ExpandPattern(L"capture_{date}_{time}", seq);
    if (ext && *ext)
    {
        std::wstring suffix = ext;
        if (name.size() < suffix.size() ||
            _wcsicmp(name.c_str() + name.size() - suffix.size(), suffix.c_str()) != 0)
            name += suffix;
    }
    std::wstring d = dir.empty() ? std::wstring(L".") : dir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/'))
        d.pop_back();
    return d + L"\\" + name;
}

void SplitPath(const std::wstring& path, std::wstring& dir, std::wstring& file)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
    {
        dir = L".";
        file = path;
        return;
    }
    dir = path.substr(0, pos);
    file = path.substr(pos + 1);
    if (dir.empty())
        dir = L"\\";
}

std::wstring EllipsizePath(const std::wstring& path, size_t maxChars)
{
    if (path.size() <= maxChars)
        return path;
    // 优先保留盘符与文件名，目录部分省略
    std::wstring dir, file;
    SplitPath(path, dir, file);
    std::wstring head = dir.substr(0, 3);   // "C:\"
    std::wstring mid = L"…";
    size_t budget = maxChars > (head.size() + mid.size() + 3)
                  ? maxChars - head.size() - mid.size() : 4;
    if (file.size() > budget)
        file = mid + file.substr(file.size() - budget);
    return head + mid + file;
}
