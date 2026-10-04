# Capture — 轻量屏幕录制工具（Windows）

## 项目概况

Windows 桌面录屏应用：捕获主显示器画面 + 可选的系统声音/麦克风（两路可各自开关、各选设备、带增益），
编码为 MP4（H.264 + AAC）。
单窗口 ImGui 界面：截屏 / 录屏 / 设置三页，含实时预览、录制控制、状态栏；
支持托盘驻留后台运行、全局快捷键、设置持久化（UTF-8 JSON）、开机自启动。

## 技术栈

- 语言/标准：C++20，MSVC（PlatformToolset v145，Unicode 字符集）
- UI：Win32 窗口 + Dear ImGui v1.93（DX11 后端，源码内嵌于 `imgui/`，勿改动）
- 图形：Direct3D 11（设备需 `BGRA_SUPPORT | VIDEO_SUPPORT`，已开多线程保护）
- 屏幕捕获：Windows.Graphics.Capture（WGC，FramePool + FrameArrived 回调）
- 编码：Media Foundation SinkWriter（H.264 硬编优先，无硬编时回退软编；AAC 音频）
- 音频：WASAPI 采集，系统声音走 Loopback、麦克风走 eCapture，输出统一为 int16 PCM 48kHz 双声道
  （float/单声道源自动转换）；两路同开时由 `AudioMixer` 按时间戳对齐后混音（增益 + 限幅）
- 工程文件：`Capture.slnx` + `Capture.vcxproj`（Debug/Release × Win32/x64，开发以 x64 为主）

## 文件结构（源码位于 `src/`，按模块分类）

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | 入口 `wWinMain`：窗口/设备/ImGui 初始化、消息循环、录制状态机（空闲 → recording → saving → 空闲）、UI 动作执行 |
| `src/app_state.h` | 全局运行状态：录制状态机、预览/截图纹理、UI→主循环动作请求、Toast/错误弹窗 |
| `src/ui/ui_app.h/.cpp` | 主界面框架：导航/状态栏/页面切换/错误弹窗；设置页（快捷键编辑、开机自启动） |
| `src/ui/page_capture.h/.cpp` | 截屏页（输出目录/文件名/保存格式/双开关） |
| `src/ui/page_record.h/.cpp` | 录屏页：实时预览、输出位置/帧率/分辨率/码率/编码方式/音频配置（系统声音与麦克风各自开关 + 设备选择器 + 增益）、录制控制 |
| `src/ui/ui_widgets.h/.cpp` | 控件库：按钮/开关/分段/下拉/卡片/路径行/焦点环（主控件自绘，下拉复用原生 popup） |
| `src/ui/ui_theme.h/.cpp` | 主题：深/浅色配色、间距与控件尺寸常量、字体槽位 |
| `src/settings/settings.h/.cpp` | 用户设置（UTF-8 JSON，原子写入）：目录/文件名模板/帧率/画质/音频（含两路开关、设备 ID、增益）/快捷键/界面/自启动意愿 |
| `src/graphics/graphics.h/.cpp` | D3D11 设备 + 交换链封装（Init/Resize/BeginFrame/Present） |
| `src/capture/capture.h/.cpp` | `ScreenCapture`：WGC 捕获封装，工作线程回调输出最新帧 BGRA 纹理 + 100ns 时间戳 |
| `src/screenshot/screenshot.h/.cpp` | `GrabLatestFrame`（纹理→CPU BGRA）/ `CropImage`（按像素矩形裁剪）/ `SaveImage`（WIC，PNG·JPEG·BMP·TIFF·GIF）/ 格式表（`ShotFormatExt`·`ShotFormatLabel`·`StripImageExtension`）/ `CopyToClipboard`（CF_DIB） |
| `src/screenshot/snipping.h/.cpp` | `RunSnipping`：全屏置顶分层窗口遮罩，冻结帧上拖拽框选；单击=整屏，Esc/右键取消 |
| `src/recorder/recorder.h/.cpp` | `Recorder`：MF SinkWriter 封装，码率/硬件加速/音频码率可配；视频走系统内存路径，GPU 回读为立即上下文拷贝 + 双暂存错帧阻塞 Map；`WriteVideoFrame`/`WriteAudio`/`Stop`（Finalize 可能阻塞数秒）/`ReleaseDeviceResources` |
| `src/audio/audio.h/.cpp` | `AudioCapture`：WASAPI 采集封装（`AudioRole::System` 走 eRender+LOOPBACK，`AudioRole::Mic` 走 eCapture），`Open`/`Start`/`Stop` 三段式，设备 ID 可指定（空 = 默认），输出恒为 48kHz/双声道/16bit（float、单声道源自动转换）；`EnumerateAudioDevices` 供设备选择器使用 |
| `src/audio/mixer.h/.cpp` | `AudioMixer`：系统声音 + 麦克风双路按时间戳对齐后相加（各路增益 + 硬限幅），窗口内缺失数据填静音；两路都开时才用，单路直接透传 |
| `src/core/hotkey.h/.cpp` | 全局快捷键注册（RegisterHotKey），与应用冲突时回退 |
| `src/core/trayicon.h/.cpp` | 托盘图标与菜单（隐藏窗口后台驻留） |
| `src/core/appicon.h/.cpp` | 应用图标加载（exe 资源 IDI_CAPTURE） |
| `src/core/autostart.h/.cpp` | 开机自启动：HKCU Run 注册表读写；启用时始终写当前 exe 路径，供启动时自愈 |
| `src/core/log.h/.cpp` | 日志模块：写入 log/ 目录下按启动时间命名的 TXT，多线程安全 |
| `.trae/documents/capture-core-recording-plan.md` | 核心录制版实施计划与设计决策记录 |

