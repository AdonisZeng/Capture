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
| `src/settings/settings.h/.cpp` | 用户设置（UTF-8 JSON，原子写入）：目录/文件名模板/帧率/画质/音频（含两路开关、设备 ID、增益）/快捷键/界面/自启动意愿。文件位置 `DataSubDir(L"config") + L"\\settings.json"`；缺失/损坏时自动重建默认配置（损坏另存 `.bak`） |
| `src/graphics/graphics.h/.cpp` | D3D11 设备 + 交换链封装（Init/Resize/BeginFrame/Present） |
| `src/capture/capture.h/.cpp` | `ScreenCapture`：WGC 捕获封装（`StartForMonitor`/`StartForWindow` + 内部共用的 `StartWithItem`），工作线程回调输出最新帧 BGRA 纹理 + 100ns 时间戳；`EnumerateMonitors/FindMonitor/EnumerateWindows` 供画面来源选择器使用 |
| `src/screenshot/screenshot.h/.cpp` | `GrabLatestFrame`（纹理→CPU BGRA）/ `CropImage`（按像素矩形裁剪）/ `SaveImage`（WIC，PNG·JPEG·BMP·TIFF·GIF）/ 格式表（`ShotFormatExt`·`ShotFormatLabel`·`StripImageExtension`）/ `CopyToClipboard`（CF_DIB） |
| `src/screenshot/snipping.h/.cpp` | `RunSnipping`：全屏置顶分层窗口遮罩，冻结帧上拖拽框选；单击=整屏，Esc/右键取消 |
| `src/recorder/recorder.h/.cpp` | `Recorder`：MF SinkWriter 封装，码率/硬件加速/音频码率可配；视频走系统内存路径，GPU 回读为立即上下文拷贝 + 双暂存错帧阻塞 Map；`WriteVideoFrame`/`WriteAudio`/`Stop`（Finalize 可能阻塞数秒）/`ReleaseDeviceResources` |
| `src/audio/audio.h/.cpp` | `AudioCapture`：WASAPI 采集封装（`AudioRole::System` 走 eRender+LOOPBACK，`AudioRole::Mic` 走 eCapture），`Open`/`Start`/`Stop` 三段式，设备 ID 可指定（空 = 默认），输出恒为 48kHz/双声道/16bit（float、单声道源自动转换）；`EnumerateAudioDevices` 供设备选择器使用 |
| `src/audio/mixer.h/.cpp` | `AudioMixer`：系统声音 + 麦克风双路按时间戳对齐后相加（各路增益 + 硬限幅），窗口内缺失数据填静音；两路都开时才用，单路直接透传 |
| `src/core/hotkey.h/.cpp` | 全局快捷键注册（RegisterHotKey），与应用冲突时回退 |
| `src/core/trayicon.h/.cpp` | 托盘图标与菜单（隐藏窗口后台驻留） |
| `src/core/appicon.h/.cpp` | 应用图标加载（exe 资源 IDI_CAPTURE） |
| `src/core/autostart.h/.cpp` | 开机自启动：HKCU Run 注册表读写；启用时始终写当前 exe 路径，供启动时自愈 |
| `src/core/util.h/.cpp` | 通用工具：编码转换、桌面路径、文件名模板展开、时间本地化等；**运行数据目录定位**（`DataRootDir`/`DataSubDir`：exe 同级优先、不可写时回退 `%APPDATA%\Capture`）、`DirIsWritable` 可写探针、`SystemBuildNumber`（ntdll `RtlGetVersion` 取真实版本号） |
| `src/core/log.h/.cpp` | 日志模块：写入 log/ 目录下按启动时间命名的 TXT，多线程安全（目录由 `util.cpp` 的 `DataSubDir(L"log")` 定位，与配置同源） |
| `src/core/version.h` | **版本号单一真源**（SemVer 三个整数宏 + 字符串宏）。纯宏、无 C++ 语法：Capture.rc 会 include 它，而 RC 预处理器不支持字符串化 |
| `app.manifest` | 应用清单（supportedOS / PerMonitorV2 / longPathAware / UTF-8 代码页 / asInvoker / Common-Controls v6），由 vcxproj 四配置的 `<AdditionalManifestFiles>` 合入 exe |
| `src/core/update.h/.cpp` | 应用内更新：查 GitHub Releases → 下载 exe → 自替换替换并重启。网络与磁盘操作全在工作线程，UI 只读状态快照 |

## 架构与线程模型

