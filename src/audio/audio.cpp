#include "audio.h"
#include "core/log.h"
#include "core/util.h"
#include <mmreg.h>
#include <ksmedia.h>
#include <propsys.h>
#include <vector>
#include <cstring>

// PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
// 本机 SDK 的 propkey.h 未导出该键，按 devpkey.h 里的定义手写一份
static const PROPERTYKEY kPkeyDeviceFriendlyName = {
    0xa45c254e, 0xdf1c, 0x4efd,
    { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 },
    14
};

AudioCapture::~AudioCapture()
{
    Stop();
}

static bool IsFloatFormat(const WAVEFORMATEX* wfx)
{
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)wfx;
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

static bool IsPcm16Format(const WAVEFORMATEX* wfx)
{
    if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16)
        return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)wfx;
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
               wfx->wBitsPerSample == 16;
    }
    return false;
}

// COM 初始化守卫：枚举函数可能从 UI 线程调用，那里没有初始化过 COM
namespace {
struct ComScope
{
    bool owned = false;
    ComScope()
    {
        // RPC_E_CHANGED_MODE 表示线程已是单线程模式（UI 主线程即如此），照样能用
        owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }
    ~ComScope() { if (owned) CoUninitialize(); }
};
}   // namespace

// 设备友好名：读 PKEY_Device_FriendlyName（SDK 里没有 WaveGetProductName 可用）
static std::wstring DeviceFriendlyName(IMMDevice* dev)
{
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, store.GetAddressOf())) && store)
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::wstring name;
        if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal)
            name = pv.pwszVal;
        PropVariantClear(&pv);
        if (!name.empty())
            return name;
    }
    return L"音频设备";
}

bool EnumerateAudioDevices(bool capture, std::vector<AudioDeviceInfo>& out)
{
    out.clear();
    ComScope com;
    const auto role = capture ? eCapture : eRender;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator),
                               (void**)enumerator.GetAddressOf())))
    {
        LOG_ERR(L"枚举音频设备失败: 无法创建设备枚举器 (capture=%d)", capture ? 1 : 0);
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(role, eConsole | eMultimedia,
                                              collection.GetAddressOf())) ||
        !collection)
    {
        LOG_WARN(L"枚举音频设备失败: 无设备集合 (capture=%d)", capture ? 1 : 0);
        return false;
    }

    // 默认设备排在最前，并打标记（设备已拔掉时列表里不会有它）
    std::wstring defaultId;
    Microsoft::WRL::ComPtr<IMMDevice> def;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(role, eConsole, def.GetAddressOf())) &&
        def)
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)) && id)
        {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    // IMMDeviceCollection::GetCount 是 (UINT*) 形式（SDK 头里仍是 out 参数）
    UINT count = 0;
    if (FAILED(collection->GetCount(&count)))
        count = 0;
    out.reserve(count);
    for (UINT i = 0; i < count; ++i)
    {
        Microsoft::WRL::ComPtr<IMMDevice> dev;
        if (FAILED(collection->Item(i, dev.GetAddressOf())) || !dev)
            continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)) || !id)
            continue;
        AudioDeviceInfo info;
        info.id = id;
        CoTaskMemFree(id);
        info.name = DeviceFriendlyName(dev.Get());
        info.isDefault = (info.id == defaultId);
        out.push_back(std::move(info));
    }

    // 默认设备置顶：设备选择器第一项永远是「系统默认」语义上的那个
    for (size_t i = 0; i < out.size(); ++i)
    {
        if (out[i].isDefault && i != 0)
        {
            std::swap(out[0], out[i]);
            break;
        }
    }
    LOG_INFO(L"枚举音频设备: 类别=%s, 数量=%d", capture ? L"录音" : L"播放", (int)out.size());
    return true;
}

// 打开指定端点；deviceId 为空取默认设备，指定设备不存在时回退默认
static Microsoft::WRL::ComPtr<IMMDevice> ResolveDevice(
    IMMDeviceEnumerator* enumerator, AudioRole role, const std::wstring& deviceId)
{
    const auto dataFlow = (role == AudioRole::Mic) ? eCapture : eRender;
    Microsoft::WRL::ComPtr<IMMDevice> dev;
    if (!deviceId.empty())
    {
        if (SUCCEEDED(enumerator->GetDevice(deviceId.c_str(), dev.GetAddressOf())) && dev)
            return dev;
        LOG_WARN(L"指定音频设备已不可用(0x%08lX), 回退默认设备", GetLastError());
    }
    enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, dev.GetAddressOf());
    return dev;
}

