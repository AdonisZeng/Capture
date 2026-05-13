/**
 * @file EncoderFactory.cpp
 * @brief Windows Media Foundation 编码器工厂实现，支持软件编码和硬件加速编码
 * @author OpenClaw Screen Capture Tool
 */

#include "EncoderFactory.h"
#include <QDebug>
#include <mferror.h>
#include <initguid.h>
#include <codecapi.h>
#include <dxgi.h>
#include <d3d11.h>

#ifdef _MSC_VER
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#endif

DEFINE_GUID(CLSID_MSH265EncoderMFT, 0x420a523d, 0xe68c, 0x49d1, 0xa6, 0x4e, 0x50, 0x4b, 0xf4, 0x1f, 0x19, 0x8f);
DEFINE_GUID(CLSID_AACMFTEncoder, 0x93af0c51, 0x2275, 0x45d2, 0xa0, 0x7b, 0x86, 0x95, 0x66, 0x58, 0x5b, 0x59);

DEFINE_GUID(CLSID_NVENC_H264, 0x60d4e5e1, 0x1d54, 0x4c3d, 0x9f, 0x8d, 0x5c, 0x4e, 0x7b, 0x8e, 0x0f, 0x1a);
DEFINE_GUID(CLSID_NVENC_H265, 0x6a8c3f7e, 0x4b5d, 0x4e8c, 0xa1, 0x2f, 0x3c, 0x9d, 0x7e, 0x8a, 0x5b, 0x2c);

DEFINE_GUID(CLSID_AMF_H264, 0x8b42f5e3, 0x7a9e, 0x4c3a, 0xb8, 0x7d, 0x5f, 0x9e, 0x6c, 0x4d, 0x3e, 0x2a);
DEFINE_GUID(CLSID_AMF_H265, 0x9c3d8e1f, 0x4b6a, 0x4d7e, 0x9a, 0x2c, 0x5f, 0x8e, 0x3d, 0x7a, 0x4b, 0x1c);

DEFINE_GUID(CLSID_QSV_H264, 0x4be8d3c0, 0x0515, 0x4a37, 0xad, 0x55, 0xe4, 0xba, 0x20, 0x8b, 0x1e, 0x27);
DEFINE_GUID(CLSID_QSV_H265, 0x761c3d8e, 0x4a5e, 0x4b7c, 0x9d, 0x3f, 0x8e, 0x2a, 0x5c, 0x7b, 0x1d, 0x4e);

EncoderFactory::EncoderFactory(QObject* parent)
    : QObject(parent)
    , m_videoEncoder(nullptr)
    , m_audioEncoder(nullptr)
    , m_videoInputType(nullptr)
    , m_videoOutputType(nullptr)
    , m_audioInputType(nullptr)
    , m_audioOutputType(nullptr)
    , m_videoWidth(1920)
    , m_videoHeight(1080)
    , m_videoFrameRate(30)
    , m_videoBitrate(8000000)
    , m_hwAccel(HardwareAccelType::None)
{
}

EncoderFactory::~EncoderFactory()
{
    close();
}

QList<HardwareAccelType> EncoderFactory::detectAvailableHardwareAccel()
{
    QList<HardwareAccelType> available;
    available.append(HardwareAccelType::None);

    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
    if (FAILED(hr)) {
        return available;
    }

    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);

        QString vendorName;
        if (desc.VendorId == 0x10DE) {
            if (!available.contains(HardwareAccelType::NVIDIA)) {
                available.append(HardwareAccelType::NVIDIA);
                qDebug() << "Detected NVIDIA GPU:" << QString::fromWCharArray(desc.Description);
            }
        } else if (desc.VendorId == 0x1002 || desc.VendorId == 0x1022) {
            if (!available.contains(HardwareAccelType::AMD)) {
                available.append(HardwareAccelType::AMD);
                qDebug() << "Detected AMD GPU:" << QString::fromWCharArray(desc.Description);
            }
        } else if (desc.VendorId == 0x8086) {
            if (!available.contains(HardwareAccelType::Intel)) {
                available.append(HardwareAccelType::Intel);
                qDebug() << "Detected Intel GPU:" << QString::fromWCharArray(desc.Description);
            }
        }

        adapter->Release();
    }

    factory->Release();
    return available;
}

