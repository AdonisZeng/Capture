#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include <algorithm>
#include <string>
#include <vector>

static FILE*        g_logFile = nullptr;
static std::wstring g_logPath;
static CRITICAL_SECTION g_cs;
static bool         g_csInit = false;

// 从 exe 所在目录向上最多 5 层查找已存在的 "log" 文件夹；
// 找不到则返回 exe 目录旁的 "log"（由调用方负责创建）
static std::wstring ResolveLogDir()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    size_t pos = exeDir.find_last_of(L"\\/");
    exeDir = (pos == std::wstring::npos) ? L"." : exeDir.substr(0, pos);

    std::wstring dir = exeDir;
    for (int i = 0; i < 5; ++i)
    {
        std::wstring candidate = dir + L"\\log";
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return candidate;

        pos = dir.find_last_of(L"\\/");
        if (pos == std::wstring::npos || dir.size() <= 3)   // 已到盘符根
            break;
        dir = dir.substr(0, pos);
    }
    return exeDir + L"\\log";
}

// 日志轮转：保留最近 kKeepLogs 份，其余删除。
// 文件名固定为 yyyyMMdd_HHmmss.txt，字典序即时间序，直接排序就能判定新旧。
// 必须在本次日志文件打开之后调用，清理记录才会写进新日志里。
static void RotateLogs(const std::wstring& dir)
{
    constexpr size_t kKeepLogs = 20;

    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\*.txt";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
            files.emplace_back(fd.cFileName);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (files.size() <= kKeepLogs)
        return;

    std::sort(files.begin(), files.end());
    const size_t delCount = files.size() - kKeepLogs;
    size_t removed = 0;
    for (size_t i = 0; i < delCount; ++i)
    {
        const std::wstring victim = dir + L"\\" + files[i];
        // 删除失败不追究：日志标记着系统，多半是被别的进程占用或已无权限
        if (DeleteFileW(victim.c_str()))
            ++removed;
    }
    LOG_INFO(L"日志轮转: 清理 %zu/%zu 份旧日志, 保留最近 %zu 份",
             removed, delCount, kKeepLogs);
}

void LogInit()
{
    if (g_logFile)
        return;
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }

    EnterCriticalSection(&g_cs);

    std::wstring dir = ResolveLogDir();
    CreateDirectoryW(dir.c_str(), nullptr);   // 已存在时失败无害

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"\\%04u%02u%02u_%02u%02u%02u.txt",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_logPath = dir + name;

    // ccs=UTF-8：新文件自动写入 UTF-8 BOM，记事本打开不乱码
    // _SH_DENYNO：允许程序运行期间用记事本等工具同时查看日志
    g_logFile = _wfsopen(g_logPath.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);

    LeaveCriticalSection(&g_cs);

    if (!g_logFile)
        return;

    // 轮转放在打开之后：清理记录才会落到本次日志里（LogInit 之前 g_logFile 为空，写了也丢）
    RotateLogs(dir);

    LOG_INFO(L"===== 程序启动 PID=%u =====", GetCurrentProcessId());
    LOG_INFO(L"日志文件: %s", g_logPath.c_str());
}

void LogShutdown()
{
    if (!g_logFile)
        return;
    LOG_INFO(L"===== 程序退出 =====");
    fclose(g_logFile);
    g_logFile = nullptr;
}

void LogWrite(const wchar_t* fmt, ...)
{
    if (!g_logFile)
        return;

    wchar_t msg[2048];
    va_list args;
    va_start(args, fmt);
    vswprintf(msg, 2048, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    EnterCriticalSection(&g_cs);
    fwprintf(g_logFile,
             L"[%04u-%02u-%02u %02u:%02u:%02u.%03u][%u] %s\n",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             GetCurrentThreadId(), msg);
    fflush(g_logFile);
    LeaveCriticalSection(&g_cs);
}
