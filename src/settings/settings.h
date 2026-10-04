#pragma once
// 用户设置：录制/截图参数 + 快捷键 + 界面状态，持久化为 UTF-8 JSON
#include <string>

// 录制分辨率档位（Settings::recResolution 取值）
// 注意：新档位只能追加到末尾。数值会写进 settings.json，
// 插在中间会让旧配置里的数字指向另一个档位（2 曾是 720P）
enum RecResolution
{
    RecResNative = 0,   // 原画：跟随画面源分辨率
    RecRes1080P  = 1,   // 1920×1080
    RecRes720P   = 2,   // 1280×720
    RecRes1440P  = 3,   // 2560×1440
    RecRes2160P  = 4,   // 3840×2160
    RecResCount  = 5
};

// 截图保存格式（Settings::shotFormat 取值）
// 只列系统 WIC 自带编码器的格式；WebP / HEIF / JPEG XL 在 WIC 里只有解码器，
// 故不在支持范围内。扩展名与显示文案统一由 screenshot.h 的
// ShotFormatExt / ShotFormatLabel 提供，不要在此处另写一份
// 注意：新格式只能追加到末尾，数值会写进 settings.json
enum ShotFormat
{
    ShotFormatPng  = 0,   // 无损，体积中等
    ShotFormatJpeg = 1,   // 有损，体积小（质量由 WIC 默认值决定，不可调）
    ShotFormatBmp  = 2,   // 未压缩，体积最大
    ShotFormatTiff = 3,   // 无损，兼容印刷软件
    ShotFormatGif  = 4,   // 256 色
    ShotFormatCount= 5
};

// 画面来源（Settings::captureSource 取值，新值只能追加）
enum CapSource
{
    CapMonitor = 0,   // 显示器（captureMonitorDevice 为空 = 主显示器）
    CapWindow  = 1,   // 指定窗口（HWND 会话内有效，不持久化）
    CapRegion  = 2,   // 区域（开始录制时框选，按所选区域原分辨率录制）
    CapSourceCount = 3
};

struct Settings
{
    // ---- 录制 ----
    std::wstring saveDir        = L"";   // 输出目录（空 = 首次运行时取桌面）
    std::wstring recordPattern  = L"录制_{date}_{time}";   // 不含扩展名
    int          fps            = 60;   // 15 / 24 / 25 / 30 / 50 / 60 / 120
    int          recResolution  = RecResNative;   // RecResolution：以分辨率决定画质
    int          bitrateMbps    = 0;    // 视频码率：0 = 按分辨率自动，否则 5..200 手动指定
    bool         hwEncode       = true; // 允许硬件加速编码（关闭则纯软件编码）
    bool         withSystemAudio= true; // 录制扬声器系统声音
    bool         withMic        = false;// 录制麦克风（与系统声音可各自独立开关）
    std::wstring sysAudioDevice = L"";  // 系统声音设备 ID（空 = 系统默认播放设备）
    std::wstring micDevice      = L"";  // 麦克风设备 ID（空 = 系统默认录音设备）
    int          sysVolume      = 100;  // 系统声音增益 %（0..200，100 = 原音量）
    int          micVolume      = 100;  // 麦克风增益 %（0..200，100 = 原音量）
    int          audioBitrate   = 192;  // AAC 音频码率 kbps：96 / 128 / 192 / 256
    bool         includeCursor  = true; // 画面含鼠标指针
    int          captureSource  = CapMonitor; // CapSource：显示器 / 窗口 / 区域
    std::wstring captureMonitorDevice = L"";  // 显示器设备名（如 \\.\DISPLAY1，空 = 主显示器）
    int          recDelaySec    = 0;    // 开始录制前的延时秒数（0 = 立即）
    bool         openFolderAfterRec = false;  // 录制保存完成后打开文件位置

    // ---- 截图 ----
    std::wstring shotDir        = L"";   // 空 = 跟随 saveDir
    std::wstring shotPattern    = L"截图_{date}_{time}";
    bool         shotSaveFile   = true;  // 存盘
    bool         shotCopyClip   = true;  // 复制到剪贴板
    int          shotFormat     = ShotFormatPng;   // 保存格式：ShotFormat

    // ---- 快捷键 ----
    std::wstring hotkeyCapture  = L"Ctrl+Alt+S";   // 区域截图（占用则回退 Win+Shift+S）
    std::wstring hotkeyRecord   = L"Ctrl+Alt+R";   // 开始/停止录制
    std::wstring hotkeyShow     = L"Ctrl+Alt+C";   // 显示主窗口

    // ---- 通用 ----
    bool         autoStart      = false; // 开机自动启动（实际生效以注册表 HKCU Run 为准）

    // ---- 更新（core/update.h）----
    bool         updateAutoCheck = true; // 启动时静默检查更新（最短间隔 24h）
    long long    updateLastCheck = 0;    // 上次检查时刻（Unix 秒，0 = 从未检查）
    std::wstring updateSkipTag   = L"";  // 用户选择忽略的版本（tag_name，如 v1.2.0）

    // ---- 界面 ----
    int          lastPage       = 0;    // 0=截屏 1=录屏
    bool         darkMode       = true; // true=深色 false=浅色
    int          windowW        = 1080;
    int          windowH        = 700;
};

// settings.json 完整路径（位于 exe 同级，向上最多 5 层查找已有数据目录）
std::wstring SettingsFilePath();
// 读取；文件不存在时返回 false 并保持 s 为默认值
bool LoadSettings(Settings& s);
// 原子写入（先写 .tmp 再 MoveFileEx 覆盖）
bool SaveSettings(const Settings& s);
// 补全空字段（首次运行：目录取桌面）
void FillDefaults(Settings& s);
// 由画面源尺寸 + 档位算出实际编码尺寸：等比缩放、只缩不放、宽高向下取偶（H.264 要求）
void RecordOutputSize(unsigned srcW, unsigned srcH, int preset,
                      unsigned& outW, unsigned& outH);
// 按输出分辨率给出推荐码率（Mbps）：分辨率越高码率越高
int RecordBitrateMbps(unsigned outW, unsigned outH);
// 实际使用的视频码率：Settings::bitrateMbps > 0 时取手动值，否则按分辨率自动。
// 录制入口与界面上的体积预估必须共用本函数，否则两者会显示不一致
int ResolveRecordBitrateMbps(const Settings& s, unsigned outW, unsigned outH);
// 按当前设置预览最终输出路径（seq=1 便于观察）
std::wstring PreviewRecordPath(const Settings& s);
std::wstring PreviewShotPath(const Settings& s);