- 单一 D3D11 设备被 UI 渲染、WGC、MF 三方共享（创建时即设 `SetMultithreadProtected(TRUE)`）。
- 执行流：
  1. UI 线程：消息循环 + ImGui 渲染 + 录制控制（Start/Stop 调用点）。
  2. WGC 帧回调线程：`OnCaptureFrame` → 录制中则 `WriteVideoFrame`，同时更新最新帧供预览。
  3. 音频采集线程（最多两条，系统声音/麦克风各一条）：`OnSysAudioData`/`OnMicAudioData`
     → 按录制开始时的路由决定去向：单路直通 `WriteAudio`，两路则 `g_mixer.Push`，
     由混音器在采集线程内合成一条 PCM 后回调 `WriteAudio`。
- 时间戳统一使用 100ns 单位（QPC/MFGetSystemTime 同一时钟基准），录制起始时刻 `baseTime` 做归零。
- 暂停：音视频回调直接丢帧，恢复时 `baseTime += 暂停时长`，时间轴无缝（暂停段被剪掉）。
- 停止录制分两阶段：Phase1 快速停写入（UI 立即显示"正在保存"），Phase2 首帧起工作线程
  执行 `Finalize`（阻塞数秒），完成后主循环回收线程并 Toast；退出/设备丢失都等 saving 清零。
- 音频启动失败不阻断视频录制，仅 UI 橙色提示；两路各自独立，一路失败只丢该路。

## 重要约定 / 踩坑记录

- **源文件必须保存为 UTF-8 编码；MSVC 编译必须带 `/utf-8`**（已在 vcxproj 四个配置中配置）。
  历史教训：UTF-8 无 BOM 的中文源文件在默认代码页 936（GBK）下会产生 C4819/C2143/C2001 等编译错误。
- 链接依赖：`d3d11.lib;dxgi.lib;d3dcompiler.lib;windowsapp.lib;mfplat.lib;mfreadwrite.lib;mfuuid.lib;ole32.lib;windowscodecs.lib;shell32.lib;comctl32.lib;shlwapi.lib;propsys.lib;winhttp.lib`
  （`propsys.lib` 读设备友好名的 `PropVariant` 接口；`winhttp.lib`
  供 `core/update.cpp` 做 HTTPS）。
- **改 `winhttp` 相关代码时注意**：`winhttp.h` 只声明 Unicode 版函数，
  名字**没有** `W` 后缀（`WinHttpQueryHeaders` 而非 `WinHttpQueryHeadersW`），
  参数是 `LPCWSTR`。取单个响应头（如 `Location`）用 `WINHTTP_QUERY_LOCATION`
  （枚举值 33），**没有** `WINHTTP_QUERY_HEADER` 这个常量，写错会一直取不到值
  从而被误判成「重定向缺少 Location」。
- 中文 UI 字体：加载 `C:\Windows\Fonts\msyh.ttc`（微软雅黑），失败回退默认字体（否则中文显示为豆腐块）。
- H.264 要求宽高为偶数，`Recorder::Init` 内自动向下取偶。
- WGC 回调中的纹理由内部持有，回调外不得长期保存裸指针，需保留自行拷贝。
- **ImGui 手动布局卡片（ChannelsSplit 自绘背景）左右两侧都没有 WindowPadding**：
  卡片不是 child，内容直接画在页面 child（`"content"`，`WindowPadding = CardPad`）里。
  - **左侧**：`Indent(pad)` 修不好「换行后光标退回内容区左缘」的问题——`Indent` 只把光标往右推，
    第 2 行起文字/控件会紧贴卡片左边界（首行因 `SetCursorScreenPos` 显式定位看不出问题，更具迷惑性）。
    修复：内容开头 `Indent(pad)`、结尾 `Unindent(pad)` 配对。
  - **右侧**：`Indent` **完全不管右边界**，而卡片右缘恰好**就是**页面 child 的内容区右缘
    （卡片 `cardW` 取的就是 `GetContentRegionAvail().x`，两者同源）。故任何按
    `GetContentRegionAvail().x` 测宽或右对齐的控件（`ToggleRow` 的开关、
    `SetNextItemWidth(-1.0f)` 的进度条/输入框）在手动布局卡片里会**精确压在卡片描边上**，
    零留白，且与上方 `BeginChild` 卡片的同名控件明显对不齐。
    **必须显式扣掉 `Control::CardPad`**：`ToggleRow` 传第四参 `rightInset`，
    整宽控件写 `-(pad + Control::EdgeInset)`（EdgeInset 另防滚动容器裁剪线削掉描边半像素）。
  - 结论：**新建卡片一律优先用 `PushCardStyle + BeginChild`**，左右 padding 自动生效，
    不要为了「高度随内容自适应」而去写手动布局——那正是右侧留白缺失的来源。
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

