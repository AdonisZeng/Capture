/**
 * @file VideoCaptureDevice.cpp
 * @brief 瑙嗛?鎹曡幏璁惧?瀹炵幇 - 浣跨敤Windows Media Foundation + DXGI鎹曡幏灞忓箷/绐楀彛
 * @author
 * @date
 */

#include "VideoCaptureDevice.h"
#include "utils/Logger.h"
#include <d3d11.h>
#include <DXGIDebug.h>
#include <QDebug>
#include <QMutex>
#include <QDateTime>
#include <QThread>
#include <wingdi.h>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

static QMutex g_comMutex;

VideoCaptureDevice::VideoCaptureDevice(QObject* parent)
    : QObject(parent)
    , m_refCount(0)
    , m_captureSize(1920, 1080)
    , m_isCapturing(false)
    , m_targetHwnd(nullptr)
    , m_fullScreen(false)
    , m_d3dDevice(nullptr)
    , m_d3dContext(nullptr)
    , m_duplication(nullptr)
    , m_dxgiFactory(nullptr)
    , m_captureThread(nullptr)
{
}

VideoCaptureDevice::~VideoCaptureDevice()
{
    stopCapture();

    if (m_captureThread) {
        m_captureThread->quit();
        m_captureThread->wait(2000);
        m_captureThread = nullptr;
    }

    if (m_duplication) {
        m_duplication->Release();
        m_duplication = nullptr;
    }

    if (m_d3dContext) {
        m_d3dContext->Release();
        m_d3dContext = nullptr;
    }

    if (m_d3dDevice) {
        m_d3dDevice->Release();
        m_d3dDevice = nullptr;
    }

    if (m_dxgiFactory) {
        m_dxgiFactory->Release();
        m_dxgiFactory = nullptr;
    }

    for (IDXGIAdapter* adapter : m_adapters) {
        adapter->Release();
    }
    m_adapters.clear();
}

STDMETHODIMP VideoCaptureDevice::QueryInterface(REFIID iid, void** ppv)
{
    if (!ppv) {
        return E_POINTER;
    }

    *ppv = nullptr;

    if (IsEqualIID(iid, IID_IUnknown)) {
        *ppv = static_cast<IUnknown*>(this);
    } else if (IsEqualIID(iid, IID_IMFSourceReaderCallback)) {
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) VideoCaptureDevice::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) VideoCaptureDevice::Release()
{
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP VideoCaptureDevice::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample)
{
    Q_UNUSED(dwStreamIndex);
    Q_UNUSED(llTimestamp);

    if (FAILED(hrStatus)) {
        emit captureError(QString("Read sample failed: %1").arg(hrStatus));
        return hrStatus;
    }

    if (dwStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        m_isCapturing = false;
        return S_OK;
    }

    if (dwStreamFlags & MF_SOURCE_READERF_STREAMTICK) {
        return S_OK;
    }

    if (!pSample) {
        return S_OK;
    }

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = pSample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr) || !buffer) {
        emit captureError(QString("Get buffer failed: %1").arg(hr));
        return hr;
    }

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    hr = buffer->Lock(&data, &maxLength, nullptr);
    if (FAILED(hr) || !data) {
        buffer->Release();
        emit captureError(QString("Lock buffer failed: %1").arg(hr));
        return hr;
    }

    QImage image(m_captureSize.width(), m_captureSize.height(), QImage::Format_RGB32);
    memcpy(image.bits(), data, qMin(static_cast<DWORD>(image.sizeInBytes()), maxLength));

    buffer->Unlock();
    buffer->Release();

    qint64 timestamp = llTimestamp / 10000;
    emit frameCaptured(image, timestamp);

    return S_OK;
}

STDMETHODIMP VideoCaptureDevice::OnEvent(DWORD dwStreamIndex, IMFMediaEvent* pEvent)
{
    Q_UNUSED(dwStreamIndex);
    Q_UNUSED(pEvent);
    return S_OK;
}

STDMETHODIMP VideoCaptureDevice::OnFlush(DWORD dwStreamIndex)
{
    Q_UNUSED(dwStreamIndex);
    return S_OK;
}

bool VideoCaptureDevice::initialize()
{
    QMutexLocker locker(&g_comMutex);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        emit captureError(QString("COM initialization failed: %1").arg(hr));
        return false;
    }

    if (!initD3D()) {
        emit captureError("Direct3D initialization failed");
        return false;
    }

    return true;
}

