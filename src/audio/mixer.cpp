#include "mixer.h"
#include "core/log.h"
#include <algorithm>
#include <cmath>
#include <climits>

namespace {
// 帧数 -> 时长（100ns）
inline long long FramesToTime(long long frames)
{
    return frames * 10000000 / 48000;
}
// 时间（100ns）-> 帧数
inline long long TimeToFrames(long long ts100ns)
{
    return ts100ns * 48000 / 10000000;
}
}   // namespace

void AudioMixer::SetChannels(bool sys, bool mic)
{
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_[0] = sys;
    enabled_[1] = mic;
}

void AudioMixer::Configure(float sysGain, float micGain)
{
    std::lock_guard<std::mutex> lock(mutex_);
    gain_[0] = sysGain;
    gain_[1] = micGain;
}

void AudioMixer::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < kChannels; ++i)
    {
        chan_[i].written = 0;
        chan_[i].firstTs = 0;
        chan_[i].started = false;
        active_[i] = true;
        lastPushTick_[i] = 0;
        // ring 内容无需清零：只读取 [0, written) 区间的帧
    }
    outStarted_ = false;
    outFrames_  = 0;
    startTick_  = 0;
    mixBuf_.clear();
    firstLogged_ = false;
}

void AudioMixer::Push(int channel, const BYTE* data, UINT32 bytes, long long ts100ns)
{
    if (channel < 0 || channel >= kChannels || !data || bytes < 4)
        return;
    if (ts100ns < 0)
        ts100ns = 0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_[channel])
        return;
    if (startTick_ == 0)
        startTick_ = GetTickCount64();
    lastPushTick_[channel] = GetTickCount64();

    Chan& c = chan_[channel];
    const long long frames = (long long)(bytes / 4);   // 源恒为双声道 16bit，每帧 4 字节

    if (!active_[channel])
    {
        // 该路之前因停摆被剔除。恢复时原点取数据时刻，并允许最多 kStaleFrames 的落后：
        // 输出游标只前进不回头，原点若直接取当前游标，该路会永久比另一路慢一段，
        // 在刚追平时又被判定停摆，表现为该路声音一闪一闪
        const long long curTs = outStarted_ ? (outTs0_ + FramesToTime(outFrames_))
                                            : ts100ns;
        const long long base = std::max(ts100ns, curTs - FramesToTime(kStaleFrames));
        c.started = true;
        c.written = 0;
        c.firstTs = base;
        active_[channel] = true;
        LOG_WARN(L"音频通道 %d 恢复, 时间原点对齐到 %+lld/100ns 后重新参与混音",
                 channel, base - ts100ns);
    }
    else if (!c.started)
    {
        c.started = true;
        c.firstTs = ts100ns;
        c.written = 0;
    }
    else
    {
        // 时间戳与已写帧数必须自洽，否则后面的对齐全错；偏差过大时以时间戳为准平移原点。
        // 阈值放大到 200ms：用 QPC 到达时刻做时间源时，包间隔天然有几毫秒抖动，
        // 阈值太小会每块都触发重置，把输出时间轴反复拽回去
        const long long expect = c.firstTs + FramesToTime(c.written);
        const long long drift  = ts100ns - expect;
        if (drift > 20000000 || drift < -20000000)
        {
            LOG_WARN(L"音频通道 %d 时间戳跳变(%+lld/100ns), 已按新时间戳对齐", channel, drift);
            c.firstTs = ts100ns - FramesToTime(c.written);
        }
    }

    const INT16* src = (const INT16*)data;
    for (long long i = 0; i < frames; ++i)
    {
        INT16* dst = &c.ring[(size_t)((c.written + i) % kRingFrames) * 2];
        dst[0] = src[i * 2];
        dst[1] = src[i * 2 + 1];
    }
    c.written += frames;

    Drain();
}