## 版本管理与自动更新

### 版本号：单一真源

**`src/core/version.h` 是全仓库唯一需要改版本号的地方**，改一处即同时生效于：

1. exe 资源属性（`Capture.rc` 的 `VS_VERSION_INFO`，`FILEVERSION` 取整数宏、
   `FileVersion`/`ProductVersion` 取字符串宏）
2. 界面左下角显示（`ui_app.cpp` 的 `Update::VersionDisplay()`）
3. 与 GitHub Release `tag_name` 的新旧比较（`update.cpp` 的 `IsNewer`）

不存在第二份需要手工同步的版本号。

**踩坑：RC 预处理器不支持字符串化（`#`）与相邻字面量拼接**，所以
`CAPTURE_VERSION_STR` 只能显式写出，无法由三个整数宏拼出来。
代价是字符串与整数可能脱节，因此 `update.cpp` 顶部有
`static_assert(VersionStrConsistent())` 在编译期校验二者一致——
只改其一时编译直接失败，不会带着两个版本号发出去。

另注：`Capture.rc` 需要 `#pragma code_page(65001)` 才能在字符串表里写中文；
而 RC 预处理器拿不到 `_DEBUG`（`_DEBUG` 不会传给 `rc.exe`），
故 `FILEFLAGS` 一律为 0，Debug/Release 不体现在版本资源里。

### 自动更新链路

`core/update.cpp`，链路为：查 `/releases/latest` → 下载 → 校验 → 替换 → 重启。

**为什么用 `/releases/latest` 而不是 `/releases` 列表**：前者自动排除草稿与
预发布（prerelease），语义恰好等于「正式版」。故测试版必须标 prerelease，
否则正式版用户会被提示去装测试版。

**替换自身复用同一个 exe 的助手模式**（命令行 `--apply-update`），不引入第二个
可执行文件。原理：**运行中的映像不能被覆盖，但可以改名**——助手等旧进程退出后，
先把当前 exe 改名成 `Capture.exe.old`，再把新文件移到位，最后拉起新版本。
`.old` 留给新实例下次启动删除（`CleanupStaleBackup`）；万一新版本起不来，
用户改名回滚即可。

**`--apply-update` 的 `RunApplyHelper()` 必须排在单实例互斥量之前**
（`wWinMain` 最开头）。否则助��这个第二实例会被互斥量判定成「已有实例」直接退出，
永远等不到该做的换文件动作。

**重启带 `--updated` 标记**，新实例据此**重试等待互斥量**（最多 10s）：
旧进程刚退出时内核尚未释放互斥量，不重试就会被秒退，用户看到的是
「程序自己关掉了且再也不出现」。

**助手也是本进程的孩子**，主进程退出后仍会继续运行，不受影响。

### 安全性边界（务必如实告知用户）

exe **没有 Authenticode 签名**，可信度只有一层：

1. WinHTTP 的 HTTPS 证书链校验（默认开启；代码里**不得**设置
   `WINHTTP_OPTION_SECURITY_FLAGS` 的 `IGNORE_*` 标志），并显式要求 TLS 1.2+
   （下载完整性只核对 Release 元数据里的 size，不做哈希）

这层挡得住传输截断，**挡不住文件被替换、CA 被攻破或本机被装根证书**。
给 `core/update.cpp` 加 `WinVerifyTrust` 验签才真正闭环，见 `docs/RELEASING.md`
的「后续」一节。**在签名之前，对外不要宣称更新包「已验证来源可信」。**

### 产物命名是硬性约定

`Capture-x64.exe` / `Capture-win32.exe`（固定名，不带版本号）。
名字对不上，用户点检查更新会得到「无法解析 GitHub 返回的发布信息」。
固定名的原因：带版本号会导致手动下载后堆出一堆旧文件，
桌面快捷方式也会因文件名变化而失效（自动更新是原地覆盖，不受影响）。
详见 `docs/RELEASING.md`。

### 其他约定

- **GitHub 未认证接口限流是每 IP 每小时 60 次**。公司/校园/云主机 NAT 出口
  容易和别人共用这个额度，用满就是 403。故 `HttpGet` 对 401/403/404/429
  分别给了可执行的中文文案，而不是干巴巴的「HTTP 403」。
- **启动自动检查最短间隔 24 小时**（`updateAutoCheck` / `updateLastCheck` /
  `updateSkipTag` 三个键落盘在 `settings.json`），避免频繁触发限流与打扰用户。
