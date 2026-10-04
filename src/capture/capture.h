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

class ScreenCapture
{
public:
    using FrameFn = std::function<void(ID3D11Texture2D* tex, long long ts100ns)>;

    ~ScreenCapture();

    // 启动主显示器捕获（预览常开）。onFrame 在 WGC 工作线程回调。
    // 纹理由内部持有，回调内不得长期保存指针；需要保留请在回调内自行拷贝。
    bool Start(ID3D11Device* device, bool includeCursor, FrameFn onFrame, std::wstring& err);
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

    ID3D11Device* device_ = nullptr;          // 外部持有，不 AddRef
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{ nullptr };
    bool running_ = false;
    bool firstFrameLogged_ = false;   // 仅记录首帧日志，避免刷屏
};
