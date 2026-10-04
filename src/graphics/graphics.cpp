#include "graphics.h"
#include "core/log.h"
#include <cstring>

bool Graphics::Init(HWND hwnd, int width, int height)
{
    hwnd_ = hwnd;
    // 每次 Init 都是全新设备：清掉旧设备的丢失标志（进程内恢复会走到这里）
    deviceLost_ = false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    // 第一步：创建设备（FLIP 交换链需 DESC1，无法用 D3D11CreateDeviceAndSwapChain 一步创建）
    D3D_FEATURE_LEVEL flGot{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        device_.GetAddressOf(), &flGot, context_.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_WARN(L"D3D11 设备创建失败(hr=0x%08lX), 降低要求重试", hr);
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            device_.GetAddressOf(), &flGot, context_.GetAddressOf());
        if (FAILED(hr))
        {
            LOG_ERR(L"D3D11 设备创建彻底失败(hr=0x%08lX)", hr);
            return false;
        }
    }
    LOG_INFO(L"D3D11 设备创建成功: 特性级别 0x%04X", flGot);

    // 记录 GPU 适配器信息（排查显卡/编码器相关问题时有用）
    {
        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(device_.As(&dxgi)) &&
            SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf())) &&
            SUCCEEDED(adapter->GetDesc(&ad)))
        {
            LOG_INFO(L"GPU 适配器: %s (专用显存 %u MB)",
                     ad.Description, ad.DedicatedVideoMemory / (1024 * 1024));
        }
    }

    // MF / WGC 跨线程使用同一设备前必须开启多线程保护
    Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(context_.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    // 第二步：经 DXGI 工厂创建 FLIP_DISCARD 交换链
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice)) ||
        FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
    {
        LOG_ERR(L"获取 DXGI 工厂失败");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width              = static_cast<UINT>(width);
    sd.Height             = static_cast<UINT>(height);
    sd.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count   = 1;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount        = 2;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr,
                                               nullptr, swapChain_.GetAddressOf())))
    {
        LOG_ERR(L"交换链创建失败");
        return false;
    }

    CreateRenderTarget();
    LOG_INFO(L"交换链创建成功: %dx%d", width, height);
    return rtv_ != nullptr;
}

void Graphics::CreateRenderTarget()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv_.GetAddressOf());
}

void Graphics::Resize(int width, int height)
{
    if (!swapChain_ || width <= 0 || height <= 0)
        return;
    rtv_.Reset();
    if (SUCCEEDED(swapChain_->ResizeBuffers(0, (UINT)width, (UINT)height,
                                            DXGI_FORMAT_UNKNOWN, 0)))
        CreateRenderTarget();
}

HRESULT Graphics::QueryDeviceRemovedReason() const
{
    if (!device_)
        return S_OK;
    // GetDeviceRemovedReason 挂在 ID3D11Device 上（不是 Context）：
    // 设备健在时返回 S_OK，真丢失时返回具体原因
    HRESULT hr = device_->GetDeviceRemovedReason();
    if (IsDeviceLostHr(hr))
        deviceLost_ = true;
    return hr;
}

void Graphics::BeginFrame()
{
    if (!rtv_)
        return;
    const FLOAT clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
}

void Graphics::Present()
{
    if (!swapChain_)
        return;
    HRESULT hr = swapChain_->Present(1, 0);
    if (FAILED(hr) && IsDeviceLostHr(hr))
    {
        // 只在首次检测到时记录，避免死设备上每帧刷屏
        const bool first = !deviceLost_.exchange(true);
        if (first)
        {
            // Present 的错误码不如 GetDeviceRemovedReason 精确，补问一次拿到真实原因
            const HRESULT reason = QueryDeviceRemovedReason();
            LOG_ERR(L"D3D11 设备已丢失 (Present hr=0x%08lX, removed reason=0x%08lX)", hr, reason);
        }
    }
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
Graphics::CreateMemorySRV(const void* bgra, UINT width, UINT height)
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (!device_ || !bgra || width == 0 || height == 0)
        return srv;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, tex.GetAddressOf())))
        return srv;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return srv;
    const size_t rowBytes = (size_t)width * 4;
    const BYTE* src = (const BYTE*)bgra;
    if (mapped.RowPitch == rowBytes)
    {
        memcpy(mapped.pData, src, rowBytes * height);
    }
    else
    {
        for (UINT y = 0; y < height; ++y)
            memcpy((BYTE*)mapped.pData + (size_t)mapped.RowPitch * y, src + rowBytes * y, rowBytes);
    }
    context_->Unmap(tex.Get(), 0);

    if (FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf())))
        return Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>();
    return srv;
}

void Graphics::Shutdown()
{
    rtv_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}
