#include "AudioCaptureDevice.h"
#include "utils/Logger.h"
#include <QDebug>
#include <QDateTime>
#include <QThread>
#include <combaseapi.h>
#include <functiondiscoverykeys.h>

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

AudioCaptureDevice::AudioCaptureDevice(QObject* parent)
    : QObject(parent)
    , m_deviceEnumerator(nullptr)
    , m_systemAudioDevice(nullptr)
    , m_microphoneDevice(nullptr)
    , m_systemAudioClient(nullptr)
    , m_microphoneClient(nullptr)
    , m_systemCaptureClient(nullptr)
    , m_micCaptureClient(nullptr)
    , m_isCapturing(false)
    , m_systemAudioEnabled(false)
    , m_microphoneEnabled(false)
    , m_sampleRate(48000)
    , m_channels(2)
    , m_bitsPerSample(16)
    , m_captureThread(nullptr)
{
}

AudioCaptureDevice::~AudioCaptureDevice()
{
    stopCapture();
    if (m_deviceEnumerator) {
        m_deviceEnumerator->Release();
        m_deviceEnumerator = nullptr;
    }
}

bool AudioCaptureDevice::initialize()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        qWarning() << "Failed to initialize COM:" << hr;
        return false;
    }

    return initDeviceEnumerator();
}

bool AudioCaptureDevice::initDeviceEnumerator()
{
    HRESULT hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void**)&m_deviceEnumerator
    );

    if (FAILED(hr)) {
        qWarning() << "Failed to create device enumerator:" << hr;
        return false;
    }

    return true;
}

QString AudioCaptureDevice::getDefaultDeviceId(EDataFlow dataFlow)
{
    QString defaultId;
    IMMDevice* defaultDevice = nullptr;

    HRESULT hr = m_deviceEnumerator->GetDefaultAudioEndpoint(
        dataFlow,
        eConsole,
        &defaultDevice
    );

    if (SUCCEEDED(hr) && defaultDevice) {
        LPWSTR pwszId = nullptr;
        hr = defaultDevice->GetId(&pwszId);
        if (SUCCEEDED(hr) && pwszId) {
            defaultId = QString::fromWCharArray(pwszId);
            CoTaskMemFree(pwszId);
        }
        defaultDevice->Release();
    }

    return defaultId;
}