QString EncoderFactory::hardwareAccelName(HardwareAccelType type)
{
    switch (type) {
        case HardwareAccelType::None:
            return QString::fromUtf8("软件编码");
        case HardwareAccelType::NVIDIA:
            return QString::fromUtf8("NVIDIA NVENC");
        case HardwareAccelType::AMD:
            return QString::fromUtf8("AMD AMF");
        case HardwareAccelType::Intel:
            return QString::fromUtf8("Intel QuickSync");
        default:
            return QString::fromUtf8("未知");
    }
}

bool EncoderFactory::createVideoEncoder(VideoCodec codec, int width, int height, int frameRate, int bitrate,
                                        HardwareAccelType hwAccel)
{
    m_videoWidth = width;
    m_videoHeight = height;
    m_videoFrameRate = frameRate;
    m_videoBitrate = bitrate;
    m_hwAccel = hwAccel;

    HRESULT hr = S_OK;

    if (hwAccel != HardwareAccelType::None) {
        if (createHardwareEncoder(codec, hwAccel)) {
            qDebug() << "Hardware encoder created:" << hardwareAccelName(hwAccel);
        } else {
            qWarning() << "Failed to create hardware encoder, falling back to software";
            m_hwAccel = HardwareAccelType::None;
        }
    }

    if (m_hwAccel == HardwareAccelType::None) {
        CLSID codecClsid;
        if (codec == VideoCodec::H264) {
            codecClsid = CLSID_MSH264EncoderMFT;
        } else {
            codecClsid = CLSID_MSH265EncoderMFT;
        }

        if (!createMFT(codecClsid, &m_videoEncoder)) {
            emit encoderError(QString::fromLatin1("Failed to create video encoder MFT"));
            return false;
        }
    }

    if (!setVideoInputType()) {
        emit encoderError(QString::fromLatin1("Failed to set video input type"));
        return false;
    }

    if (!setVideoOutputType(codec)) {
        emit encoderError(QString::fromLatin1("Failed to set video output type"));
        return false;
    }

    hr = m_videoEncoder->SetInputType(0, m_videoInputType, 0);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to set video input type on encoder: %1").arg(hr));
        return false;
    }

    hr = m_videoEncoder->SetOutputType(0, m_videoOutputType, 0);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to set video output type on encoder: %1").arg(hr));
        return false;
    }

    hr = m_videoEncoder->GetOutputStreamInfo(0, &m_videoOutputInfo);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to get video output stream info: %1").arg(hr));
        return false;
    }

    hr = m_videoEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to begin streaming: %1").arg(hr));
        return false;
    }

    return true;
}

bool EncoderFactory::createHardwareEncoder(VideoCodec codec, HardwareAccelType hwAccel)
{
    CLSID encoderClsid;
    bool found = false;

    switch (hwAccel) {
        case HardwareAccelType::NVIDIA:
            encoderClsid = (codec == VideoCodec::H264) ? CLSID_NVENC_H264 : CLSID_NVENC_H265;
            break;
        case HardwareAccelType::AMD:
            encoderClsid = (codec == VideoCodec::H264) ? CLSID_AMF_H264 : CLSID_AMF_H265;
            break;
        case HardwareAccelType::Intel:
            encoderClsid = (codec == VideoCodec::H264) ? CLSID_QSV_H264 : CLSID_QSV_H265;
            break;
        default:
            return false;
    }

    HRESULT hr = CoCreateInstance(
        encoderClsid,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IMFTransform,
        reinterpret_cast<void**>(&m_videoEncoder)
    );

    if (SUCCEEDED(hr)) {
        qDebug() << "Hardware encoder instance created";
        return true;
    }

    IMFActivate** ppActivates = nullptr;
    UINT32 count = 0;

    MFT_REGISTER_TYPE_INFO outputType = {};
    outputType.guidMajorType = MFMediaType_Video;
    outputType.guidSubtype = (codec == VideoCodec::H264) ? MFVideoFormat_H264 : MFVideoFormat_H265;

    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &outputType,
        &ppActivates,
        &count
    );

    if (SUCCEEDED(hr) && count > 0) {
        for (UINT32 i = 0; i < count; ++i) {
            LPWSTR name = nullptr;
            hr = ppActivates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, nullptr);
            if (SUCCEEDED(hr) && name) {
                QString encoderName = QString::fromWCharArray(name);
                qDebug() << "Found hardware encoder:" << encoderName;
                CoTaskMemFree(name);
            }

            hr = ppActivates[i]->ActivateObject(IID_IMFTransform, (void**)&m_videoEncoder);
            if (SUCCEEDED(hr)) {
                found = true;
                break;
            }
        }

        for (UINT32 i = 0; i < count; ++i) {
            ppActivates[i]->Release();
        }
        CoTaskMemFree(ppActivates);
    }

    return found;
}