## 架构与线程模型

- 单一 D3D11 设备被 UI 渲染、WGC、MF 三方共享（创建时即设 `SetMultithreadProtected(TRUE)`）。
- 执行流：
  1. UI 线程：消息循环 + ImGui 渲染 + 录制控制（Start/Stop 调用点）。
  2. WGC 帧回调线程：`OnCaptureFrame` → 录制中则 `WriteVideoFrame`，同时更新最新帧供预览。
  3. 音频采集线程（最多两条，系统声音/麦克风各一条）：`OnSysAudioData`/`OnMicAudioData`
     → 按录制开始时的路由决定去向：单路直通 `WriteAudio`，两路则 `g_mixer.Push`，
     由混音器在采集线程内合成一条 PCM 后回调 `WriteAudio`。
- 时间戳统一使用 100ns 单位（QPC/MFGetSystemTime 同一时钟基准），录制起始时刻 `baseTime` 做归零。
- 停止录制分两阶段：Phase1 快速停写入（UI 立即显示"正在保存"），Phase2 帧末阻塞执行 `Finalize`。
- 音频启动失败不阻断视频录制，仅 UI 橙色提示；两路各自独立，一路失败只丢该路。

## 重要约定 / 踩坑记录

- **源文件必须保存为 UTF-8 编码；MSVC 编译必须带 `/utf-8`**（已在 vcxproj 四个配置中配置）。
  历史教训：UTF-8 无 BOM 的中文源文件在默认代码页 936（GBK）下会产生 C4819/C2143/C2001 等编译错误。
- 链接依赖：`d3d11.lib;dxgi.lib;d3dcompiler.lib;windowsapp.lib;mfplat.lib;mfreadwrite.lib;mfuuid.lib;ole32.lib;windowscodecs.lib;shell32.lib;comctl32.lib;shlwapi.lib;propsys.lib`（`propsys.lib` 用于读设备友好名的 `PropVariant` 接口）。
- 中文 UI 字体：加载 `C:\Windows\Fonts\msyh.ttc`（微软雅黑），失败回退默认字体（否则中文显示为豆腐块）。
- H.264 要求宽高为偶数，`Recorder::Init` 内自动向下取偶。
- WGC 回调中的纹理由内部持有，回调外不得长期保存裸指针，需保留自行拷贝。
- **ImGui 手动布局卡片（ChannelsSplit 自绘背景）没有 WindowPadding**：内容区不是 child，
  换行后光标退回内容区左缘，第 2 行起文字/控件会紧贴卡片左边界（首行因
  `SetCursorScreenPos` 显式定位看不出问题，更具迷惑性）。修复：内容开头 `Indent(pad)`、
  结尾 `Unindent(pad)` 配对；新建卡片应优先用 `PushCardStyle + BeginChild` 让 padding 自动生效。
