#pragma once
// D3D11 设备与交换链封装
// 设备创建标志带 VIDEO_SUPPORT（MF 共享设备必需）与 BGRA_SUPPORT（ImGui/预览必需）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include "core/util.h"

class Graphics
{
public:
    bool Init(HWND hwnd, int width, int height);
    void Shutdown();
    void Resize(int width, int height);
    void BeginFrame();  // 清屏并绑定渲染目标
    void Present();

    ID3D11Device*        Device()  const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    // 设备是否已丢失（Present/BeginFrame 期间检测到）。置位后不会再翻转
    bool IsDeviceLost() const { return deviceLost_.load(); }
    // 主动询问驱动（开销极低，仅在怀疑丢失时调用）；返回 S_OK 表示设备仍在
    HRESULT QueryDeviceRemovedReason() const;

    // 由 CPU 端 BGRA 像素（4 字节/像素，紧密排列）创建纹理并返回 SRV，
    // 供 ImGui::Image 显示（截图缩略图等）
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
    CreateMemorySRV(const void* bgra, UINT width, UINT height);

private:
    void CreateRenderTarget();

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>        swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    // QueryDeviceRemovedReason() 是 const，但需要能在其中置位丢失标志
    mutable std::atomic<bool> deviceLost_{ false };
};
