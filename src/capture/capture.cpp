#include "capture.h"
#include "core/log.h"
#include <dxgi1_2.h>
#include <atomic>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// SDK interop 头中的 C++ 命名空间接口（非 winrt projection）
namespace dx3dinterop = ::Windows::Graphics::DirectX::Direct3D11;

ScreenCapture::~ScreenCapture()
{
    Stop();
}

bool ScreenCapture::Start(ID3D11Device* device, bool includeCursor,
                          FrameFn onFrame, std::wstring& err)
{
    try
    {
        device_ = device;
        onFrame_ = std::move(onFrame);

        // 主显示器 HMONITOR
        HMONITOR hmon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

        // IGraphicsCaptureItemInterop::CreateForMonitor：Win10 1803+ 全系可用
        auto interop = get_activation_factory<GraphicsCaptureItem,
                                              IGraphicsCaptureItemInterop>();
        com_ptr<::IInspectable> itemInspectable;
        check_hresult(interop->CreateForMonitor(
            hmon, guid_of<GraphicsCaptureItem>(), itemInspectable.put_void()));
        item_ = itemInspectable.as<GraphicsCaptureItem>();

        // ID3D11Device -> IDirect3DDevice（WGC 需要）
        com_ptr<IDXGIDevice> dxgiDevice;
        check_hresult(device_->QueryInterface(guid_of<IDXGIDevice>(), dxgiDevice.put_void()));
        com_ptr<::IInspectable> d3dInspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(),
                                                          d3dInspectable.put()));
        auto d3dDeviceForWgc = d3dInspectable.as<IDirect3DDevice>();

        framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3dDeviceForWgc, DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, item_.Size());
        frameRevoker_ = framePool_.FrameArrived(auto_revoke,
            { this, &ScreenCapture::OnFrameArrived });
        session_ = framePool_.CreateCaptureSession(item_);

        // 鼠标光标捕获（低版本 API 缺失时静默降级）
        try { session_.IsCursorCaptureEnabled(includeCursor); } catch (...) {}

        // 黄边框：裸 exe 无包身份时设置会抛异常，静默降级（保留黄框）
        try { session_.IsBorderRequired(false); } catch (...) {}

        session_.StartCapture();
        running_ = true;
        LOG_INFO(L"WGC 捕获已启动: 显示器 %ux%u, 光标=%d",
                 item_.Size().Width, item_.Size().Height, includeCursor ? 1 : 0);
        return true;
    }
    catch (hresult_error const& e)
    {
        err = std::wstring(L"WGC 捕获启动失败: ") + std::wstring(e.message().c_str());
        LOG_ERR(L"%s (hr=0x%08lX)", err.c_str(), (unsigned long)e.code().value);
        Stop();
        return false;
    }
    catch (...)
    {
        err = L"WGC 捕获启动失败: 未知错误";
        LOG_ERR(L"%s", err.c_str());
        Stop();
        return false;
    }
}

void ScreenCapture::Stop()
{
    if (running_)
        LOG_INFO(L"WGC 捕获停止");
    // 先撤销帧回调，再关闭会话，避免 Stop 后仍访问已释放对象
    frameRevoker_.revoke();
    if (session_) { try { session_.Close(); } catch (...) {} session_ = nullptr; }
    if (framePool_) { try { framePool_.Close(); } catch (...) {} framePool_ = nullptr; }
    item_ = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    latestTex_.Reset();
    latestW_ = latestH_ = 0;
    running_ = false;
}

void ScreenCapture::SetCursorCapture(bool enable)
{
    if (session_)
    {
        try
        {
            session_.IsCursorCaptureEnabled(enable);
            LOG_INFO(L"光标捕获已%s", enable ? L"开启" : L"关闭");
        }
        catch (...) {}
    }
}

void ScreenCapture::GetLatestFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTex,
                                   UINT& outW, UINT& outH)
{
    std::lock_guard<std::mutex> lock(mutex_);
    outTex = latestTex_;
    outW = latestW_;
    outH = latestH_;
}

void ScreenCapture::OnFrameArrived(const Direct3D11CaptureFramePool& sender,
                                   const winrt::Windows::Foundation::IInspectable&)
{
    Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
    if (!frame)
        return;

    try
    {
        auto access = frame.Surface().as<dx3dinterop::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> srcTex;
        check_hresult(access->GetInterface(guid_of<ID3D11Texture2D>(), srcTex.put_void()));

        // 帧纹理可能大于内容区（对齐填充），纹理尺寸 - ContentSize = 双侧留边
        D3D11_TEXTURE2D_DESC srcDesc = {};
        srcTex->GetDesc(&srcDesc);
        const UINT contentW = static_cast<UINT>(frame.ContentSize().Width);
        const UINT contentH = static_cast<UINT>(frame.ContentSize().Height);
        if (contentW == 0 || contentH == 0 || contentW > srcDesc.Width || contentH > srcDesc.Height)
            return;
        const UINT offsetX = (srcDesc.Width - contentW) / 2;
        const UINT offsetY = (srcDesc.Height - contentH) / 2;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> newTex;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!latestTex_ || contentW != latestW_ || contentH != latestH_)
            {
                // 画面源尺寸变化（切分辨率 / 插拔显示器 / 旋转屏幕）：
                // 正在进行的录制会因编码器尺寸不再匹配而被中止，必须留痕便于事后定位
                if (latestW_ != 0 && (contentW != latestW_ || contentH != latestH_))
                    LOG_WARN(L"画面源尺寸变化: %ux%u -> %ux%u（录制中的文件将被中止）",
                             latestW_, latestH_, contentW, contentH);

                D3D11_TEXTURE2D_DESC desc = srcDesc;
                desc.Width = contentW;
                desc.Height = contentH;
                // RENDER_TARGET: 部分 MF 硬件编码 MFT 要求输入纹理可作渲染目标
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                desc.MiscFlags = 0;
                desc.CPUAccessFlags = 0;

                Microsoft::WRL::ComPtr<ID3D11Texture2D> created;
                if (FAILED(device_->CreateTexture2D(&desc, nullptr, created.GetAddressOf())))
                {
                    LOG_ERR(L"创建帧纹理副本失败 (%ux%u)", contentW, contentH);
                    return;
                }
                latestTex_ = created;
                latestW_ = contentW;
                latestH_ = contentH;
            }
            newTex = latestTex_;
        }

        // 拷贝内容区（GPU-GPU blit，极低成本）。latestTex_ 重建只发生在本回调线程，
        // 因此锁外使用 newTex 安全
        D3D11_BOX srcBox;
        srcBox.left = offsetX;
        srcBox.top = offsetY;
        srcBox.front = 0;
        srcBox.right = offsetX + contentW;
        srcBox.bottom = offsetY + contentH;
        srcBox.back = 1;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        device_->GetImmediateContext(ctx.GetAddressOf());
        ctx->CopySubresourceRegion(newTex.Get(), 0, 0, 0, 0,
                                   srcTex.get(), 0, &srcBox);

        long long ts = frame.SystemRelativeTime().count(); // 已是 100ns 单位（QPC 基准）
        if (!firstFrameLogged_)
        {
            firstFrameLogged_ = true;
            LOG_INFO(L"收到首帧: 内容区 %ux%u", contentW, contentH);
        }
        if (onFrame_)
            onFrame_(newTex.Get(), ts);
    }
    catch (...)
    {
        // 单帧异常不终止会话
        static std::atomic<int> logCount{ 0 };
        if (logCount.fetch_add(1) < 3)
            LOG_ERR(L"帧处理发生异常(第 %d 次)", logCount.load());
    }
}
