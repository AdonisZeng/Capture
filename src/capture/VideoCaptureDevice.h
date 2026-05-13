#ifndef VIDEOCAPTUREDEVICE_H
#define VIDEOCAPTUREDEVICE_H

#include <QObject>
#include <QSize>
#include <QImage>
#include <QThread>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <dxgi1_2.h>
#include <d3d11.h>

class VideoCaptureDevice : public QObject, public IMFSourceReaderCallback
{
    Q_OBJECT
public:
    explicit VideoCaptureDevice(QObject* parent = nullptr);
    ~VideoCaptureDevice();

    bool initialize();
    bool startCapture(HWND hwnd, bool fullScreen);
    bool startCaptureRegion(const QRect& region);
    void stopCapture();

    QSize captureSize() const;
    bool isCapturing() const;

signals:
    void frameCaptured(const QImage& frame, qint64 timestamp);
    void captureError(const QString& error);

public:
    // IMFSourceReaderCallback
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();
    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample);
    STDMETHODIMP OnEvent(DWORD dwStreamIndex, IMFMediaEvent* pEvent);
    STDMETHODIMP OnFlush(DWORD dwStreamIndex);

protected:
    bool initD3D();
    bool createScreenCapture();
    QImage convertToImage(ID3D11Texture2D* texture);

private:
    long m_refCount;
    QSize m_captureSize;
    bool m_isCapturing;
    HWND m_targetHwnd;
    bool m_fullScreen;
    QRect m_captureRegion;
    QThread* m_captureThread;

    ID3D11Device* m_d3dDevice;
    ID3D11DeviceContext* m_d3dContext;
    IDXGIOutputDuplication* m_duplication;
    IDXGIFactory2* m_dxgiFactory;
    QList<IDXGIAdapter*> m_adapters;

private slots:
    void captureLoop();
};

#endif