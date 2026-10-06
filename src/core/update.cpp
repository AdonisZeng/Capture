#include "update.h"

#include "version.h"
#include "log.h"
#include "util.h"

#include <winhttp.h>
#include <shellapi.h>   // ShellExecuteW：用默认浏览器打开 Release 页

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
// 版本号
// ===========================================================================
namespace {

// 由 version.h 的整数宏拼出期望的版本字符串。RC 预处理器不支持字符串化，
// 故 version.h 里显式给了 CAPTURE_VERSION_STR；下面这个 constexpr 校验保证
// 「整数宏」与「字符串宏」不会各改各的——只改其一时编译直接失败。
constexpr bool VersionStrConsistent()
{
    const int nums[3] = { CAPTURE_VERSION_MAJOR, CAPTURE_VERSION_MINOR, CAPTURE_VERSION_PATCH };
    char expect[32] = {};
    int n = 0;
    for (int k = 0; k < 3; ++k)
    {
        if (k) expect[n++] = '.';
        int v = nums[k];
        if (v >= 100) { expect[n++] = char('0' + v / 100); v %= 100; }
        if (v >= 10)  { expect[n++] = char('0' + v / 10); }
        expect[n++] = char('0' + v % 10);
    }
    expect[n] = '\0';
    const char* p = CAPTURE_VERSION_STR;
    for (int i = 0; i < n; ++i)
        if (p[i] != expect[i]) return false;
    return p[n] == '\0';
}
static_assert(VersionStrConsistent(),
              "core/version.h: CAPTURE_VERSION_STR 与 MAJOR/MINOR/PATCH 不一致");

}   // namespace

const char* Update::VersionString()  { return CAPTURE_VERSION_STR; }

const char* Update::VersionDisplay()
{
    static const std::string s = std::string("v") + CAPTURE_VERSION_STR;
    return s.c_str();
}

const char* Update::ArchName()
{
#if defined(_M_X64) || defined(_M_AMD64)
    return "x64";
#elif defined(_M_IX86)
    return "win32";
#else
    return "unknown";
#endif
}

// 最小化启动标志透传（开机自启 --minimized 经 Apply → 助手 → 新版本）。
// 文件顶部定义：Apply 在文件靠前位置，而 HasToken 在文件靠后的匿名命名空间里，
// 在那里直接调用会因声明不可见而编译失败
namespace {
bool StartupMinimizedFlagPresent()
{
    const std::wstring cmd = GetCommandLineW();
    const wchar_t* tokens[2] = { L"--minimized", L"--tray" };
    for (const wchar_t* token : tokens)
    {
        const size_t n = wcslen(token);
        size_t pos = 0;
        while ((pos = cmd.find(token, pos)) != std::wstring::npos)
        {
            const bool leftOk = (pos == 0) || cmd[pos - 1] == L' ' ||
                                cmd[pos - 1] == L'\t' || cmd[pos - 1] == L'"';
            const wchar_t rc = (pos + n < cmd.size()) ? cmd[pos + n] : L'\0';
            if (leftOk && (rc == L'\0' || rc == L' ' || rc == L'\t' || rc == L'"'))
                return true;
            pos += n;
        }
    }
    return false;
}
}   // namespace

// ===========================================================================
// 常量与共享状态
// ===========================================================================
namespace {

constexpr wchar_t kApiHost[]   = L"api.github.com";
constexpr wchar_t kWebHost[]   = L"github.com";
constexpr wchar_t kRepoOwner[] = L"AdonisZeng";
constexpr wchar_t kRepoName[]  = L"Capture";

// 启动自动检查的最小间隔（秒）
constexpr long long kAutoCheckIntervalSec = 24LL * 3600;

// JSON 响应上限（Release 正文可能很长，2MB 已经荒谬地富余）
constexpr size_t kMaxJsonBytes = 2u * 1024 * 1024;
// 单个 exe 产物的下载上限。产物大小以 Release 元数据里的 size 为准并逐字节核对，
// 这里只作兜底：响应体出问题时不能让它无限制往磁盘里写
constexpr size_t kMaxAssetBytes = 256u * 1024 * 1024;
// 网络/下载的块大小
constexpr DWORD kChunkBytes = 64 * 1024;
// 最多跟随的重定向跳数（产物直链会 302 到对象存储）
constexpr int kMaxRedirect = 5;

// 本文件把实现细节放在匿名命名空间里，故此处不能用 Update:: 前缀省略：
// 匿名命名空间在全局作用域，看不到 Update 里的类型名
struct Shared
{
    std::mutex   mtx;
    Update::State state = Update::State::Idle;
    Update::Info  info;
    std::string  error;
    long long    doneBytes = 0;
    long long    totalBytes = 0;
    std::wstring newFile;          // 已下载待安装的 exe 完整路径
    bool         noticePending = false;   // 有新版本待提示（appIdle 时由 Tick 消费）
    std::wstring skipTag;          // 用户选择忽略的版本
    bool         autoCheck = true;
    long long    lastCheckUnix = 0;
    // 失败是否可直接重试下载：仅下载阶段的失败置 true（发布信息仍有效），
    // 检查阶段与 Apply 阶段的失败置 false（需重新检查）。只在 Failed 态有意义，
    // 离开 Failed 即清零，避免拿着过期信息去下旧包
    bool         failRetryDownload = false;
};

Shared         g;
std::thread    g_worker;
std::atomic<bool> g_workerAlive { false };
// 退出时的中止标志：Shutdown() 置位，下载循环在每个块之间检查
std::atomic<bool> g_cancel { false };

void SetState(Update::State s)
{
    std::lock_guard<std::mutex> lk(g.mtx);
    g.state = s;
}

void SetError(const std::string& utf8Msg)
{
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.state = Update::State::Failed;
        g.error = utf8Msg;
        g.failRetryDownload = false;
    }
    LOG_ERR(L"更新失败: %s", Utf8ToWide(utf8Msg.c_str()).c_str());
}

