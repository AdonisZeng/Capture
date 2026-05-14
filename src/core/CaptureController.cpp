#include "CaptureController.h"

#include <QDateTime>
#include <QDir>
#include <QSaveFile>
#include <WinDef.h>
#include <wingdi.h>
#include <WinUser.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

CaptureController::CaptureController(QObject* parent)
    : QObject(parent)
    , m_lastCapturedWindow(nullptr)
{
}

CaptureController::~CaptureController()
{
}

bool CaptureController::captureFullScreen(const QString& savePath)
{
    HWND desktopHwnd = GetDesktopWindow();
    RECT desktopRect;
    GetWindowRect(desktopHwnd, &desktopRect);

    QRect region(0, 0, desktopRect.right - desktopRect.left, desktopRect.bottom - desktopRect.top);
    QImage image = captureScreenInternal(desktopHwnd, region);

    if (image.isNull()) {
        emit captureFailed("Failed to capture full screen");
        return false;
    }

    QString filePath = savePath;
    if (filePath.isEmpty()) {
        QDateTime now = QDateTime::currentDateTime();
        filePath = QString("Screenshot_%1.png").arg(now.toString("yyyyMMdd_HHmmss"));
    }

    if (!saveImage(image, filePath)) {
        emit captureFailed("Failed to save screenshot");
        return false;
    }

    emit captureCompleted(filePath);
    return true;
}

bool CaptureController::captureWindow(HWND hwnd, const QString& savePath)
{
    if (!hwnd || !IsWindow(hwnd)) {
        emit captureFailed("Invalid window handle");
        return false;
    }

    m_lastCapturedWindow = hwnd;

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);

    QImage image = captureScreenInternal(hwnd, QRect(0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top));

    if (image.isNull()) {
        emit captureFailed("Failed to capture window");
        return false;
    }

    QString filePath = savePath;
    if (filePath.isEmpty()) {
        QDateTime now = QDateTime::currentDateTime();
        filePath = QString("Screenshot_%1.png").arg(now.toString("yyyyMMdd_HHmmss"));
    }

    if (!saveImage(image, filePath)) {
        emit captureFailed("Failed to save screenshot");
        return false;
    }

    emit captureCompleted(filePath);
    return true;
}

bool CaptureController::captureRegion(const QRect& region, const QString& savePath)
{
    HWND desktopHwnd = GetDesktopWindow();

    QImage image = captureScreenInternal(desktopHwnd, region);

    if (image.isNull()) {
        emit captureFailed("Failed to capture region");
        return false;
    }

    QString filePath = savePath;
    if (filePath.isEmpty()) {
        QDateTime now = QDateTime::currentDateTime();
        filePath = QString("Screenshot_%1.png").arg(now.toString("yyyyMMdd_HHmmss"));
    }

    if (!saveImage(image, filePath)) {
        emit captureFailed("Failed to save screenshot");
        return false;
    }

    emit captureCompleted(filePath);
    return true;
}

QImage CaptureController::captureScreenInternal(HWND hwnd, const QRect& region)
{
    Q_UNUSED(hwnd);
    int width = region.width();
    int height = region.height();

    if (width <= 0 || height <= 0) {
        return QImage();
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    BitBlt(hdcMem, 0, 0, width, height, hdcScreen, region.x(), region.y(), SRCCOPY | CAPTUREBLT);

    SelectObject(hdcMem, hOldBitmap);

    QImage image(width, height, QImage::Format_ARGB32);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    GetDIBits(hdcScreen, hBitmap, 0, height, image.bits(), &bmi, DIB_RGB_COLORS);

    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    return image;
}

bool CaptureController::saveImage(const QImage& image, const QString& filePath)
{
    if (image.isNull()) {
        return false;
    }

    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            return false;
        }
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    if (!image.save(&file, "PNG", 100)) {
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        return false;
    }

    return true;
}

QImage CaptureController::captureFullScreenOnly()
{
    HWND desktopHwnd = GetDesktopWindow();
    RECT desktopRect;
    GetWindowRect(desktopHwnd, &desktopRect);

    QRect region(0, 0, desktopRect.right - desktopRect.left, desktopRect.bottom - desktopRect.top);
    return captureScreenInternal(desktopHwnd, region);
}

QImage CaptureController::captureRegionOnly(const QRect& region)
{
    HWND desktopHwnd = GetDesktopWindow();
    return captureScreenInternal(desktopHwnd, region);
}

QImage CaptureController::captureWindowOnly(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) {
        return QImage();
    }

    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    return captureScreenInternal(hwnd, QRect(0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top));
}