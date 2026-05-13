#include "Mp4Writer.h"
#include <QDebug>
#include <mferror.h>

Mp4Writer::Mp4Writer(QObject* parent)
    : QObject(parent)
    , m_isOpen(false)
    , m_sinkWriter(nullptr)
    , m_videoStreamIndex(0)
    , m_audioStreamIndex(0)
    , m_videoTimestampOffset(0)
    , m_audioTimestampOffset(0)
    , m_lastVideoTime(0)
    , m_lastAudioTime(0)
{
}

Mp4Writer::~Mp4Writer()
{
    close();
}

bool Mp4Writer::open(const QString& filePath, int width, int height, int frameRate,
                     int sampleRate, int channels, int videoBitrate, int audioBitrate)
{
    if (m_isOpen) {
        close();
    }

    m_filePath = filePath;

    IMFMediaType* videoType = nullptr;
    IMFMediaType* audioType = nullptr;
    HRESULT hr = S_OK;

    hr = MFCreateSinkWriterFromURL(filePath.toStdWString().c_str(), nullptr, nullptr, &m_sinkWriter);
    if (FAILED(hr)) {
        emit error("Failed to create sink writer");
        return false;
    }

    hr = MFCreateMediaType(&videoType);
    if (FAILED(hr)) {
        emit error("Failed to create video media type");
        return false;
    }

    hr = videoType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = videoType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = videoType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(videoType, MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(videoType, MF_MT_FRAME_RATE, frameRate, 1);
    if (SUCCEEDED(hr)) hr = videoType->SetUINT32(MF_MT_AVG_BITRATE, videoBitrate);

    if (SUCCEEDED(hr)) {
        hr = m_sinkWriter->AddStream(videoType, &m_videoStreamIndex);
    }
    videoType->Release();

    if (FAILED(hr)) {
        emit error("Failed to add video stream");
        return false;
    }

    hr = MFCreateMediaType(&audioType);
    if (FAILED(hr)) {
        emit error("Failed to create audio media type");
        return false;
    }

    hr = audioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = audioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(hr)) hr = audioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    if (SUCCEEDED(hr)) hr = audioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = audioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = audioType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audioBitrate / 8);

    if (SUCCEEDED(hr)) {
        hr = m_sinkWriter->AddStream(audioType, &m_audioStreamIndex);
    }
    audioType->Release();

    if (FAILED(hr)) {
        emit error("Failed to add audio stream");
        return false;
    }

    hr = m_sinkWriter->BeginWriting();
    if (FAILED(hr)) {
        emit error("Failed to begin writing");
        return false;
    }

    m_isOpen = true;
    return true;
}

void Mp4Writer::close()
{
    if (m_sinkWriter != nullptr) {
        m_sinkWriter->Finalize();
        m_sinkWriter->Release();
        m_sinkWriter = nullptr;
    }

    m_videoTimestampOffset = 0;
    m_audioTimestampOffset = 0;
    m_lastVideoTime = 0;
    m_lastAudioTime = 0;
    m_isOpen = false;
}

bool Mp4Writer::writeVideoSample(const QByteArray& data, qint64 timestamp, bool keyFrame)
{
    Q_UNUSED(keyFrame)

    if (!m_isOpen || m_sinkWriter == nullptr) {
        return false;
    }

    if (m_videoTimestampOffset == 0) {
        m_videoTimestampOffset = timestamp;
    }

    qint64 mfTimestamp = (timestamp - m_videoTimestampOffset) * 10;
    m_lastVideoTime = mfTimestamp;

    return writeSample(m_videoStreamIndex, data, mfTimestamp);
}

bool Mp4Writer::writeAudioSample(const QByteArray& data, qint64 timestamp)
{
    if (!m_isOpen || m_sinkWriter == nullptr) {
        return false;
    }

    if (m_audioTimestampOffset == 0) {
        m_audioTimestampOffset = timestamp;
    }

    qint64 mfTimestamp = (timestamp - m_audioTimestampOffset) * 10;
    m_lastAudioTime = mfTimestamp;

    return writeSample(m_audioStreamIndex, data, mfTimestamp);
}

bool Mp4Writer::writeSample(DWORD streamIndex, const QByteArray& data, qint64 mfTimestamp)
{
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        emit error("Failed to create sample");
        return false;
    }

    DWORD bufferSize = data.size();
    hr = MFCreateMemoryBuffer(bufferSize, &buffer);
    if (FAILED(hr)) {
        sample->Release();
        emit error("Failed to create buffer");
        return false;
    }

    BYTE* bufferData = nullptr;
    hr = buffer->Lock(&bufferData, nullptr, nullptr);
    if (FAILED(hr)) {
        buffer->Release();
        sample->Release();
        emit error("Failed to lock buffer");
        return false;
    }

    memcpy(bufferData, data.constData(), bufferSize);
    buffer->Unlock();
    buffer->SetCurrentLength(bufferSize);

    sample->AddBuffer(buffer);
    buffer->Release();

    hr = sample->SetSampleTime(mfTimestamp);
    if (SUCCEEDED(hr)) {
        hr = sample->SetSampleDuration(333333);
    }

    if (SUCCEEDED(hr)) {
        hr = m_sinkWriter->WriteSample(streamIndex, sample);
    }
    sample->Release();

    if (FAILED(hr)) {
        emit error("Failed to write sample");
        return false;
    }

    return true;
}
