#include "settings.h"
#include "core/util.h"
#include "core/log.h"
#include "screenshot/screenshot.h"   // ShotFormatExt：预览路径的扩展名与实际编码同源
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>      // time：校验 updateLastCheck 不落在未来
#include <vector>

// ---------------------------------------------------------------------------
// 极简 JSON 读写：仅支持「扁平对象 + 字符串/数字/布尔」，无依赖、无转义数组
// ---------------------------------------------------------------------------
namespace {

std::string JsonEscape(const std::wstring& s)
{
    std::string out;
    out.reserve(s.size() * 2 + 8);
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'\\': out += "\\\\"; break;
        case L'"':  out += "\\\""; break;
        case L'\n': out += "\\n";  break;
        case L'\r': out += "\\r";  break;
        case L'\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[8] = {};
                sprintf_s(buf, "\\u%04x", (unsigned)c);
                out += buf;
            }
            else
            {
                // wchar_t -> UTF-8 逐字符（设置项均为 BMP 内字符）
                char buf[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, buf, 4, nullptr, nullptr);
                out.append(buf, n);
            }
            break;
        }
    }
    return out;
}

// 两阶段：原始字节先累积到 utf8 缓冲，转义出现（或字符串结束）时整批解码成宽字符。
// 严禁按单个字节调用 MultiByteToWideChar：多字节序列被拆开后每次输入都是不完整序列，
// 会返回 U+FFFD，中文全部变成问号（实测 "录制" -> "??????"）。
std::wstring JsonUnescape(const std::string& s)
{
    std::wstring out;
    out.reserve(s.size());
    std::string utf8;   // 已累积、尚未解码的原始 UTF-8 片段

    // 把累积的原始字节一次性解成宽字符并追加
    auto flush = [&]()
    {
        if (utf8.empty())
            return;
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        if (n > 0)
        {
            std::wstring w((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], n);
            out.append(w);
        }
        utf8.clear();
    };

    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '\\' || i + 1 >= s.size())
        {
            utf8.push_back(s[i]);   // 原始字节，交给 flush 整批处理
            continue;
        }
        char c = s[++i];
        switch (c)
        {
        case 'n': flush(); out.push_back(L'\n'); break;
        case 'r': flush(); out.push_back(L'\r'); break;
        case 't': flush(); out.push_back(L'\t'); break;
        case 'u':
        {
            flush();   // \uXXXX 直接产出宽字符，先把前面的原始字节解掉以保证顺序
            if (i + 4 < s.size())
            {
                unsigned code = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (code >= 0xD800 && code <= 0xDBFF && i + 6 < s.size() &&
                    s[i + 1] == '\\' && s[i + 2] == 'u')
                {
                    unsigned low = (unsigned)strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16);
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                if (code < 0x10000)
                    out.push_back((wchar_t)code);
                else
                {
                    code -= 0x10000;
                    out.push_back((wchar_t)(0xD800 + (code >> 10)));
                    out.push_back((wchar_t)(0xDC00 + (code & 0x3FF)));
                }
            }
            break;
        }
        default: flush(); out.push_back((wchar_t)(unsigned char)c); break;
        }
    }
    flush();   // 收尾：尾部若还有未解码的原始字节必须补解
    return out;
}

// 去掉字符串两侧空白
std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

struct Cursor
{
    const char* p;
    const char* end;

    void SkipWs() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool Eat(char c)
    {
        SkipWs();
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }
    // 读 "key" 或 key（双引号可选）
    bool ReadKey(std::string& out)
    {
        SkipWs();
        bool quoted = (p < end && *p == '"');
        if (quoted) ++p;
        std::string k;
        while (p < end && *p != '"' && *p != ':' && *p != '\n')
            k.push_back(*p++);
        if (quoted && p < end && *p == '"') ++p;
        if (!Eat(':')) return false;
        out = Trim(k);
        return true;
    }
    bool ReadString(std::string& out)
    {
        SkipWs();
        if (p >= end || *p != '"') return false;
        ++p;
        std::string v;
        while (p < end && *p != '"')
        {
            if (*p == '\\' && p + 1 < end) { v.push_back(*p++); }
            v.push_back(*p++);
        }
        if (p < end) ++p;
        out = v;
        return true;
    }
    bool ReadScalar(std::string& out)
    {
        SkipWs();
        std::string v;
        while (p < end && *p != ',' && *p != '}' && *p != '\n')
            v.push_back(*p++);
        out = Trim(v);
        return !out.empty();
    }
};

}   // namespace

// ---------------------------------------------------------------------------
// 路径 / 读写
// ---------------------------------------------------------------------------
std::wstring SettingsFilePath()
{
    return FindDataDir(L"config") + L"\\settings.json";
}

