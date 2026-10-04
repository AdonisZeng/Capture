#include "screenshot.h"
#include "capture/capture.h"
#include "settings/settings.h"
#include "core/log.h"
#include "core/util.h"
#include <wrl/client.h>
#include <wincodec.h>   // 已间接带来 IPropertyBag2 / PROPVARIANT（经 ocidl.h、propidl.h）
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <shlwapi.h>
#include <cstring>
#include <cwchar>

using Microsoft::WRL::ComPtr;

bool GrabLatestFrame(ScreenCapture& cap, ImageBGRA& out)
{
    ComPtr<ID3D11Texture2D> tex;
    UINT w = 0, h = 0;
    cap.GetLatestFrame(tex, w, h);
    if (!tex || w == 0 || h == 0)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        LOG_WARN(L"截屏: 不支持的纹理格式 %u", (unsigned)desc.Format);
        return false;
    }

    ComPtr<ID3D11Device> dev;
    tex->GetDevice(dev.GetAddressOf());
    if (!dev)
        return false;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(ctx.GetAddressOf());

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, staging.GetAddressOf())))
    {
        LOG_ERR(L"截屏: 创建 staging 纹理失败");
        return false;
    }
    ctx->CopyResource(staging.Get(), tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        LOG_ERR(L"截屏: staging 纹理映射失败");
        return false;
    }

    out.w = w;
    out.h = h;
    const size_t rowBytes = (size_t)w * 4;
    out.pixels.resize(rowBytes * h);
    const BYTE* src = (const BYTE*)mapped.pData;
    const size_t srcStride = (size_t)mapped.RowPitch;
    if (srcStride == rowBytes)
    {
        memcpy(out.pixels.data(), src, rowBytes * h);
    }
    else
    {
        for (UINT y = 0; y < h; ++y)
            memcpy(out.pixels.data() + rowBytes * y, src + srcStride * y, rowBytes);
    }
    ctx->Unmap(staging.Get(), 0);
    return true;
}

bool CropImage(const ImageBGRA& src, const RECT& px, ImageBGRA& out)
{
    if (!src.Valid())
        return false;

    LONG l = px.left, t = px.top, r = px.right, b = px.bottom;
    if (r < l) { LONG tmp = l; l = r; r = tmp; }
    if (b < t) { LONG tmp = t; t = b; b = tmp; }
    l = l < 0 ? 0 : (l > (LONG)src.w ? (LONG)src.w : l);
    t = t < 0 ? 0 : (t > (LONG)src.h ? (LONG)src.h : t);
    r = r < l ? l : (r > (LONG)src.w ? (LONG)src.w : r);
    b = b < t ? t : (b > (LONG)src.h ? (LONG)src.h : b);
    if (l >= r || t >= b)
        return false;

    out.w = (UINT)(r - l);
    out.h = (UINT)(b - t);
    const size_t rowBytes = (size_t)out.w * 4;
    out.pixels.resize(rowBytes * out.h);

    const size_t srcStride = src.Stride();
    const BYTE* sp = src.pixels.data() + (size_t)t * srcStride + (size_t)l * 4;
    BYTE* dp = out.pixels.data();
    for (UINT y = 0; y < out.h; ++y)
        memcpy(dp + rowBytes * y, sp + srcStride * y, rowBytes);
    return true;
}

// ---------------------------------------------------------------------------
// 保存格式表：只列系统 WIC 自带编码器的格式。
// WIC 只有解码器、没有编码器的格式（WebP / HEIF / JPEG XL）不在此列——
// 硬塞进来只会在 CreateEncoder 阶段失败
// ---------------------------------------------------------------------------
namespace {

struct FormatInfo
{
    int            format;
    const wchar_t* ext;        // 含点
    const char*    shortName;  // UTF-8
    const char*    label;      // UTF-8，下拉框显示
    GUID           container;  // WIC 容器格式
};

const FormatInfo kFormats[] = {
    { ShotFormatPng,  L".png",  "PNG",  "PNG（无损，推荐）",       GUID_ContainerFormatPng },
    { ShotFormatJpeg, L".jpg",  "JPEG", "JPEG（有损，体积小）",   GUID_ContainerFormatJpeg },
    { ShotFormatBmp,  L".bmp",  "BMP",  "BMP（未压缩，体积大）", GUID_ContainerFormatBmp },
    { ShotFormatTiff, L".tiff", "TIFF", "TIFF（无损，兼容印刷）", GUID_ContainerFormatTiff },
    { ShotFormatGif,  L".gif",  "GIF",  "GIF（256 色，体积小）", GUID_ContainerFormatGif },
};

// 越界格式一律退回 PNG：配置文件里的数字可能来自旧版本
const FormatInfo& Fmt(int format)
{
    for (const FormatInfo& f : kFormats)
        if (f.format == format)
            return f;
    return kFormats[0];
}

}   // namespace

const wchar_t* ShotFormatExt(int format)
{
    return Fmt(format).ext;
}

const char* ShotFormatShortName(int format)
{
    return Fmt(format).shortName;
}

const char* ShotFormatLabel(int format)
{
    return Fmt(format).label;
}

std::wstring StripImageExtension(const std::wstring& pattern)
{
    static const wchar_t* const kExts[] = { L".png", L".jpg",  L".jpeg", L".bmp",
                                            L".tif", L".tiff", L".gif" };
    std::wstring name = pattern;
    for (const wchar_t* e : kExts)
    {
        const size_t n = wcslen(e);
        if (name.size() > n && _wcsicmp(name.c_str() + name.size() - n, e) == 0)
        {
            name.resize(name.size() - n);
            break;
        }
    }
    return name;
}

