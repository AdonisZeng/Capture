#pragma once
// Media Foundation SinkWriter 封装：BGRA 纹理 -> H.264 硬编 + AAC，输出 MP4
// 时间戳统一 100ns 单位（QPC 基准，与音频一致）
// 视频采用系统内存写入路径：GPU 纹理 -> staging 暂存纹理 -> CPU 缓冲。
// 不使用 MF_SINK_WRITER_D3D_MANAGER（GPU 纹理直写在 AMD 硬编上会
// WriteSample E_INVALIDARG / MF_E_HW_MFT_FAILED_START_STREAMING）。
// 回读约定（重要，曾致 TDR，详见 AGNETS.md 踩坑记录）：
//   1. 拷贝一律走立即上下文，绝不用延迟上下文（CreateDeferredContext /
//      ExecuteCommandList 以 60fps 回放会触发 AMD 驱动内部错误）；
//   2. 双暂存错帧回读：本帧只拷贝，读的是上一帧的拷贝（GPU 已有一帧
//      时间完成），因此用最普通的阻塞 Map 即可立即返回，既不占住
//      D3D 全局锁卡 UI，也不需要 DO_NOT_WAIT 轮询。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
#include <string>

// 探测系统首个硬件视频编码器名称（空 = 无硬编）。
// 独立成自由函数以便界面在开始录制前展示硬件编码能力
std::wstring ProbeHardwareEncoderName();

class Recorder
{
public:
    ~Recorder();

    // 创建 MP4 文件并开始写入。
    // srcW/srcH = 捕获源尺寸；outW/outH = 期望编码尺寸（宽高内部向下取偶）。
    // 目标尺寸小于源尺寸时启用 GPU 缩放（延迟上下文 + 全屏三角形线性采样）；
    // 缩放不可用会自动退回源尺寸录制。
    // crop = 区域录制的源像素矩形（相对捕获帧左上角，右/下不含）：
    //   非空时 outW/outH 被忽略，按裁剪区原分辨率录制（向下取偶），不做 GPU 缩放。
    // hwEncode=false 时禁止硬件变换（纯软件编码）；
    // audioBitrateKbps <= 0 时按 192 kbps 处理。
    bool Init(ID3D11Device* device, const wchar_t* path,
              UINT srcW, UINT srcH, UINT outW, UINT outH,
              UINT fps, UINT bitrateMbps, bool withAudio,
              bool hwEncode, int audioBitrateKbps, std::wstring& err,
              const RECT* crop = nullptr);
    // 结束并 Finalize（可能阻塞数秒）。之后可再次 Init 录制新文件。
    void Stop();
    // GPU 设备丢失后由主循环调用（须在非录制/非保存态）：清掉所有绑定在
    // 旧设备上的缓存资源（暂存/缩放/着色器/采样器），下次 Init 按需重建。
    // 设备重建成功能让后续录制回到干净状态
    void ReleaseDeviceResources();

    void WriteVideoFrame(ID3D11Texture2D* tex, long long ts100ns);
    void WriteAudio(const BYTE* data, UINT32 bytes, long long ts100ns);