bool EncoderFactory::createAudioEncoder(AudioCodec codec, int sampleRate, int channels, int bitrate)
{
    Q_UNUSED(codec)
    Q_UNUSED(sampleRate)
    Q_UNUSED(channels)
    Q_UNUSED(bitrate)

    HRESULT hr = S_OK;

    if (!createMFT(CLSID_AACMFTEncoder, &m_audioEncoder)) {
        emit encoderError(QString::fromLatin1("Failed to create audio encoder MFT"));
        return false;
    }

    if (!setAudioInputType()) {
        emit encoderError(QString::fromLatin1("Failed to set audio input type"));
        return false;
    }

    if (!setAudioOutputType()) {
        emit encoderError(QString::fromLatin1("Failed to set audio output type"));
        return false;
    }

    hr = m_audioEncoder->SetInputType(0, m_audioInputType, 0);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to set audio input type on encoder: %1").arg(hr));
        return false;
    }

    hr = m_audioEncoder->SetOutputType(0, m_audioOutputType, 0);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to set audio output type on encoder: %1").arg(hr));
        return false;
    }

    hr = m_audioEncoder->GetOutputStreamInfo(0, &m_audioOutputInfo);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to get audio output stream info: %1").arg(hr));
        return false;
    }

    hr = m_audioEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to begin audio streaming: %1").arg(hr));
        return false;
    }

    return true;
}

QByteArray EncoderFactory::encodeVideo(const QByteArray& rawData, qint64 timestamp, bool& keyFrame)
{
    if (!m_videoEncoder || rawData.isEmpty()) {
        return QByteArray();
    }

    keyFrame = false;

    IMFSample* inputSample = nullptr;
    IMFMediaBuffer* inputBuffer = nullptr;

    HRESULT hr = MFCreateSample(&inputSample);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to create input sample: %1").arg(hr));
        return QByteArray();
    }

    hr = MFCreateMemoryBuffer(rawData.size(), &inputBuffer);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to create input buffer: %1").arg(hr));
        inputSample->Release();
        return QByteArray();
    }

    BYTE* data = nullptr;
    DWORD maxLength = 0;

    hr = inputBuffer->Lock(&data, &maxLength, nullptr);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to lock input buffer: %1").arg(hr));
        inputBuffer->Release();
        inputSample->Release();
        return QByteArray();
    }

    memcpy(data, rawData.constData(), rawData.size());
    inputBuffer->Unlock();

    inputBuffer->SetCurrentLength(rawData.size());
    inputSample->AddBuffer(inputBuffer);
    inputSample->SetSampleTime(timestamp * 10000);

    hr = m_videoEncoder->ProcessInput(0, inputSample, 0);
    if (FAILED(hr)) {
        inputBuffer->Release();
        inputSample->Release();
        return QByteArray();
    }

    inputBuffer->Release();
    inputSample->Release();

    QByteArray result;

    MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
    IMFSample* outputSample = nullptr;
    IMFMediaBuffer* outputMediaBuffer = nullptr;

    hr = MFCreateSample(&outputSample);
    if (FAILED(hr)) {
        return QByteArray();
    }

    hr = MFCreateMemoryBuffer(m_videoOutputInfo.cbSize, &outputMediaBuffer);
    if (FAILED(hr)) {
        outputSample->Release();
        return QByteArray();
    }

    outputSample->AddBuffer(outputMediaBuffer);

    outputDataBuffer.dwStreamID = 0;
    outputDataBuffer.pSample = outputSample;
    outputDataBuffer.pEvents = nullptr;

    DWORD status = 0;
    hr = m_videoEncoder->ProcessOutput(0, 1, &outputDataBuffer, &status);

    if (SUCCEEDED(hr)) {
        IMFMediaBuffer* pBuffer = nullptr;
        hr = outputSample->GetBufferByIndex(0, &pBuffer);
        if (SUCCEEDED(hr)) {
            BYTE* outputData = nullptr;
            DWORD outputLength = 0;

            hr = pBuffer->Lock(&outputData, &maxLength, &outputLength);
            if (SUCCEEDED(hr)) {
                if (outputLength > 0) {
                    result = QByteArray(reinterpret_cast<const char*>(outputData), outputLength);
                }
                pBuffer->Unlock();
            }
            pBuffer->Release();
        }
    }

    outputMediaBuffer->Release();
    outputSample->Release();

    return result;
}