// 下载阶段的失败：发布信息仍有效，可直接按原信息重试下载，无需重新检查
void SetDownloadError(const std::string& utf8Msg)
{
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.state = Update::State::Failed;
        g.error = utf8Msg;
        g.failRetryDownload = true;
    }
    LOG_ERR(L"更新失败(可重试下载): %s", Utf8ToWide(utf8Msg.c_str()).c_str());
}

void SetProgress(long long done, long long total)
{
    std::lock_guard<std::mutex> lk(g.mtx);
    g.doneBytes = done;
    g.totalBytes = total;
}

// 起一个工作线程；上一个还没结束就拒绝（调用方都先查 Busy()）
bool SpawnWorker(void (*fn)())
{
    if (g_workerAlive.load())
        return false;
    if (g_worker.joinable())
        g_worker.join();   // 已结束，立即返回
    g_workerAlive.store(true);
    g_cancel.store(false); // 上一轮遗留的中止标志不能毒化本轮
    g_worker = std::thread([fn]() {
        fn();
        g_workerAlive.store(false);
    });
    return true;
}

// ===========================================================================
// 杂项小工具
// ===========================================================================
std::wstring W(const std::string& s)
{
    return Utf8ToWide(s.c_str());
}

std::wstring W(const char* s)
{
    return Utf8ToWide(s);
}

bool IsSpaceAscii(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (LowerAscii(s[s.size() - n + i]) != LowerAscii(suffix[i]))
            return false;
    return true;
}

bool StartsWithNoCase(const std::string& s, const char* prefix)
{
    const size_t n = strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (LowerAscii(s[i]) != LowerAscii(prefix[i]))
            return false;
    return true;
}

// 下载产物名来自服务器（不可信输入），而它会被拼进 --apply-update 的命令行。
// 只放行「字母数字 + . - _」，挡掉路径穿越（..）、引号与 Windows 保留字符
bool IsSafeAssetName(const std::wstring& name)
{
    if (name.empty() || name == L"." || name == L"..")
        return false;
    for (wchar_t c : name)
    {
        const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                        (c >= L'A' && c <= L'Z') || c == L'.' || c == L'-' || c == L'_';
        if (!ok)
            return false;
    }
    return true;
}

std::string FormatBytes(long long bytes)
{
    wchar_t buf[64] = {};
    if (bytes >= 1024 * 1024)
        swprintf_s(buf, 64, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        swprintf_s(buf, 64, L"%.0f KB", (double)bytes / 1024.0);
    else
        swprintf_s(buf, 64, L"%lld 字节", bytes);
    return WideToUtf8Str(buf);
}

// 当前 exe 完整路径（动态缓冲，避免长路径被 MAX_PATH 截断）
std::wstring SelfExePath()
{
    std::vector<wchar_t> buf(1024);
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0)
            return L"";
        if (n < buf.size() - 1)
            return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

// 临时下载目录：%TEMP%\Capture-update。临时目录必然可写，
// 且允许与 exe 目录不同卷（最后一步 MoveFileEx 会自动退化为复制+删除）
std::wstring TempUpdateDir()
{
    wchar_t tmp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = (n > 0 && n <= MAX_PATH) ? std::wstring(tmp, n) : std::wstring(L".");
    if (dir.empty() || dir.back() != L'\\')
        dir += L'\\';
    return dir + L"Capture-update";
}

// 目录是否可写：已迁到 core/util.cpp（DirIsWritable），与配置/日志的可写判定
// 走同一份实现，避免两处探针逻辑漂移

// ===========================================================================
// 极简 JSON 取值
//
// 只做「按 key 取字符串 / 取整数」，够用即可，不构成通用解析器。
// 已知取舍（改动前先读）：
//   - assets 数组靠花括号配平切分，不处理字符串里的花括号；
//     GitHub 的产物名与直链里不含花括号，故对本仓库的响应是安全的
//   - 解析失败时会把响应开头 2KB 打进日志，GitHub 改了字段格式能立刻看出来
// ===========================================================================
void AppendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80)
    {
        out += (char)cp;
    }
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

unsigned Hex4(const std::string& s, size_t i)
{
    unsigned v = 0;
    for (int k = 0; k < 4 && i + k < s.size(); ++k)
    {
        const char c = s[i + k];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
    }
    return v;
}

// 还原 JSON 字符串字面量里的转义序列（含 \uXXXX -> UTF-8，支持代理对）
std::string UnescapeJson(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] != '\\' || i + 1 >= raw.size())
        {
            out += raw[i];
            continue;
        }
        const char e = raw[++i];
        switch (e)
        {
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'u':
        {
            unsigned cp = Hex4(raw, i + 1);
            i += 4;
            // 代理对：高代理后必须紧跟 \uDCxx 低代理，否则按替换字符处理
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < raw.size() &&
                raw[i + 1] == '\\' && raw[i + 2] == 'u')
            {
                const unsigned lo = Hex4(raw, i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0xFFFD;
            AppendUtf8(out, cp);
            break;
        }
        default:   // 未知转义：原样保留（含反斜杠）
            out += '\\';
            out += e;
            break;
        }
    }
    return out;
}