namespace {
void ClampInt(int& v, int lo, int hi, int fallback)
{
    if (v < lo || v > hi) v = fallback;
}
}   // namespace

void FillDefaults(Settings& s)
{
    if (s.saveDir.empty())
        s.saveDir = DesktopDir();
    if (s.shotDir.empty())
        s.shotDir = s.saveDir;
    if (s.recordPattern.empty())
        s.recordPattern = L"录制_{date}_{time}";
    if (s.shotPattern.empty())
        s.shotPattern = L"截图_{date}_{time}";
    if (s.hotkeyCapture.empty())  s.hotkeyCapture  = L"Ctrl+Alt+S";
    if (s.hotkeyRecord.empty())   s.hotkeyRecord   = L"Ctrl+Alt+R";
    if (s.hotkeyShow.empty())     s.hotkeyShow     = L"Ctrl+Alt+C";
    ClampInt(s.fps, 15, 120, 60);
    ClampInt(s.recResolution, 0, RecResCount - 1, RecResNative);
    ClampInt(s.bitrateMbps, 0, 200, 0);
    ClampInt(s.audioBitrate, 64, 320, 192);
    ClampInt(s.sysVolume, 0, 200, 100);
    ClampInt(s.micVolume, 0, 200, 100);
    ClampInt(s.captureSource, 0, CapSourceCount - 1, CapMonitor);
    ClampInt(s.recDelaySec, 0, 30, 0);
    ClampInt(s.shotFormat, 0, ShotFormatCount - 1, ShotFormatPng);
    ClampInt(s.lastPage, 0, 3, 0);
    ClampInt(s.windowW, 900, 3840, 1080);
    ClampInt(s.windowH, 600, 2160, 700);
    // 上次检查时刻必须落在 (0, 现在]：负数与「未来的时间戳」都要归零。
// 未来值（系统时间回拨、手改配置）会让 now - updateLastCheck 恒为负，
// 距上次检查永远达不到 24h，自动检查被永久饿死且没有任何提示
    if (s.updateLastCheck < 0 || s.updateLastCheck > (long long)time(nullptr) + 3600)
        s.updateLastCheck = 0;
}

