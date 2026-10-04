#include "recorder.h"
#include "core/log.h"
#include "core/util.h"   // IsDeviceLostHr
#include <mfapi.h>
#include <mfobjects.h>
#include <mferror.h>
#include <d3dcompiler.h>
#include <cfloat>
#include <cstring>

// 常见 HRESULT 的可读名称（无匹配则返回空串），用于日志快速定位
static std::wstring HrSuffix(HRESULT hr)
{
    const wchar_t* name = nullptr;
    switch (hr)
    {
    case E_INVALIDARG:                       name = L"E_INVALIDARG(参数无效)"; break;
    case E_OUTOFMEMORY:                      name = L"E_OUTOFMEMORY"; break;
    case E_ACCESSDENIED:                     name = L"E_ACCESSDENIED(拒绝访问)"; break;
    case MF_E_INVALIDMEDIATYPE:              name = L"MF_E_INVALIDMEDIATYPE(媒体类型不支持)"; break;
    case MF_E_INVALIDREQUEST:                name = L"MF_E_INVALIDREQUEST(当前状态不允许)"; break;
    case MF_E_SHUTDOWN:                      name = L"MF_E_SHUTDOWN(已关闭)"; break;
    case MF_E_NOTACCEPTING:                  name = L"MF_E_NOTACCEPTING(不接受新数据)"; break;
    case MF_E_HW_MFT_FAILED_START_STREAMING: name = L"MF_E_HW_MFT_FAILED_START_STREAMING(硬件MFT启动流失败)"; break;
    case MF_E_TRANSFORM_NEED_MORE_INPUT:     name = L"MF_E_TRANSFORM_NEED_MORE_INPUT"; break;
    default: break;
    }
    return name ? std::wstring(L", ") + name : std::wstring();
}

// ---------------------------------------------------------------------------
// 分辨率缩放：全屏三角形 + 线性采样，把捕获帧缩放到编码尺寸
// 用 SV_VertexID 生成顶点，免去顶点缓冲（FL 10+ 可用）
// ---------------------------------------------------------------------------
namespace {

const char* const kScalerHLSL =
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VS(uint id : SV_VertexID)\n"
    "{\n"
    "    float2 p = float2(float((id << 1) & 2), float(id & 2));\n"
    "    VSOut o;\n"
    "    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "    o.uv = p;\n"
    "    return o;\n"
    "}\n"
    "Texture2D srcTex : register(t0);\n"
    "SamplerState srcSamp : register(s0);\n"
    "float4 PS(VSOut i) : SV_Target { return srcTex.Sample(srcSamp, i.uv); }\n";

Microsoft::WRL::ComPtr<ID3DBlob> CompileScalerShader(const char* entry, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    HRESULT hr = D3DCompile(kScalerHLSL, strlen(kScalerHLSL), nullptr, nullptr, nullptr,
                            entry, target, 0, 0, code.GetAddressOf(), nullptr);
    if (FAILED(hr))
    {
        LOG_ERR(L"缩放着色器编译失败(%s, hr=0x%08lX)",
                (entry[0] == 'V') ? L"顶点" : L"像素", hr);
        return nullptr;
    }
    return code;
}

// 设备丢失类错误统一在此判定：置位标志并打日志（只打一次，避免 60fps 刷屏）。
// 设备一丢，视频帧必然全部失败，主循环轮询到标志后会走停止流程并提示用户
void NoteDeviceLost(std::atomic<bool>& flag, HRESULT hr)
{
    if (!IsDeviceLostHr(hr))
        return;
    bool expected = false;
    if (flag.compare_exchange_strong(expected, true))
        LOG_ERR(L"GPU 设备已丢失(hr=0x%08lX, 驱动重置/TDR), 本次录制将被中止", hr);
}

}   // namespace