// 定位 "key" 的值位置；结构不符返回 false
bool FindJsonValue(const std::string& js, const char* key, size_t& valPos)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = js.find(pat);
    if (pos == std::string::npos)
        return false;
    pos += pat.size();
    while (pos < js.size() && IsSpaceAscii(js[pos])) ++pos;
    if (pos >= js.size() || js[pos] != ':') return false;
    ++pos;
    while (pos < js.size() && IsSpaceAscii(js[pos])) ++pos;
    valPos = pos;
    return pos < js.size();
}

bool JsonFindString(const std::string& js, const char* key, std::string& out)
{
    size_t pos = 0;
    if (!FindJsonValue(js, key, pos) || js[pos] != '"')
        return false;
    ++pos;
    std::string raw;
    while (pos < js.size() && js[pos] != '"')
    {
        // 反斜杠转义必须连下一个字符一起收，否则 \" 会提前结束字符串
        if (js[pos] == '\\' && pos + 1 < js.size())
        {
            raw += js[pos];
            raw += js[pos + 1];
            pos += 2;
        }
        else
        {
            raw += js[pos];
            ++pos;
        }
    }
    if (pos >= js.size())
        return false;
    out = UnescapeJson(raw);
    return true;
}

bool JsonFindInt(const std::string& js, const char* key, long long& out)
{
    size_t pos = 0;
    if (!FindJsonValue(js, key, pos))
        return false;
    const size_t start = pos;
    if (pos < js.size() && (js[pos] == '-' || js[pos] == '+')) ++pos;
    while (pos < js.size() && js[pos] >= '0' && js[pos] <= '9') ++pos;
    if (pos == start)
        return false;
    out = _strtoi64(js.substr(start, pos - start).c_str(), nullptr, 10);
    return true;
}

// ===========================================================================
// SemVer
// ===========================================================================
// 解析 "v1.2.3" / "1.2.3-rc1" / "v1.2"，只取前三段数字。
// 预发布后缀与构建号一律忽略：/releases/latest 本来就排除 prerelease，
// 真要发测试版应当用 prerelease 标记，那时它根本不会出现在 latest 里
bool ParseSemVer(const std::string& text, int& major, int& minor, int& patch)
{
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == 'v' || text[i] == 'V')) ++i;
    int out[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; ++k)
    {
        if (k > 0)
        {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        int v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
            v = v * 10 + (text[i++] - '0');
        out[k] = v;
    }
    major = out[0];
    minor = out[1];
    patch = out[2];
    return true;
}

bool IsNewer(int rmaj, int rmin, int rpat)
{
    if (rmaj != CAPTURE_VERSION_MAJOR) return rmaj > CAPTURE_VERSION_MAJOR;
    if (rmin != CAPTURE_VERSION_MINOR) return rmin > CAPTURE_VERSION_MINOR;
    return rpat > CAPTURE_VERSION_PATCH;
}

// ===========================================================================
// WinHTTP
// ===========================================================================
// 把 https://host/path?query 拆成主机与路径
bool SplitUrl(const std::string& url, std::wstring& host, std::wstring& path)
{
    const std::string prefix = "https://";
    if (url.size() <= prefix.size() || url.compare(0, prefix.size(), prefix) != 0)
        return false;
    const size_t slash = url.find('/', prefix.size());
    if (slash == std::string::npos)
        return false;
    host = W(url.substr(prefix.size(), slash - prefix.size()));
    path = W(url.substr(slash));
    return !host.empty() && !path.empty();
}

// 打开会话：UA + 强制 TLS 1.2 及以上。
// 切勿设置 WINHTTP_OPTION_SECURITY_FLAGS 的 IGNORE_* 标志——那等于关掉证书链校验，
// 而证书链是更新通道唯一的可信度来源（无签名、无哈希校验）
bool OpenSession(HINTERNET& hSession, std::string& err)
{
    const std::wstring ua = W(std::string("Capture/") + Update::VersionString());
    hSession = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        wchar_t buf[64] = {};
        swprintf_s(buf, 64, L"WinHttpOpen 失败(0x%08lX)", GetLastError());
        err = WideToUtf8Str(buf);
        return false;
    }
    // 显式要求 TLS 1.2 及以上，并把 1.3 一并放行（只写 1.2 等于主动放弃 1.3）。
    // 老 SDK 没有 1.3 常量，条件编译掉即可
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protos |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos)))
    {
        // 置不上就必须停：继续走会退回到 WinHTTP 默认协议集，与「显式要求 1.2+」的意图相反
        wchar_t buf[64] = {};
        swprintf_s(buf, 64, L"无法设置 TLS 协议(0x%08lX)", GetLastError());
        err = WideToUtf8Str(buf);
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        return false;
    }
    DWORD timeout = 15000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    DWORD rto = 30000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &rto, sizeof(rto));
    return true;
}

struct FetchResult
{
    bool        ok = false;
    bool        cancelled = false;  // 用户退出导致的主动中止，不是错误
    long long   bytes = 0;
    std::string body;       // fileSink 为空时响应体收在这里
    std::string err;
};

