#include "appicon.h"
#include "resource.h"
#include "core/log.h"

#include <cmath>
#include <cstring>

namespace {

// 32bpp BGRA 位图头：高度取负 = 自上而下，DIB 行序与 pixels 缓冲一致
BITMAPINFO MakeHeader(UINT cx, UINT cy)
{
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)cx;
    bi.bmiHeader.biHeight = -(LONG)cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return bi;
}

// 取 HICON 的颜色位图数据（32bpp BGRA，自上而下）
bool IconToBGRA(HICON icon, UINT cx, UINT cy, std::vector<BYTE>& out)
{
    out.clear();
    if (!icon || cx == 0 || cy == 0)
        return false;

    ICONINFO ii = {};
    if (!GetIconInfo(icon, &ii))
        return false;

    BITMAPINFO bi = MakeHeader(cx, cy);
    out.assign((size_t)cx * cy * 4, 0);

    bool ok = false;
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc)
    {
        ok = GetDIBits(dc, ii.hbmColor, 0, cy, out.data(), &bi, DIB_RGB_COLORS) != 0;
        DeleteDC(dc);
    }
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    if (!ok)
    {
        out.clear();
        return false;
    }

    // 少数老式图标把透明度只写在掩码位图里（颜色位图 alpha 全 0），此时按全不透明处理
    bool anyAlpha = false;
    for (size_t i = 3; i < out.size(); i += 4)
    {
        if (out[i] != 0)
        {
            anyAlpha = true;
            break;
        }
    }
    if (!anyAlpha)
        for (size_t i = 3; i < out.size(); i += 4)
            out[i] = 255;
    return true;
}

// 右下角叠加录制徽标：白色描边圆 + 红色实心圆（BGRA 顺序：B,G,R,A）
void DrawRecordingBadge(std::vector<BYTE>& px, UINT n)
{
    const float fsize = (float)n;
    const float bcx = fsize * 0.74f, bcy = fsize * 0.74f;   // 徽标中心
    const float rOut = fsize * 0.30f;                        // 白描边外径
    const float rIn = fsize * 0.23f;                         // 红色区域半径

    for (UINT y = 0; y < n; ++y)
    {
        for (UINT x = 0; x < n; ++x)
        {
            float dx = (float)x + 0.5f - bcx, dy = (float)y + 0.5f - bcy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > rOut)
                continue;
            BYTE* p = &px[((size_t)y * n + x) * 4];
            if (d > rIn)
            {
                p[0] = 255; p[1] = 255; p[2] = 255;   // 白
            }
            else
            {
                p[0] = 48; p[1] = 48; p[2] = 226;     // 红
            }
            p[3] = 255;
        }
    }
}

}   // namespace

namespace AppIcon
{

HICON Load(UINT cx, UINT cy)
{
    if (cx == 0) cx = 16;
    if (cy == 0) cy = 16;

    HINSTANCE mod = GetModuleHandleW(nullptr);
    HICON icon = (HICON)LoadImageW(mod, MAKEINTRESOURCEW(IDI_CAPTURE), IMAGE_ICON,
                                   (int)cx, (int)cy, LR_DEFAULTCOLOR);
    if (!icon)
        LOG_WARN(L"加载应用图标资源失败(size=%ux%u, 错误码=%lu)", cx, cy, GetLastError());
    return icon;
}

bool LoadBGRA(UINT size, std::vector<BYTE>& out)
{
    if (size == 0)
    {
        out.clear();
        return false;
    }

    HICON icon = Load(size, size);
    if (!icon)
    {
        out.clear();
        return false;
    }
    bool ok = IconToBGRA(icon, size, size, out);
    DestroyIcon(icon);
    return ok;
}

HICON FromBGRA(const BYTE* bgra, UINT cx, UINT cy)
{
    if (!bgra || cx == 0 || cy == 0)
        return nullptr;

    BITMAPINFO bi = MakeHeader(cx, cy);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return nullptr;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HICON icon = nullptr;
    if (color && bits)
    {
        memcpy(bits, bgra, (size_t)cx * cy * 4);
        HBITMAP mask = CreateBitmap((int)cx, (int)cy, 1, 1, nullptr);   // 空掩码：只用 alpha
        ICONINFO ii = {};
        ii.fIcon = TRUE;
        ii.hbmColor = color;
        ii.hbmMask = mask;
        icon = CreateIconIndirect(&ii);
        if (mask)
            DeleteObject(mask);
    }
    if (color)
        DeleteObject(color);
    DeleteDC(dc);
    return icon;
}

HICON LoadRecording(UINT cx, UINT cy)
{
    // 徽标按正方形绘制，取长宽较小值作为工作尺寸
    const UINT n = cx < cy ? cx : cy;
    if (n == 0)
        return nullptr;

    std::vector<BYTE> px;
    if (!LoadBGRA(n, px))
        return Load(cx, cy);   // 取不到像素时退回原图标，至少保证有图标

    DrawRecordingBadge(px, n);
    return FromBGRA(px.data(), n, n);
}

}   // namespace AppIcon