void AudioMixer::Drain()
{
    if (!out_)
        return;

    if (!outStarted_)
    {
        // 起步时只等一小段窗口让另一路也到齐（避免开头缺一路听起来忽远忽近）。
        // 不能无条件等齐：系统声音在播放端无活动时 loopback 根本不产数据，
        // 等下去整条音轨会永远卡在起点（实测坑，见文件头注释）
        if (startTick_ != 0 && GetTickCount64() - startTick_ < kWaitFirstMs)
        {
            for (int i = 0; i < kChannels; ++i)
                if (enabled_[i] && !chan_[i].started)
                    return;
        }
        bool any = false;
        for (int i = 0; i < kChannels; ++i)
        {
            if (!enabled_[i] || !chan_[i].started)
                continue;
            outTs0_ = any ? std::max(outTs0_, chan_[i].firstTs) : chan_[i].firstTs;
            any = true;
        }
        if (!any)
            return;
        outStarted_ = true;
    }

    long long outTs = outTs0_ + FramesToTime(outFrames_);

    // 上界取「各路都已就绪的交集」= 各路末尾的最小值，窗口内缺失的部分填静音。
    // 这样相位稍慢的一路不会被整段跳过（取最大值会导致它永远落在游标之前）。
    // 代价是停摆的一路会拖住输出，故按墙钟剔除「超过 kStaleMs 没有新数据」的通道。
    // 剔除判据不能用「输出游标落后量」：交集推进下游标永不超前，落后量恒为 0，死锁。
    const ULONGLONG now = GetTickCount64();
    long long availEnd = LLONG_MAX;
    int act = 0;
    for (int i = 0; i < kChannels; ++i)
    {
        if (!enabled_[i] || !chan_[i].started)
            continue;
        if (active_[i] && lastPushTick_[i] != 0 && now - lastPushTick_[i] > kStaleMs)
        {
            active_[i] = false;
            LOG_ERR(L"音频通道 %d 已 %llu 毫秒无数据, 本次录制该路停止混音",
                    i, (unsigned long long)kStaleMs);
        }
        if (!active_[i])
            continue;
        ++act;
        const Chan& c = chan_[i];
        availEnd = std::min(availEnd, c.firstTs + FramesToTime(c.written));
    }
    if (act == 0 || availEnd == LLONG_MAX || outTs >= availEnd)
        return;

    // 单块上限，避免某一路卡顿后一次性吐出超大块把 MF 写入打断
    long long targetTs = std::min(availEnd, outTs + FramesToTime(kMaxBlock));
    long long n = TimeToFrames(targetTs - outTs);
    if (n <= 0)
        return;

    mixBuf_.resize((size_t)n * 2);
    // 各通道在输出游标处的起始帧（可能为负 = 该路起点略晚或数据已过期）
    long long off[kChannels] = {};
    for (int i = 0; i < kChannels; ++i)
    {
        if (!enabled_[i] || !active_[i] || !chan_[i].started)
            continue;
        off[i] = TimeToFrames(outTs - chan_[i].firstTs);
    }

    for (long long f = 0; f < n; ++f)
    {
        INT32 l = 0, r = 0;
        for (int i = 0; i < kChannels; ++i)
        {
            if (!enabled_[i] || !active_[i] || !chan_[i].started)
                continue;
            const Chan& c = chan_[i];
            const long long idx = off[i] + f;
            if (idx < 0 || idx >= c.written)
                continue;   // 该路在此区间无数据 -> 该声道按静音
            const INT16* s = &c.ring[(size_t)(idx % kRingFrames) * 2];
            l += (INT32)lrintf((float)s[0] * gain_[i]);
            r += (INT32)lrintf((float)s[1] * gain_[i]);
        }
        // 两路相加必然超幅，硬限幅到 int16 范围（不做软限幅，避免两路相消时音量忽大忽小）
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        mixBuf_[(size_t)f * 2]     = (INT16)l;
        mixBuf_[(size_t)f * 2 + 1] = (INT16)r;
    }

    outFrames_ += n;
    if (!firstLogged_)
    {
        firstLogged_ = true;
        LOG_INFO(L"混音首块已输出: 帧=%lld, 增益=系统 %.2f / 麦克风 %.2f",
                 n, gain_[0], gain_[1]);
    }
    out_((const BYTE*)mixBuf_.data(), (UINT32)(n * 4), outTs);
}
