#pragma once
// 全局运行状态：录制状态机、预览/截图纹理、UI 动作请求、提示消息
// UI 层只读渲染并写"动作请求"，主循环消费后执行真实操作（跨线程职责单一）
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include "screenshot/screenshot.h"

enum class RunState
{
    Idle,        // 空闲
    Recording,   // 录制中
    Saving,      // 停止写入，正在 Finalize
};

struct AppState
{
    // ---- 录制状态机：Idle -> Recording -> Saving -> Idle ----
    std::atomic<bool> recording { false };
    bool saving = false;
    long long baseTime = 0;         // 录制起始时刻（100ns，MF 时钟基准，写帧时间戳用）
    ULONGLONG recordStartTick = 0;  // 录制起始的 GetTickCount64（UI 计时显示用）

    // ---- 实时预览（来自 WGC 的最新帧，节流刷新）----
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previewSRV;
    ID3D11Texture2D* previewTex = nullptr;   // SRV 对应纹理（用于失效判断）
    UINT previewW = 0, previewH = 0;

    // ---- 上次截图 ----
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shotSRV;   // 缩略图
    ImageBGRA lastShot;            // CPU 像素（用于"再次复制到剪贴板"）
    UINT  shotW = 0, shotH = 0;
    std::wstring shotPath;          // 已保存的截图文件路径（空 = 只进了剪贴板）
    bool  shotCopied = false;       // 上次截图是否已复制到剪贴板
    ULONGLONG shotTick = 0;         // GetTickCount64，用于"刚刚"提示

    // ---- 应用图标（导航栏左上角标识，由 main 从 exe 资源生成为 D3D 纹理）----
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> appIconSRV;

    // ---- 统计 ----
    std::atomic<long long> frameCount { 0 };
    long long  lastFrameCount = 0;
    double     capFps = 0.0;
    ULONGLONG  lastFpsTick = 0;

    // ---- 音频状态提示（空 = 正常；两路各自独立，互不影响）----
    std::wstring audioSysInfo;   // 系统声音不可用的原因
    std::wstring audioMicInfo;   // 麦克风不可用的原因

    // ---- UI -> 主循环 的动作请求 ----
    bool wantStartRecord = false;
    bool wantStopRecord  = false;
    bool wantShot        = false;
    bool wantShowWindow  = false;
    bool wantSettingsPage = false;
    bool wantQuit        = false;

    // ---- 反馈 ----
    std::string  errPopup;      // 模态错误框（UTF-8）
    std::wstring toast;         // 状态栏提示（UTF-16）
    ULONGLONG    toastTick = 0;

    RunState State() const
    {
        if (recording) return RunState::Recording;
        if (saving)    return RunState::Saving;
        return RunState::Idle;
    }

    bool IsBusy() const { return recording || saving; }

    void SetToast(const std::wstring& text)
    {
        toast = text;
        toastTick = GetTickCount64();
    }
};
