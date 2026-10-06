#pragma once
// 全局快捷键：解析/格式化热键串，注册失败时自动回退到候选组合
// 热键通过 WM_HOTKEY 投递到主窗口，wParam 为下列 ID
#include <windows.h>
#include <string>

struct Settings;

#define HK_CAPTURE_ID 1   // 触发截图
#define HK_RECORD_ID  2   // 开始/停止录制
#define HK_SHOW_ID    3   // 显示主窗口

class HotkeyManager
{
public:
    // 按 settings 重新注册全部热键。注册成功时 settings 保持不变；
    // 与系统冲突时自动改用候选组合，并就地写回 settings（保证 UI 显示的是实际生效值）。
    void Apply(HWND hwnd, Settings& s);
    void Unregister();

    bool IsCaptureBound() const { return boundCapture_; }
    bool IsRecordBound()  const { return boundRecord_; }
    bool IsShowBound()    const { return boundShow_; }

    // "Ctrl+Alt+S" -> (mods, vk)。支持 Ctrl/Alt/Shift/Win + A-Z 0-9 F1-F12 Space Insert Delete
    static bool Parse(const std::wstring& text, UINT& mods, UINT& vk);
    static std::wstring Format(UINT mods, UINT vk);
    // 由按键捕获直接组装规范串（至少需要一个修饰键，主键须在支持集内）；失败返回 false
    static bool Compose(bool ctrl, bool alt, bool shift, bool win, UINT vk,
                        std::wstring& out);
    // 文本可被解析
    static bool IsValidText(const std::wstring& text);

private:
    // 依次尝试 text 及候选组合，成功时把实际值写回 out
    static bool BindOne(HWND hwnd, int id, const std::wstring& text,
                        std::wstring& out, std::wstring& err);
    HWND hwnd_ = nullptr;
    int  idCapture_ = 0, idRecord_ = 0, idShow_ = 0;
    bool boundCapture_ = false, boundRecord_ = false, boundShow_ = false;
};