- **`update.cpp` 的极简 JSON 取值器**只做「按 key 取字符串/整数」，
  `assets` 数组靠花括号配平切分（产物名与直链里不含花括号）。
  解析失败时会把响应开头 2KB 打进日志——GitHub 改了字段格式能立刻看出来。
- **录制/保存期间更新按钮一律禁用**：换文件必须等程序完全空闲，
  否则助手要等主进程退出、而主进程此时还在等 `Finalize`，用户看到的是「点了没反应」。

## 构建与运行

- 用 Visual Studio 打开 `Capture.slnx`（或 msbuild `Capture.vcxproj`），选 x64 配置编译。
- 输出为 Win32 GUI 程序（`wWinMain`），运行后主显示器预览常开，默认输出路径为 `桌面\录制_yyyyMMdd_HHmmss.mp4`。

### 本机工具链位置（不在 PATH，也不在默认安装路径）

Visual Studio 装在 **D 盘**，所以 `where msbuild` / `where cl` 一律找不到，
`C:\Program Files*\Microsoft Visual Studio` 下只有 Installer（没有 C++ 工具链）。
实际可用路径：

| 用途 | 路径 |
| --- | --- |
| MSBuild（命令行编译首选，amd64） | `D:\Software\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe` |
| 环境初始化脚本 | `D:\Software\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat` |
| `cl.exe`（v145 工具集） | `D:\Software\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe` |
| Windows SDK 头文件（核对 API 用） | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0` |

命令行编译（工作目录为仓库根）：

```powershell
& 'D:\Software\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  Capture.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
```

要点：

- **`v145` 工具集只存在于 MSBuild 侧目录 `MSBuild\Microsoft\VC\`（那里有 v150/v160/v170/v180，没有 v145）；
  MSVC 编译器本体在 `VC\Tools\MSVC\14.51.36231`（另有旧版 14.29.30133）。两者版本号不对应，
  不要按 `VC\Tools\MSVC\` 下的数字去拼工具集名。**
- 工程里 `WindowsTargetPlatformVersion` 写的是 `10.0`（跟随最新已装 SDK），
  本机实装 10.0.26100.0 与 10.0.28000.0。
- 想直接调 `cl.exe` 必须先跑 `vcvars64.bat`，否则 `INCLUDE`/`LIB` 为空、WinRT 与 WIC 头都找不到。

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

- 2026-10-06 更新流程可见性（Release|x64 重建通过）：
  - 下载/校验中新增全页面浮层进度（`DrawUpdateProgressOverlay`）：之前进度只在设置页
    卡片里，而发现弹窗一点就关且无 toast，不在设置页就表现为「点了没反应」；
    弹窗点的下载也补了 toast。
  - 下载完成新增全局「更新已就绪」弹窗 + toast（之前只在设置页出现按钮，无任何通知）；
    录制/保存中安装按钮禁用，与卡片一致。
  - 更新失败新增全局「更新失败」弹窗（含重试/打开发布页/关闭，按错误文本去重）；
    仅用户主动操作后才弹，开机自动检查的失败只写状态行，避免无网络时每次启动都打扰。
  - 注意：本改动须随下个 PATCH 版本发布，不可直接替换 v1.0.1 附件（exe 变了 SHA-256
    就对不上，1.0.0 客户端会校验失败）。
> 本节是**仓库内部**的实现记录，粒度比面向用户的 `CHANGELOG.md` 粗。
> 发版时要写给用户看的内容整理到 `CHANGELOG.md`；发布流程见 `docs/RELEASING.md`。

- 2026-10-06 设置页热键支持按键捕获（Release|x64 重建通过）：
  - 每行新增「更改」按钮：点击后该行进入捕获态，直接按组合键录入，
    成功即填入并自动应用保存 + 重注册；Esc / 「取消」/ 切页作废。
  - 新增 `HotkeyManager::Compose`（修饰键 + 主键组装规范串，裸键/不支持的主键拒绝），
    主键集与 `Parse` 同源（A-Z / 0-9 / F1-F24 / Space / Insert / Delete /
    Pause / Tab / Home / End / PrtSc），`KeyName` 是唯一数据源。
  - 捕获期间主循环吞掉 `WM_HOTKEY`（`UiApp::IsCapturingHotkey`），避免按到旧组合
    误触截图/录制；捕获行输入框禁用展示，他行输入框锁定但可切换捕获目标；
    手动输入框与「应用并保存」保留，走同一 `ApplyAllHotkeys` 入口。
- 2026-10-06 开机自启动只驻留托盘（Release|x64 重建通过）：
  - 自启动注册表命令追加 ` --minimized`，开机由 Explorer 拉起时主窗口保持
    `SW_HIDE`、仅托盘图标驻留；用户双击/正常启动仍走 `nShowCmd` 原样显示。
  - 旧值迁移：`AutostartIsEnabled` 只比对 exe 路径（带/不带标志都算已启用，
    设置页开关不乱跳），缺标志的旧值由主入口启动时补写一次；
    新增 `AutostartHasMinimizedFlag` 供迁移判断，`--tray` 也认作同义标志。
  - 更新重启透传最小化态：`Apply` → 助手 → 新版本三段命令行都携带
    `--minimized`，开机自启后更新不会突然弹主窗口。
- 2026-10-04 版本管理 + 应用内自动更新（Release/Debug × x64/Win32 四配置
  重建通过；设置项改动按 AGENTS.md 要求做了 JSON 黑盒往返实测 18 项全过；
  网络层对 github.com 实测 TLS/证书链/分块读取/落盘字节数一致，
  404 与 403 两条错误文案均真实触发验证）：
  - **版本单一真源** `src/core/version.h`（SemVer 宏，RC 预处理器安全），
    驱动 exe `VS_VERSION_INFO`、界面显示、与 Release tag 比较；
    `static_assert` 校验字符串宏与整数宏一致，防脱节。
    `Capture.rc` 加 `#pragma code_page(65001)` 以便字符串表写中文。
    原先 `kVersion = "v1.0"` 硬编码在 `ui_app.cpp`，已移除。
  - **`core/update.h/.cpp`**：查 `/releases/latest` → 下载 → SHA-256 校验
    （BCrypt）→ 自替换替换并重启。助手复用本 exe 的 `--apply-update` 模式，
    靠「运行中映像可改名不可覆盖」实现无第二个可执行文件的替换；
    旧进程 exe 留 `.old` 备份，新实例启动时清理。
  - **入口顺序**：`RunApplyHelper()` 必须在单实例互斥量之前（否则助手被
    互斥量秒退）；重启实例带 `--updated` 时**重试等待互斥量最多 10s**
    （旧进程刚退出时内核尚未释放互斥量，否则用户看到程序自己关掉不再出现）。
  - **UI**：设置页新增「版本与更新」卡片（当前版本 / 状态行 / 进度条 /
    按钮组 / 自动检查开关），高度按内容实际结束位置反算——状态在
    检查中/有新版本/下载中/已就绪/失败之间切换，行数会变，写死必然溢出。
    发现新版本弹模态框（含发布说明与四个动作）。托盘菜单加「检查更新…」。
  - **配置新增三键**：`updateAutoCheck` / `updateLastCheck`（Unix 秒）/
    `updateSkipTag`；启动静默检查最短间隔 24h，避免频繁触发 GitHub 限流。
  - **HTTP 错误文案分状态码**：GitHub 未认证接口是每 IP 每小时 60 次，
    NAT 出口容易共用额度被打满（403），故 401/403/404/429 各给可执行文案。
  - **`TrayAction::CheckUpdate` 必须是枚举最后一个成员**：`OnMessage` 用
    「值落在 `Show..CheckUpdate` 区间」识别主动投递，新增成员忘改上界
    会表现为「菜单点了没反应」。
