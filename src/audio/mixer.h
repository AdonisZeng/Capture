#pragma once
// 系统声音 + 麦克风双路 PCM 混合器
//
// 场景：两路采集都开启时，MP4 里只有一条 AAC 音轨，故必须在写入前把两路
// 48kHz/双声道/16bit 的块按时间戳对齐后相加（增益 + 限幅），再交给 Recorder。
// 只用一路时不经过本类，直接透传该路（见 main.cpp 的路由逻辑）。
//
// 对齐策略（重要）：
//   输出时间轴从各路首块的「较晚者」起步，之后由「各路末尾的交集（最小值）」推进，
//   窗口内某路没有数据的部分按静音填充。这样两路才能真正按时间对齐后相加。
//   代价是某一路停摆会把输出拖住，因此对「落后输出超过 kLagFrames」的通道做剔除，
//   它恢复送数据时由 Push 重新纳入。
//   注意不能改用「各路末尾的最大值」推进：那样相位稍慢的一路会永远落在输出游标
//   之前而被整段跳过（实测麦克风会被系统声音吞掉）。也不能无条件等齐：Windows 的
//   loopback 在播放端无活动时根本不产包（实测），等下去整条音轨会卡在起点。
//
// 两路采集各在自己的线程调用 Push，故内部加锁；输出回调在锁内调用，
// 频率约每 10ms 一次、临界区内工作量是几 KB 的乘加，可接受。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX   // windows.h 的 min/max 宏与 std::min/std::max 冲突
#include <windows.h>
#include <functional>
#include <mutex>
#include <vector>

class AudioMixer
{
public:
    // data: int16 PCM 交错样本块（48kHz/双声道）; ts100ns: 本块首样本时间戳
    using DataFn = std::function<void(const BYTE* data, UINT32 bytes, long long ts100ns)>;

    // 混好后 PCM 的出口（通常是 Recorder::WriteAudio）；可在运行期随时设置
    void SetOutput(DataFn fn) { out_ = std::move(fn); }
    // 参与混音的通道。登记了但迟迟不送数据的通道会被自动降级（见 kStaleMs）
    void SetChannels(bool sys, bool mic);
    // 两路音量系数（1.0 = 原音量），录制开始前设置
    void Configure(float sysGain, float micGain);
    // 每次录制开始前清空缓冲与时间轴
    void Reset();

    // channel: 0 = 系统声音，1 = 麦克风（由采集线程调用）
    void Push(int channel, const BYTE* data, UINT32 bytes, long long ts100ns);

private:
    void Drain();   // 需持有 mutex_

    static constexpr int    kChannels = 2;
    static constexpr long long kRingFrames = 48000 * 4;   // 每路 4 秒环形缓冲
    static constexpr long long kMaxBlock  = 4096;        // 单次输出上限（帧）
    static constexpr ULONGLONG kWaitFirstMs = 200;      // 起步时等另一路到齐的窗口
    static constexpr ULONGLONG kStaleMs     = 200;      // 墙钟：超过该时长无数据即判停摆
    static constexpr long long kStaleFrames = 48000 / 5;   // = 200ms，恢复时用于对齐原点

    struct Chan
    {
        std::vector<INT16> ring = std::vector<INT16>((size_t)kRingFrames * 2);
        long long written   = 0;   // 已写入帧数（同时充当环形缓冲的写指针）
        long long firstTs   = 0;   // 时间原点：本路第 n 帧的时间为 firstTs + n*1e7/48000
        bool     started   = false;
    };

    std::mutex  mutex_;
    Chan        chan_[kChannels];
    bool        enabled_[kChannels] = { true, true };   // 登记参与混音的通道
    bool        active_[kChannels]  = { true, true };   // false = 已判定停摆，恢复后重置原点再用
    ULONGLONG   lastPushTick_[kChannels] = { 0, 0 };    // 各路最后一次送数据的墙钟时刻
    float       gain_[kChannels] = { 1.0f, 1.0f };
    DataFn      out_;
    bool        outStarted_ = false;
    long long   outTs0_    = 0;      // 输出时间轴原点（各路首块时间的较晚者）
    long long   outFrames_ = 0;      // 输出游标（帧）
    ULONGLONG   startTick_ = 0;      // 首次 Push 的墙钟时刻（用于首包超时降级）
    std::vector<INT16> mixBuf_;
    bool        firstLogged_ = false;
};
