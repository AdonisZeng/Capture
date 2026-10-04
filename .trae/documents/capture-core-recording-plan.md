# Capture 轻量录屏工具 — 核心录制版实施计划

## 一、概要（Summary）

在现有 VS 工程基础上，实现路线图步骤 1-3 + 扬声器音频：

- 主窗口（DX11 + ImGui）：录制控制条、屏幕实时预览、状态显示
- WGC 全屏捕获：主显示器帧 → D3D11 纹理（显存零拷贝）
- MF SinkWriter：H.264 硬件编码 + AAC 音频，输出 MP4
- WASAPI Loopback：采集扬声器声音混入 MP4

**本次不做**：选区遮罩录制（步骤4）、麦克风混音、全局快捷键（后续迭代，现有模块结构已为其预留扩展点）。

## 二、现状分析（Current State）

| 项 | 现状 |
| --- | --- |
| main.cpp | 仅 ImGui::CreateContext + MessageBox 验证，**无窗口/设备/消息循环** |
| 工程配置 | x64 / C++20 / `/utf-8` 已配好；SDK=10.0(最新)；已链接 d3d11/dxgi/d3dcompiler |
| 缺失 | `windowsapp/mfplat/mfreadwrite/mfuuid/ole32` 未链接；无任何业务模块 |
| ImGui | v1.93.0 WIP（NUM 19297），DX11 后端 `ImGui_ImplDX11_Init(device, ctx)` 新签名，`imgui\backends\` 为唯一版本 |
| 运行验证 | MSBuild x64 Debug 编译通过 |

## 三、新增文件与工程改动

### 模块划分（均放项目根目录，与 main.cpp 同级）

```
main.cpp        入口：窗口/D3D11设备/SwapChain/ImGui初始化/消息循环/UI绘制
graphics.h/.cpp D3D11 设备与交换链封装（创建、Resize、Present、SRV 帮助函数）
capture.h/.cpp  WGC 封装：FramePool(FreeThreaded)+Session+帧回调，输出最新帧纹理
recorder.h/.cpp MF SinkWriter 封装：H264视频+AAC音频，WriteVideoFrame/WriteAudio/Finalize
audio.h/.cpp    WASAPI Loopback 采集线程：扬声器 PCM → int16 → recorder
```

### 3.1 Capture.vcxproj 修改

- `<ClCompile>` 增加 4 个新 .cpp；`<ClInclude>` 增加 4 个新 .h
- 4 个配置的 `<AdditionalDependencies>` 追加：`windowsapp.lib;mfplat.lib;mfreadwrite.lib;mfuuid.lib;ole32.lib;`
- `Capture.vcxproj.filters` 同步新文件（归入 imgui 之外，源文件/头文件筛选器）

### 3.2 graphics 模块

- `CreateDeviceAndSwapChain(hwnd, w, h)`：
  - 设备 Flags：`D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT`（VIDEO_SUPPORT 是 MF 共享设备必需）
  - Feature level 11_0 → 10_0 降级尝试
  - 创建后立即 `ID3D11Multithread::SetMultithreadProtected(TRUE)`（MF/WGC 跨线程必需）
  - SwapChain：`DXGI_FORMAT_R8G8B8A8_UNORM`，`DXGI_SWAP_EFFECT_FLIP_DISCARD`，BufferCount=2
- `ResizeSwapChain(w, h)` 处理窗口尺寸变化（backbuffer 重建后重取 RTV）

### 3.3 main.cpp（重写）

- `wWinMain`：注册窗口类 → 创建 960x600 主窗口 → graphics 初始化 → `ImGui_ImplWin32_Init` + `ImGui_ImplDX11_Init`
- WndProc：转发 `ImGui_ImplWin32_WndProcHandler`；WM_SIZE→Resize；WM_DESTROY→PostQuitMessage
- 消息循环：PeekMessage 非阻塞循环 → `ImGui_ImplDX11_NewFrame/NewFrame` → `UI 绘制` → `Render/RenderDrawData` → `Present(1)`
- 中文 UI 字体：加载 `C:\Windows\Fonts\msyh.ttc`（微软雅黑）+ GetGlyphRangesChineseSimplifiedCommon，否则中文显示豆腐块
- UI 布局（单主窗口）：
  - 顶部状态行：分辨率、实际帧率、录制时长
  - 中部预览：`ImGui::Image(previewSRV, 等比缩放尺寸)`
  - 底部控制条：`开始录制 / 停止录制` 按钮 + "包含鼠标光标" Checkbox + 文件名（默认 `桌面\录制_yyyyMMdd_HHmmss.mp4`，可用 FileDialog? 不引入，保持简单：默认路径显示为只读 + ImGui 输入框可改）
  - 录制中按钮态禁用、停止后显示"正在写入文件…"直到 Finalize 完成

### 3.4 capture 模块（C++/WinRT，核心零拷贝）

- 头：`winrt/Windows.Graphics.Capture.h`、`winrt/Windows.Graphics.DirectX.Direct3D11.h`、`d3d11_4.h`
- 兼容性关键：用 **`IGraphicsCaptureItemInterop::CreateForMonitor`**（Win10 1803+ 全系可用）而非 20348+ 才有的 `CreateFromMonitorId`；主显示器 HMONITOR 由 `MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY)` 获取
- `Direct3D11CaptureFramePool::CreateFreeThreaded(device, DXGI_FORMAT_B8G8R8A8_UNORM, 2, item.Size())`——FreeThreaded 免去 DispatcherQueue
- `FrameArrived` 回调（DXGI 工作线程）：
  1. `TryGetNextFrame()` 取帧，`frame.SystemRelativeTime()` 记录时间戳
  2. `IDirect3DSurface → ID3D11Texture2D`（`GetInterface<>()`，wgc 纹理与主设备同设备，无拷贝）
  3. **CopyResource 到内部复用纹理**（WGC 复用同一池纹理，必须拷贝一份供后续使用；这是一次 GPU-GPU blit，成本极低）
  4. 唤醒：预览 SRV 直接指向内部纹理；若录制中 → 交 recorder.WriteVideoFrame
- 会话控制：`session.IsCursorCaptureEnabled(checkbox)`（低版本 try/catch 降级）；`IsBorderRequired(false)` 尝试设置，**失败静默降级**（裸 exe 无包身份会抛异常，接受黄边框存在）
- 捕获尺寸变化：`item.Closed` 事件 + 帧尺寸与 FramePool 不符时 `Recreate`（此版简化：尺寸不符时丢弃帧并重建 FramePool）

### 3.5 recorder 模块（MF SinkWriter）

- `MFStartup(MF_VERSION)`（引用计数安全，程序退出时 Shutdown）
- `Initialize(path, w, h, fps, hasAudio)`：
  1. `MFCreateDXGIDeviceManager(&token, &mgr)` → `mgr->ResetDevice(d3dDevice, token)`
  2. `MFCreateAttributes`：`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = TRUE`、`mfTranscodeContainerContentType=MFMPEG4Format`、D3D manager 挂入
  3. `MFCreateSinkWriterFromURL(path, nullptr, attrs, &writer)`
  4. **视频流**：OutputType=`MFVideoFormat_H264`（宽高向下取偶、MFSetAttributeRatio 60/1、MF_MT_AVG_BITRATE=16Mbps）；InputType=`MFVideoFormat_ARGB32`（同尺寸，让 SinkWriter 自动插入 GPU VideoProcessor 做 BGRA→NV12 转换，全程不落 CPU）
  5. **音频流**（可选启用）：OutputType=`MFAudioFormat_AAC`(48000/2ch/192kbps)、InputType=`MFAudioFormat_PCM`(48000/2ch/16bit)
  6. `BeginWriting()`
- `WriteVideoFrame(ID3D11Texture2D* tex, LONGLONG ts100ns)`：`MFCreateDXGISurfaceBuffer` 包纹理 → `MFCreateSample` → `SetSampleTime(ts)` → `WriteSample(0, sample)`；时间戳统一 `MFGetSystemTime()`（QPC→100ns）基准，与音频天然同步
- `WriteAudio(const BYTE* data, UINT32 bytes, LONGLONG ts100ns)`：样本内存 `MFCreateMemoryBuffer` 拷贝提交
- `Stop()`：`writer->Finalize()`（可能阻塞数秒，UI 提示保存中）；`MFShutdown` 仅在程序退出
- 硬件编码器保障：除 `ENABLE_HARDWARE_TRANSFORMS` 外，再 `MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE|SORTANDFILTER,...)` 探测硬件 MFT 存在性（仅用于状态栏显示"NVIDIA/AMD/Intel 硬编"或"软编回退"提示，不改写流配置）

### 3.6 audio 模块（WASAPI Loopback）

- 独立采集线程：`GetDefaultAudioEndpoint(eRender, eConsole)` → `IAudioClient::Initialize(SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 100ms)` → `GetService(IAudioCaptureClient)`
- 事件驱动（`AUDCLNT_BUFFERFLAGS` 轮询 + Sleep(5ms) 简化，避免 EVENTFSET 时钟句柄复杂度）
- 格式约定：**仅支持 MixFormat 为 48000Hz/2ch**（float32 或 int16 都接受，float 转 int16 后提交）；其他采样率/声道 → 启动时记录日志并禁用音频轨（不实现重采样，降低复杂度；Win10/11 默认共享模式即 48k/2ch）
- 音频时间戳：`GetBuffer` 包时间 `qpcPosition` 与视频同一 QPC 基准换算 100ns

## 四、关键技术决策（已定案）

| # | 决策 | 理由 |
| --- | --- | --- |
| D1 | FramePool 用 `CreateFreeThreaded` | 免 DispatcherQueue，线程模型最简 |
| D2 | 用 `IGraphicsCaptureItemInterop::CreateForMonitor` | 兼容 Win10 1803+，`CreateFromMonitorId` 需 20348+ |
| D3 | 帧回调内 CopyResource 一次 GPU blit | WGC 池纹理会被复用，必须留副本 |
| D4 | SinkWriter 输入类型 BGRA32，让 MF 自动 GPU 转换 | 免手写 VideoProcessor；`VIDEO_SUPPORT`+MultithreadProtected 是前提 |
| D5 | 时间戳统一 `MFGetSystemTime()` | 音视频同一 QPC 时钟，免手工对齐 |
| D6 | 黄边框尝试关闭，失败静默 | 裸 exe 无包身份必失败；稀疏包方案留给后续 |
| D7 | 音频仅 48k/2ch，否则禁用音频轨 | 免重采样器；覆盖绝大多数系统默认配置 |
| D8 | 无对话框依赖，文件名 ImGui 输入框 + 默认桌面路径 | 保持单文件轻量、零额外依赖 |
| D9 | 全部错误走 MessageBox + 有序退出 | 工具类应用最直观 |

## 五、实施顺序（执行时按此推进）

1. 工程配置：vcxproj/filters 增文件与链接库
2. graphics 模块 + main.cpp 重写（窗口+设备+ImGui 完整帧循环，中文字体）→ **验证点A：编译通过，窗口显示中文字体 UI**
3. capture 模块接入 → 预览显示主屏画面 → **验证点B：预览流畅，与屏幕内容一致**
4. recorder 模块 + 录制按钮 → **验证点C：录制 MP4 可播放、时长正确、尺寸正确**
5. audio 模块 → **验证点D：MP4 内含 AAC 音轨且与画面同步**
6. 收尾：状态栏信息、停止时 UI 反馈、MSBuild 全量验证

## 六、验证步骤

1. `MSBuild Capture.vcxproj /p:Configuration=Debug /p:Platform=x64` 编译零错误
2. 运行 exe：窗口出现，预览区实时显示主屏幕内容（动起来）
3. 勾选/取消"包含鼠标"重启捕获，行为正确
4. 点击"开始录制"→ 播放一段带声音的视频 → 点击"停止录制"
5. 打开生成的 MP4（位于桌面或自定义路径）：
   - 画面与实际操作一致、60fps 左右
   - 属性中编码器为 h264、有 aac 音轨
   - 音画同步（口播/歌词对得上）
6. 录制过程中调整窗口大小 → 不崩溃、预览自适应
7. 停止后再次开始录制 → 第二个文件正常

## 七、风险与降级

| 风险 | 缓解 |
| --- | --- |
| 用户显卡无硬件 H264 MFT | MF 自动回退微软软编，状态栏提示 |
| WGC 黄边框无法隐藏（无包身份） | try/catch 静默，计划后续用稀疏包解决 |
| SinkWriter 不接受 BGRA 输入（个别驱动） | 降级方案：手写 ID3D11VideoProcessor 转 NV12（预留接口） |
| 非主显示器/多屏混合 DPI | 本版固定主显示器，DPI 用 per-monitor aware 清单声明 |
| Finalize 阻塞 UI | 按钮文字变"正在保存…"，仍由 UI 线程阻塞完成（数秒可接受） |