- 2026-10-04 P1+P2（Debug/Release ×64 重建通过）：
  - **暂停/继续**：`AppState::paused` + 主循环 `wantPause/wantResume`，
    回调层丢帧、恢复时 `baseTime += 暂停时长`（暂停段被剪掉，混音器按跳变自愈）；
    录屏页暂停时双按钮（继续/停止），状态栏/托盘/预览计时经 `RecElapsedSec` 同源扣除暂停段；
    录制热键在暂停态按一次=继续，托盘菜单增“暂停/继续”项。
  - **画面来源**：录屏页新增来源下拉（显示器/窗口/区域）；
    `capture` 新增 `EnumerateMonitors/FindMonitor/EnumerateWindows` 与
    `StartForMonitor/StartForWindow`（公共会话逻辑下沉 `StartWithItem`）；
    显示器按设备名持久化（拔出回退主显示器），窗口为会话内 HWND（关闭则中止录制），
    区域在开始时复用框选、按裁剪区原分辨率录制（`Recorder::Init` 新增 `crop` 参数，
    暂存按裁剪尺寸建、`ConsumeStagedFrame` 改传有效像素）。
    来源切换经 `wantCaptureRestart` 在空闲时重启预览；`RecoverFromDeviceLost` 同走新入口。
  - **延时开始**：设置 `recDelaySec`（立即/3/5/10 秒），倒计时显示在录制按钮区，
    再按一次开始键/停止键取消；切换画面来源自动取消倒计时。
  - **画质预设**：清晰优先（原画60fps自动）/均衡（1080P30fps自动）/小体积（720P30fps8M）。
  - **录后动作**：`openFolderAfterRec` 开关，异步保存完成后 `RevealInExplorer`。
  - **异步保存**：`StopRecordingPhase2` 首帧起线程跑 `Finalize`，主循环轮询
    `g_saveDone` 后 join 收尾；保存期间界面不卡，退出/设备丢失照常等待收尾。
  - **电平表**：`AudioCapture::Level()`（真实包峰值+150ms 衰零，试音/录制共用），
    主循环 20fps 刷到 `AppState::sysLevel/micLevel`，录屏页两路音量下滑条显示；
    新增空闲“试音 5 秒”（只采电平不写文件，开始录制自动顶掉）。
  - **框选放大镜+微调**：遮罩拖拽时光标旁 8 倍放大镜（十字线+自动避让选区），
    方向键 1px 微调（Shift=10px），回车确认。
  - **历史**：截图/录制各记最近 8 条（会话内），两页底部最多 4 个定位按钮。
  - **小修**：窗口外框尺寸 `WM_EXITSIZEMOVE` 回存；光标开关经 `wantCursorRefresh`
    实时刷到 WGC 会话；托盘增“打开输出目录”；Toast 4s→5s。
