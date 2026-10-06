#include "hotkey.h"
#include "settings/settings.h"
#include "core/util.h"
#include "core/log.h"
#include <windows.h>
#include <vector>
#include <string>
#include <cwctype>

namespace {

constexpr UINT HOTKEY_MOD_CTRL  = 0x0002;
constexpr UINT HOTKEY_MOD_ALT   = 0x0001;
constexpr UINT HOTKEY_MOD_SHIFT = 0x0004;
constexpr UINT HOTKEY_MOD_WIN   = 0x0008;

std::wstring Upper(std::wstring s)
{
    for (wchar_t& c : s)
        c = (wchar_t)towupper(c);
    return s;
}

std::wstring Trim(std::wstring s)
{
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos)
        return L"";
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

// 单个键名 -> 虚拟键码；失败返回 0
UINT KeyFromName(const std::wstring& name)
{
    if (name.empty()) return 0;
    if (name.size() == 1)
    {
        wchar_t c = name[0];
        if (c >= L'A' && c <= L'Z') return (UINT)c;
        if (c >= L'0' && c <= L'9') return (UINT)c;
        return 0;
    }
    if (name[0] == L'F' && name.size() >= 2 && name.size() <= 3)
    {
        int n = _wtoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return (UINT)(VK_F1 + n - 1);
        return 0;
    }
    if (name == L"SPACE") return VK_SPACE;
    if (name == L"INSERT") return VK_INSERT;
    if (name == L"DELETE" || name == L"DEL") return VK_DELETE;
    if (name == L"PAUSE") return VK_PAUSE;
    if (name == L"TAB") return VK_TAB;
    if (name == L"HOME") return VK_HOME;
    if (name == L"END") return VK_END;
    if (name == L"PRTSC" || name == L"PRINT" || name == L"SNAPSHOT") return VK_SNAPSHOT;
    return 0;
}

std::wstring KeyName(UINT vk)
{
    wchar_t buf[16];
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        swprintf(buf, 16, L"F%d", (int)(vk - VK_F1 + 1));
        return buf;
    }
    if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9'))
    {
        buf[0] = (wchar_t)vk;
        buf[1] = 0;
        return buf;
    }
    switch (vk)
    {
    case VK_SPACE:    return L"Space";
    case VK_INSERT:   return L"Insert";
    case VK_DELETE:   return L"Delete";
    case VK_PAUSE:    return L"Pause";
    case VK_TAB:      return L"Tab";
    case VK_HOME:     return L"Home";
    case VK_END:      return L"End";
    case VK_SNAPSHOT: return L"PrtSc";
    default:          return L"?";
    }
}

}   // namespace

bool HotkeyManager::Parse(const std::wstring& text, UINT& mods, UINT& vk)
{
    mods = 0;
    vk = 0;
    if (text.empty())
        return false;

    // 按 '+' 切分：末段为键名，其余为修饰键（重复出现自动合并）
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true)
    {
        size_t pos = text.find(L'+', start);
        if (pos == std::wstring::npos)
        {
            parts.push_back(Upper(Trim(text.substr(start))));
            break;
        }
        parts.push_back(Upper(Trim(text.substr(start, pos - start))));
        start = pos + 1;
    }
    if (parts.size() < 2)   // 至少需要「一个修饰键 + 一个键」
        return false;

    UINT key = KeyFromName(parts.back());
    if (key == 0)
        return false;
    vk = key;

    for (size_t i = 0; i + 1 < parts.size(); ++i)
    {
        const std::wstring& m = parts[i];
        if (m == L"CTRL" || m == L"CONTROL")             mods |= HOTKEY_MOD_CTRL;
        else if (m == L"ALT")                              mods |= HOTKEY_MOD_ALT;
        else if (m == L"SHIFT")                            mods |= HOTKEY_MOD_SHIFT;
        else if (m == L"WIN" || m == L"CMD" || m == L"META") mods |= HOTKEY_MOD_WIN;
        else return false;   // 未知修饰键
    }
    return true;
}

std::wstring HotkeyManager::Format(UINT mods, UINT vk)
{
    std::wstring out;
    if (mods & HOTKEY_MOD_CTRL)  out += L"Ctrl+";
    if (mods & HOTKEY_MOD_ALT)   out += L"Alt+";
    if (mods & HOTKEY_MOD_SHIFT) out += L"Shift+";
    if (mods & HOTKEY_MOD_WIN)   out += L"Win+";
    out += KeyName(vk);
    return out;
}

bool HotkeyManager::IsValidText(const std::wstring& text)
{
    UINT mods = 0, vk = 0;
    return Parse(text, mods, vk);
}

