#pragma once
// WASAPI 音频采集：扬声器 Loopback 与麦克风（eCapture）共用一套封装
// 输出 int16 PCM 48000Hz 双声道数据块（float 源自动转换，单声道源自动补成双声道）
// 时间戳：QPC -> 100ns（与视频帧同一时钟基准）
//
// 生命周期拆成两步是为了让录制入口先探测两路是否可用，再决定要不要给 MP4 建音轨：
//   Open（枚举设备/Activate/Initialize/GetService，调用线程）
//   -> Start（client->Start + 起采集线程）
//   -> Stop（停线程 + 释放设备，同步 join）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>

// 采集角色
enum class AudioRole
{
    System,   // 默认播放设备的回放混音（eRender + LOOPBACK）
    Mic       // 录音设备采集（eCapture）
};

// 一个音频端点（设备选择器用）
struct AudioDeviceInfo
{
    std::wstring id;    // IMMDevice::GetId 的设备 ID（写进 settings.json）
    std::wstring name;  // 友好名（界面显示）
    bool isDefault = false;
};

// 枚举音频端点：capture=false 列播放设备，true 列录音设备。
// 内部自行做 COM 初始化，可在任意线程调用；成功但列表为空表示系统没有该类设备
bool EnumerateAudioDevices(bool capture, std::vector<AudioDeviceInfo>& out);

class AudioCapture
{
public:
    // data: int16 PCM 交错样本块（恒为 48kHz/双声道/16bit）; ts100ns: 本块首样本时间戳
    using DataFn = std::function<void(const BYTE* data, UINT32 bytes, long long ts100ns)>;

    ~AudioCapture();

    // 打开设备但不开始取数。deviceId 为空表示系统默认设备；
    // 指定的设备已不存在时回退到默认设备。失败返回 false 并填 err。
    bool Open(AudioRole role, const std::wstring& deviceId, std::wstring& err);
    // 开始取数（必须在 Open 成功后调用）
    bool Start(DataFn onData);
    void Stop();

    bool IsOpen()    const { return client_ != nullptr; }
    bool IsRunning() const { return running_; }
    const std::wstring& DeviceId() const { return deviceId_; }

private:
    void ThreadProc();

    DataFn onData_;
    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopFlag_{ false };
    std::wstring deviceId_;

    // COM/设备对象只在 Open/Start/Stop（即线程外）访问
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    UINT frameBytes_ = 0;        // 源每帧字节（立体声 16bit 时为 4）
    UINT channels_ = 2;          // 源声道数（单声道源会在采集线程里补成双声道）
    bool floatSrc_ = false;
    bool deviceClockUsable_ = true;   // 设备的 qpcPos 是否可用（部分硬件恒不推进）
    long long lastDeviceTs_ = 0;      // 上一块的设备时间戳（用于检测是否推进）
    long long lastTs_ = 0;            // 上一块输出的时间戳（保证严格递增）
};