// 确保暂存纹理按当前尺寸成对存在（惰性创建，尺寸变化时重建）。
// 返回 false = 创建失败，本帧放弃。
// 历史教训（曾致 TDR，勿回退）：这里绝不能改回延迟上下文拷贝或
// DO_NOT_WAIT 轮询式 Map，详见 recorder.h 头部注释与 AGNETS.md。
bool Recorder::EnsureStagingPair(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc)
{
    if (stagingTex_[0] && stagingTex_[1] && stagingW_ == desc.Width && stagingH_ == desc.Height)
        return true;

    stagingTex_[0].Reset();
    stagingTex_[1].Reset();
    stageHasPending_ = false;
    stageNext_ = 0;

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    for (int i = 0; i < 2; ++i)
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
        HRESULT hr = device->CreateTexture2D(&sd, nullptr, t.GetAddressOf());
        if (FAILED(hr))
        {
            stagingTex_[0].Reset();
            stagingTex_[1].Reset();
            ++videoErrTotal_;
            if (videoErrCount_ < 3)
            { ++videoErrCount_; LOG_ERR(L"创建暂存纹理失败 (%ux%u, hr=0x%08lX)", sd.Width, sd.Height, hr); }
            return false;
        }
        stagingTex_[i] = t;
    }
    stagingW_ = desc.Width;
    stagingH_ = desc.Height;
    LOG_INFO(L"视频写入: 系统内存路径(双暂存错帧回读), 暂存纹理 %ux%u x2 (格式=0x%08X)",
             stagingW_, stagingH_, sd.Format);
    return true;
}

Recorder::~Recorder()
{
    Stop();
}

bool Recorder::SetupScaler(ID3D11Device* device)
{
    if (!device)
        return false;

    // 缩放目标：与捕获帧同格式（WGC 帧固定 BGRA），需可作渲染目标
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = outW_;
    td.Height = outH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    scaledTex_.Reset();
    scaledRTV_.Reset();
    if (FAILED(device->CreateTexture2D(&td, nullptr, scaledTex_.GetAddressOf())))
    {
        LOG_ERR(L"缩放目标纹理创建失败 (%ux%u)", outW_, outH_);
        return false;
    }
    if (FAILED(device->CreateRenderTargetView(scaledTex_.Get(), nullptr,
                                              scaledRTV_.GetAddressOf())))
    {
        LOG_ERR(L"缩放目标 RTV 创建失败 (%ux%u)", outW_, outH_);
        return false;
    }

    if (!sampler_)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = FLT_MAX;
        if (FAILED(device->CreateSamplerState(&sd, sampler_.GetAddressOf())))
        {
            LOG_ERR(L"缩放采样器创建失败");
            return false;
        }
    }

    if (!vs_)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> code = CompileScalerShader("VS", "vs_4_0");
        if (!code) return false;
        if (FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                                              nullptr, vs_.GetAddressOf())))
        {
            LOG_ERR(L"缩放顶点着色器创建失败");
            return false;
        }
    }
    if (!ps_)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> code = CompileScalerShader("PS", "ps_4_0");
        if (!code) return false;
        if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                             nullptr, ps_.GetAddressOf())))
        {
            LOG_ERR(L"缩放像素着色器创建失败");
            return false;
        }
    }
    return true;
}

bool Recorder::ScaleToOutput(ID3D11DeviceContext* imm, ID3D11Texture2D* src)
{
    if (!src || !scaledTex_ || !scaledRTV_ || !vs_ || !ps_ || !sampler_)
        return false;

    // 捕获帧纹理会被复用，SRV 按纹理指针缓存，避免每帧重建
    if (!srcSRV_ || srcTexForSRV_ != src)
    {
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        src->GetDevice(dev.GetAddressOf());
        D3D11_TEXTURE2D_DESC d{};
        src->GetDesc(&d);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = d.Format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(src, &sv, srcSRV_.ReleaseAndGetAddressOf())))
        {
            LOG_ERR(L"缩放源 SRV 创建失败 (%ux%u)", d.Width, d.Height);
            return false;
        }
        srcTexForSRV_ = src;
    }

    // 直接在立即上下文上绘制。D3D11 多线程保护下与 UI 线程的并发是安全的
    // （capture.cpp 每帧同样在立即上下文上拷贝，该形态长期稳定）；
    // 缩放仅一个全屏三角形，立即上下文占用可忽略。
    // 不使用延迟上下文：见 recorder.h 头部的 TDR 教训注释
    ID3D11RenderTargetView* rtv = scaledRTV_.Get();
    imm->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = (float)outW_;
    vp.Height = (float)outH_;
    vp.MaxDepth = 1.0f;
    imm->RSSetViewports(1, &vp);

    imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    imm->IASetInputLayout(nullptr);
    imm->VSSetShader(vs_.Get(), nullptr, 0);
    imm->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = srcSRV_.Get();
    imm->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* samp = sampler_.Get();
    imm->PSSetSamplers(0, 1, &samp);
    imm->Draw(3, 0);

    // 立刻解绑，尽量少影响 UI 线程的绘制状态
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11RenderTargetView* nullRtv = nullptr;
    imm->PSSetShaderResources(0, 1, &nullSrv);
    imm->OMSetRenderTargets(1, &nullRtv, nullptr);
    return true;
}