QList<AudioDeviceInfo> AudioCaptureDevice::enumAudioDevices()
{
    QList<AudioDeviceInfo> deviceList;

    if (!m_deviceEnumerator) {
        if (!initDeviceEnumerator()) {
            return deviceList;
        }
    }

    QString defaultRenderId = getDefaultDeviceId(eRender);
    QString defaultCaptureId = getDefaultDeviceId(eCapture);

    {
        IMMDeviceCollection* pCollection = nullptr;
        HRESULT hr = m_deviceEnumerator->EnumAudioEndpoints(
            eRender,
            DEVICE_STATE_ACTIVE,
            &pCollection
        );

        if (SUCCEEDED(hr) && pCollection) {
            UINT count = 0;
            pCollection->GetCount(&count);

            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDevice = nullptr;
                hr = pCollection->Item(i, &pDevice);

                if (SUCCEEDED(hr) && pDevice) {
                    LPWSTR pwszId = nullptr;
                    hr = pDevice->GetId(&pwszId);

                    if (SUCCEEDED(hr) && pwszId) {
                        AudioDeviceInfo info;
                        info.id = QString::fromWCharArray(pwszId);
                        info.isDefault = (info.id == defaultRenderId);
                        info.isSystemAudio = true;
                        info.isMicrophone = false;

                        IPropertyStore* pProp = nullptr;
                        hr = pDevice->OpenPropertyStore(STGM_READ, &pProp);

                        if (SUCCEEDED(hr) && pProp) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);

                            hr = pProp->GetValue(PKEY_Device_FriendlyName, &varName);
                            if (SUCCEEDED(hr) && varName.pwszVal) {
                                info.name = QString::fromWCharArray(varName.pwszVal);
                            }

                            PropVariantClear(&varName);
                            pProp->Release();
                        }

                        if (info.name.isEmpty()) {
                            info.name = QString::fromWCharArray(pwszId);
                        }

                        deviceList.append(info);
                        CoTaskMemFree(pwszId);
                    }
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
    }

    {
        IMMDeviceCollection* pCollection = nullptr;
        HRESULT hr = m_deviceEnumerator->EnumAudioEndpoints(
            eCapture,
            DEVICE_STATE_ACTIVE,
            &pCollection
        );

        if (SUCCEEDED(hr) && pCollection) {
            UINT count = 0;
            pCollection->GetCount(&count);

            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDevice = nullptr;
                hr = pCollection->Item(i, &pDevice);

                if (SUCCEEDED(hr) && pDevice) {
                    LPWSTR pwszId = nullptr;
                    hr = pDevice->GetId(&pwszId);

                    if (SUCCEEDED(hr) && pwszId) {
                        AudioDeviceInfo info;
                        info.id = QString::fromWCharArray(pwszId);
                        info.isDefault = (info.id == defaultCaptureId);
                        info.isSystemAudio = false;
                        info.isMicrophone = true;

                        IPropertyStore* pProp = nullptr;
                        hr = pDevice->OpenPropertyStore(STGM_READ, &pProp);

                        if (SUCCEEDED(hr) && pProp) {
                            PROPVARIANT varName;
                            PropVariantInit(&varName);

                            hr = pProp->GetValue(PKEY_Device_FriendlyName, &varName);
                            if (SUCCEEDED(hr) && varName.pwszVal) {
                                info.name = QString::fromWCharArray(varName.pwszVal);
                            }

                            PropVariantClear(&varName);
                            pProp->Release();
                        }

                        if (info.name.isEmpty()) {
                            info.name = QString::fromWCharArray(pwszId);
                        }

                        deviceList.append(info);
                        CoTaskMemFree(pwszId);
                    }
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
    }

    return deviceList;
}

bool AudioCaptureDevice::createSystemAudioClient()
{
    if (!m_deviceEnumerator) {
        return false;
    }

    HRESULT hr = m_deviceEnumerator->GetDefaultAudioEndpoint(
        eRender,
        eConsole,
        &m_systemAudioDevice
    );

    if (FAILED(hr)) {
        qWarning() << "Failed to get default render device:" << hr;
        return false;
    }

    hr = m_systemAudioDevice->Activate(
        IID_IAudioClient,
        CLSCTX_ALL,
        nullptr,
        (void**)&m_systemAudioClient
    );

    if (FAILED(hr)) {
        qWarning() << "Failed to activate system audio client:" << hr;
        return false;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = m_systemAudioClient->GetMixFormat(&pwfx);

    if (FAILED(hr) || !pwfx) {
        qWarning() << "Failed to get mix format:" << hr;
        return false;
    }

    m_sampleRate = pwfx->nSamplesPerSec;
    m_channels = pwfx->nChannels;
    m_bitsPerSample = pwfx->wBitsPerSample;

    REFERENCE_TIME hnsRequestedDuration = 10000000;
    hr = m_systemAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsRequestedDuration,
        0,
        pwfx,
        nullptr
    );

    CoTaskMemFree(pwfx);

    if (FAILED(hr)) {
        qWarning() << "Failed to initialize system audio client:" << hr;
        return false;
    }

    hr = m_systemAudioClient->GetService(
        IID_IAudioCaptureClient,
        (void**)&m_systemCaptureClient
    );

    if (FAILED(hr)) {
        qWarning() << "Failed to get system audio capture client:" << hr;
        return false;
    }

    return true;
}

bool AudioCaptureDevice::createMicrophoneClient()
{
    if (!m_deviceEnumerator) {
        return false;
    }

    HRESULT hr = m_deviceEnumerator->GetDefaultAudioEndpoint(
        eCapture,
        eConsole,
        &m_microphoneDevice
    );

    if (FAILED(hr)) {
        qWarning() << "Failed to get default capture device:" << hr;
        return false;
    }

    hr = m_microphoneDevice->Activate(
        IID_IAudioClient,
        CLSCTX_ALL,
        nullptr,
        (void**)&m_microphoneClient
    );

    if (FAILED(hr)) {
        qWarning() << "Failed to activate microphone client:" << hr;
        return false;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = m_microphoneClient->GetMixFormat(&pwfx);

    if (FAILED(hr) || !pwfx) {
        qWarning() << "Failed to get capture mix format:" << hr;
        return false;
    }

    REFERENCE_TIME hnsRequestedDuration = 10000000;
    hr = m_microphoneClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsRequestedDuration,
        0,
        pwfx,
        nullptr
    );

    CoTaskMemFree(pwfx);

    if (FAILED(hr)) {
        qWarning() << "Failed to initialize microphone client:" << hr;
        return false;
    }

    hr = m_microphoneClient->GetService(
        IID_IAudioCaptureClient,
        (void**)&m_micCaptureClient
    );

    if (FAILED(hr)) {
        qWarning() << "Failed to get microphone capture client:" << hr;
        return false;
    }

    return true;
}

bool AudioCaptureDevice::startCapture(bool systemAudio, bool microphone)
{
    if (m_isCapturing) {
        stopCapture();
    }

    Logger::instance()->info("AudioCaptureDevice", QString("Starting capture: systemAudio=%1, microphone=%2")
                                  .arg(systemAudio).arg(microphone));

    m_systemAudioEnabled = systemAudio;
    m_microphoneEnabled = microphone;

    if (m_systemAudioEnabled) {
        if (!createSystemAudioClient()) {
            emit captureError("Failed to create system audio client");
            return false;
        }

        HRESULT hr = m_systemAudioClient->Start();
        if (FAILED(hr)) {
            emit captureError("Failed to start system audio capture");
            return false;
        }
    }

    if (m_microphoneEnabled) {
        if (!createMicrophoneClient()) {
            emit captureError("Failed to create microphone client");
            if (m_systemAudioEnabled && m_systemAudioClient) {
                m_systemAudioClient->Stop();
            }
            return false;
        }

        HRESULT hr = m_microphoneClient->Start();
        if (FAILED(hr)) {
            emit captureError("Failed to start microphone capture");
            return false;
        }
    }

    if (!m_systemAudioEnabled && !m_microphoneEnabled) {
        emit captureError("No audio source selected");
        return false;
    }

    int bytesPerSample = m_bitsPerSample / 8;
    int frameSize = m_channels * bytesPerSample;
    m_mixedBuffer.reserve(48000 * frameSize);

    m_isCapturing = true;

    m_captureThread = new QThread(this);
    connect(m_captureThread, &QThread::started, this, &AudioCaptureDevice::captureLoop);
    connect(m_captureThread, &QThread::finished, m_captureThread, &QThread::deleteLater);
    m_captureThread->start();

    return true;
}

void AudioCaptureDevice::stopCapture()
{
    Logger::instance()->info("AudioCaptureDevice", "Stopping capture");
    m_isCapturing = false;

    if (m_captureThread && m_captureThread->isRunning()) {
        m_captureThread->quit();
        m_captureThread->wait(2000);
        m_captureThread = nullptr;
    }

    if (m_systemAudioClient) {
        m_systemAudioClient->Stop();
        m_systemAudioClient->Reset();
    }

    if (m_microphoneClient) {
        m_microphoneClient->Stop();
        m_microphoneClient->Reset();
    }

    if (m_systemCaptureClient) {
        m_systemCaptureClient->Release();
        m_systemCaptureClient = nullptr;
    }

    if (m_micCaptureClient) {
        m_micCaptureClient->Release();
        m_micCaptureClient = nullptr;
    }

    if (m_systemAudioClient) {
        m_systemAudioClient->Release();
        m_systemAudioClient = nullptr;
    }

    if (m_microphoneClient) {
        m_microphoneClient->Release();
        m_microphoneClient = nullptr;
    }

    if (m_systemAudioDevice) {
        m_systemAudioDevice->Release();
        m_systemAudioDevice = nullptr;
    }

    if (m_microphoneDevice) {
        m_microphoneDevice->Release();
        m_microphoneDevice = nullptr;
    }

    QMutexLocker locker(&m_bufferMutex);
    m_mixedBuffer.clear();
}

void AudioCaptureDevice::mixAudioSamples(BYTE* systemBuffer, BYTE* micBuffer, int frameCount)
{
    if (!systemBuffer && !micBuffer) {
        return;
    }

    int bytesPerSample = m_bitsPerSample / 8;
    int frameSize = m_channels * bytesPerSample;
    int totalBytes = frameCount * frameSize;

    QMutexLocker locker(&m_bufferMutex);

    int currentSize = m_mixedBuffer.size();
    int neededSize = currentSize + totalBytes;

    if (m_mixedBuffer.capacity() < neededSize) {
        m_mixedBuffer.reserve(neededSize);
    }

    if (m_systemAudioEnabled && m_microphoneEnabled && systemBuffer && micBuffer) {
        BYTE* mixed = new BYTE[totalBytes];

        if (m_bitsPerSample == 16) {
            short* systemSamples = reinterpret_cast<short*>(systemBuffer);
            short* micSamples = reinterpret_cast<short*>(micBuffer);
            short* mixedSamples = reinterpret_cast<short*>(mixed);
            int sampleCount = totalBytes / sizeof(short);

            for (int i = 0; i < sampleCount; ++i) {
                int mixedValue = static_cast<int>(systemSamples[i]) + static_cast<int>(micSamples[i]);
                mixedValue = qBound(-32768, mixedValue, 32767);
                mixedSamples[i] = static_cast<short>(mixedValue);
            }
        } else if (m_bitsPerSample == 32) {
            float* systemSamples = reinterpret_cast<float*>(systemBuffer);
            float* micSamples = reinterpret_cast<float*>(micBuffer);
            float* mixedSamples = reinterpret_cast<float*>(mixed);
            int sampleCount = totalBytes / sizeof(float);

            for (int i = 0; i < sampleCount; ++i) {
                float mixedValue = systemSamples[i] + micSamples[i];
                mixedValue = qBound(-1.0f, mixedValue, 1.0f);
                mixedSamples[i] = mixedValue;
            }
        }

        m_mixedBuffer.append(reinterpret_cast<const char*>(mixed), totalBytes);
        delete[] mixed;
    } else if (systemBuffer) {
        m_mixedBuffer.append(reinterpret_cast<const char*>(systemBuffer), totalBytes);
    } else if (micBuffer) {
        m_mixedBuffer.append(reinterpret_cast<const char*>(micBuffer), totalBytes);
    }
}

QByteArray AudioCaptureDevice::getMixedSamples() const
{
    QMutexLocker locker(&m_bufferMutex);
    return m_mixedBuffer;
}

void AudioCaptureDevice::captureLoop()
{
    Logger::instance()->info("AudioCaptureDevice", "Capture loop started");
    qDebug() << "Audio capture loop started";
    const int bufferDurationMs = 100;
    int bytesPerSample = m_bitsPerSample / 8;
    int frameSize = m_channels * bytesPerSample;
    int framesPerBuffer = (m_sampleRate * bufferDurationMs) / 1000;

    while (m_isCapturing) {
        if (m_systemAudioEnabled && m_systemCaptureClient) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            HRESULT hr = m_systemCaptureClient->GetBuffer(
                &pData, &numFrames, &flags, nullptr, nullptr);

            if (SUCCEEDED(hr)) {
                QMutexLocker locker(&m_bufferMutex);
                m_mixedBuffer.append(reinterpret_cast<const char*>(pData),
                                     numFrames * frameSize);
                m_systemCaptureClient->ReleaseBuffer(numFrames);
            }
        }

        if (m_microphoneEnabled && m_micCaptureClient) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            HRESULT hr = m_micCaptureClient->GetBuffer(
                &pData, &numFrames, &flags, nullptr, nullptr);

            if (SUCCEEDED(hr)) {
                QMutexLocker locker(&m_bufferMutex);
                m_mixedBuffer.append(reinterpret_cast<const char*>(pData),
                                     numFrames * frameSize);
                m_micCaptureClient->ReleaseBuffer(numFrames);
            }
        }

        if (m_mixedBuffer.size() >= framesPerBuffer * frameSize * 10) {
            QByteArray dataToEmit;
            {
                QMutexLocker locker(&m_bufferMutex);
                dataToEmit = m_mixedBuffer;
                m_mixedBuffer.clear();
            }
            if (!dataToEmit.isEmpty()) {
                qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
                emit audioCaptured(dataToEmit, timestamp);
            }
        }

        QThread::msleep(bufferDurationMs);
    }

    if (!m_mixedBuffer.isEmpty()) {
        qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
        emit audioCaptured(m_mixedBuffer, timestamp);
        QMutexLocker locker(&m_bufferMutex);
        m_mixedBuffer.clear();
    }

    qDebug() << "Audio capture loop ended";
    Logger::instance()->info("AudioCaptureDevice", "Capture loop ended");
}