- 2026-10-04 P0 健壮性三件套（Debug/Release ×64 重建通过）：
  - **尾帧刷入**：`Recorder::StopLocked` 在 `Finalize` 前调新增 `FlushPendingFrame()`，
    把双暂存里压着的最后一帧送编码，否则尾部恒丢一帧且音频尾比视频长。
    另加 `lastAudioTs_/audioHasData_` 对音频 `WriteSample` 时间戳做单调钳位，
    防静音保活块与真实包交错时倒退报 `MF_E_INVALIDREQUEST`。
  - **静音保活**：`AudioCapture::ThreadProc` 空闲超 20ms 即补一块 960 帧静音
    （QPC 时间戳+单调化）。Loopback 无播放时本来 0 包，单路直通音轨会出空洞；
    补后混音路由也不再因一路静默频繁触发 200ms 停摆剔除。
  - **磁盘预检+录中保护**：新增 `core/util::GetFreeDiskBytes`；
    `StartRecording` 按“2 分钟预估”预检（<100MB 拒绝，<500MB/不足 2 分钟 Toast 警告），
    录制中每 2s 轮询一次，<100MB 自动走标准停止并弹窗，避免 0 字节残文件。
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
- 2026-10-04 审查问题修复（11 项；Debug/Release ×64 全量重建通过）：
  - 区域录制的界面预估与实际同源：`CapRegion` 时分辨率档位实际不生效（`Recorder::Init`
    用裁剪区覆盖 `outW/outH`），故录屏页把分辨率下拉置灰并改提示文案；底部体积预估
    改为「按所选区域尺寸计算」，不再报一个与文件不符的数字。预设仍可用（帧率/码率生效）。
  - 截屏页「最近截图」计入 `MeasureFooterHeight`（新增 `FooterMetrics::historyRows`）：
    此前追加在固定区之后却不预留高度，卡片内容溢出、child 长出滚动条，缩略图与底部
    固定区的比例关系失效。显示行数由 `HistoryRowCount()` 统一给出，避免测量与绘制写死不同值。
  - 「最近录制」移出 `BeginDisabled` 作用域：录制/保存中也能点开上一次的成片。
  - 裁剪区钳位补 `min(copyW, outW_)` / `min(copyH, outH_)`：`Init` 已把裁剪夹到源尺寸内，
    该分支当前不可达，但一旦触发，`ConsumeStagedFrame` 慢路径会按 `copyW*4` 写出超过
    `dstStride` 的行、末行越过 MF 缓冲末尾。
  - `AppState::baseTime` 改 `std::atomic<long long>`：暂停恢复会在录制中途改写它，
    而 WGC 帧回调线程与音频线程都在读，普通 `long long` 属数据竞争。
  - 区域框选改为与截图同构的两阶段（`RecPhase::WaitRegionFrame` + 主循环轮询），
    不再用 `Sleep` 阻塞消息循环，避免窗口进入「未响应」。补齐与之冲突的动作：等帧期间的
    截图/试音被拦下并提示，换来源、退出、再次按开始都会作废该阶段并把隐藏的窗口放回来
    （帧号已丢弃，无法继续推进）。
  - `StartPreviewCapture` 回退到主显示器时一并把 `captureSource` 复位为 `CapMonitor`
    并落盘，界面状态与实际预览一致；录屏页在窗口失效时给出橙色提示。
  - 主题补 `ImGuiCol_PlotHistogram/Hovered`：`ProgressBar` 取的是 PlotHistogram 而非
    PlotLines，此前电平条用 ImGui 默认色渲染，深浅主题下都不协调。
  - `EnumerateWindows` 结果按标题排序（标题相同再按句柄）：`EnumWindows` 返回 Z 序，
    界面里表现为顺序随机。
  - 移除已删除的 `.trae/documents/capture-core-recording-plan.md` 引用。