bool LoadSettings(Settings& s)
{
    std::wstring path = SettingsFilePath();
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f)
    {
        LOG_INFO(L"未找到配置文件, 使用默认设置: %s", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024)
    {
        fclose(f);
        LOG_WARN(L"配置文件大小异常(%ld 字节), 忽略", size);
        return false;
    }
    std::vector<char> buf((size_t)size + 1, 0);
    size_t got = fread(buf.data(), 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    // 跳过 UTF-8 BOM
    const char* begin = buf.data();
    if (got >= 3 && (unsigned char)begin[0] == 0xEF &&
        (unsigned char)begin[1] == 0xBB && (unsigned char)begin[2] == 0xBF)
        begin += 3;

    Cursor cur{ begin, buf.data() + got };
    if (!cur.Eat('{'))
    {
        LOG_WARN(L"配置文件格式错误(缺少 '{'), 已忽略");
        return false;
    }

    int parsed = 0;
    // 第一个键之前没有逗号，故不能无条件 Eat(',')：那样光标停在 '"' 上，
    // 逗号与 '}' 都吃不到，循环会在读第一个键之前就 break —— 整个文件一个键都不加载，
    // 每次启动全部回到默认值（实测 27 个键 -> parsed=0，详见 AGNETS.md）
    bool needComma = false;
    while (cur.p < cur.end)
    {
        cur.SkipWs();
        if (cur.Eat('}')) break;                     // 空对象或已读到对象尾部
        if (needComma && !cur.Eat(',')) break;       // 键之间必须有分隔符，否则视为格式错误
        needComma = true;

        std::string key, val;
        if (!cur.ReadKey(key)) break;
        if (key.empty()) continue;

        // ReadKey 只吃到冒号为止，冒号与值之间可能有空格/换行。
        // 不先跳过空白，*cur.p 是空格而非 '"'，带引号的字符串会被当成标量读取，
        // 于是 key=="saveDir" 之类的分支全部落空（值变成 "\"D:\\xxx\""，atoi 归 0）
        cur.SkipWs();
        if (cur.p < cur.end && *cur.p == '"')
        {
            if (!cur.ReadString(val)) break;
            std::wstring wv = JsonUnescape(val);
            if      (key == "saveDir")       s.saveDir       = wv;
            else if (key == "recordPattern") s.recordPattern = wv;
            else if (key == "shotDir")       s.shotDir       = wv;
            else if (key == "shotPattern")   s.shotPattern   = wv;
            else if (key == "hotkeyCapture") s.hotkeyCapture = wv;
            else if (key == "hotkeyRecord")  s.hotkeyRecord  = wv;
            else if (key == "hotkeyShow")    s.hotkeyShow    = wv;
            else if (key == "sysAudioDevice") s.sysAudioDevice = wv;
            else if (key == "micDevice")     s.micDevice     = wv;
            else if (key == "captureMonitorDevice") s.captureMonitorDevice = wv;
            else if (key == "updateSkipTag")  s.updateSkipTag  = wv;
            else parsed++;
        }
        else
        {
            if (!cur.ReadScalar(val)) break;
            if      (key == "fps")           s.fps          = atoi(val.c_str());
            else if (key == "recResolution") s.recResolution = atoi(val.c_str());
            else if (key == "bitrateMbps")  s.bitrateMbps  = atoi(val.c_str());
            else if (key == "audioBitrate")  s.audioBitrate  = atoi(val.c_str());
            else if (key == "sysVolume")     s.sysVolume     = atoi(val.c_str());
            else if (key == "micVolume")     s.micVolume     = atoi(val.c_str());
            else if (key == "captureSource") s.captureSource = atoi(val.c_str());
            else if (key == "recDelaySec")   s.recDelaySec   = atoi(val.c_str());
            else if (key == "lastPage")      s.lastPage     = atoi(val.c_str());
            else if (key == "shotFormat")    s.shotFormat   = atoi(val.c_str());
            else if (key == "windowW")       s.windowW      = atoi(val.c_str());
            else if (key == "windowH")       s.windowH      = atoi(val.c_str());
            else if (key == "updateLastCheck") s.updateLastCheck = _strtoi64(val.c_str(), nullptr, 10);
            // withAudio 是旧配置里的系统声音开关，读取时映射到 withSystemAudio 保持兼容
            else if (key == "withAudio" || key == "withSystemAudio")
                                        s.withSystemAudio = (val == "true");
            // withMic 由 SaveSettings 写出，此前漏了读取分支导致麦克风开关无法持久化
            else if (key == "withMic")     s.withMic      = (val == "true");
            else if (key == "includeCursor") s.includeCursor= (val == "true");
            else if (key == "openFolderAfterRec") s.openFolderAfterRec = (val == "true");
            else if (key == "hwEncode")      s.hwEncode     = (val == "true");
            else if (key == "shotSaveFile")  s.shotSaveFile = (val == "true");
            else if (key == "shotCopyClip")  s.shotCopyClip = (val == "true");
            else if (key == "darkMode")      s.darkMode     = (val == "true");
            else if (key == "autoStart")     s.autoStart    = (val == "true");
            else if (key == "updateAutoCheck") s.updateAutoCheck = (val == "true");
            else parsed++;
        }
    }

    FillDefaults(s);
    LOG_INFO(L"配置已加载: %s (未识别键 %d)", path.c_str(), parsed);
    return true;
}

bool SaveSettings(const Settings& s)
{
    std::wstring path = SettingsFilePath();
    std::wstring dir, file;
    SplitPath(path, dir, file);
    if (!EnsureDir(dir))
    {
        LOG_ERR(L"无法创建配置目录: %s", dir.c_str());
        return false;
    }
    std::wstring tmp = path + L".tmp";

    FILE* f = _wfsopen(tmp.c_str(), L"wb", _SH_DENYNO);
    if (!f)
    {
        LOG_ERR(L"配置文件写入失败: %s", tmp.c_str());
        return false;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"saveDir\": \"%s\",\n",       JsonEscape(s.saveDir).c_str());
    fprintf(f, "  \"recordPattern\": \"%s\",\n", JsonEscape(s.recordPattern).c_str());
    fprintf(f, "  \"shotDir\": \"%s\",\n",       JsonEscape(s.shotDir).c_str());
    fprintf(f, "  \"shotPattern\": \"%s\",\n",   JsonEscape(s.shotPattern).c_str());
    fprintf(f, "  \"hotkeyCapture\": \"%s\",\n", JsonEscape(s.hotkeyCapture).c_str());
    fprintf(f, "  \"hotkeyRecord\": \"%s\",\n",  JsonEscape(s.hotkeyRecord).c_str());
    fprintf(f, "  \"hotkeyShow\": \"%s\",\n",    JsonEscape(s.hotkeyShow).c_str());
    fprintf(f, "  \"sysAudioDevice\": \"%s\",\n", JsonEscape(s.sysAudioDevice).c_str());
    fprintf(f, "  \"micDevice\": \"%s\",\n",    JsonEscape(s.micDevice).c_str());
    fprintf(f, "  \"captureMonitorDevice\": \"%s\",\n", JsonEscape(s.captureMonitorDevice).c_str());
    fprintf(f, "  \"captureSource\": %d,\n",    s.captureSource);
    fprintf(f, "  \"recDelaySec\": %d,\n",      s.recDelaySec);
    fprintf(f, "  \"fps\": %d,\n",              s.fps);
    fprintf(f, "  \"recResolution\": %d,\n",    s.recResolution);
    fprintf(f, "  \"bitrateMbps\": %d,\n",      s.bitrateMbps);
    fprintf(f, "  \"hwEncode\": %s,\n",         s.hwEncode ? "true" : "false");
    fprintf(f, "  \"withSystemAudio\": %s,\n", s.withSystemAudio ? "true" : "false");
    fprintf(f, "  \"withMic\": %s,\n",         s.withMic ? "true" : "false");
    fprintf(f, "  \"sysVolume\": %d,\n",       s.sysVolume);
    fprintf(f, "  \"micVolume\": %d,\n",       s.micVolume);
    fprintf(f, "  \"audioBitrate\": %d,\n",     s.audioBitrate);
    fprintf(f, "  \"includeCursor\": %s,\n",    s.includeCursor ? "true" : "false");
    fprintf(f, "  \"openFolderAfterRec\": %s,\n", s.openFolderAfterRec ? "true" : "false");
    fprintf(f, "  \"shotSaveFile\": %s,\n",     s.shotSaveFile ? "true" : "false");
    fprintf(f, "  \"shotCopyClip\": %s,\n",     s.shotCopyClip ? "true" : "false");
    fprintf(f, "  \"shotFormat\": %d,\n",       s.shotFormat);
    fprintf(f, "  \"lastPage\": %d,\n",         s.lastPage);
    fprintf(f, "  \"autoStart\": %s,\n",        s.autoStart ? "true" : "false");
    fprintf(f, "  \"darkMode\": %s,\n",         s.darkMode ? "true" : "false");
    fprintf(f, "  \"windowW\": %d,\n",          s.windowW);
    fprintf(f, "  \"windowH\": %d,\n",          s.windowH);
    fprintf(f, "  \"updateAutoCheck\": %s,\n",  s.updateAutoCheck ? "true" : "false");
    fprintf(f, "  \"updateLastCheck\": %lld,\n", s.updateLastCheck);
    fprintf(f, "  \"updateSkipTag\": \"%s\"\n", JsonEscape(s.updateSkipTag).c_str());
    fprintf(f, "}\n");
    fclose(f);

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        LOG_ERR(L"配置文件替换失败(0x%08lX): %s", GetLastError(), path.c_str());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void RecordOutputSize(unsigned srcW, unsigned srcH, int preset,
                      unsigned& outW, unsigned& outH)
{
    outW = outH = 0;
    if (srcW == 0 || srcH == 0)
        return;

    unsigned boxW = 0, boxH = 0;   // 档位上限（0 = 不限，取原画）
    switch (preset)
    {
    case RecRes1080P: boxW = 1920; boxH = 1080; break;
    case RecRes720P:  boxW = 1280; boxH = 720;  break;
    case RecRes1440P: boxW = 2560; boxH = 1440; break;
    case RecRes2160P: boxW = 3840; boxH = 2160; break;
    default: break;
    }

    if (boxW == 0 || boxH == 0)
    {
        outW = srcW & ~1u;
        outH = srcH & ~1u;   // H.264 要求宽高为偶数
        return;
    }

    // 等比装进档位框内，且只缩不放（源小于档位时按源尺寸录制）
    const double sx = (double)boxW / (double)srcW;
    const double sy = (double)boxH / (double)srcH;
    double s = sx < sy ? sx : sy;
    if (s > 1.0)
        s = 1.0;

    unsigned w = (unsigned)(srcW * s + 0.5);
    unsigned h = (unsigned)(srcH * s + 0.5);
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    outW = w & ~1u;
    outH = h & ~1u;
}

int RecordBitrateMbps(unsigned outW, unsigned outH)
{
    const double px = (double)outW * (double)outH;
    if (px >= 3840.0 * 2160.0) return 40;
    if (px >= 2560.0 * 1440.0) return 24;
    if (px >= 1920.0 * 1080.0) return 16;
    if (px >= 1280.0 * 720.0)  return 10;
    return 6;
}

int ResolveRecordBitrateMbps(const Settings& s, unsigned outW, unsigned outH)
{
    return s.bitrateMbps > 0 ? s.bitrateMbps : RecordBitrateMbps(outW, outH);
}

std::wstring PreviewRecordPath(const Settings& s)
{
    return BuildFilePath(s.saveDir, s.recordPattern, L".mp4", 1);
}

std::wstring PreviewShotPath(const Settings& s)
{
    std::wstring dir = s.shotDir.empty() ? s.saveDir : s.shotDir;
    return BuildFilePath(dir, StripImageExtension(s.shotPattern), ShotFormatExt(s.shotFormat), 1);
}