- **ImGui 1.92+ child 样式变化**：`BeginChild` 不传 `ImGuiChildFlags_Borders` 时被视为无边框，
  边框与 WindowPadding 被强制清零，`PushCardStyle` 会整体失效。
- **枚举型配置值只能追加**：档位值会写进 `settings.json`（如 `RecResolution`）。
  在枚举中间插值会让旧配置里的数字指向另一个档位（曾是 720P），新档位一律追加到末尾。
- **界面预估与实际取值必须同源**：录制码率同时用于 `Recorder::Init` 与界面的体积预估，
  统一走 `ResolveRecordBitrateMbps`，不要在两处各写一套推导逻辑。
- **`ImGui::Combo` 参数顺序是 `(label, current, items, count)`**（不是 items 在前）；
  多个 Combo 必须各自 `PushID`，否则 ImGui 按控件 ID 缓存的预览值会串到相邻控件。
  选项超过 3 个用 `ComboRow` 而非 `Segmented`：分段控件会把每项压到不足 60px，中文标签必然截断。
- **WIC 只对部分格式提供编码器**：可写的是 PNG / JPEG / BMP / TIFF / GIF；
  WebP、HEIF、JPEG XL 在系统里只有解码器，`CreateEncoder` 必然失败，故不在格式表内。
  格式表（扩展名/短名/文案/GUID）是唯一数据源，UI 选项与路径后缀都从它派生。
- **WIC 无法设置编码质量，别加 JPEG 质量选项**：SDK 里没有任何 WIC 质量 API/GUID；
  `IWICBitmapEncoder::CreateNewFrame` 拿到的 `IPropertyBag2`（ocidl 版，只有 `Read/Write/…`）
  对 `"WIC/JPEG/Quality"`/`"Quality"` 写入一律返回 `E_NOTIMPL`（0x80004005），
  枚举它的属性再逐个 `Write` 甚至会直接访问冲突。实测 640×400 噪声图：
  PNG 874K / JPEG 219K / BMP 1000K / TIFF 1055K / GIF 344K，JPEG 用的是编码器默认质量。
  要可控质量只能引第三方编码库（如 libjpeg-turbo / stb），当前不引入。
- **`IPropertyBag2` 在 wincodec.h 链里是 ocidl.h 那个版本**（`Read/Write/CountProperties/
  GetPropertyInfo/LoadObject`，参数是 `PROPBAG2`+`VARIANT`），没有 `WriteValue`；
  `PROPVARIANT` 的数值成员是 `fltVal/uiVal/…`（`r4` 是 `VARIANT` 的成员名，不是 PROPVARIANT 的）。
  注意 `AppState::errPopup` 是 **UTF-8 的 `std::string`**，`toast` 才是 `std::wstring`，
  别给 `errPopup` 赋 `Utf8ToWide(...)` 的结果。
- **WASAPI loopback 在播放端无活动时完全不产数据**（`GetNextPacketSize` 恒为 0，
  `Initialize`/`Start` 全部成功但 2 秒 0 个包；同一设备伴随播放提示音时 2 秒 199 个包）。
  因此任何「等两路数据到齐再输出」的策略都会把整条音轨卡死在起点。
