#include "ocr.h"
#include "core/log.h"

#include <winrt/base.h>
#include <MemoryBuffer.h>   // Windows::Foundation::IMemoryBufferByteAccess（SDK winrt 目录）
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

std::thread          g_thread;
std::atomic<bool>    g_busy { false };
std::atomic<bool>    g_done { false };
std::atomic<bool>    g_stopping { false };
std::mutex           g_mu;
OcrOutcome           g_result;

void Finish(bool ok, std::wstring text, std::wstring error)
{
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_result.ok    = ok;
        g_result.text  = std::move(text);
        g_result.error = std::move(error);
    }
    g_busy.store(false);
    g_done.store(true);
}

std::wstring AvailableLanguages()
{
    std::wstring out;
    try
    {
        for (const auto& lang : winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages())
        {
            if (!out.empty())
                out += L", ";
            out += lang.LanguageTag().c_str();
        }
    }
    catch (...)
    {
    }
    return out.empty() ? L"（无）" : out;
}

// BGRA 像素拷贝进 SoftwareBitmap（系统 OCR 唯一接受的输入形式）
void CopyToSoftwareBitmap(const ImageBGRA& img,
                          winrt::Windows::Graphics::Imaging::SoftwareBitmap& bitmap)
{
    using namespace winrt::Windows::Graphics::Imaging;

    bitmap = SoftwareBitmap(BitmapPixelFormat::Bgra8,
                            (int32_t)img.w, (int32_t)img.h,
                            BitmapAlphaMode::Ignore);
    auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
    auto reference = buffer.CreateReference();

    winrt::com_ptr<::Windows::Foundation::IMemoryBufferByteAccess> access;
    IUnknown* unknown = reinterpret_cast<IUnknown*>(winrt::get_abi(reference));
    winrt::check_hresult(unknown->QueryInterface(
        winrt::guid_of<::Windows::Foundation::IMemoryBufferByteAccess>(),
        access.put_void()));

    BYTE* data = nullptr;
    UINT32 capacity = 0;
    winrt::check_hresult(access->GetBuffer(&data, &capacity));

    // 按平面描述的步长逐行拷贝：不能假设 stride == width * 4
    const auto plane = buffer.GetPlaneDescription(0);
    const size_t need = (size_t)plane.StartIndex + (size_t)plane.Stride * plane.Height;
    if (!data || capacity < need || plane.Stride < (int32_t)(img.w * 4))
        throw winrt::hresult_error(E_FAIL, L"OCR 位图缓冲区大小异常");
    for (UINT y = 0; y < img.h; ++y)
    {
        memcpy(data + plane.StartIndex + (size_t)y * plane.Stride,
               img.pixels.data() + (size_t)y * img.w * 4,
               (size_t)img.w * 4);
    }

    // 必须显式解锁：未关闭的 IMemoryBufferReference / 仍存活的 BitmapBuffer 会让
    // RecognizeAsync 报 WINCODEC_ERR_ALREADYLOCKED (0x88982F0D)
    reference.Close();
    buffer = nullptr;
}