bool AudioCapture::Open(AudioRole role, const std::wstring& deviceId,
                        std::wstring& err)
{
    Stop();   // 幂等
    err.clear();
    const wchar_t* roleName = (role == AudioRole::Mic) ? L"麦克风" : L"系统声音";
    const bool loopback = (role == AudioRole::System);

    // COM 初始化改用作用域守卫：Open 跑在 UI 线程，函数返回即释放。
    // 原实现把套间状态记在成员上、由 Stop 去 CoUninitialize 配对，等于每次启停录制
    // 都在主线程开关一遍 COM，而采集线程自身却从未初始化过，全靠"主线程开的 MTA
    // 全进程共享"这一隐含前提。现由 ThreadProc 自行初始化，各线程各管一套。
    ComScope com;
    HRESULT hr = S_OK;

    do
    {
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              (void**)enumerator.GetAddressOf());
        if (FAILED(hr)) { err = L"创建设备枚举器失败"; break; }

        Microsoft::WRL::ComPtr<IMMDevice> dev = ResolveDevice(enumerator.Get(), role, deviceId);
        if (!dev)
        {
            err = (role == AudioRole::Mic) ? L"未找到录音设备" : L"未找到默认扬声器设备";
            break;
        }

        LPWSTR idRaw = nullptr;
        if (SUCCEEDED(dev->GetId(&idRaw)) && idRaw)
        {
            deviceId_ = idRaw;
            CoTaskMemFree(idRaw);
        }
        const std::wstring devName = DeviceFriendlyName(dev.Get());

        Microsoft::WRL::ComPtr<IAudioClient> client;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           (void**)client.GetAddressOf());
        if (FAILED(hr)) { err = L"激活音频客户端失败"; break; }

        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (loopback)
            flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

        // 首选路径: 让 WASAPI 自动重采样为 48kHz/双声道/16bit
        // (设备原生格式可能是 44.1k/96k/单声道/32bit float，Win8.1+ 支持自动转换)
        WAVEFORMATEX req{};
        req.wFormatTag = WAVE_FORMAT_PCM;
        req.nChannels = 2;
        req.nSamplesPerSec = 48000;
        req.wBitsPerSample = 16;
        req.nBlockAlign = 4;
        req.nAvgBytesPerSec = 48000 * 4;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                1000000 /*100ms*/, 0, &req, nullptr);
        if (SUCCEEDED(hr))
        {
            channels_ = 2;
            floatSrc_ = false;
            frameBytes_ = 4;
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)))
            {
                LOG_INFO(L"%s 设备原生格式: %uHz/%u声道/%ubit (设备=%ls), 已自动转换为 48kHz/双声道/16bit",
                         roleName, mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample,
                         devName.c_str());
                CoTaskMemFree(mix);
            }
        }
        else
        {
            // 回退路径: 少数驱动不支持自动转换，直接用设备原生格式，
            // 此时只接受 48kHz 的 1/2 声道（单声道会在采集线程里补成双声道）
            LOG_WARN(L"%s 自动重采样不可用(hr=0x%08lX), 回退到设备原生格式采集", roleName, hr);
            WAVEFORMATEX* wfx = nullptr;
            hr = client->GetMixFormat(&wfx);
            if (FAILED(hr)) { err = L"GetMixFormat 失败"; break; }

            const bool chOk = (wfx->nChannels == 1 || wfx->nChannels == 2);
            if (wfx->nSamplesPerSec != 48000 || !chOk ||
                (!IsFloatFormat(wfx) && !IsPcm16Format(wfx)))
            {
                const UINT32 nativeRate = wfx->nSamplesPerSec;
                const UINT32 nativeCh = wfx->nChannels;
                CoTaskMemFree(wfx);
                err = std::wstring(roleName) + L"设备格式非 48kHz，本次录制不含该路音频";
                LOG_WARN(L"音频启动跳过: %s (实际格式: %uHz/%u声道)",
                         err.c_str(), nativeRate, nativeCh);
                break;
            }

            channels_ = wfx->nChannels;
            floatSrc_ = IsFloatFormat(wfx);
            frameBytes_ = wfx->nBlockAlign;

            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    1000000 /*100ms*/, 0, wfx, nullptr);
            CoTaskMemFree(wfx);
            if (FAILED(hr)) { err = L"音频客户端初始化失败"; break; }
            LOG_INFO(L"%s 使用设备原生格式: 48kHz/%u声道/%s (设备=%ls)",
                     roleName, channels_, floatSrc_ ? L"float" : L"16bit", devName.c_str());
        }

        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                (void**)capture_.GetAddressOf());
        if (FAILED(hr)) { err = L"获取采集服务失败"; break; }

        client_ = client;
        LOG_INFO(L"音频设备已打开: %s / %ls", roleName, devName.c_str());
        return true;
    } while (false);

    if (!err.empty())
        LOG_ERR(L"%s 不可用: %s (hr=0x%08lX)", roleName, err.c_str(), hr);
    capture_.Reset();
    client_.Reset();
    return false;
}