- **部分 USB 耳机麦克风返回的 `IAudioCaptureClient::GetBuffer` 的 qpcPos 不随时间推进**
  （实测该设备 qpcPos 恒定，块时间戳间隔显示 0ms）。用它做时间源会让音轨时间轴停在原地，
  表现为「音频只录到开头一瞬」（混音器/编码器都按时间戳推进）。
  现策略：优先用 qpcPos，检测到「增量 < 本块帧时长的一半」即切换为块到达时刻（QPC），并做单调化。
- **混音输出上界必须取各路末尾的交集（min），不能取最大值**：取最大值时相位稍慢的一路会
  永远落在输出游标之前而被整段跳过（实测麦克风被系统声音吞掉）。取交集则要防停摆：
  停摆的一路会拖住输出，故按墙钟剔除「超过 200ms 无新数据」的通道，它恢复送数据时
  `Push` 重置时间原点重新纳入（原点对齐到数据时刻，允许最多 200ms 落后）。
- **剔除判据不能用「输出游标落后量」**：交集推进下游标永不超前，落后量恒为 0，永远剔不掉（死锁）。
  只能用墙钟（最后一次 Push 的时刻）。
- **SDK 里没有 `WaveGetProductName`**（10.0.28000.0 的 um 头全部搜不到）；设备友好名只能读
  `PKEY_Device_FriendlyName`，而本机 `propkey.h` 未导出该键，需按 `devpkey.h` 的定义手写
  `PROPERTYKEY`（fmtid `{a45c254e-df1c-4efd-8020-67d146a850e0}`，pid 14），并链 `propsys.lib`。
- **`IMMDeviceCollection::GetCount` 是 out 参数形式**（`GetCount(UINT*)`），不是属性式 `GetCount()`。
- **极简 JSON 解析器的三个坑（`settings.cpp`，均已实测复现，改前务必先看懂）：**
  1. **键之间的分隔符不能无条件吃掉**。循环里写 `if (!cur.Eat(',')) break;` 时，第一个键之前根本没有逗号，
     光标停在 `"` 上，逗号与 `}` 都吃不到，循环会在读第一个键之前就退出 —— 整个文件**零个键**加载，
     而函数照样 `return true` 并打印"配置已加载"，隐蔽性极强（表现为重启后所有设置回默认值）。
     正确做法是加 `bool needComma`：首键免逗号，之后每轮必须先吃到 `,`。
  2. **`ReadKey` 只吃到冒号为止，判断值类型前必须 `SkipWs()`**。
     `"key": "value"` 里冒号后有空格，`*cur.p == '"'` 判不出来，带引号的字符串会被当标量读取，
     于是所有字符串 key 的分支全部落空；数值类则读到 `"1080"` 这种带引号的串，`atoi` 归 0，
     再被 `FillDefaults` 的兜底值盖住 —— 看起来"能读出一部分配置"，实则全错，极易误判为已修好。
  3. **UTF-8 解码严禁按单字节调用 `MultiByteToWideChar`**。多字节序列被逐个拆开后每次输入都是不完整
     序列，返回 `U+FFFD`，中文全部变成问号（实测 `录制_{date}` → `??????_{date}`）。
     必须先把连续原始字节累积到缓冲，再整批转换。
  结论：这三条属于「编译无错、运行无警、日志正常」型缺陷，**任何配置读写的改动都要拿真实
  `settings.json` 跑一遍黑盒验证**（把字段预设成哨兵值，读不到就是没生效），只靠编译通过会漏。
- **`ID3D11DeviceContext::Map` 会占住 D3D 全局锁**：`Recorder::WriteVideoFrame` 跑在 WGC 帧回调线程，
  原先在持锁状态下做阻塞式 `Map`，等待 GPU 期间 UI 线程的 ImGui 绘制被串行阻塞，录制时界面明显卡顿。
  正确解法是**双暂存错帧回读**（见下一条）：读的永远是上一帧的拷贝，GPU 早已完成，
  普通（阻塞）Map 立即返回，锁占用趋近于零。曾试过的 DO_NOT_WAIT 轮询 + 延迟上下文
  方案已被推翻（下一条，致 TDR），勿回退。