std::wstring ProbeHardwareEncoderName()
{
    std::wstring name;
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    // MFTEnumEx 共 6 参数（第 4 参为输出类型过滤）
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                            nullptr, nullptr, &acts, &count)) && count > 0)
    {
        WCHAR* str = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &str, &len)))
        {
            name = str;
            CoTaskMemFree(str);
        }
        for (UINT32 i = 0; i < count; ++i)
            acts[i]->Release();
        CoTaskMemFree(acts);
    }
    return name;
}

bool Recorder::Init(ID3D11Device* device, const wchar_t* path,
                    UINT srcW, UINT srcH, UINT outW, UINT outH,
                    UINT fps, UINT bitrateMbps, bool withAudio,
                    bool hwEncode, int audioBitrateKbps, std::wstring& err)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        StopLocked();
    }
    err.clear();
    // 重置本次录制的状态与统计
    stagingTex_[0].Reset();
    stagingTex_[1].Reset();
    stagingW_ = stagingH_ = 0;
    stageNext_ = 0;
    stageHasPending_ = false;
    stagedTs_ = 0;
    srcW_ = srcH_ = 0;
    srcSizeBroken_ = false;
    deviceLost_ = false;
    scaledTex_.Reset();
    scaledRTV_.Reset();
    srcSRV_.Reset();
    srcTexForSRV_ = nullptr;
    scale_ = false;
    videoFrames_ = audioBlocks_ = 0;
    videoErrCount_ = audioErrCount_ = 0;
    videoErrTotal_ = audioErrTotal_ = 0;
    videoFirstLogged_ = audioFirstLogged_ = false;
    if (srcW == 0 || srcH == 0)
    {
        err = L"尚未捕获到任何帧，无法确定录制尺寸";
        LOG_ERR(L"Recorder Init 失败: %s", err.c_str());
        return false;
    }

    LOG_INFO(L"Recorder Init: 文件=%s, 源尺寸=%ux%u, 目标=%ux%u, 帧率=%u, 码率=%uMbps, 含音频=%d, 音频码率=%dkbps, 硬件加速=%d",
             path, srcW, srcH, outW, outH, fps, bitrateMbps, withAudio ? 1 : 0,
             audioBitrateKbps, hwEncode ? 1 : 0);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr))
    {
        if (hr == MF_E_ALREADY_INITIALIZED)
            mfStarted_ = false;   // 已由其他组件初始化，不配对 Shutdown
        else { err = L"MFStartup 失败"; LOG_ERR(L"%s (hr=0x%08lX)", err.c_str(), hr); return false; }
    }
    else
    {
        mfStarted_ = true;
    }

    do
    {
        // H.264 要求宽高为偶数；目标非法或大于源（不允许放大）时按源尺寸录制
        const UINT srcEvenW = srcW & ~1u;
        const UINT srcEvenH = srcH & ~1u;
        outW_ = outW & ~1u;
        outH_ = outH & ~1u;
        if (outW_ == 0 || outH_ == 0 || outW_ > srcEvenW || outH_ > srcEvenH)
        {
            outW_ = srcEvenW;
            outH_ = srcEvenH;
        }
        // 记下本次录制的源尺寸基准：WriteVideoFrame 据此判断画面是否中途变了
        srcW_ = srcEvenW;
        srcH_ = srcEvenH;
        scale_ = (outW_ != srcEvenW) || (outH_ != srcEvenH);
        if (scale_ && !SetupScaler(device))
        {
            LOG_WARN(L"分辨率缩放不可用, 退回原始分辨率 %ux%u 录制", srcEvenW, srcEvenH);
            scale_ = false;
            outW_ = srcEvenW;
            outH_ = srcEvenH;
        }
        fps_ = fps ? fps : 60;
        withAudio_ = withAudio;
        hwEncode_ = hwEncode;
        duration100ns_ = 10000000LL / fps_;

        // 只在允许硬件加速时才记录编码器名：禁用时对外一律显示为空
        encoderName_ = hwEncode_ ? ProbeHardwareEncoderName() : std::wstring();
        LOG_INFO(L"硬件编码器: %s",
                 encoderName_.empty() ? (hwEncode_ ? L"(未检测到, 将回退软编)"
                                                   : L"(已关闭硬件加速, 纯软编)")
                                      : encoderName_.c_str());

        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        hr = MFCreateAttributes(attrs.GetAddressOf(), 2);
        if (FAILED(hr)) { err = L"MFCreateAttributes 失败"; break; }
        // 关闭硬件加速时禁止硬件变换，H.264 会走纯软件编码器
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hwEncode_ ? TRUE : FALSE);
        // 注意: 此处刻意不设置 MF_SINK_WRITER_D3D_MANAGER。
        // GPU 纹理零拷贝直写在 AMD 硬件编码器上会首帧 WriteSample E_INVALIDARG,
        // 硬件 MFT 始终无法启动流(Finalize 报 MF_E_HW_MFT_FAILED_START_STREAMING)。
        // 改用系统内存路径: 硬件编码器仍可用(MFT 内部自行上传), 兼容性最好。
        hr = MFCreateSinkWriterFromURL(path, nullptr, attrs.Get(), writer_.GetAddressOf());
        if (FAILED(hr)) { err = L"创建 SinkWriter 失败 (无法写入该路径?)"; break; }

        // ---- 视频输出流: H.264 ----
        Microsoft::WRL::ComPtr<IMFMediaType> outType;
        hr = MFCreateMediaType(outType.GetAddressOf());
        if (FAILED(hr)) break;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE,
                           (bitrateMbps ? bitrateMbps : 16u) * 1000000u);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, outW_, outH_);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, fps_, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = writer_->AddStream(outType.Get(), &videoStream_);
        if (FAILED(hr)) { err = L"添加视频流失败"; break; }

        // ---- 视频输入流: BGRA 系统内存（与 WGC 纹理同格式，MF 自动插入色彩转换） ----
        Microsoft::WRL::ComPtr<IMFMediaType> inType;
        hr = MFCreateMediaType(inType.GetAddressOf());
        if (FAILED(hr)) break;
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, outW_, outH_);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, fps_, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = writer_->SetInputMediaType(videoStream_, inType.Get(), nullptr);
        if (FAILED(hr)) { err = L"设置视频输入类型失败"; break; }

        // ---- 音频流: AAC / PCM 16bit 48k 立体声 ----
        if (withAudio_)
        {
            Microsoft::WRL::ComPtr<IMFMediaType> aOut;
            hr = MFCreateMediaType(aOut.GetAddressOf());
            if (FAILED(hr)) break;
            aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            // AAC 恒定码率：字节率 = kbps * 1000 / 8
            const UINT aKbps = (audioBitrateKbps > 0) ? (UINT)audioBitrateKbps : 192u;
            aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, aKbps * 1000u / 8u);

            hr = writer_->AddStream(aOut.Get(), &audioStream_);
            if (FAILED(hr)) { err = L"添加音频流失败"; break; }

            Microsoft::WRL::ComPtr<IMFMediaType> aIn;
            hr = MFCreateMediaType(aIn.GetAddressOf());
            if (FAILED(hr)) break;
            aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

            hr = writer_->SetInputMediaType(audioStream_, aIn.Get(), nullptr);
            if (FAILED(hr)) { err = L"设置音频输入类型失败"; break; }
        }

        hr = writer_->BeginWriting();
        if (FAILED(hr)) { err = L"BeginWriting 失败"; break; }

        LOG_INFO(L"SinkWriter 就绪, 开始写入: 输出=%ux%u@%u, H.264+AAC=%d, 视频=系统内存路径, GPU缩放=%d, 硬件加速=%d",
                 outW_, outH_, fps_, withAudio_ ? 1 : 0, scale_ ? 1 : 0, hwEncode_ ? 1 : 0);
        return true;
    } while (false);

    if (err.empty())
        err = L"Recorder 初始化失败，HRESULT=0x" + std::to_wstring((unsigned)hr);
    LOG_ERR(L"%s (hr=0x%08lX)", err.c_str(), hr);
    writer_.Reset();
    if (mfStarted_) { MFShutdown(); mfStarted_ = false; }
    return false;
}

