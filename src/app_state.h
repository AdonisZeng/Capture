#pragma once
// 全局运行状态：录制状态机、预览/截图纹理、UI 动作请求、提示消息
// UI 层只读渲染并写"动作请求"，主循环消费后执行真实操作（跨线程职责单一）
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <vector>
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
    // 暂停：仍处 Recording 态（配置继续锁定），音视频回调直接丢帧，
    // 恢复时把 baseTime 前移暂停时长，时间轴无缝（暂停段被剪掉）
    std::atomic<bool> paused { false };
    // 录制起始时刻（100ns，MF 时钟基准，写帧时间戳用）。
    // 原子量：UI 线程写（开始录制 + 恢复时前移暂停时长），WGC 帧回调线程与
    // 音频采集线程读。暂停恢复会在录制中途改写它，普通 long long 属数据竞争
    std::atomic<long long> baseTime { 0 };
    ULONGLONG recordStartTick = 0;  // 录制起始的 GetTickCount64（UI 计时显示用）
    long long pauseStartTs = 0;     // 本次暂停开始的 MF 时钟时刻（绝对值，用于恢复时前移 baseTime）
    ULONGLONG pauseStartTick = 0;   // 本次暂停开始的 GetTickCount64（计时显示用，0 = 未暂停）
    long long pausedMsTotal = 0;    // 历史暂停累计毫秒（计时显示用，恢复时累加）

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
    bool wantPause       = false;   // 暂停录制（仅 Recording 且未暂停时有效）
    bool wantResume      = false;   // 继续录制（仅已暂停时有效）
    bool wantShot        = false;
    bool wantShowWindow  = false;
    bool wantSettingsPage = false;
    bool wantQuit        = false;
    bool wantCaptureRestart = false;  // 画面来源变化，空闲时重启预览捕获
    bool wantCursorRefresh  = false;  // 光标开关变化，实时刷到 WGC 会话
    bool wantMicTest        = false;  // 空闲时打开麦克风做电平试音（5 秒自动停）
    bool wantCancelCountdown = false; // 取消延时开始的倒计时

    // ---- 反馈 ----
    std::string  errPopup;      // 模态错误框（UTF-8）
    std::wstring toast;         // 状态栏提示（UTF-16）
    ULONGLONG    toastTick = 0;

    // ---- 画面来源（运行时）----
    HWND         capWindow = nullptr;  // 窗口捕获选中的窗口（会话内有效，不持久化）
    std::wstring capWindowTitle;       // 选中窗口标题（界面显示用）

    // ---- 延时开始的倒计时 ----
    bool      countdownActive = false;
    ULONGLONG countdownEnd = 0;        // GetTickCount64 到期时刻

    // ---- 电平表（0..100，主循环每 ~50ms 从采集器刷新）----
    std::atomic<int> sysLevel { 0 };
    std::atomic<int> micLevel { 0 };

    // ---- 历史（会话内，最多各 8 条）----
    std::vector<std::wstring> recentShots;
    std::vector<std::wstring> recentRecs;

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

// 历史记录压栈：去重后放最前，超 8 条丢最旧的（只记仍存在的路径由调用方保证）
inline void PushRecent(std::vector<std::wstring>& v, const std::wstring& path)
{
    if (path.empty())
        return;
    for (auto it = v.begin(); it != v.end(); ++it)
    {
        if (*it == path)
        {
            v.erase(it);
            break;
        }
    }
    v.insert(v.begin(), path);
    while (v.size() > 8)
        v.pop_back();
}

// 录制已耗时（秒，扣除全部暂停段）：录屏页/状态栏/托盘三处显示同源
inline long long RecElapsedSec(const AppState& st)
{
    const ULONGLONG now = GetTickCount64();
    long long ms = (long long)(now - st.recordStartTick) - st.pausedMsTotal;
    if (st.paused && st.pauseStartTick != 0)
        ms -= (long long)(now - st.pauseStartTick);
    return ms > 0 ? ms / 1000 : 0;
};