bool HotkeyManager::Compose(bool ctrl, bool alt, bool shift, bool win, UINT vk,
                            std::wstring& out)
{
    out.clear();
    UINT mods = 0;
    if (ctrl)  mods |= HOTKEY_MOD_CTRL;
    if (alt)   mods |= HOTKEY_MOD_ALT;
    if (shift) mods |= HOTKEY_MOD_SHIFT;
    if (win)   mods |= HOTKEY_MOD_WIN;
    if (mods == 0)
        return false;   // 裸键不允许：既易与正常打字冲突，RegisterHotKey 也多半被系统占用
    if (KeyName(vk) == L"?")
        return false;   // 主键不在支持集内（与 Parse 的 KeyFromName 同源）
    out = Format(mods, vk);
    return true;
}

bool HotkeyManager::BindOne(HWND hwnd, int id, const std::wstring& text,
                            std::wstring& out, std::wstring& err)
{
    const wchar_t* fallback[] = { L"Win+Shift+", L"Ctrl+Alt+", L"Alt+Shift+" };

    UINT mods = 0, vk = 0;
    if (Parse(text, mods, vk))
    {
        std::wstring canon = Format(mods, vk);
        if (RegisterHotKey(hwnd, id, mods, vk) == TRUE)
        {
            out = canon;
            LOG_INFO(L"热键已注册: %ls", canon.c_str());
            return true;
        }
        LOG_WARN(L"热键 %ls 注册失败(0x%08lX), 尝试回退", canon.c_str(), GetLastError());
        err = canon + L" 已被其他程序占用";
    }
    else
    {
        err = text + L" 格式无法识别";
        LOG_WARN(L"热键串无法解析: %ls", text.c_str());
    }

    // 取配置串的末段作为回退键名
    std::wstring tail = text;
    size_t pos = tail.find_last_of(L'+');
    if (pos != std::wstring::npos)
        tail = tail.substr(pos + 1);
    tail = Upper(Trim(tail));
    if (KeyFromName(tail) == 0)
        tail = L"A";

    for (const wchar_t* prefix : fallback)
    {
        std::wstring cand = std::wstring(prefix) + tail;
        UINT m = 0, v = 0;
        if (!Parse(cand, m, v))
            continue;
        if (RegisterHotKey(hwnd, id, m, v) == TRUE)
        {
            out = Format(m, v);
            LOG_INFO(L"热键回退生效: %ls", out.c_str());
            return true;
        }
    }
    LOG_ERR(L"热键 %ls 及其所有回退组合均注册失败", text.c_str());
    return false;
}

void HotkeyManager::Apply(HWND hwnd, Settings& s)
{
    Unregister();
    hwnd_ = hwnd;

    // 三个热键必须互不相同：逐个绑定，与已生效者重复的（后注册者）解绑
    const int ids[3] = { HK_CAPTURE_ID, HK_RECORD_ID, HK_SHOW_ID };
    std::wstring* outs[3] = { &s.hotkeyCapture, &s.hotkeyRecord, &s.hotkeyShow };
    std::wstring results[3];
    bool oks[3] = { false, false, false };
    std::string errors;

    for (int i = 0; i < 3; ++i)
    {
        std::wstring err;
        oks[i] = BindOne(hwnd, ids[i], *outs[i], results[i], err);
        if (oks[i])
            *outs[i] = results[i];      // 生效值（可能被回退改写）
        else
            *outs[i] = L"";             // 未生效，UI 置灰提示
        if (!oks[i])
            errors += WideToUtf8Str(err) + "; ";
    }
    for (int i = 0; i < 3; ++i)
    {
        if (!oks[i]) continue;
        for (int j = i + 1; j < 3; ++j)
        {
            if (!oks[j]) continue;
            if (_wcsicmp(results[i].c_str(), results[j].c_str()) == 0)
            {
                UnregisterHotKey(hwnd, ids[j]);
                oks[j] = false;
                *outs[j] = L"";
                errors += WideToUtf8Str(results[i]) + " 重复使用; ";
                LOG_WARN(L"热键重复, 已解绑: %ls", results[j].c_str());
            }
        }
    }

    boundCapture_ = oks[0];
    boundRecord_  = oks[1];
    boundShow_    = oks[2];
    idCapture_ = oks[0] ? ids[0] : 0;
    idRecord_  = oks[1] ? ids[1] : 0;
    idShow_    = oks[2] ? ids[2] : 0;

    LOG_INFO(L"全局热键生效值: 截图=%ls 录制=%ls 显示=%ls",
             s.hotkeyCapture.c_str(), s.hotkeyRecord.c_str(), s.hotkeyShow.c_str());
    if (!errors.empty())
        LOG_WARN(L"热键部分未生效: %s", errors.c_str());
}

void HotkeyManager::Unregister()
{
    if (!hwnd_)
        return;
    if (idCapture_) { UnregisterHotKey(hwnd_, idCapture_); idCapture_ = 0; }
    if (idRecord_)  { UnregisterHotKey(hwnd_, idRecord_);  idRecord_  = 0; }
    if (idShow_)    { UnregisterHotKey(hwnd_, idShow_);    idShow_    = 0; }
    boundCapture_ = boundRecord_ = boundShow_ = false;
}