QByteArray EncoderFactory::encodeAudio(const QByteArray& rawData, qint64 timestamp)
{
    if (!m_audioEncoder || rawData.isEmpty()) {
        return QByteArray();
    }

    IMFSample* inputSample = nullptr;
    IMFMediaBuffer* inputBuffer = nullptr;

    HRESULT hr = MFCreateSample(&inputSample);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to create audio input sample: %1").arg(hr));
        return QByteArray();
    }

    hr = MFCreateMemoryBuffer(rawData.size(), &inputBuffer);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to create audio input buffer: %1").arg(hr));
        inputSample->Release();
        return QByteArray();
    }

    BYTE* data = nullptr;
    DWORD maxLength = 0;

    hr = inputBuffer->Lock(&data, &maxLength, nullptr);
    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("Failed to lock audio input buffer: %1").arg(hr));
        inputBuffer->Release();
        inputSample->Release();
        return QByteArray();
    }

    memcpy(data, rawData.constData(), rawData.size());
    inputBuffer->Unlock();

    inputBuffer->SetCurrentLength(rawData.size());
    inputSample->AddBuffer(inputBuffer);
    inputSample->SetSampleTime(timestamp * 10000);

    hr = m_audioEncoder->ProcessInput(0, inputSample, 0);
    if (FAILED(hr)) {
        inputBuffer->Release();
        inputSample->Release();
        return QByteArray();
    }

    inputBuffer->Release();
    inputSample->Release();

    QByteArray result;

    MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
    IMFSample* outputSample = nullptr;
    IMFMediaBuffer* outputMediaBuffer = nullptr;

    hr = MFCreateSample(&outputSample);
    if (FAILED(hr)) {
        return QByteArray();
    }

    hr = MFCreateMemoryBuffer(m_audioOutputInfo.cbSize, &outputMediaBuffer);
    if (FAILED(hr)) {
        outputSample->Release();
        return QByteArray();
    }

    outputSample->AddBuffer(outputMediaBuffer);

    outputDataBuffer.dwStreamID = 0;
    outputDataBuffer.pSample = outputSample;
    outputDataBuffer.pEvents = nullptr;

    DWORD status = 0;
    hr = m_audioEncoder->ProcessOutput(0, 1, &outputDataBuffer, &status);

    if (SUCCEEDED(hr)) {
        IMFMediaBuffer* pBuffer = nullptr;
        hr = outputSample->GetBufferByIndex(0, &pBuffer);
        if (SUCCEEDED(hr)) {
            BYTE* outputData = nullptr;
            DWORD outputLength = 0;

            hr = pBuffer->Lock(&outputData, &maxLength, &outputLength);
            if (SUCCEEDED(hr)) {
                if (outputLength > 0) {
                    result = QByteArray(reinterpret_cast<const char*>(outputData), outputLength);
                }
                pBuffer->Unlock();
            }
            pBuffer->Release();
        }
    }

    outputMediaBuffer->Release();
    outputSample->Release();

    return result;
}