- 2026-10-04 复审问题修复（6 项；Debug/Release ×64 全量重建通过）：
  - **等帧期间的截图拦截是死代码**：拦截块排在 `if (g_st.wantShot)` 之后，而后者会无条件
    清掉 `wantShot` 并直接 `BeginShot()`。区域框选等帧期按截图热键会先弹一层截图遮罩，
    紧接着同一轮主循环再弹一层区域遮罩（两层顺序叠加，而非注释描述的「先拦下」）。
    已把拦截块前移到 `BeginShot` 之前。
  - **设备恢复时区域阶段只作废、不还原窗口**：`RecoverFromDeviceLost` 里
    `g_recPhase = RecPhase::None` 漏了 `RestoreWindowAfterRegion()`，与其余三处作废点
    不一致。设备丢失恰好落在「已 `SW_HIDE` 主窗口、等区域帧」的 ≤400ms 窗口内时，
    主窗口会永久停在 `SW_HIDE`（界面还在渲染到隐藏窗口，用户只能走托盘唤回）。
  - **试音失败不释放麦克风**：`StartMicTest` 中 `Open` 已把句柄挂到 `AudioCapture` 上，
    `Start` 失败时直接 return，而 `g_micTestActive` 仍为 false ——
    5 秒超时回收与 `StartRecording` 的「先停掉试音」都不会来，句柄一直挂到下次
    `Open`/进程退出。补 `g_micAudio.Stop()`。
  - **窗口被关后预览不会自动回退**：`StartPreviewCapture` 的回退分支只在启动与
    `wantCaptureRestart`（换来源/选窗口）时触发；空闲期目标窗口被关，没有任何东西触发它，
    预览永久冻结在最后一帧、按开始录制只会弹「所选窗口已关闭」。主循环新增检测：
    空闲 + `CapWindow` + `capWindow` 非空且已失效 → 置 `wantCaptureRestart`，
    复用既有回退路径（回退成功后 `capWindow` 清空 + `captureSource` 变 `CapMonitor`，
    条件自然不再命中，不会每帧重试）。录屏页提示文案同步改为「将回退到显示器」。
  - `WM_EXITSIZEMOVE` 补 `IsZoomed` 判断：双击标题栏最大化同样会走一次 EXITSIZEMOVE，
    原先会把整屏尺寸当成外框尺寸存进 `windowW/windowH`，下次启动还原成「几乎全屏」。
  - 删除已无调用方的 `ScreenCapture::Start`（`main.cpp` 一律走
    `StartPreviewCapture` → `StartForMonitor/StartForWindow`），避免捕获层多一个入口。

## 发布适配（发 GitHub 前必看）

「发版前要检查什么」这一节写在 `docs/RELEASING.md`，本节只记**实现层面的坑**。

### 运行数据的位置：exe 同级优先，不可写时回退

配置 `config/settings.json` 与日志 `log/` 都由 `core/util.cpp` 的
`DataRootDir()` / `DataSubDir()` 定位：

- **exe 目录可写**（便携模式）→ 就用它。exe 与 `config/`、`log/` 同一个文件夹，
  拷走即带走配置。