// 超过 OCR 引擎上限的截图等比缩小（双线性）。
// 4K 全屏在部分系统/语言包下会超过 OcrEngine::MaxImageDimension()，旧逻辑
// 直接判失败，导致这些机器上整屏识别完全不可用；缩放后仍然可以识别。
// 返回无效 ImageBGRA 表示无需缩放或缩放失败（调用方保留原图）。
ImageBGRA DownscaleToFit(const ImageBGRA& src, uint32_t maxDim)
{
    ImageBGRA out;
    if (!src.Valid() || maxDim == 0)
        return out;

    // 用带括号的 (std::min)/(std::max)：windows.h 的 min/max 宏会破坏 std:: 限定名
    const double scale = (std::min)((double)maxDim / (double)src.w,
                                    (double)maxDim / (double)src.h);
    if (scale >= 1.0)
        return out;

    out.w = (UINT)(std::max)(1.0, std::floor(src.w * scale));
    out.h = (UINT)(std::max)(1.0, std::floor(src.h * scale));
    out.pixels.resize((size_t)out.w * out.h * 4);

    // 预先算好每列的源坐标与水平权重，行内只做插值
    std::vector<int>   x0(out.w), x1(out.w);
    std::vector<float> fx(out.w);
    for (UINT x = 0; x < out.w; ++x)
    {
        const double sx = (x + 0.5) * src.w / out.w - 0.5;
        int ix = (int)std::floor(sx);
        const double frac = sx - ix;
        if (ix < 0)
            ix = 0;
        int ix1 = ix + 1;
        if (ix1 >= (int)src.w)
            ix1 = (int)src.w - 1;
        x0[x] = ix;
        x1[x] = ix1;
        fx[x] = (float)frac;
    }

    for (UINT y = 0; y < out.h; ++y)
    {
        const double sy = (y + 0.5) * src.h / out.h - 0.5;
        int iy = (int)std::floor(sy);
        const float fy = (float)(sy - iy);
        if (iy < 0)
            iy = 0;
        int iy1 = iy + 1;
        if (iy1 >= (int)src.h)
            iy1 = (int)src.h - 1;

        const BYTE* row0 = src.pixels.data() + (size_t)iy * src.Stride();
        const BYTE* row1 = src.pixels.data() + (size_t)iy1 * src.Stride();
        BYTE* dst = out.pixels.data() + (size_t)y * out.Stride();
        for (UINT x = 0; x < out.w; ++x)
        {
            const BYTE* p00 = row0 + (size_t)x0[x] * 4;
            const BYTE* p10 = row0 + (size_t)x1[x] * 4;
            const BYTE* p01 = row1 + (size_t)x0[x] * 4;
            const BYTE* p11 = row1 + (size_t)x1[x] * 4;
            const float wx = fx[x];
            for (int c = 0; c < 3; ++c)
            {
                const float top    = p00[c] + (p10[c] - p00[c]) * wx;
                const float bottom = p01[c] + (p11[c] - p01[c]) * wx;
                const int v = (int)(top + (bottom - top) * fy + 0.5f);
                dst[x * 4 + c] = (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
            dst[x * 4 + 3] = 255;   // OCR 忽略 alpha，固定不透明
        }
    }
    return out;
}

void RunOcr(std::shared_ptr<ImageBGRA> frame)
{
    bool apartment = false;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;

        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Globalization;
        using namespace winrt::Windows::Media::Ocr;

        const uint32_t maxDim = OcrEngine::MaxImageDimension();

        // 超过引擎上限的截图不能直接识别。旧逻辑直接判失败（例如部分系统上
        // 4K 整屏 > 2600px 时整屏 OCR 完全不可用）；改为等比缩放后继续识别。
        std::shared_ptr<ImageBGRA> input;
        if (frame->w > maxDim || frame->h > maxDim)
        {
            ImageBGRA scaled = DownscaleToFit(*frame, maxDim);
            if (!scaled.Valid())
            {
                wchar_t msg[160];
                swprintf_s(msg, L"截图尺寸 %ux%u 超过系统 OCR 上限 %u 像素，且缩放失败",
                           frame->w, frame->h, maxDim);
                Finish(false, L"", msg);
            }
            else
            {
                LOG_INFO(L"文字识别: 截图 %ux%u 超过上限 %u，已缩放至 %ux%u",
                         frame->w, frame->h, maxDim, scaled.w, scaled.h);
                input = std::make_shared<ImageBGRA>(std::move(scaled));
            }
        }
        else
        {
            input = frame;
        }

        if (input)
        {
            SoftwareBitmap bitmap{ nullptr };
            CopyToSoftwareBitmap(*input, bitmap);

            // 优先中文识别引擎；系统没装中文 OCR 时退回用户语言（至少能识别英文）
            OcrEngine engine{ nullptr };
            for (const wchar_t* tag : { L"zh-Hans", L"zh-Hant" })
            {
                engine = OcrEngine::TryCreateFromLanguage(Language(tag));
                if (engine)
                    break;
            }
            if (!engine)
                engine = OcrEngine::TryCreateFromUserProfileLanguages();

            if (!engine)
            {
                const std::wstring langs = AvailableLanguages();
                std::wstring err =
                    L"系统没有可用的文字识别引擎。\n"
                    L"请在 Windows 设置 > 时间和语言 > 语言和区域中安装中文语言包"
                    L"（可选功能里的“光学字符识别”）。\n\n可用识别语言：";
                err += langs;
                Finish(false, L"", err);
            }
            else
            {
                auto result = engine.RecognizeAsync(bitmap).get();
                std::wstring text = result.Text().c_str();
                Finish(true, std::move(text), L"");
            }
        }
    }
    catch (const winrt::hresult_error& e)
    {
        wchar_t msg[320];
        swprintf_s(msg, L"文字识别失败(0x%08lX)：%s", (unsigned long)e.code().value,
                   e.message().c_str());
        Finish(false, L"", msg);
    }
    catch (const std::exception& e)
    {
        wchar_t msg[320];
        swprintf_s(msg, L"文字识别失败：%hs", e.what());
        Finish(false, L"", msg);
    }
    catch (...)
    {
        Finish(false, L"", L"文字识别失败：未知错误");
    }

    if (apartment)
        winrt::uninit_apartment();
}

}   // namespace

bool StartOcr(const ImageBGRA& img)
{
    if (!img.Valid() || g_stopping.load())
        return false;
    if (g_busy.exchange(true))
        return false;

    // 上一轮若已结束但主循环还没取走结果，直接丢弃：新请求优先。
    // 主循环每帧轮询，正常情况下结果窗不足一帧就会被取走
    if (g_thread.joinable())
        g_thread.join();

    auto frame = std::make_shared<ImageBGRA>(img);
    g_done.store(false);
    try
    {
        g_thread = std::thread([frame]() { RunOcr(frame); });
    }
    catch (const std::system_error& e)
    {
        g_busy.store(false);
        LOG_ERR(L"文字识别: 创建线程失败: %hs", e.what());
        return false;
    }
    LOG_INFO(L"文字识别: 开始识别 %ux%u", img.w, img.h);
    return true;
}

bool PollOcr(OcrOutcome& out)
{
    if (!g_done.load())
        return false;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        out = g_result;
        g_result = OcrOutcome{};
    }
    g_done.store(false);
    if (g_thread.joinable())
        g_thread.join();
    LOG_INFO(L"文字识别: 结束 ok=%d 字符数=%zu", out.ok ? 1 : 0, out.text.size());
    return true;
}

bool OcrBusy()
{
    return g_busy.load();
}

void ShutdownOcr()
{
    g_stopping.store(true);
    if (g_thread.joinable())
        g_thread.join();
}