void EncoderFactory::flush()
{
    if (m_videoEncoder) {
        m_videoEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    if (m_audioEncoder) {
        m_audioEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    m_videoOutputBuffers.clear();
    m_audioOutputBuffers.clear();
}

void EncoderFactory::close()
{
    if (m_videoEncoder) {
        m_videoEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_videoEncoder->Release();
        m_videoEncoder = nullptr;
    }

    if (m_audioEncoder) {
        m_audioEncoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_audioEncoder->Release();
        m_audioEncoder = nullptr;
    }

    if (m_videoInputType) {
        m_videoInputType->Release();
        m_videoInputType = nullptr;
    }

    if (m_videoOutputType) {
        m_videoOutputType->Release();
        m_videoOutputType = nullptr;
    }

    if (m_audioInputType) {
        m_audioInputType->Release();
        m_audioInputType = nullptr;
    }

    if (m_audioOutputType) {
        m_audioOutputType->Release();
        m_audioOutputType = nullptr;
    }

    m_videoOutputBuffers.clear();
    m_audioOutputBuffers.clear();
}

bool EncoderFactory::createMFT(const CLSID& clsid, IMFTransform** encoder)
{
    HRESULT hr = CoCreateInstance(
        clsid,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IMFTransform,
        reinterpret_cast<void**>(encoder)
    );

    if (FAILED(hr)) {
        emit encoderError(QString::fromLatin1("CoCreateInstance failed: %1").arg(hr));
        return false;
    }

    return true;
}

bool EncoderFactory::setVideoInputType()
{
    HRESULT hr = MFCreateMediaType(&m_videoInputType);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_videoInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_videoInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_videoInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) {
        return false;
    }

    hr = MFSetAttributeSize(m_videoInputType, MF_MT_FRAME_SIZE, m_videoWidth, m_videoHeight);
    if (FAILED(hr)) {
        return false;
    }

    hr = MFSetAttributeRatio(m_videoInputType, MF_MT_FRAME_RATE, m_videoFrameRate, 1);
    if (FAILED(hr)) {
        return false;
    }

    return true;
}

bool EncoderFactory::setVideoOutputType(VideoCodec codec)
{
    HRESULT hr = MFCreateMediaType(&m_videoOutputType);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_videoOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) {
        return false;
    }

    if (codec == VideoCodec::H264) {
        hr = m_videoOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    } else {
        hr = m_videoOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H265);
    }
    if (FAILED(hr)) {
        return false;
    }

    hr = m_videoOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) {
        return false;
    }

    hr = MFSetAttributeSize(m_videoOutputType, MF_MT_FRAME_SIZE, m_videoWidth, m_videoHeight);
    if (FAILED(hr)) {
        return false;
    }

    hr = MFSetAttributeRatio(m_videoOutputType, MF_MT_FRAME_RATE, m_videoFrameRate, 1);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_videoOutputType->SetUINT32(MF_MT_AVG_BITRATE, m_videoBitrate);
    if (FAILED(hr)) {
        return false;
    }

    return true;
}

bool EncoderFactory::setAudioInputType()
{
    HRESULT hr = MFCreateMediaType(&m_audioInputType);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioInputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioInputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioInputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioInputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    if (FAILED(hr)) {
        return false;
    }

    return true;
}

bool EncoderFactory::setAudioOutputType()
{
    HRESULT hr = MFCreateMediaType(&m_audioOutputType);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (FAILED(hr)) {
        return false;
    }

    hr = m_audioOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    if (FAILED(hr)) {
        return false;
    }

    return true;
}