void Recorder::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked();
}

// 需持有 mutex_ 调用
void Recorder::StopLocked()
{
    if (writer_)
    {
        LOG_INFO(L"Recorder Stop: 写入统计: 视频帧=%lld, 音频块=%lld, 帧错误=%d, 音频错误=%d",
                 videoFrames_, audioBlocks_, videoErrTotal_, audioErrTotal_);
        LOG_INFO(L"Recorder Stop: 开始 Finalize (编码器 flush, 可能阻塞数秒)...");
        HRESULT hr = writer_->Finalize();   // 编码器 flush，可能阻塞数秒
        writer_.Reset();
        if (FAILED(hr))
            LOG_ERR(L"Finalize 失败 (hr=0x%08lX%s), MP4 文件可能不完整",
                    hr, HrSuffix(hr).c_str());
        else
            LOG_INFO(L"Recorder Stop: Finalize 完成, MP4 文件已生成");
    }
    // 释放回读与缩放相关显存（着色器/采样器保留复用；暂存纹理双缓冲一并释放，
    // 彻底解除对设备的绑定，设备丢失后的进程内恢复依赖这一点）
    stagingTex_[0].Reset();
    stagingTex_[1].Reset();
    stagingW_ = stagingH_ = 0;
    stageHasPending_ = false;
    stageNext_ = 0;
    scaledTex_.Reset();
    scaledRTV_.Reset();
    srcSRV_.Reset();
    srcTexForSRV_ = nullptr;
    scale_ = false;
    if (mfStarted_)
    {
        MFShutdown();
        mfStarted_ = false;
    }
}