    bool     IsRecording() const { return writer_ != nullptr; }
    bool     HasAudio()    const { return withAudio_; }
    // 录制过程中画面源尺寸是否发生变化（切分辨率 / 插拔显示器 / 旋转屏幕）。
    // 一旦置位即不再写入任何帧，调用方应立刻走标准停止流程并告知用户
    bool     IsSrcSizeBroken() const { return srcSizeBroken_.load(); }
    // GPU 设备是否已丢失（TDR/驱动重置）。视频写入会因此静默失败，
    // 调用方必须立刻停止录制并提示用户重启，否则只会得到残缺文件
    bool     IsDeviceLost()    const { return deviceLost_.load(); }
    // 探测到的硬件编码器名（空 = 无硬编，或本次录制关闭了硬件加速）
    const std::wstring& EncoderName() const { return encoderName_; }
    UINT    Width()  const { return outW_; }
    UINT    Height() const { return outH_; }

private:
    void StopLocked();   // 需持有 mutex_ 调用
    // 把双暂存里还压着的最后一帧刷进编码器（Stop/Finalize 前调用，需持有 mutex_）。
    // 不刷则尾部恒丢一帧，且音频尾比视频长一截
    bool FlushPendingFrame();
    // ---- 分辨率缩放 ----
    bool SetupScaler(ID3D11Device* device);          // 目标纹理/RTV/着色器/采样器
    bool ScaleToOutput(ID3D11DeviceContext* imm, ID3D11Texture2D* src);
    // 读回并送编码暂存中待处理的那帧。copyW/H = 本次拷贝进暂存的有效像素
    // （区域录制时小于输出尺寸，其余填零）。返回 false = 设备/编码器已不可用，
    // 调用方应放弃本帧后续写入。无论成败，待处理帧都视为已消费
    bool ConsumeStagedFrame(ID3D11DeviceContext* ctx, UINT copyW, UINT copyH);
    // 确保双暂存纹理按 desc 尺寸就绪（惰性创建，尺寸变化时成对重建）
    bool EnsureStagingPair(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc);

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer_;
    std::mutex mutex_;   // 串行化 Write*/Stop：消除停止录制时与在途写入的竞态
    DWORD videoStream_ = 0;
    DWORD audioStream_ = 1;
    UINT  outW_ = 0, outH_ = 0;
    UINT  srcW_ = 0, srcH_ = 0;      // 录制开始时的偶数源尺寸，用于检测中途变化
    std::atomic<bool> srcSizeBroken_{ false };
    std::atomic<bool> deviceLost_{ false };   // GPU 设备丢失（TDR/驱动重置）
    // 系统内存写入路径的双暂存纹理（CPU 可读），尺寸变化时成对重建。
    // 错帧使用：本帧拷进 stageNext_，读回的是上一帧拷进 stageNext_^1 的那份
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTex_[2];
    UINT  stagingW_ = 0, stagingH_ = 0;
    int   stageNext_ = 0;                 // 下一帧拷贝目标缓冲（0/1 交替）
    bool  stageHasPending_ = false;       // stagingTex_[stageNext_^1] 有上一帧待读
    long long stagedTs_ = 0;              // 待读那帧的采集时间戳
    UINT  pendingCopyW_ = 0, pendingCopyH_ = 0;  // 待读那帧的有效像素（区域录制时 < 输出尺寸）
    // 区域裁剪（源像素坐标，右/下不含）：启用时 outW/H 即裁剪区偶数尺寸，不做 GPU 缩放
    bool  useCrop_ = false;
    UINT  cropX_ = 0, cropY_ = 0, cropW_ = 0, cropH_ = 0;
    // 分辨率缩放（目标小于源时启用）
    bool  scale_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        scaledTex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> scaledRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srcSRV_;
    ID3D11Texture2D* srcTexForSRV_ = nullptr;   // srcSRV_ 对应纹理（捕获纹理会复用，按指针缓存）
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    UINT  fps_ = 60;
    bool  withAudio_ = false;
    bool  hwEncode_ = true;   // 本次是否允许硬件加速编码
    bool  mfStarted_ = false;   // MFStartup 是否由本对象发起（配对 Shutdown）
    long long duration100ns_ = 0;   // 单帧时长
    std::wstring encoderName_;
    // 日志标记：首帧记一条，错误记前 3 条（避免 60fps 刷屏）
    bool videoFirstLogged_ = false, audioFirstLogged_ = false;
    int  videoErrCount_ = 0, audioErrCount_ = 0;   // 已打印的错误数
    int  videoErrTotal_ = 0, audioErrTotal_ = 0;   // 错误总数（Stop 时汇报）
    long long videoFrames_ = 0, audioBlocks_ = 0;  // 成功写入计数
    long long lastAudioTs_ = 0;   // 已写入音频的最大时间戳（单调化用，防静音保活与真实包竞态倒退）
    bool audioHasData_ = false;
};
