#include "WindowPicker.h"
#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <wingdi.h>

#ifndef PW_DEFAULT
#define PW_DEFAULT 0
#endif

WindowPicker::WindowPicker(QObject* parent)
    : QObject(parent)
{
}

WindowPicker::~WindowPicker()
{
    clearHighlight();
}

static bool isWindowRecordable(HWND hwnd)
{
    if (!IsWindowVisible(hwnd)) {
        return false;
    }

    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != nullptr) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_TOOLWINDOW) {
        return false;
    }

    if (exStyle & WS_EX_NOACTIVATE) {
        return false;
    }

    if ((style & WS_CHILD) || (exStyle & WS_EX_LAYERED)) {
        return false;
    }

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        return false;
    }

    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }

    if (rect.right - rect.left < 10 || rect.bottom - rect.top < 10) {
        return false;
    }

    wchar_t className[256];
    GetClassName(hwnd, className, 256);

    if (wcscmp(className, L"Shell_TrayWnd") == 0 ||
        wcscmp(className, L"Progman") == 0 ||
        wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"DV2ControlHost") == 0 ||
        wcscmp(className, L"MsgrIMEWindowClass") == 0 ||
        wcscmp(className, L"SysShadow") == 0 ||
        wcscmp(className, L"Button") == 0 ||
        wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
        return false;
    }

    return true;
}

QList<WindowInfo> WindowPicker::enumWindows()
{
    m_windows.clear();
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(this));
    return m_windows;
}

BOOL CALLBACK WindowPicker::EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    WindowPicker* picker = reinterpret_cast<WindowPicker*>(lParam);

    if (isWindowRecordable(hwnd)) {
        WindowInfo info;
        info.hwnd = hwnd;
        info.title = picker->getWindowTitle(hwnd);

        wchar_t className[256];
        GetClassName(hwnd, className, 256);
        info.className = QString::fromWCharArray(className);

        RECT rect;
        GetWindowRect(hwnd, &rect);
        info.geometry = QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);

        picker->m_windows.append(info);
    }

    return TRUE;
}

void WindowPicker::highlightWindow(HWND hwnd)
{
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return;
    }

    if (m_highlightedWindows.contains(hwnd)) {
        return;
    }

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        return;
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, rect.right - rect.left, rect.bottom - rect.top);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdc, hBitmap);

    BitBlt(hdc, 0, 0, rect.right - rect.left, rect.bottom - rect.top, hdcScreen, rect.left, rect.top, SRCCOPY);

    HPEN hPen = CreatePen(PS_SOLID, 4, RGB(0, 120, 215));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hBrush = CreateSolidBrush(RGB(0, 120, 215));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);

    Rectangle(hdc, 0, 0, rect.right - rect.left, rect.bottom - rect.top);

    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hPen);
    DeleteObject(hBrush);

    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);

    SelectObject(hdc, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hdcScreen);

    m_highlightedWindows.append(hwnd);
}

void WindowPicker::clearHighlight()
{
    for (HWND hwnd : m_highlightedWindows) {
        if (IsWindow(hwnd)) {
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
        }
    }
    m_highlightedWindows.clear();
}

QString WindowPicker::getWindowTitle(HWND hwnd) const
{
    if (!IsWindow(hwnd)) {
        return QString();
    }

    int length = GetWindowTextLength(hwnd);
    if (length == 0) {
        return QString();
    }

    wchar_t* buffer = new wchar_t[length + 1];
    if (GetWindowText(hwnd, buffer, length + 1) == 0) {
        delete[] buffer;
        return QString();
    }

    QString title = QString::fromWCharArray(buffer);
    delete[] buffer;
    return title;
}

QPixmap WindowPicker::getWindowThumbnail(HWND hwnd, int maxWidth, int maxHeight)
{
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return QPixmap();
    }

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        return QPixmap();
    }

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    double scaleX = static_cast<double>(maxWidth) / width;
    double scaleY = static_cast<double>(maxHeight) / height;
    double scale = qMin(scaleX, scaleY);

    int scaledWidth = static_cast<int>(width * scale);
    int scaledHeight = static_cast<int>(height * scale);

    HDC hdcScreen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdc, hBitmap);

    BOOL result = PrintWindow(hwnd, hdc, PW_DEFAULT);

    if (!result) {
        BitBlt(hdc, 0, 0, width, height, hdcScreen, rect.left, rect.top, SRCCOPY);
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    QImage image(width, height, QImage::Format_ARGB32);

    if (GetDIBits(hdc, hBitmap, 0, height, image.bits(), &bmi, DIB_RGB_COLORS)) {
        image = image.flipped(Qt::Vertical);
    } else {
        image = QImage();
    }

    SelectObject(hdc, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hdcScreen);

    if (image.isNull()) {
        return QPixmap();
    }

    QPixmap thumbnail = QPixmap::fromImage(image);
    if (scaledWidth != width || scaledHeight != height) {
        thumbnail = thumbnail.scaled(scaledWidth, scaledHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    return thumbnail;
}