// 执行 GET（自动跟随重定向，最多 kMaxRedirect 跳）。
// fileSink 非空则边收边写文件，并用 expectTotal 作进度总量（0 表示用 Content-Length）
FetchResult HttpGet(const std::string& url, const char* acceptHeader,
                    FILE* fileSink, size_t maxBytes, long long expectTotal)
{
    FetchResult res;
    HINTERNET hSession = nullptr;
    if (!OpenSession(hSession, res.err))
        return res;

    std::string cur = url;

    for (int hop = 0; hop <= kMaxRedirect; ++hop)
    {
        std::wstring host, path;
        if (!SplitUrl(cur, host, path))
        {
            res.err = "下载地址无法解析";
            break;
        }
        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                           INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect)
        {
            res.err = "无法连接 " + WideToUtf8Str(host);
            break;
        }
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
        if (!hReq)
        {
            WinHttpCloseHandle(hConnect);
            res.err = "无法发起请求";
            break;
        }
        if (acceptHeader && *acceptHeader)
        {
            // winhttp.h 只声明 Unicode 版（名字没有 W 后缀），参数是 LPCWSTR
            const std::wstring hdr = W(std::string(acceptHeader) + "\r\n");
            WinHttpAddRequestHeaders(hReq, hdr.c_str(), (DWORD)hdr.size(),
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        const BOOL sent = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                       && WinHttpReceiveResponse(hReq, nullptr);
        if (!sent)
        {
            wchar_t buf[64] = {};
            swprintf_s(buf, 64, L"网络请求失败(0x%08lX)", GetLastError());
            res.err = WideToUtf8Str(buf);
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            break;
        }

        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);

        // 3xx：取 Location 继续跳（产物直链会 302 到对象存储的签名 URL）
        if (status >= 300 && status < 400)
        {
            wchar_t loc[2048] = {};
            DWORD locLen = (DWORD)(sizeof(loc) / sizeof(wchar_t) - 1);
            std::string next;
            // 按名字取单个响应头：枚举里就有 WINHTTP_QUERY_LOCATION（值 33），
            // 比 WINHTTP_QUERY_CUSTOM 稳；用 0 会一直取不到从而误判成「缺少 Location」
            if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                    loc, &locLen, WINHTTP_NO_HEADER_INDEX))
                next = WideToUtf8Str(loc);
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            if (next.empty())
            {
                res.err = "重定向响应缺少 Location";
                break;
            }
            // 只跟随 https 与同主机相对路径。http 目标不能「拼成 https」，
            // 那会构造出一个不存在的地址；更新通道必须拒绝而不是勉强拼
            if (StartsWithNoCase(next, "https://"))
                cur = next;
            else if (StartsWithNoCase(next, "http://"))
            {
                res.err = "重定向目标不是 HTTPS，已拒绝";
                break;
            }
            else if (next[0] == '/')
                cur = "https://" + WideToUtf8Str(host) + next;
            else
            {
                res.err = "重定向地址无法解析";
                break;
            }
            continue;
        }

        if (status != 200)
        {
            // 这几个状态码在真实使用中都会遇到，给出可执行的原因而不是「HTTP 403」
            wchar_t buf[192] = {};
            switch (status)
            {
            case 401:
            case 403:
                // GitHub 未认证接口是「每 IP 每小时 60 次」。公司/校园/云主机 NAT
                // 出口容易和其他人共用这个额度，用满就是 403
                swprintf_s(buf, 192, L"GitHub 拒绝了请求（0x%X）："
                                     L"多为接口调用限额已用尽（每 IP 每小时 60 次），"
                                     L"或网络被代理拦截，请稍后再试", status);
                break;
            case 404:
                swprintf_s(buf, 192, L"GitHub 上找不到 %s/%s 的正式发布（0x%X）："
                                     L"仓库可能还没创建任何 Release",
                           kRepoOwner, kRepoName, status);
                break;
            case 429:
                swprintf_s(buf, 192, L"请求过于频繁（0x%X），请稍后再试", status);
                break;
            default:
                swprintf_s(buf, 192, L"服务器返回 HTTP %u", status);
                break;
            }
            res.err = WideToUtf8Str(buf);
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            break;
        }

        DWORD contentLen = 0;
        DWORD clLen = sizeof(contentLen);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &contentLen, &clLen,
                            WINHTTP_NO_HEADER_INDEX);
        const long long totalForProgress =
            (expectTotal > 0) ? expectTotal : (long long)contentLen;

        std::vector<char> buf(kChunkBytes);
        bool aborted = false;
        for (;;)
        {
            // 用户退出：主动中止，别让退出路径干等整个包下载完
            if (g_cancel.load())
            {
                res.cancelled = true;
                aborted = true;
                break;
            }
            DWORD avail = 0;
            // 先按返回值分派，再看 avail：读完之后 avail 会是 0，那是正常 EOF。
            // 不能用 GetLastError 来判断 EOF —— 它保留上一次调用的残留错误码，
            // 正常读完也会被误判成读取失败
            if (!WinHttpQueryDataAvailable(hReq, &avail))
            {
                wchar_t b[64] = {};
                swprintf_s(b, 64, L"读取响应数据失败(0x%08lX)", GetLastError());
                res.err = WideToUtf8Str(b);
                aborted = true;
                break;
            }
            if (avail == 0)
                break;   // 响应体读完
            const DWORD want = (avail < kChunkBytes) ? avail : kChunkBytes;
            DWORD got = 0;
            if (!WinHttpReadData(hReq, buf.data(), want, &got))
            {
                wchar_t b[64] = {};
                swprintf_s(b, 64, L"读取响应数据失败(0x%08lX)", GetLastError());
                res.err = WideToUtf8Str(b);
                aborted = true;
                break;
            }
            if (got == 0)
                break;
            // 落盘同样要设上限：响应体异常（截断后被续传、代理改写）时
            // 不能让它一直写，磁盘写爆比下载失败难处理得多
            if (fileSink)
            {
                if (res.bytes + got > (long long)maxBytes)
                {
                    res.err = "下载内容超出上限，已中止";
                    aborted = true;
                    break;
                }
                if (fwrite(buf.data(), 1, got, fileSink) != got)
                {
                    res.err = "写入临时文件失败（磁盘空间不足或临时目录不可写）";
                    aborted = true;
                    break;
                }
                res.bytes += got;
                SetProgress(res.bytes, totalForProgress);
            }
            else
            {
                if (res.body.size() + got > maxBytes)
                {
                    res.err = "服务器响应超出预期大小";
                    aborted = true;
                    break;
                }
                res.body.append(buf.data(), got);
                res.bytes += got;
            }
        }

        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        if (!aborted)
            res.ok = true;
        break;
    }

    WinHttpCloseHandle(hSession);
    return res;
}