void Recorder::WriteVideoFrame(ID3D11Texture2D* tex, long long ts100ns)
{
    if (!tex)
        return;

    // ---- 源尺寸守卫（锁外，纹理描述符只由本线程使用）----
    // 录制中途显示器分辨率变化 / 插拔显示器 / 旋转屏幕时，纹理尺寸会变，
    // 但 SinkWriter 已按旧尺寸配置。继续写会把新画面裁/补成旧尺寸（拉伸变形），
    // 且下面那条惰性重建会变成「每帧销毁并重建一张数 MB 的暂存纹理」。
    // 就地判定中止，由主循环轮询 IsSrcSizeBroken() 走标准停止流程。
    {
        D3D11_TEXTURE2D_DESC srcDesc{};
        tex->GetDesc(&srcDesc);
        const UINT evenW = srcDesc.Width & ~1u;
        const UINT evenH = srcDesc.Height & ~1u;
        if (srcW_ != 0 && (evenW != srcW_ || evenH != srcH_))
        {
            bool expected = false;
            if (srcSizeBroken_.compare_exchange_strong(expected, true))
                LOG_ERR(L"录制中止: 画面源尺寸由 %ux%u 变为 %ux%u，编码器配置已不适用",
                        srcW_, srcH_, evenW, evenH);
            return;
        }
    }

    // 锁内判 writer_：与 Stop/Finalize 串行，消除在途写入竞态
    std::lock_guard<std::mutex> lock(mutex_);
    if (!writer_)
        return;

    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    tex->GetDevice(dev.GetAddressOf());
    dev->GetImmediateContext(ctx.GetAddressOf());

    // 需要缩放时先把源帧画进目标尺寸纹理，后续取像素都以该纹理为准
    ID3D11Texture2D* workTex = tex;
    if (scale_)
    {
        if (!ScaleToOutput(ctx.Get(), tex))
        {
            ++videoErrTotal_;
            if (videoErrCount_ < 3)
            { ++videoErrCount_; LOG_ERR(L"视频帧缩放失败"); }
            return;
        }
        workTex = scaledTex_.Get();
    }

    D3D11_TEXTURE2D_DESC desc{};
    workTex->GetDesc(&desc);

    // 双暂存纹理（CPU 可读）惰性创建，尺寸变化时成对重建
    if (!EnsureStagingPair(dev.Get(), desc))
        return;

    // ---- 先读回上一帧拷进暂存的画面并送编码（stagedTs_ 是它的采集时刻）----
    // 错帧双缓冲：读的是上一帧的拷贝，GPU 已有一整帧时间完成，
    // 最普通的阻塞 Map 实际立即返回，既不占 D3D 全局锁卡 UI，
    // 也不依赖任何非常规调用形态（见 recorder.h 头部的 TDR 教训）
    if (stageHasPending_ && !ConsumeStagedFrame(ctx.Get(), desc))
        return;   // 设备/编码器已不可用，本帧不再拷贝

    // ---- 再把本帧拷进另一个暂存，留给下一帧读回 ----
    // 裁剪到编码尺寸（宽高向下取偶）。在立即上下文上下命令：
    // D3D11 多线程保护下与 UI 线程并发安全，单次拷贝微秒级
    const UINT copyW = min(desc.Width, outW_);
    const UINT copyH = min(desc.Height, outH_);
    D3D11_BOX box{};
    box.right = copyW;
    box.bottom = copyH;
    box.back = 1;
    ctx->CopySubresourceRegion(stagingTex_[stageNext_].Get(), 0, 0, 0, 0, workTex, 0, &box);
    stagedTs_ = ts100ns;
    stageHasPending_ = true;
    stageNext_ ^= 1;
}