- **录制中途改变显示器分辨率必须中止，不能硬写**：帧纹理尺寸会变但 SinkWriter 已按旧尺寸配置，
  继续写会把新画面裁/补成旧尺寸（拉伸变形），且暂存纹理的惰性重建会退化成「每帧销毁重建数 MB 纹理」。
  现由 `Recorder` 在 `Init` 记下源尺寸基准、`WriteVideoFrame` 入口比对后置 `IsSrcSizeBroken()`，
  主循环轮询该标志走标准两阶段停止流程并弹提示。
- **录制回读管线严禁延迟上下文与 DO_NOT_WAIT 轮询（曾致 TDR，"AMD 硬编干崩"系误诊）**：
  2026-10-03 为解决「回读 Map 占 D3D 全局锁卡 UI」引入的形态——每帧
  `CreateDeferredContext`/`FinishCommandList`/`ExecuteCommandList` 回放 +
  `Map(READ|DO_NOT_WAIT)` 轮询——在本机 AMD RX 9060 XT 驱动上必然触发
  `DXGI_ERROR_DRIVER_INTERNAL_ERROR(0x887A0020)`：录制开始后 150~260ms 设备被移除
  （硬编第 ~22 帧，软编第 ~5 帧），**与硬件/软件编码完全无关**。
  早前"AMDh264Encoder 高码率 TDR"是误诊（当时只见过硬编场景崩溃，且误信
  「延迟上下文从第 1 帧就在跑而崩溃在第 ~22 帧，时序对不上，已排除」的推理——
  驱动内部错误随帧累积，不必然首帧触发）。现形态：拷贝与缩放绘制全部走立即上下文
  （多线程保护下跨线程安全，capture.cpp 每帧同形态已长期稳定）+ **双暂存错帧回读**：
  本帧只拷贝进 `staging[stageNext_]`，读的是上一帧拷进另一个 staging 的数据
  （GPU 已有一帧时间完成），普通阻塞 Map 立即返回——既不占锁卡 UI，
  也不用任何非常规标志。修复后软编/硬编各实测 10s：142/145 帧、0 帧错误、无 TDR。
  判断诱因时注意：错误码出现在 `Map` 上不代表 `Map` 是元凶，它只是第一个撞上死设备的调用点。
- **GPU 设备丢失后做进程内恢复，不再退出程序**：设备一丢所有渲染停摆是既定事实
  （`IsDeviceLostHr()`（`core/util.h`）统一判定；`Graphics::Present` /
  `Recorder::WriteVideoFrame` 首次命中即置位标志只记一次日志；主循环轮询后先走
  标准两阶段停止把手头帧 Finalize 进文件），但随后**在进程内重建设备链即可继续运行**，
  无需退出：ImGui 上下文/字体图集、窗口/托盘/热键都与设备无关。
  恢复顺序（`RecoverFromDeviceLost`，main.cpp）：停 WGC → `ImGui_ImplDX11_Shutdown` →
  清 UI 纹理 → `Recorder::ReleaseDeviceResources()`（暂存/缩放/着色器/采样器全绑定
  旧设备，必须清）→ `Graphics::Shutdown/Init` → `ImGui_ImplDX11_Init` → 重建图标与
  缩略图 SRV → WGC 重启 → 继续。注意 `Graphics::Init` 与 `Recorder::Init` 都会清各自的
  `deviceLost_`；设备丢失期间主循环跳过整段 UI 渲染（ImGui DX11 后端在死设备上
  Map 失败的行为不保证）。恢复失败（重建设备失败）才弹窗退出。

## 构建与运行

- 用 Visual Studio 打开 `Capture.slnx`（或 msbuild `Capture.vcxproj`），选 x64 配置编译。
- 输出为 Win32 GUI 程序（`wWinMain`），运行后主显示器预览常开，默认输出路径为 `桌面\录制_yyyyMMdd_HHmmss.mp4`。

## 截屏链路（区域 / 全屏合一）