- **exe 目录不可写**（典型如 `C:\Program Files`）→ 回退 `%APPDATA%\Capture\`。
  实际落点写在启动日志第一行。

踩坑与约定：

- **原先的实现是「从 exe 目录向上最多 5 层找已有的 `config`/`log` 子目录」**
  （`FindDataDir` / `log.cpp` 的 `ResolveLogDir`，两份重复代码）。那是开发期的便利，
  对发布版是隐患：exe 放在 `D:\Tools\` 时会误命中 `D:\config` 这类无关目录，
  于是配置与日志凭空跑到别处，用户和排查问题的人都不知道去哪找。
  现已统一到 `DataSubDir()` 一处，`FindDataDir` 已删除（`update.cpp` 里那份
  重复的 `DirIsWritable` 也一并并回 `util.cpp`）。
- **`DirIsWritable` 用 `FILE_FLAG_DELETE_ON_CLOSE` 探针**：句柄一关文件就没，
  不会在用户的 exe 目录或 `%APPDATA%` 里留垃圾。注意**不要**顺手改成普通的
  创建+删除：程序崩溃时残留的探针文件比少一个判据麻烦得多。
- **`ExeDir()` 用动态缓冲而非 `MAX_PATH` 数组**：长路径安装目录下 `GetModuleFileNameW`
  会被截断成半截路径，随后所有配置/日志路径全错。`update.cpp` 的 `SelfExePath()`
  本来就是动态的，现在两边一致。
- **配置缺失或损坏时自动重建**：文件不存在走「按默认值初始化」；
  文件存在但解析不出（大小异常 / 缺 `{`）时，**先复制一份 `settings.json.bak`**
  再写默认配置 —— 用户手改坏 JSON 时不该连旧设置一起丢。
  重建前会 `s = Settings()` 清零，避免把上次解析出的半截值当成默认值写回去。
- **`DataRootDir()` 的缓存不是线程安全的**，只允许主线程调用（配置读写、
  `LogInit` 都在启动路径的主线程上）。
- ⚠️ **本机 `%APPDATA%\Capture` 已存在**，且里面有一个**别的程序**留下的
  `ScreenRecorder.ini`。回退路径撞名不是本项目引入的，但要知道这个目录不干净。

### Release 必须静态链接 CRT（发布阻塞项）

`vcxproj` 的 Release 两个配置写了 `<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`
（Debug 保持 `/MDd`）。原先没写，Release 继承默认 `/MD`，产物导入
`MSVCP140.dll` / `VCRUNTIME140.dll` / `VCRUNTIME140_1.dll` —— **那三个 DLL
只随 VC++ Redistributable 分发，系统默认不带**。本机因为装了 VS 一直正常，
从 GitHub 下载的用户会直接报「VCRUNTIME140.dll 缺失」起不来。
这是典型的「本地全绿、发布即废」，改链接方式后必须用
`dumpbin /dependents` 验一次（`docs/RELEASING.md` 第 5.1 节）。

### 应用清单 app.manifest

根目录新增 `app.manifest`，通过 `<AdditionalManifestFiles>` 挂进
**四个配置**（只挂 Release 会让 Debug 构不出正确清单）。内含：

| 声明 | 为什么 |
| --- | --- |
| `supportedOS` Win10/11 | 缺了它系统按 Win7 处理进程（DPI 虚拟化、部分 API 走旧分支） |
| `dpiAware=true/pm` + `PerMonitorV2` | 混合 DPI 多屏不糊。清单是权威来源（须在窗口创建前生效），`main.cpp` 的 `ImGui_ImplWin32_EnableDpiAwareness()` 只是运行时兜底 |
| `longPathAware` | 配置/日志路径拼在 exe 目录上，深目录下可能超 260 |
| `activeCodePage=UTF-8` | 本程序文件 IO 全走 W 版 API 不受影响，给第三方/系统组件兜底 |
| `asInvoker` | 录屏要抓整个桌面，提权无必要且部分机器上会让 WGC 取不到画面 |
| `Common-Controls v6` | 资源管理器里 exe 不显示成经典外观 |

- **`<heapType>SegmentHeap</heapType>` 由 vcxproj 的 `<EnableSegmentHeap>` 自动注入，
  `app.manifest` 里不要重复写**（mt.exe 合并时会冲突）。
- 验证方式：`mt.exe -inputresource:<exe>;#1`（`docs/RELEASING.md` 第 5.2 节）。

### 启动时检查系统版本下限

`main.cpp` 在 `LogInit()` 之后立刻用 `SystemBuildNumber()`（`util.cpp`，
走 ntdll 的 `RtlGetVersion`）判断是否 ≥ 17763，不足则弹框并退出。

- **不用 `GetVersionEx`**：它受清单 `supportedOS` 影响，没声明 Win10 就返回 6.2。
- **不用 `VerifyVersionInfo`**：参数容易填错（`dwBuildNumber` 在 16 位域里），
  填错时静默判定为「不支持」。
- 取不到版本号（返回 0）一律放行 —— 宁可让用户试，也不该因为探测失败把人挡在门外。

## 后续规划（未实现）

框选后手柄调整（拖动/缩放已选区）、麦克风回声消除（AEC）。