// ===========================================================================
// Release 解析
// ===========================================================================
// 从 assets 数组里挑出本架构的 exe 产物。固定名、不带版本号：
//     Capture-x64.exe / Capture-win32.exe
// 大小写不敏感比较（GitHub 附件名原样返回，正常就是这个写法）。
// 同一 Release 下两个架构各一个附件，不会重名。
bool ParseAssets(const std::string& js, const char* arch,
                 std::string& name, std::string& url, long long& size)
{
    const size_t keyPos = js.find("\"assets\"");
    if (keyPos == std::string::npos)
        return false;
    const size_t lb = js.find('[', keyPos);
    if (lb == std::string::npos)
        return false;

    // 产物命名约定：Capture-<arch>.exe（固定名，不带版本号）
    // （docs/RELEASING.md 的发布清单用的就是这个形式）
    const std::string archSuffix = std::string("-") + arch + ".exe";

    int depth = 0;
    size_t objStart = std::string::npos;
    for (size_t i = lb; i < js.size(); ++i)
    {
        const char c = js[i];
        if (c == '{')
        {
            if (depth == 0) objStart = i;
            ++depth;
        }
        else if (c == '}')
        {
            if (depth == 0)
                break;
            --depth;
            if (depth == 0 && objStart != std::string::npos)
            {
                const std::string block = js.substr(objStart, i - objStart + 1);
                objStart = std::string::npos;
                std::string an, au;
                long long asz = 0;
                if (JsonFindString(block, "name", an) &&
                    JsonFindString(block, "browser_download_url", au))
                {
                    JsonFindInt(block, "size", asz);
                    // 固定名匹配（大小写不敏感）：必须同时满足前缀与架构后缀，
                    // 避免同 Release 里其他 exe 被误挑
                    if (StartsWithNoCase(an, "Capture-") &&
                        EndsWithNoCase(an, archSuffix.c_str()))
                    {
                        name = an;
                        url = au;
                        size = asz;
                        return true;
                    }
                }
            }
        }
        else if (c == ']' && depth == 0)
        {
            break;
        }
    }

    return false;
}

bool ParseRelease(const std::string& js, Update::Info& info)
{
    if (!JsonFindString(js, "tag_name", info.tag))
        return false;
    JsonFindString(js, "body", info.notes);
    JsonFindString(js, "html_url", info.pageUrl);
    if (!ParseSemVer(info.tag, info.major, info.minor, info.patch))
        return false;
    if (!ParseAssets(js, Update::ArchName(), info.assetName, info.assetUrl,
                     info.assetSize))
        return false;
    return true;
}

// ===========================================================================
// 工作线程
// ===========================================================================
void CheckWorker()
{
    const std::string url = std::string("https://") + WideToUtf8Str(kApiHost) +
                            "/repos/" + WideToUtf8Str(kRepoOwner) + "/" +
                            WideToUtf8Str(kRepoName) + "/releases/latest";
    const FetchResult res =
        HttpGet(url, "Accept: application/vnd.github+json", nullptr, kMaxJsonBytes, 0);
    if (res.cancelled)
        return;   // 用户正在退出，别把主动中止报成检查失败
    if (!res.ok)
    {
        SetError(res.err);
        return;
    }

    Update::Info info;
    if (!ParseRelease(res.body, info))
    {
        // 结构不认识时把响应开头打出来：GitHub 改了字段能一眼看出
        LOG_ERR(L"Release 响应解析失败, 前 2KB: %s",
                W(res.body.substr(0, 2048)).c_str());
        SetError("无法解析 GitHub 返回的发布信息");
        return;
    }

    std::wstring skip;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        skip = g.skipTag;
    }
    const bool skipped = !skip.empty() && skip == W(info.tag);

    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.info = info;
        g.failRetryDownload = false;
        if (!IsNewer(info.major, info.minor, info.patch) || skipped)
        {
            g.state = Update::State::UpToDate;
            return;
        }
        g.state = Update::State::Available;
        g.noticePending = true;
    }
    // %s 在宽字符日志里对应 wchar_t*，窄串必须转宽，否则日志里是乱码
    LOG_INFO(L"发现新版本 %s (当前 %s), 产物 %s (%s)", W(info.tag).c_str(),
             W(CAPTURE_VERSION_STR).c_str(), W(info.assetName).c_str(),
             W(FormatBytes(info.assetSize)).c_str());
}