- 单一入口（按钮或热键）→ `BeginShot`：主窗口可见时先 `SW_HIDE`，等 WGC 送来 2 帧新画面
  （超时 400ms 兜底），确保冻结帧里不含本程序窗口；窗口本就隐藏（托盘）则直接进框选。
- `RunSnipping` 建 `WS_POPUP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED` 遮罩窗口覆盖主显示器，
  底图为冻结帧；`UpdateLayeredWindow(ULW_OPAQUE)` 推画面，故无需关心 GDI 写出的 alpha 字节。
- 渲染分三层 DIB：帧原图（只填一次）→ 压暗图（初始化时逐通道乘 0.42）→ 合成图
  （每次重绘只在压暗底上还原选区，鼠标移动时无逐像素运算）。
- 交互：按下左键拖动出矩形→区域截图；位移 ≤6px 视为单击→整屏；Esc / 右键取消。
  遮罩的消息循环只分发遮罩窗口自身的消息，避免热键/托盘消息重入（WM_QUIT 必须放行）。

## 变更记录（只记功能级变更，小修不记）

- 2026-10-04 TDR 根因纠正 + 设备丢失进程内恢复：本机"录制开始即 TDR"的真正根因是
  10-03 读回管线引入的延迟上下文拷贝 + DO_NOT_WAIT 轮询（纯软编同样崩，第 5~6 帧，
  与 AMD 硬编 MFT 无关，推翻同日早前的"硬编干崩驱动"结论）；回读改为立即上下文 +
  双暂存错帧阻塞 Map（`ConsumeStagedFrame`/`EnsureStagingPair`）。设备丢失后不再
  弹窗即退：新增 `RecoverFromDeviceLost()` 重建设备链（WGC/ImGui 后端/UI 纹理/
  设备与交换链），程序回到可用状态继续运行；丢失期间跳过 UI 渲染。
  软编/硬编各实测 10s：142/145 帧、0 帧错误、无 TDR。
- 2026-10-04 GPU 设备丢失（TDR）处理：新增 `IsDeviceLostHr()` 统一判定丢失类错误码；
  `Graphics` 暴露 `IsDeviceLost()`/`QueryDeviceRemovedReason()`，`Recorder` 暴露 `IsDeviceLost()`；
  主循环检测到后走标准两阶段停止、弹出原生错误框并退出（不进程内重建设备）。
  背景：本机 AMD 硬编在 2560×1440@60 下实测把驱动干崩，原先表现为静默卡死 + 残缺 MP4。
  （注：本条"退出"策略与"硬编干崩"归因当日稍后均被上方条目取代——根因是回读管线，
  且已改为进程内恢复。）