bool VideoCaptureDevice::initD3D()
{
    UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        deviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        nullptr,
        &m_d3dContext
    );

    if (FAILED(hr)) {
        qWarning() << "D3D11CreateDevice failed:" << hr;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            deviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &m_d3dDevice,
            nullptr,
            &m_d3dContext
        );
        if (FAILED(hr)) {
            qWarning() << "D3D11CreateDevice (WARP) failed:" << hr;
            return false;
        }
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        qWarning() << "QueryInterface IDXGIDevice failed:" << hr;
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();

    if (FAILED(hr)) {
        qWarning() << "GetAdapter failed:" << hr;
        return false;
    }

    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    qDebug() << "Using adapter:" << desc.Description;

    hr = adapter->GetParent(IID_PPV_ARGS(&m_dxgiFactory));
    if (FAILED(hr)) {
        qWarning() << "GetParent IDXGIFactory2 failed:" << hr;
        adapter->Release();
        return false;
    }

    m_adapters.append(adapter);

    for (UINT i = 0;; ++i) {
        IDXGIAdapter* additionalAdapter = nullptr;
        hr = m_dxgiFactory->EnumAdapters(i, &additionalAdapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            continue;
        }

        DXGI_ADAPTER_DESC additionalDesc;
        additionalAdapter->GetDesc(&additionalDesc);
        if (additionalDesc.VendorId != desc.VendorId || additionalDesc.DeviceId != desc.DeviceId) {
            qDebug() << "Found additional adapter:" << additionalDesc.Description;
            m_adapters.append(additionalAdapter);
        } else {
            additionalAdapter->Release();
        }
    }

    return true;
}

bool VideoCaptureDevice::createScreenCapture()
{
    if (m_duplication) {
        m_duplication->Release();
        m_duplication = nullptr;
    }

    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;

    if (m_fullScreen && m_targetHwnd) {
        HMONITOR hMonitor = MonitorFromWindow(m_targetHwnd, MONITOR_DEFAULTTOPRIMARY);
        if (!hMonitor) {
            qWarning() << "MonitorFromWindow failed";
            return false;
        }

        for (IDXGIAdapter* adapter : m_adapters) {
            for (UINT i = 0;; ++i) {
                HRESULT hr = adapter->EnumOutputs(i, &output);
                if (hr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(hr)) {
                    continue;
                }

                hr = output->QueryInterface(IID_PPV_ARGS(&output1));
                output->Release();

                if (FAILED(hr)) {
                    continue;
                }

                DXGI_OUTPUT_DESC outputDesc;
                output1->GetDesc(&outputDesc);
                if (outputDesc.Monitor == hMonitor) {
                    HRESULT dupHr = output1->DuplicateOutput(m_d3dDevice, &m_duplication);
                    output1->Release();
                    if (SUCCEEDED(dupHr)) {
                        qDebug() << "Duplication created for monitor:" << outputDesc.DeviceName;
                        return true;
                    }
                    qWarning() << "DuplicateOutput failed:" << dupHr;
                }
                output1->Release();
            }
        }
    } else {
        if (!m_adapters.isEmpty()) {
            IDXGIAdapter* adapter = m_adapters.first();
            HRESULT hr = adapter->EnumOutputs(0, &output);
            if (FAILED(hr)) {
                qWarning() << "EnumOutputs failed:" << hr;
                return false;
            }

            hr = output->QueryInterface(IID_PPV_ARGS(&output1));
            output->Release();

            if (FAILED(hr)) {
                qWarning() << "QueryInterface IDXGIOutput1 failed:" << hr;
                return false;
            }

            hr = output1->DuplicateOutput(m_d3dDevice, &m_duplication);
            output1->Release();

            if (FAILED(hr)) {
                qWarning() << "DuplicateOutput failed:" << hr;
                return false;
            }

            qDebug() << "Screen duplication created";
            return true;
        }
    }

    emit captureError("Failed to create screen capture");
    return false;
}

QImage VideoCaptureDevice::convertToImage(ID3D11Texture2D* texture)
{
    if (!texture) {
        return QImage();
    }

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_d3dContext->Map(texture, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (FAILED(hr)) {
        qWarning() << "Map failed:" << hr;
        return QImage();
    }

    QImage image(desc.Width, desc.Height, QImage::Format_RGB32);

    if (desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) {
        const uchar* srcData = reinterpret_cast<const uchar*>(mappedResource.pData);
        int srcPitch = mappedResource.RowPitch;
        int lineSize = desc.Width * 4;

        for (UINT y = 0; y < desc.Height; ++y) {
            const uchar* srcLine = srcData + y * srcPitch;
            uchar* dstLine = image.scanLine(y);
            memcpy(dstLine, srcLine, lineSize);
        }
    } else if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        for (UINT y = 0; y < desc.Height; ++y) {
            const uchar* srcLine = reinterpret_cast<const uchar*>(mappedResource.pData) + y * mappedResource.RowPitch;
            uchar* dstLine = image.scanLine(y);
            for (UINT x = 0; x < desc.Width; ++x) {
                dstLine[x * 4 + 0] = srcLine[x * 4 + 2];
                dstLine[x * 4 + 1] = srcLine[x * 4 + 1];
                dstLine[x * 4 + 2] = srcLine[x * 4 + 0];
                dstLine[x * 4 + 3] = 255;
            }
        }
    } else {
        qWarning() << "Unsupported texture format:" << desc.Format;
        m_d3dContext->Unmap(texture, 0);
        return QImage();
    }

    m_d3dContext->Unmap(texture, 0);

    if (!m_captureRegion.isNull() && !m_captureRegion.isEmpty()) {
        image = image.copy(m_captureRegion);
    }

    return image;
}

