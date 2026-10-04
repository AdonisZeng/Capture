#pragma once
// 截屏：把 WGC 最新帧搬到 CPU 内存，再存图（系统 WIC 编码器）/ 复制到剪贴板
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <vector>

class ScreenCapture;

// CPU 端 BGRA 像素缓冲（首行对应画面顶部）
struct ImageBGRA
{
    std::vector<BYTE> pixels;
    UINT w = 0, h = 0;

    bool   Valid() const { return w > 0 && h > 0 && !pixels.empty(); }
    size_t Stride() const { return (size_t)w * 4; }
    void   Clear() { pixels.clear(); pixels.shrink_to_fit(); w = h = 0; }
};

// 从捕获模块取最新帧到 CPU（DXGI 纹理为 top-down，首行即画面顶部）
bool GrabLatestFrame(ScreenCapture& cap, ImageBGRA& out);

// 按像素矩形裁剪（left/top 含、right/bottom 不含）；越界自动收敛，不足 1px 返回 false
bool CropImage(const ImageBGRA& src, const RECT& px, ImageBGRA& out);

// 存图（系统 WIC 编码器）：format 取 settings.h 的 ShotFormat，format 越界时按 PNG 处理。
// 注意：WIC 没有公开的编码质量设置接口（编码器的 IPropertyBag2 写不进去），
// JPEG 只能用编码器自身的默认质量，故没有 quality 参数
bool SaveImage(const std::wstring& path, const ImageBGRA& img, int format);

// 格式的扩展名（含点，如 L".jpg"），供拼路径与预览复用；format 越界返回 L".png"
const wchar_t* ShotFormatExt(int format);
// 格式短名（UTF-8，如 "JPEG"），用于 Toast / 错误提示等拼接文案
const char* ShotFormatShortName(int format);
// 格式在下拉框里的显示文本（UTF-8，含画质/体积提示）
const char* ShotFormatLabel(int format);
// 去掉文件名模板末尾的图片扩展名：用户在"文件名"里手打了 .png 又选了 JPEG 时，
// 否则 BuildFilePath 会保留它，导致扩展名与实际编码格式不一致
std::wstring StripImageExtension(const std::wstring& pattern);

// 复制到剪贴板（CF_DIB / BITMAPINFOHEADER，无文件头）
bool CopyToClipboard(const ImageBGRA& img);