bool SaveImage(const std::wstring& path, const ImageBGRA& img, int format)
{
    if (!img.Valid())
        return false;

    const FormatInfo& fmt = Fmt(format);

    std::wstring dir, file;
    SplitPath(path, dir, file);
    if (!EnsureDir(dir))
    {
        LOG_ERR(L"截屏: 无法创建目录 %s", dir.c_str());
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()))))
    {
        LOG_ERR(L"截屏: WIC 工厂创建失败");
        return false;
    }

    ComPtr<IWICBitmap> bmp;
    HRESULT hr = factory->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppBGRA,
                                                 img.w * 4, (UINT)img.pixels.size(),
                                                 const_cast<BYTE*>(img.pixels.data()), bmp.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: CreateBitmapFromMemory 失败(0x%08lX)", hr);
        return false;
    }

    // 编码到内存流再落盘：SHCreateStreamOnFileEx 的 shell 流对 WIC 编码器会返回
    // WINCODEC_ERR_STREAMWRITEFAILED(0x88982F71)，内存流最稳定
    ComPtr<IStream> stream;
    {
        IStream* mem = SHCreateMemStream(nullptr, 0);
        if (!mem)
        {
            LOG_ERR(L"截屏: 创建内存流失败");
            return false;
        }
        stream.Attach(mem);
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(fmt.container, nullptr, encoder.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: %hs 编码器创建失败(0x%08lX)", fmt.shortName, hr);
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: 编码器初始化失败(0x%08lX)", hr);
        return false;
    }

    // 只取帧、不取属性包：WIC 的编码质量无法设置（详见头文件注释）
    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: 创建编码帧失败(0x%08lX)", hr);
        return false;
    }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: 编码帧初始化失败(0x%08lX)", hr);
        return false;
    }
    // 必须先声明尺寸与像素格式，否则 WriteSource 会返回 WINCODEC_ERR_WRONGSTATE
    hr = frame->SetSize(img.w, img.h);
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: 设置尺寸失败(0x%08lX)", hr);
        return false;
    }
    WICPixelFormatGUID srcFormat = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&srcFormat);
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: 设置像素格式失败(0x%08lX)", hr);
        return false;
    }
    hr = frame->WriteSource(bmp.Get(), nullptr);
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: 写入像素失败(0x%08lX)", hr);
        return false;
    }
    hr = frame->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();
    if (FAILED(hr))
    {
        LOG_ERR(L"截屏: %hs 提交失败(0x%08lX)", fmt.shortName, hr);
        return false;
    }

    // ---- 内存流 -> 文件 ----
    LARGE_INTEGER zero = {};
    ULARGE_INTEGER total = {};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_END, &total)) ||
        FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
    {
        LOG_ERR(L"截屏: 定位内存流失败");
        return false;
    }
    if (total.QuadPart == 0 || total.QuadPart > 512ULL * 1024 * 1024)
    {
        LOG_ERR(L"截屏: 编码结果大小异常(%llu)", total.QuadPart);
        return false;
    }

    std::vector<BYTE> buf((size_t)total.QuadPart);
    ULONG read = 0;
    if (FAILED(stream->Read(buf.data(), (ULONG)buf.size(), &read)) || read == 0)
    {
        LOG_ERR(L"截屏: 读取编码结果失败");
        return false;
    }

    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
    {
        LOG_ERR(L"截屏: 创建文件失败(0x%08lX) %s", GetLastError(), path.c_str());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(hf, buf.data(), read, &written, nullptr);
    CloseHandle(hf);
    if (!ok || written != read)
    {
        LOG_ERR(L"截屏: 写文件失败(0x%08lX)", GetLastError());
        return false;
    }
    return true;
}

bool CopyToClipboard(const ImageBGRA& img)
{
    if (!img.Valid())
        return false;
    if (!OpenClipboard(nullptr))
    {
        LOG_ERR(L"剪贴板: OpenClipboard 失败(0x%08lX)", GetLastError());
        return false;
    }
    EmptyClipboard();

    // CF_DIB = BITMAPINFOHEADER + 自下而上的 BGRA 像素（无文件头）
    const DWORD headerSize = sizeof(BITMAPINFOHEADER);
    const DWORD total = headerSize + (DWORD)img.pixels.size();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!mem)
    {
        CloseClipboard();
        return false;
    }
    void* ptr = GlobalLock(mem);
    if (!ptr)
    {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }

    BYTE* base = (BYTE*)ptr;
    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)base;
    bih->biSize = headerSize;
    bih->biWidth = (LONG)img.w;
    bih->biHeight = (LONG)img.h;   // 正数 = 自下而上
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)img.pixels.size();
    bih->biXPelsPerMeter = 0;
    bih->biYPelsPerMeter = 0;
    bih->biClrUsed = 0;
    bih->biClrImportant = 0;

    BYTE* dst = base + headerSize;
    const size_t rowBytes = (size_t)img.w * 4;
    for (UINT y = 0; y < img.h; ++y)
        memcpy(dst + rowBytes * y, img.pixels.data() + rowBytes * (img.h - 1 - y), rowBytes);

    GlobalUnlock(mem);
    if (!SetClipboardData(CF_DIB, mem))
    {
        LOG_ERR(L"剪贴板: SetClipboardData 失败(0x%08lX)", GetLastError());
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

