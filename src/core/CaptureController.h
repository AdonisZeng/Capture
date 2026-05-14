#ifndef CAPTURECONTROLLER_H
#define CAPTURECONTROLLER_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QRect>
#include <windows.h>

class CaptureController : public QObject
{
    Q_OBJECT
public:
    explicit CaptureController(QObject* parent = nullptr);
    ~CaptureController();

    bool captureFullScreen(const QString& savePath);
    bool captureWindow(HWND hwnd, const QString& savePath);
    bool captureRegion(const QRect& region, const QString& savePath);

    QImage captureFullScreenOnly();
    QImage captureRegionOnly(const QRect& region);
    QImage captureWindowOnly(HWND hwnd);

signals:
    void captureCompleted(const QString& filePath);
    void captureFailed(const QString& error);

private:
    QImage captureScreenInternal(HWND hwnd, const QRect& region);

public:
    bool saveImage(const QImage& image, const QString& filePath);

private:
    HWND m_lastCapturedWindow;
};

#endif