#pragma once
// Windows.Graphics.Capture 全屏捕获封装
// 帧到达回调（WGC 工作线程）输出内容尺寸的 BGRA D3D11 纹理副本 + 100ns 系统时间戳
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// 一台显示器（设备名可持久化，HMONITOR 会话内有效）
struct MonitorInfo
{
    HMONITOR     hmon = nullptr;
    std::wstring device;   // 如 L"\\\\.\\DISPLAY1"（写进 settings.json）
    std::wstring label;    // 界面显示，如 L"DISPLAY1 · 2560×1440（主）"
    bool         primary = false;
};

// 一个可捕获的顶层窗口（HWND 会话内有效，不持久化）
struct WindowInfo
{
    HWND         hwnd = nullptr;
    std::wstring title;
};

// 主显示器排第一。失败返回 false（out 清空）
bool EnumerateMonitors(std::vector<MonitorInfo>& out);
// 按设备名找 HMONITOR；为空或找不到时回退主显示器（一定成功则返回非空）
HMONITOR FindMonitor(const std::wstring& device);
// 列可见顶层窗口（跳过无标题/最小化/本程序主窗口）。失败返回 false
bool EnumerateWindows(std::vector<WindowInfo>& out, HWND exclude);

class ScreenCapture
{
public:
    using FrameFn = std::function<void(ID3D11Texture2D* tex, long long ts100ns)>;

    ~ScreenCapture();

    // 启动指定显示器 / 指定窗口捕获。onFrame 在 WGC 工作线程回调。
    // 纹理由内部持有，回调内不得长期保存指针；需要保留请在回调内自行拷贝。
    // 两个入口互斥，且内部先 Stop 旧会话，调用前无需自行 Stop
    bool StartForMonitor(HMONITOR hmon, ID3D11Device* device, bool includeCursor,
                         FrameFn onFrame, std::wstring& err);
    bool StartForWindow(HWND hwnd, ID3D11Device* device, bool includeCursor,
                        FrameFn onFrame, std::wstring& err);
    void Stop();
    void SetCursorCapture(bool enable);

    bool IsRunning() const { return running_; }
    // 取最新帧快照（锁内拷贝 ComPtr，线程安全）
    void GetLatestFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTex,
                        UINT& outW, UINT& outH);

private:
    void OnFrameArrived(
        const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender,
        const winrt::Windows::Foundation::IInspectable&);

    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latestTex_;   // 内容尺寸的帧副本（稳定复用）
    UINT latestW_ = 0, latestH_ = 0;
    FrameFn onFrame_;

    // 会话配置：StartForMonitor/StartForWindow 统一汇总到 StartWithItem
    bool StartWithItem(const winrt::Windows::Graphics::Capture::GraphicsCaptureItem& item,
                       ID3D11Device* device, bool includeCursor, FrameFn onFrame,
                       const wchar_t* srcDesc, std::wstring& err);

    ID3D11Device* device_ = nullptr;          // 外部持有，不 AddRef
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{ nullptr };
    bool running_ = false;
    bool firstFrameLogged_ = false;   // 仅记录首帧日志，避免刷屏
};
