#pragma once
// 通用工具：编码转换、可执行文件目录定位、文件名模板展开
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// ---- D3D/DXGI ----
// 判定是否为「设备已丢失」类错误（驱动重置 / TDR / 驱动内部错误）。
// 一旦命中，本进程内该设备基本不可恢复（WGC / MF / UI 共用同一设备），
// 调用方应停止录制并提示用户重启，而不是继续往死设备上写
inline bool IsDeviceLostHr(HRESULT hr)
{
    switch (hr)
    {
    case DXGI_ERROR_DEVICE_REMOVED:         // 0x887A0005
    case DXGI_ERROR_DEVICE_HUNG:            // 0x887A0006
    case DXGI_ERROR_DEVICE_RESET:           // 0x887A0007
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:  // 0x887A0020
        return true;
    default:
        return false;
    }
}

// ---- 编码转换（UI 层与 ImGui 的 std::string 均按 UTF-8 处理）----
std::string  WideToUtf8(const wchar_t* s);
std::wstring Utf8ToWide(const char* s);
std::string  WideToUtf8Str(const std::wstring& s);

// ---- 目录 ----
// 可执行文件所在目录（不带尾部分隔符）
std::wstring ExeDir();
// 从 exe 目录向上最多 5 层查找已存在的 name 子目录；找不到则返回 exeDir\name
std::wstring FindDataDir(const wchar_t* name);
// 桌面路径（SHGetKnownFolderPath 失败时返回 "."）
std::wstring DesktopDir();
// 确保目录存在（逐级 CreateDirectoryW）
bool EnsureDir(const std::wstring& dir);
// 用系统「打开文件夹」资源管理器选中文件；失败返回 false
bool RevealInExplorer(const std::wstring& file);

// ---- 文件名 ----
// 当前本地时间
void LocalTimeParts(int& y, int& mo, int& d, int& h, int& mi, int& s);
// 展开模板占位符：{date}=yyyyMMdd {time}=HHmmss {n}=序号
std::wstring ExpandPattern(const std::wstring& pattern, int seq);
// 组合目录 + 展开后的文件名（pattern 不含扩展名时补 .ext）
std::wstring BuildFilePath(const std::wstring& dir, const std::wstring& pattern,
                           const wchar_t* ext, int seq);
// 从完整路径中拆出目录与文件名（含扩展名）
void SplitPath(const std::wstring& path, std::wstring& dir, std::wstring& file);
// 输出完整路径的超长显示形式（超长时中间省略）
std::wstring EllipsizePath(const std::wstring& path, size_t maxChars);