void VideoCaptureDevice::captureLoop()
{
    Logger::instance()->info("VideoCaptureDevice", "Capture loop started");
    qDebug() << "Video capture loop started";
    qint64 frameInterval = 1000 / 60;

    while (m_isCapturing) {
        if (!m_duplication) {
            break;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        IDXGIResource* resource = nullptr;
        HRESULT hr = m_duplication->AcquireNextFrame(500, &frameInfo, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }

        if (FAILED(hr)) {
            qWarning() << "AcquireNextFrame failed:" << hr;
            QThread::msleep(frameInterval);
            continue;
        }

        if (frameInfo.LastPresentTime.QuadPart == 0) {
            m_duplication->ReleaseFrame();
            continue;
        }

        ID3D11Texture2D* texture = nullptr;
        hr = resource->QueryInterface(IID_PPV_ARGS(&texture));
        resource->Release();

        if (FAILED(hr) || !texture) {
            m_duplication->ReleaseFrame();
            continue;
        }

        QImage image = convertToImage(texture);
        texture->Release();

        if (!image.isNull()) {
            qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
            emit frameCaptured(image, timestamp);
        }

        m_duplication->ReleaseFrame();
        QThread::msleep(frameInterval);
    }

    qDebug() << "Video capture loop ended";
    Logger::instance()->info("VideoCaptureDevice", "Capture loop ended");
}

bool VideoCaptureDevice::startCapture(HWND hwnd, bool fullScreen)
{
    Logger::instance()->info("VideoCaptureDevice", QString("Starting capture: fullScreen=%1").arg(fullScreen));
    if (m_isCapturing) {
        stopCapture();
    }

    m_targetHwnd = hwnd;
    m_fullScreen = fullScreen;
    m_captureRegion = QRect();

    if (!createScreenCapture()) {
        emit captureError("Failed to create screen capture");
        return false;
    }

    if (m_targetHwnd) {
        RECT rect;
        if (fullScreen) {
            GetWindowRect(m_targetHwnd, &rect);
        } else {
            GetClientRect(m_targetHwnd, &rect);
        }
        m_captureSize = QSize(rect.right - rect.left, rect.bottom - rect.top);
    } else {
        if (!m_adapters.isEmpty()) {
            IDXGIAdapter* adapter = m_adapters.first();
            IDXGIOutput* output = nullptr;
            HRESULT hr = adapter->EnumOutputs(0, &output);
            if (SUCCEEDED(hr)) {
                DXGI_OUTPUT_DESC desc;
                output->GetDesc(&desc);
                m_captureSize = QSize(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                                      desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
                output->Release();
            }
        }
    }

    if (m_captureSize.width() <= 0 || m_captureSize.height() <= 0) {
        m_captureSize = QSize(1920, 1080);
    }

    m_captureThread = new QThread(this);
    connect(m_captureThread, &QThread::started, this, &VideoCaptureDevice::captureLoop);
    connect(m_captureThread, &QThread::finished, m_captureThread, &QThread::deleteLater);

    m_isCapturing = true;
    m_captureThread->start();
    qDebug() << "Capture started:" << m_captureSize << "fullScreen:" << fullScreen;
    return true;
}

bool VideoCaptureDevice::startCaptureRegion(const QRect& region)
{
    if (m_isCapturing) {
        stopCapture();
    }

    m_targetHwnd = nullptr;
    m_fullScreen = false;
    m_captureRegion = region;

    if (!createScreenCapture()) {
        emit captureError("Failed to create screen capture");
        return false;
    }

    if (!m_adapters.isEmpty()) {
        IDXGIAdapter* adapter = m_adapters.first();
        IDXGIOutput* output = nullptr;
        HRESULT hr = adapter->EnumOutputs(0, &output);
        if (SUCCEEDED(hr)) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            m_captureSize = QSize(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                                  desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
            output->Release();
        }
    }

    m_isCapturing = true;
    qDebug() << "Region capture started:" << region;
    return true;
}

void VideoCaptureDevice::stopCapture()
{
    Logger::instance()->info("VideoCaptureDevice", "Stopping capture");
    m_isCapturing = false;

    if (m_captureThread && m_captureThread->isRunning()) {
        m_captureThread->quit();
        m_captureThread->wait(2000);
        m_captureThread = nullptr;
    }

    if (m_duplication) {
        m_duplication->Release();
        m_duplication = nullptr;
    }

    qDebug() << "Capture stopped";
}

QSize VideoCaptureDevice::captureSize() const
{
    return m_captureSize;
}

bool VideoCaptureDevice::isCapturing() const
{
    return m_isCapturing;
}