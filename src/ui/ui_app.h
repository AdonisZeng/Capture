#pragma once
// UI 主框架：左侧导航 + 内容区 + 底部状态栏 + 错误弹窗
#include <windows.h>
#include <string>

struct AppState;
struct Settings;

// 目录选择对话框（shell32 的 SHBrowseForFolder）；取消返回 false
bool PickFolderDialog(HWND ownerHwnd, std::wstring& dir);

// 页面渲染所需的上下文（由 UiApp 持有并转发给各页面）
struct UiContext
{
    AppState*  st = nullptr;   // 运行状态
    Settings*  cfg = nullptr;  // 用户设置
    HWND       ownerHwnd = nullptr;   // 父窗口（目录对话框用）
    // 实际注册生效的全局热键（注册失败时为空串）
    std::wstring hotkeyCapture, hotkeyRecord, hotkeyShow;
    bool* hotkeyDirty = nullptr;   // 置 true 表示需要重新注册热键
};

class UiApp
{
public:
    void Init(AppState* st, Settings* cfg, HWND ownerHwnd);
    void Shutdown();

    // 同步实际生效的热键文本（HotkeyManager::Apply 之后调用）
    void SetHotkeyText(const std::wstring& capture, const std::wstring& record,
                       const std::wstring& show);
    // 切换到指定页面（0=截屏 1=录屏 2=设置）
    void SetPage(int page);
    // 由主循环消费：是否需要重新注册热键
    bool ConsumeHotkeyDirty();

    // 每帧绘制（须在 ImGui::NewFrame 之后）
    void Draw();

    int Page() const { return page_; }

private:
    void DrawNav(float height);
    void DrawStatusBar(float width);
    void DrawErrorPopup();

    AppState* st_ = nullptr;
    Settings* cfg_ = nullptr;
    int  page_ = 0;
    bool hotkeyDirty_ = false;
    UiContext ctx_;

    // 状态栏上一次的提示（用于自动过期）
    std::wstring lastToast_;
};