void DownloadWorker()
{
    Update::Info info;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        info = g.info;
    }
    if (info.assetUrl.empty())
    {
        SetError("没有可下载的更新包");
        return;
    }

    const std::wstring dir = TempUpdateDir();
    if (!EnsureDir(dir))
    {
        SetDownloadError("无法创建临时目录：" + WideToUtf8Str(dir));
        return;
    }
    // 产物名来自服务器（不可信输入），而它会被拼进 --apply-update 的命令行：
    // 白名单放行「字母数字 + . - _」，同时挡掉路径穿越、引号与 Windows 保留字符
    std::wstring fileName = W(info.assetName);
    const size_t slash = fileName.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        fileName = fileName.substr(slash + 1);
    if (!IsSafeAssetName(fileName))
    {
        SetError("发布产物名非法");
        return;
    }
    const std::wstring dest = dir + L"\\" + fileName;

    // 先删旧文件，避免上次的半截文件被当成完整包
    DeleteFileW(dest.c_str());

    FILE* f = _wfsopen(dest.c_str(), L"wb", _SH_DENYRW);
    if (!f)
    {
        SetDownloadError("无法写入临时文件：" + WideToUtf8Str(dest));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.state = Update::State::Downloading;
        g.failRetryDownload = false;   // 已离开 Failed，标记只在 Failed 态有意义
        g.doneBytes = 0;
        g.totalBytes = info.assetSize;
    }

    // Accept: application/octet-stream 让接口返回 302 到对象存储的实际文件，
    // 而不是 JSON 描述；配合 HttpGet 里的重定向跟随即可
    // maxBytes 传产物上限而非 JSON 上限：HttpGet 对落盘分支同样按它收口
    const FetchResult res = HttpGet(info.assetUrl, "Accept: application/octet-stream",
                                    f, kMaxAssetBytes, info.assetSize);
    fclose(f);

    if (res.cancelled)
    {
        DeleteFileW(dest.c_str());   // 半截文件留着只会污染下次启动的清理
        return;
    }
    if (!res.ok)
    {
        DeleteFileW(dest.c_str());
        SetDownloadError(res.err);
        return;
    }
    if (info.assetSize > 0 && res.bytes != info.assetSize)
    {
        DeleteFileW(dest.c_str());
        SetDownloadError("下载文件大小与发布信息不符（" + FormatBytes(res.bytes) + " / " +
                         FormatBytes(info.assetSize) + "），已丢弃");
        return;
    }

    // 完整性只核对 Release 元数据里的 size：截断/续传损坏能发现，
    // 但等长替换发现不了。可信度只有 HTTPS 一层（见 update.h 头注释）。
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.newFile = dest;
        g.state = Update::State::Ready;
        g.failRetryDownload = false;
    }
    LOG_INFO(L"新版本已下载: %s (%s)", W(info.assetName).c_str(),
             W(FormatBytes(res.bytes)).c_str());
}

}   // namespace

// ===========================================================================
// 对外接口
// ===========================================================================
Update::State Update::CurrentState()
{
    std::lock_guard<std::mutex> lk(g.mtx);
    return g.state;
}

Update::Info Update::LatestInfo()
{
    // 按值返回：info 由工作线程改、UI 线程读，返回引用会留下数据竞争
    std::lock_guard<std::mutex> lk(g.mtx);
    return g.info;
}

bool Update::Busy()
{
    const State s = CurrentState();
    return s == State::Checking || s == State::Downloading;
}

double Update::Progress()
{
    std::lock_guard<std::mutex> lk(g.mtx);
    if (g.state != State::Downloading || g.totalBytes <= 0)
        return 0.0;
    const double p = (double)g.doneBytes / (double)g.totalBytes;
    return p > 1.0 ? 1.0 : p;
}

std::string Update::ErrorText()
{
    std::lock_guard<std::mutex> lk(g.mtx);
    return g.state == State::Failed ? g.error : std::string();
}

std::string Update::StatusText()
{
    State s;
    Info info;
    long long done = 0, total = 0;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        s = g.state;
        info = g.info;
        done = g.doneBytes;
        total = g.totalBytes;
    }
    switch (s)
    {
    case State::Idle:
        return "尚未检查更新";
    case State::Checking:
        return "正在检查…";
    case State::UpToDate:
        return std::string("已是最新版本（") + VersionDisplay() + "）";
    case State::Available:
        return "发现新版本 " + info.tag + "（" + FormatBytes(info.assetSize) + "）";
    case State::Downloading:
    {
        const int pct = (total > 0) ? (int)((double)done * 100.0 / (double)total) : 0;
        const std::string d = FormatBytes(done), t = FormatBytes(total);
        return std::string("正在下载… ") + std::to_string(pct) + "%（" + d + " / " + t + "）";
    }
    case State::Ready:
        return info.tag + " 已下载完成，重启后生效";
    case State::Failed:
        return "检查/更新失败：" + ErrorText();
    }
    return "";
}

std::wstring Update::SkipTag()
{
    std::lock_guard<std::mutex> lk(g.mtx);
    return g.skipTag;
}

void Update::SetAutoCheck(bool on)
{
    std::lock_guard<std::mutex> lk(g.mtx);
    g.autoCheck = on;
}

long long Update::LastCheckUnix()
{
    std::lock_guard<std::mutex> lk(g.mtx);
    return g.lastCheckUnix;
}

void Update::Init(bool autoCheck, long long lastCheckUnix, const std::wstring& skipTag)
{
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.autoCheck = autoCheck;
        g.lastCheckUnix = lastCheckUnix;
        // 忽略状态只在 settings.json 里持久化，不载入的话：
        // CheckWorker 永远看不到 skipTag（提示照弹），而退出时 SkipTag()
        // 又会把磁盘上的值回写成空 —— 等于每次启动都丢掉用户的忽略操作
        g.skipTag = skipTag;
    }
    // 清掉上次运行遗留的下载临时文件。此刻本进程还没有待安装的包，
    // 整个目录都可以安全清空
    const std::wstring dir = TempUpdateDir();
    WIN32_FIND_DATAW fd = {};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    const long long now = (long long)time(nullptr);
    if (autoCheck && (now - lastCheckUnix) >= kAutoCheckIntervalSec)
    {
        LOG_INFO(L"启动自动检查更新（距上次 %lld 秒）", now - lastCheckUnix);
        CheckNow();
    }
}