bool Recorder::ConsumeStagedFrame(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& desc)
{
    ID3D11Texture2D* ready = stagingTex_[stageNext_ ^ 1].Get();

    const UINT copyW = min(desc.Width, outW_);
    const UINT copyH = min(desc.Height, outH_);
    const UINT dstStride = outW_ * 4;
    const SIZE_T bufLen = (SIZE_T)dstStride * outH_;

    // 阻塞 Map：上一帧的拷贝早已完成，立即返回。
    // 失败基本只剩一种原因——设备丢失（TDR / 驱动重置）
    D3D11_MAPPED_SUBRESOURCE mr{};
    HRESULT hr = ctx->Map(ready, 0, D3D11_MAP_READ, 0, &mr);
    if (FAILED(hr))
    {
        NoteDeviceLost(deviceLost_, hr);
        ++videoErrTotal_;
        if (videoErrCount_ < 3)
        { ++videoErrCount_; LOG_ERR(L"视频帧读取失败: Map 暂存纹理 (hr=0x%08lX)", hr); }
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer((DWORD)bufLen, buffer.GetAddressOf());
    if (FAILED(hr))
    {
        ctx->Unmap(ready, 0);
        stageHasPending_ = false;   // 这帧放弃，不留待下一帧重复送
        ++videoErrTotal_;
        if (videoErrCount_ < 3)
        { ++videoErrCount_; LOG_ERR(L"视频帧写入失败: MFCreateMemoryBuffer (hr=0x%08lX)", hr); }
        return true;
    }

    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr))
    {
        ctx->Unmap(ready, 0);
        stageHasPending_ = false;
        ++videoErrTotal_;
        if (videoErrCount_ < 3)
        { ++videoErrCount_; LOG_ERR(L"视频帧写入失败: Buffer Lock (hr=0x%08lX)", hr); }
        return true;
    }

    // 显卡行距可能大于 宽*4，需按行搬运；裁剪产生的空洞填零
    const BYTE* src = (const BYTE*)mr.pData;
    if (mr.RowPitch == dstStride && copyW == outW_ && copyH == outH_)
    {
        memcpy(dst, src, bufLen);
    }
    else
    {
        memset(dst, 0, bufLen);
        for (UINT y = 0; y < copyH; ++y)
            memcpy(dst + (SIZE_T)y * dstStride,
                   src + (SIZE_T)y * mr.RowPitch, (SIZE_T)copyW * 4);
    }
    ctx->Unmap(ready, 0);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)bufLen);

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(sample.GetAddressOf());
    if (FAILED(hr))
    {
        stageHasPending_ = false;
        ++videoErrTotal_;
        if (videoErrCount_ < 3)
        { ++videoErrCount_; LOG_ERR(L"视频帧写入失败: MFCreateSample (hr=0x%08lX)", hr); }
        return true;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(stagedTs_);
    sample->SetSampleDuration(duration100ns_);
    hr = writer_->WriteSample(videoStream_, sample.Get());
    stageHasPending_ = false;   // 已消费（送出或放弃）
    if (FAILED(hr))
    {
        NoteDeviceLost(deviceLost_, hr);
        ++videoErrTotal_;
        if (videoErrCount_ < 3)
        {
            ++videoErrCount_;
            LOG_ERR(L"视频帧写入失败: WriteSample (hr=0x%08lX%s), 纹理=%ux%u, 缓冲=%zu 字节, 行距=%u, ts=%lld",
                    hr, HrSuffix(hr).c_str(), desc.Width, desc.Height, bufLen, mr.RowPitch, stagedTs_);
        }
        return false;
    }
    ++videoFrames_;
    if (!videoFirstLogged_)
    {
        videoFirstLogged_ = true;
        LOG_INFO(L"视频首帧已写入 (纹理=%ux%u, 行距=%u, 缓冲=%zu, ts=%lld)",
                 desc.Width, desc.Height, mr.RowPitch, bufLen, stagedTs_);
    }
    return true;
}