bool AudioCapture::Start(DataFn onData)
{
    if (!client_ || !capture_ || !onData)
    {
        LOG_ERR(L"音频启动失败: 设备未打开或回调未设置");
        return false;
    }
    if (running_)
        return true;

    HRESULT hr = client_->Start();   // Start/Stop 属于 IAudioClient，非 IAudioCaptureClient
    if (FAILED(hr))
    {
        LOG_ERR(L"音频启动失败: IAudioClient::Start (hr=0x%08lX)", hr);
        return false;
    }

    onData_ = std::move(onData);
    stopFlag_ = false;
    thread_ = std::thread([this]() { ThreadProc(); });
    running_ = true;
    LOG_INFO(L"音频采集已启动: 输出 48kHz/双声道/16bit, 源帧=%u 字节", frameBytes_);
    return true;
}

// ---- 格式转换：统一把任意源格式整理成 48kHz/双声道/16bit 交错块 ----

static void FloatToInt16(const float* src, INT16* dst, UINT32 samples)
{
    for (UINT32 i = 0; i < samples; ++i)
    {
        float f = src[i];
        if (f > 1.0f) f = 1.0f;
        else if (f < -1.0f) f = -1.0f;
        dst[i] = (INT16)(f * 32767.0f);
    }
}

// 单声道 16bit -> 双声道（左复制到右）
static void Mono16ToStereo(const INT16* src, INT16* dst, UINT32 frames)
{
    for (UINT32 i = 0; i < frames; ++i)
    {
        const INT16 v = src[i];
        dst[i * 2] = v;
        dst[i * 2 + 1] = v;
    }
}

// 单声道 float32 -> 双声道 16bit（一次完成格式与声道转换）
static void MonoFloatToStereo16(const float* src, INT16* dst, UINT32 frames)
{
    for (UINT32 i = 0; i < frames; ++i)
    {
        float f = src[i];
        if (f > 1.0f) f = 1.0f;
        else if (f < -1.0f) f = -1.0f;
        const INT16 v = (INT16)(f * 32767.0f);
        dst[i * 2] = v;
        dst[i * 2 + 1] = v;
    }
}