void Update::Shutdown()
{
    // 先置中止标志：join 一个正在下几十 MB 的线程会让退出界面卡住，
    // 下载循环在每个块之间检查该标志，会尽快自行退出
    g_cancel.store(true);
    if (g_worker.joinable())
        g_worker.join();
}

bool Update::Tick(bool appIdle)
{
    // 回收已结束的工作线程，顺带让 Shutdown 立即返回
    if (g_worker.joinable() && !g_workerAlive.load())
        g_worker.join();
    if (!appIdle)
        return false;
    std::lock_guard<std::mutex> lk(g.mtx);
    if (!g.noticePending || g.state != State::Available)
        return false;
    g.noticePending = false;
    return true;
}

bool Update::CheckNow()
{
    if (Busy())
        return false;
    State prev;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        prev = g.state;
        g.lastCheckUnix = (long long)time(nullptr);
        g.error.clear();
        g.failRetryDownload = false;
        g.state = State::Checking;
    }
    // 起线程失败（上一轮工作线程刚结束还没被回收）时必须回滚状态：
    // 停在 Checking 且没人去改它，界面会一直显示「正在检查…」
    if (!SpawnWorker(CheckWorker))
    {
        SetState(prev);
        return false;
    }
    return true;
}

bool Update::StartDownload()
{
    if (CurrentState() != State::Available)
        return false;
    if (!SpawnWorker(DownloadWorker))
    {
        SetState(State::Available);
        return false;
    }
    return true;
}

bool Update::CanRetryDownload()
{
    std::lock_guard<std::mutex> lk(g.mtx);
    return g.state == State::Failed && g.failRetryDownload && !g.info.assetUrl.empty();
}

bool Update::RetryDownload()
{
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        if (g.state != State::Failed || !g.failRetryDownload || g.info.assetUrl.empty())
            return false;
        if (g_workerAlive.load())
            return false;
        // 不在这里清 error：起线程失败时保持原错误文案，调用方据返回值决定
        // 不关弹窗；DownloadWorker 成功/失败都会覆盖 error 或离开 Failed
    }
    // DownloadWorker 读 g.info 按原发布信息重下，不经过 Available 中转；
    // 起线程失败则保持 Failed（调用方据返回值决定不关弹窗）
    if (!SpawnWorker(DownloadWorker))
        return false;
    return true;
}

bool Update::Apply()
{
    std::wstring newFile;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        if (g.state != State::Ready || g.newFile.empty())
            return false;
        newFile = g.newFile;
    }
    if (GetFileAttributesW(newFile.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        SetDownloadError("下载文件已丢失，请重新下载");
        return false;
    }

    const std::wstring self = SelfExePath();
    if (self.empty())
    {
        SetError("无法定位当前程序路径");
        return false;
    }
    // 装在 Program Files 这类受保护位置时提前给出明确原因，
    // 否则助手会失败在「新文件移到位」那一步，只留一行日志
    if (!DirIsWritable(ExeDir()))
    {
        SetError("程序所在目录不可写（可能安装在受保护位置），请手动下载新版本覆盖");
        return false;
    }

    // 助手就是本 exe 的另一个实例，带 --apply-update 运行。
    // 命令行按 CreateProcessW 规则手工加引号：lpCommandLine 不会被规范化，
    // GetCommandLineW 拿到的就是这一串，助手按同样的引号规则解析。
    // 最小化启动态一并透传：开机自启（--minimized）后更新重启不应突然弹主窗口
    std::wstring args = L"\"" + self + L"\" --apply-update \"" + newFile + L"\" --pid " +
                        std::to_wstring(GetCurrentProcessId());
    if (StartupMinimizedFlagPresent())
        args += L" --minimized";
    std::vector<wchar_t> argBuf(args.begin(), args.end());
    argBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(self.c_str(), argBuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi))
    {
        wchar_t buf[64] = {};
        swprintf_s(buf, 64, L"无法启动替换助手(0x%08lX)", GetLastError());
        SetError(WideToUtf8Str(buf));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG_INFO(L"替换助手已启动, 本进程退出后安装: %s", newFile.c_str());
    return true;
}

void Update::SkipVersion()
{
    std::string tag;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        if (g.info.tag.empty())
            return;
        g.skipTag = W(g.info.tag);
        g.noticePending = false;
        tag = g.info.tag;
    }
    LOG_INFO(L"已忽略版本: %s", W(tag).c_str());
}

void Update::OpenReleasePage()
{
    std::string url;
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        url = g.info.pageUrl;
    }
    if (url.empty())
    {
        url = std::string("https://") + WideToUtf8Str(kWebHost) + "/" +
              WideToUtf8Str(kRepoOwner) + "/" + WideToUtf8Str(kRepoName) + "/releases";
    }
    ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ===========================================================================