void Recorder::ReleaseDeviceResources()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stagingTex_[0].Reset();
    stagingTex_[1].Reset();
    stagingW_ = stagingH_ = 0;
    stageHasPending_ = false;
    stageNext_ = 0;
    stagedTs_ = 0;
    scaledTex_.Reset();
    scaledRTV_.Reset();
    srcSRV_.Reset();
    srcTexForSRV_ = nullptr;
    scale_ = false;
    // 着色器/采样器等"跨录制复用"的缓存同样绑定在旧设备上，一并清掉
    vs_.Reset();
    ps_.Reset();
    sampler_.Reset();
}

void Recorder::WriteAudio(const BYTE* data, UINT32 bytes, long long ts100ns)
{
    if (!data || bytes == 0)
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!writer_ || !withAudio_)
        return;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bytes, buffer.GetAddressOf());
    if (FAILED(hr))
    {
        ++audioErrTotal_;
        if (audioErrCount_ < 3)
        { ++audioErrCount_; LOG_ERR(L"音频写入失败: MFCreateMemoryBuffer (hr=0x%08lX)", hr); }
        return;
    }

    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr))
    {
        ++audioErrTotal_;
        if (audioErrCount_ < 3)
        { ++audioErrCount_; LOG_ERR(L"音频写入失败: Buffer Lock (hr=0x%08lX)", hr); }
        return;
    }
    memcpy(dst, data, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);

    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(sample.GetAddressOf());
    if (FAILED(hr))
    {
        ++audioErrTotal_;
        if (audioErrCount_ < 3)
        { ++audioErrCount_; LOG_ERR(L"音频写入失败: MFCreateSample (hr=0x%08lX)", hr); }
        return;
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(ts100ns);
    hr = writer_->WriteSample(audioStream_, sample.Get());
    if (FAILED(hr))
    {
        ++audioErrTotal_;
        if (audioErrCount_ < 3)
        {
            ++audioErrCount_;
            LOG_ERR(L"音频写入失败: WriteSample (hr=0x%08lX%s), bytes=%u, ts=%lld",
                    hr, HrSuffix(hr).c_str(), bytes, ts100ns);
        }
        return;
    }
    ++audioBlocks_;
    if (!audioFirstLogged_)
    {
        audioFirstLogged_ = true;
        LOG_INFO(L"音频首块已写入 (bytes=%u, ts=%lld)", bytes, ts100ns);
    }
}