void AudioCapture::ThreadProc()
{
    // 采集线程自行初始化 COM：不再依赖 Open 在主线程留下的套间。
    // 线程退出时由 ComScope 析构释放，与 Start/Stop 的生命周期严格对齐
    ComScope com;
    LOG_INFO(L"音频采集线程运行中 (线程ID=%u)", GetCurrentThreadId());
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);

    std::vector<BYTE> conv;    // 格式/声道转换输出缓冲
    std::vector<BYTE> silent;  // 静音填充
    // Loopback 在播放端无活动时 GetNextPacketSize 恒为 0（Start 照样成功），
    // 若什么都不送，单路直通时音轨会出现空洞、音画逐渐错位。空闲超阈值
    // 就按 20ms 一块主动补静音，保持音轨连续（混音路由同样受益，不再频繁触发停摆剔除）
    ULONGLONG lastDataTick = GetTickCount64();
    constexpr ULONGLONG kIdleEmitMs = 20;
    constexpr UINT32 kIdleFrames = 960;   // 20ms @48kHz
    std::vector<BYTE> idleSilence((size_t)kIdleFrames * 4, 0);

    while (!stopFlag_)
    {
        Sleep(5);
        UINT32 packet = 0;
        bool gotData = false;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpcPos = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &qpcPos, nullptr)))
            {
                LOG_ERR(L"音频 GetBuffer 失败, 采集线程退出");
                return;
            }

            // 输出恒为双声道 16bit，故每帧 4 字节
            const UINT32 outBytes = frames * 4;
            const BYTE* out = data;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                if (silent.size() < outBytes) silent.resize(outBytes, 0);
                memset(silent.data(), 0, outBytes);
                out = silent.data();
            }
            else if (channels_ == 1)
            {
                conv.resize(outBytes);
                if (floatSrc_)
                    MonoFloatToStereo16((const float*)data, (INT16*)conv.data(), frames);
                else
                    Mono16ToStereo((const INT16*)data, (INT16*)conv.data(), frames);
                out = conv.data();
            }
            else if (floatSrc_)
            {
                const UINT32 sampleCount = frames * 2;
                conv.resize(sampleCount * sizeof(INT16));
                FloatToInt16((const float*)data, (INT16*)conv.data(), sampleCount);
                out = conv.data();
            }

            long long ts = 0;
            // 时间戳优先用设备给的 qpcPos（平滑）；但实测某些 USB 耳机麦克风返回的
            // qpcPos 几乎不随时间推进，此时音轨时间轴会停在原地（混音与 MF 写入
            // 都按时间戳推进，表现为音频只录到开头一瞬），故检测到不推进就改用到达时刻
            const long long frameTime = (long long)frames * 10000000 / 48000;
            if (deviceClockUsable_)
            {
                if (qpcPos != 0)
                {
                    const long long devTs =
                        (long long)(qpcPos * 10000000ULL / (UINT64)qpcFreq.QuadPart);
                    if (lastDeviceTs_ != 0 && devTs - lastDeviceTs_ < frameTime / 2)
                    {
                        LOG_WARN(L"音频设备时钟不推进(qpcPos 增量 %lld < 帧时长 %lld), 改用块到达时刻",
                                 devTs - lastDeviceTs_, frameTime);
                        deviceClockUsable_ = false;
                    }
                    else
                    {
                        ts = devTs;
                        lastDeviceTs_ = devTs;
                    }
                }
                else
                {
                    deviceClockUsable_ = false;
                }
            }
            if (ts == 0)
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                ts = (long long)(now.QuadPart * 10000000ULL / (UINT64)qpcFreq.QuadPart);
            }
            // 回退到 QPC 时得到的时刻可能略小于上一块（包到达有抖动），而 MF 的
            // WriteSample 要求时间戳严格递增，故做单调化（1 单位 = 0.1 微秒，听不出来）
            if (ts <= lastTs_)
                ts = lastTs_ + 1;
            lastTs_ = ts;

            if (onData_)
                onData_(out, outBytes, ts);

            capture_->ReleaseBuffer(frames);
            gotData = true;

            // 电平表：真实包才更新峰值（静音保活块不参与，否则响一声就被清零）
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                int peak = 0;
                const INT16* s = (const INT16*)out;
                const UINT32 n = outBytes / 2;
                for (UINT32 i = 0; i < n; ++i)
                {
                    int v = s[i] < 0 ? -(int)s[i] : (int)s[i];
                    if (v > peak)
                        peak = v;
                }
                peak_ = peak;
                peakTick_ = GetTickCount64();
            }
        }
        if (gotData)
        {
            lastDataTick = GetTickCount64();
        }
        else if (onData_ && GetTickCount64() - lastDataTick >= kIdleEmitMs)
        {
            // 空闲保活：送一块静音，时间戳用块到达时刻（QPC）并做单调化
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            long long ts = (long long)(now.QuadPart * 10000000ULL / (UINT64)qpcFreq.QuadPart);
            if (ts <= lastTs_)
                ts = lastTs_ + 1;
            lastTs_ = ts;
            onData_(idleSilence.data(), (UINT32)idleSilence.size(), ts);
            lastDataTick = GetTickCount64();
        }
    }
    LOG_INFO(L"音频采集线程退出");
}

int AudioCapture::Level() const
{
    if (GetTickCount64() - peakTick_.load() > 150)
        return 0;
    const int peak = peak_.load();
    int pct = peak * 100 / 32768;
    return pct > 100 ? 100 : pct;
}

void AudioCapture::Stop()
{
    if (running_)
        LOG_INFO(L"音频采集停止");
    if (thread_.joinable())
    {
        stopFlag_ = true;
        thread_.join();     // 循环 5ms 粒度，join 最多等待十几毫秒
    }
    if (client_)
    {
        client_->Stop();    // Start/Stop 属于 IAudioClient
        client_.Reset();
    }
    capture_.Reset();
    running_ = false;
    onData_ = nullptr;
    lastTs_ = 0;
    lastDeviceTs_ = 0;
    deviceClockUsable_ = true;
    peak_ = 0;
    // COM 不再由本对象管理：Open 用作用域守卫，采集线程自带 ComScope
}