// 助手模式
// ===========================================================================
namespace {

// 命令行里是否存在独立的一个 token（避免 "--updated" 命中路径里的片段）
bool HasToken(const wchar_t* token)
{
    const std::wstring cmd = GetCommandLineW();
    const size_t n = wcslen(token);
    size_t pos = 0;
    while ((pos = cmd.find(token, pos)) != std::wstring::npos)
    {
        const bool leftOk = (pos == 0) || cmd[pos - 1] == L' ' || cmd[pos - 1] == L'\t' ||
                            cmd[pos - 1] == L'"';
        const bool rightOk = cmd[pos + n] == L'\0' || cmd[pos + n] == L' ' ||
                             cmd[pos + n] == L'\t' || cmd[pos + n] == L'"';
        if (leftOk && rightOk)
            return true;
        pos += n;
    }
    return false;
}

// 取某个开关后面的值，支持 --flag "带空格的值" 与 --flag value。
// 必须按独立 token 定位：exe 路径里出现同名片段时（如 D:\--pid\Capture.exe）
// 直接 find 会命中路径，那一段被当成命令行后半段解析出乱七八糟的值
std::wstring ArgValue(const wchar_t* flag)
{
    const std::wstring cmd = GetCommandLineW();
    const size_t n = wcslen(flag);
    for (size_t pos = 0; (pos = cmd.find(flag, pos)) != std::wstring::npos; pos += n)
    {
        // 左边界：行首或空白 —— 排除路径内部命中
        if (pos != 0 && cmd[pos - 1] != L' ' && cmd[pos - 1] != L'\t')
            continue;
        size_t i = pos + n;
        while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;
        if (i >= cmd.size())
            return L"";
        if (cmd[i] == L'"')
        {
            const size_t e = cmd.find(L'"', i + 1);
            return (e == std::wstring::npos) ? L"" : cmd.substr(i + 1, e - i - 1);
        }
        const size_t e = cmd.find_first_of(L" \t", i);
        return (e == std::wstring::npos) ? cmd.substr(i) : cmd.substr(i, e - i);
    }
    return L"";
}

void ApplyFail(const wchar_t* reason)
{
    LOG_ERR(L"更新失败: %s", reason);
    // 助手是后台进程、没有界面，只能弹原生框告知失败原因与回滚办法
    MessageBoxW(nullptr, reason, L"Capture 更新", MB_ICONERROR | MB_TOPMOST);
}

}   // namespace

bool Update::HasRelaunchFlag()
{
    return HasToken(L"--updated");
}

bool Update::RunApplyHelper()
{
    if (!HasToken(L"--apply-update"))
        return false;

    const std::wstring newFile = ArgValue(L"--apply-update");
    const DWORD oldPid = (DWORD)_wtoi(ArgValue(L"--pid").c_str());

    LogInit();
    LOG_INFO(L"更新助手启动, 待安装 %s (旧进程 pid=%u)", newFile.c_str(), oldPid);

    // 等旧进程彻底退出。进程句柄有信号 == 所有线程已终止、文件句柄已释放，
    // 此时才可以改名 / 覆盖它的 exe
    if (oldPid != 0)
    {
        HANDLE hOld = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
        if (hOld)
        {
            WaitForSingleObject(hOld, 60000);
            CloseHandle(hOld);
        }
        else
        {
            // 旧进程已经没了（句柄查不到），留一点余量给内核清理文件句柄
            Sleep(500);
        }
    }

    const std::wstring self = SelfExePath();
    const std::wstring backup = self + L".old";
    if (self.empty() || GetFileAttributesW(newFile.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        ApplyFail(L"更新失败：待安装的文件已不存在。\n请重新检查更新。");
        LogShutdown();
        return true;
    }

    // 运行中的映像不能被覆盖，但可以改名——先把当前 exe 让开
    DeleteFileW(backup.c_str());
    if (!MoveFileExW(self.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        wchar_t msg[192] = {};
        swprintf_s(msg, 192, L"更新失败：无法重命名当前程序（0x%08lX）。\n"
                              L"可能有程序正在占用它，请手动下载新版本覆盖。", GetLastError());
        ApplyFail(msg);
        LogShutdown();
        return true;
    }

    // 新文件移到位。MOVEFILE_COPY_ALLOWED 不能省：临时目录（通常在 C 盘）与 exe
    // 所在盘不同的机器很常见，缺这个标志跨卷直接返回 ERROR_NOT_SAME_DEVICE，
    // 「自动退化为复制 + 删除」并不会自己发生（同一目录下改名不需要跨卷，故上面那步无需）
    if (!MoveFileExW(newFile.c_str(), self.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
    {
        wchar_t msg[192] = {};
        swprintf_s(msg, 192, L"更新失败：无法写入新程序（0x%08lX）。\n已回滚到原版本。",
                   GetLastError());
        MoveFileExW(backup.c_str(), self.c_str(),    // 回滚
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        ApplyFail(msg);
        LogShutdown();
        return true;
    }

    // 拉起新版本。带 --updated：新实例据此重试等待单实例互斥量，
    // 否则旧进程刚退出、互斥量尚未释放时会被判定成「已有实例」而秒退。
    // 最小化启动态一并透传（见 Apply 的注释）
    std::wstring args = L"\"" + self + L"\" --updated";
    if (HasToken(L"--minimized") || HasToken(L"--tray") || StartupMinimizedFlagPresent())
        args += L" --minimized";
    std::vector<wchar_t> argBuf(args.begin(), args.end());
    argBuf.push_back(L'\0');
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(self.c_str(), argBuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi))
    {
        wchar_t msg[256] = {};
        swprintf_s(msg, 256, L"更新已完成，但新版本无法启动（0x%08lX）。\n"
                              L"目录下已留有 %s.old，可将其改回原名后重新运行。",
                   GetLastError(), L"原程序");
        ApplyFail(msg);
        LogShutdown();
        return true;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // .old 留给新实例下次启动时删；万一新版本起不来，用户还能改名回滚
    LOG_INFO(L"更新完成, 已启动新版本 %s", W(CAPTURE_VERSION_STR).c_str());
    LogShutdown();
    return true;
}

void Update::CleanupStaleBackup()
{
    const std::wstring self = SelfExePath();
    if (self.empty())
        return;
    const std::wstring backup = self + L".old";
    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (DeleteFileW(backup.c_str()))
            LOG_INFO(L"已清理上次更新遗留的备份: %s", backup.c_str());
        else
            LOG_WARN(L"备份文件清理失败(0x%08lX): %s", GetLastError(), backup.c_str());
    }
}
