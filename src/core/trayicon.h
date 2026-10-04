#pragma once
// 系统托盘：动态生成图标 + 右键菜单，关闭按钮转最小化
#include <windows.h>
#include <string>

// 托盘消息（发往主窗口；wParam = TrayAction，lParam = 原始鼠标事件）
#define WM_TRAYICON (WM_APP + 110)

enum class TrayAction
{
    None = 0,
    Show,       // 显示/激活主窗口
    Capture,    // 触发截图
    Record,     // 开始/停止录制
    Settings,   // 打开设置（跳到"关于与设置"）
    Quit,       // 退出程序
    Pause,      // 暂停/继续录制（仅录制中有效）
    OpenDir,    // 打开输出目录
    // 注意：本成员必须是最后一个。OnMessage 用「值落在 Show..CheckUpdate 区间」
    // 来识别主动投递的动作，新增成员必须同步放宽区间上界，否则菜单点了没反应
    CheckUpdate,// 检查更新（跳到设置页并立即发起一次检查）
};

class TrayIcon
{
public:
    ~TrayIcon();

    bool Create(HWND hwnd);
    void Destroy();
    bool Valid() const { return hwnd_ != nullptr; }

    // 更新提示文字与图标颜色（录制中显示红点）
    void Update(const std::wstring& tooltip, bool recording);
    // 暂停态（录制中有效）：菜单文案在 暂停/继续 之间切换
    void SetPaused(bool paused) { paused_ = paused; }
    // 气泡通知（title/msg 为 UTF-16 文本）
    void Notify(const std::wstring& title, const std::wstring& msg);

    // 转发给 TrayIcon::OnMessage，返回 true 表示已处理
    // wParam 用 WPARAM 而非 UINT：x64 下 WPARAM 是 UINT_PTR，窄化成 UINT 会被截断
    bool OnMessage(WPARAM wParam, LPARAM lParam, TrayAction& action);

private:
    HICON MakeIcon(UINT cx, UINT cy, bool recording) const;
    void   ShowContextMenu();

    HWND        hwnd_ = nullptr;
    UINT        msgId_ = WM_TRAYICON;
    HICON       icon_ = nullptr;
    HICON       iconRecording_ = nullptr;
    std::wstring tip_;
    bool        recording_ = false;
    bool        paused_ = false;   // 录制暂停态（菜单文案用）
    bool        added_ = false;
};
