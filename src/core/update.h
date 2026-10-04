#pragma once
// 应用内更新：查询 GitHub Releases -> 下载 exe -> SHA-256 校验 -> 自替换替换并重启
//
// 更新源：本仓库的 GitHub Releases。用 /releases/latest 而不是 /releases 列表，
// 它自动排除草稿与预发布（prerelease），正好等于「正式版」语义。
//
// 产物命名约定（发布时必须遵守，见 docs/RELEASING.md）：
//     Capture-vX.Y.Z-x64.exe    Capture-vX.Y.Z-x64.exe.sha256
//     Capture-vX.Y.Z-win32.exe  Capture-vX.Y.Z-win32.exe.sha256
// 校验文件名固定为 <exe 名>.sha256，内容是 sha256sum 格式（十六进制 + 两空格 + 文件名）。
//
// 安全性：exe 没有 Authenticode 签名，可信度只依赖两层——
//   1. WinHTTP 的 HTTPS 证书链校验（默认开启，代码里不关闭任何校验标志，
//      并显式要求 TLS 1.2 及以上，避免老系统上协商到 TLS 1.0）
//   2. 发布物 SHA-256 与 Release 上的 .sha256 附件比对
// 这挡得住传输损坏与中间人降级，挡不住 CA 被攻破或本机安装了根证书的场合。
// 真要闭环得给 exe 签名（signtool + 代码签名证书），见 docs/RELEASING.md 的「后续」。
//
// 替换自身复用同一个 exe 的助手模式（命令行 --apply-update），不引入第二个可执行文件：
// 运行中的 exe 不能被覆盖，但可以被改名（映像节允许 rename），故助手等旧进程退出后
// 先把当前 exe 改名成 <exe>.old，再把新文件移到位，最后拉起新版本；
// .old 留给新实例在下次启动时删掉，新版本起不来时用户还能手动改名回滚。
//
// 线程模型：网络与磁盘操作全在工作线程，UI 只读状态快照。
// 主循环每帧调 Tick()，由它推进状态并决定何时弹 Toast。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace Update {

// 流程状态。Available / Ready 是两个「等用户决定」的停顿态，
// 其余为过程态或终态；任何错误统一落到 Failed，原因见 ErrorText()
enum class State
{
    Idle,        // 还没检查过
    Checking,    // 正在查 GitHub
    UpToDate,    // 已是最新
    Available,   // 有新版本，等用户确认下载
    Downloading, // 下载中（Progress 有意义）
    Verifying,   // SHA-256 校验中
    Ready,       // 已下载并校验通过，等用户确认替换
    Failed,      // 失败，原因见 ErrorText()
};

// 一次检查拿到的发布信息
struct Info
{
    std::string tag;        // tag_name，如 "v1.2.0"
    std::string notes;      // Release 正文（Markdown 原文，未加工）
    std::string pageUrl;    // Release 网页地址（下载失败时的兜底出口）
    std::string assetName;  // 匹配到的产物名，如 Capture-v1.2.0-x64.exe
    std::string assetUrl;   // 产物直链
    std::string shaUrl;     // 校验文件直链（assetUrl + ".sha256"）
    long long   assetSize = 0;   // 产物字节数（下载后比对，不符即判失败）
    int         major = 0, minor = 0, patch = 0;
};

// ---- 版本号（由 core/version.h 的宏拼出，不手写第二份）----
const char* VersionString();    // "1.0.0"
const char* VersionDisplay();   // "v1.0.0"
const char* ArchName();         // "x64" / "win32"，与产物命名后缀一致

// ---- 状态快照（UI 线程读，内部已加锁：Info 按值返回，不留数据竞争）----
State        CurrentState();
Info         LatestInfo();
std::string  StatusText();      // 设置页状态行（UTF-8）
std::string  ErrorText();       // 失败原因（UTF-8），非 Failed 时为空
double       Progress();         // 下载进度 0..1，非 Downloading 时为 0
bool         Busy();             // Checking / Downloading / Verifying

// ---- 生命周期 ----
// 启动检查：autoCheck 为配置里的开关，lastCheckUnix 为上次检查时刻（Unix 秒），
// skipTag 为用户上次忽略的 tag。距上次超过 24h（或从未检查过）且开关打开时才发起，
// 静默进行，不打扰用户
// skipTag 必须由调用方从 settings.json 传入：忽略状态只存配置，
// 漏传会导致「忽略此版本」跨重启失效，且退出时回写会把磁盘上的值抹成空
void Init(bool autoCheck, long long lastCheckUnix, const std::wstring& skipTag);
// 收尾：请求中止进行中的下载并等工作线程结束（不阻塞在网络 IO 上）
void Shutdown();
// 主循环每帧调用：回收已结束的工作线程、清理临时目录、决定要不要提示新版本。
// appIdle 为 false（正在录制或保存）时不提示，等空闲后再补，避免打断录制。
// 返回 true 表示「本次调用消费掉了一条发现新版本的提示」，调用方据此弹一次 Toast
bool Tick(bool appIdle);
// 自动检查开关（设置页切换时调用，立即生效，不必等下次启动）
void SetAutoCheck(bool on);
// 上次检查时刻（Unix 秒），main 在退出时落盘到 settings.json
long long LastCheckUnix();

// ---- 用户动作（均可在任意状态调用，内部自行判断是否合法）----
void CheckNow();          // 手动检查（设置页按钮 / 托盘菜单）
void StartDownload();     // 确认后下载并校验
bool Apply();             // 确认后拉起替换助手；返回 true 表示调用方应退出主进程
void SkipVersion();       // 忽略当前 tag（由 SkipTag() 落盘到 settings.json）
void OpenReleasePage();   // 用默认浏览器打开 Release 网页

// 被忽略的版本号（用户点「忽略此版本」时由 SkipVersion 写入，main 负责落盘）
std::wstring SkipTag();

// ---- 助手模式（wWinMain 最前面调用）----
// 命令行带 --apply-update 时：等旧进程退出 -> 换文件 -> 拉起新版本，
// 返回 true 表示调用方应立即 return。必须早于单实例互斥量判定，
// 否则第二个实例会被互斥量直接退出掉，助手永远等不到该做的事
bool RunApplyHelper();
// 命令行是否带 --updated（更新后重启的实例用它来重试等待互斥量）
bool HasRelaunchFlag();
// 清理上次替换遗留的 <exe>.old（正常启动时调用）
void CleanupStaleBackup();

}   // namespace Update