- 2026-10-03 全量缺陷修复（功能级，12 项，Debug/Release ×64 全量重建通过；
   JSON 解析改动另用链接真实 `settings.cpp` 的小程序做了黑盒实测）：
  - **配置持久化修复（致命）**：`LoadSettings` 的解析循环首键无条件吃逗号导致**一个键都读不到**；
    追加 needComma 标记修复。同时修掉「判断值类型前未 SkipWs 使字符串被当标量」（第二条隐藏缺陷）
    与「`JsonUnescape` 逐字节解码把中文转成 U+FFFD」两条，并补上缺失的 `withMic` 读取分支。
    修复前每次启动全部回默认值，且日志仍显示"配置已加载"。
  - **保存阶段的退出请求不再丢失**：原逻辑在 `Saving` 态三个分支都不走且 `wantQuit` 已清零，
     用户点退出毫无反应；改为登记 `g_quitPending`，由 Finalize 完成后执行销毁并给出 Toast。
  - **录制中改变画面尺寸自动中止**：`Recorder` 记录源尺寸基准并暴露 `IsSrcSizeBroken()`，
     主循环轮询后走标准停止流程 + 错误弹窗；`capture` 侧对源尺寸变化补一条告警日志。
  - **录制时的 D3D 回读不再阻塞 UI**：新增 `MapReadNonBlocking`（DO_NOT_WAIT 轮询 + 超时降级阻塞），
     抽出 `EnsureDeferred` 供缩放/非缩放两条路径共用，拷贝命令改由延迟上下文录制后一次性 Execute。
  - **开始录制前先 `EnsureDir`**：目录不存在时不再只报含糊的"创建 SinkWriter 失败"。
  - **音频 COM 生命周期归位**：`AudioCapture::Open` 改用 `ComScope` 作用域守卫，
     采集线程在 `ThreadProc` 内自行初始化，删除主线程反复开关 COM 的 `comInited_` 成员；
     一并删掉从未被读取的 `formatSupported` 出参。
  - **单实例**：`Local\Capture.SingleInstance` 互斥量放在 `LogInit` 之前判定，
     检测到已有实例则 `PostMessage(WM_CAPTURE_WAKE)` 把它的窗口唤到前台后直接退出（不碰日志与配置）。
  - **日志轮转**：`LogInit` 在打开本次日志后按文件名排序（字典序即时间序）保留最近 20 份，其余删除。
  - **框选遮罩覆盖全部显示器**：窗口由主显示器扩到虚拟屏，引入 `monX/monY/monW/monH` 参与坐标换算，
     选区夹在主显示器矩形内（非主显示器无底图，填纯暗底）；单显示器时数值与改动前完全等价。
  - **框选 Esc 兜底**：新增 `WM_TIMER` 轮询 `GetAsyncKeyState(VK_ESCAPE)`，
     解决遮罩失焦后 WM_KEYDOWN 收不到 Esc、只能右键退出的卡死问题。
- 2026-10-03 音频由「只录系统声音」扩为系统声音 + 麦克风双路：两路各自独立开关、
  各自可选设备（下拉选择器 + 刷新按钮）、各自增益（10~300%）；两路同开时由
  `src/audio/mixer` 按时间戳对齐后相加再编码（MP4 只有一条 AAC 音轨）；
  `LoopbackAudio` 重构为 `AudioCapture`（可指定角色与设备 ID），新增 `EnumerateAudioDevices`；
  采集链路 `Open`/`Start` 分离，开始录制时先探测设备再决定要不要建音轨，一路失败只丢该路；
  混音器自带停摆降级（某路 >200ms 无数据即剔除并可恢复），不会出现「有麦克风却录不进声音」。
  配置新增 `withSystemAudio`/`withMic`/`sysAudioDevice`/`micDevice`/`sysVolume`/`micVolume`；
  `withAudio` 作为旧字段保留并作为 `withSystemAudio` 的初值读取。
- 2026-10-03 截图保存格式由「只存 PNG」扩到 5 种并改用下拉框选择：PNG（无损）/
  JPEG / BMP / TIFF / GIF，均为系统 WIC 自带编码器（实测 5 种全部可写）；
  `SavePng` 改为 `SaveImage(path, img, format)`，扩展名/短名/下拉文案集中在
  `screenshot.cpp` 的格式表；配置新增 `shotFormat`；文件名模板里的图片扩展名会被自动替换为所选格式。
- 2026-10-03 录屏配置项补齐并改用下拉框：帧率扩到 7 档（15/24/25/30/50/60/120）、
  新增 1440p 与 4K 档位、视频码率（自动 + 6 档手动）、硬件加速编码开关、AAC 音频码率；
  控件库新增 `ComboRow`，配置量大的项不再用分段控件；底部预估同步显示实际 Mbps。
- 2026-10-03 设置页新增「开机自启动」开关：`core/autostart` 读写 HKCU Run 注册表；
  启动时按配置意愿自愈（exe 位置变化/值丢失自动重写）；开关态每帧读注册表真实状态。
- v1.0 前（阶段性）：UI 拆分至 `src/ui/`（页面/控件库/主题）；设置持久化 `settings.json`；
  全局快捷键 + 托盘驻留；全屏遮罩框选截图链路（见「截屏链路」节）。

## 后续规划（未实现）

选区遮罩录制、框选后再调整（拖动/缩放手柄）、放大镜辅助精确框选、麦克风回声消除（AEC）。
