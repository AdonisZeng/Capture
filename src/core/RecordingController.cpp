/**
 * @file RecordingController.cpp
 * @brief 录制控制器实现
 * @author OpenClaw Screen Capture Tool
 */

#include "RecordingController.h"
#include "config/SettingsManager.h"
#include "utils/Logger.h"
#include <QDebug>
#include <QDateTime>
#include <QImage>
#include <QDir>

RecordingController::RecordingController(QObject* parent)
    : QObject(parent)
    , m_isRecording(false)
    , m_startTime(0)
    , m_duration(0)
    , m_currentFilePath()
    , m_outputFile()
    , m_videoCapture(nullptr)
    , m_audioCapture(nullptr)
    , m_videoEncoder(nullptr)
    , m_audioEncoder(nullptr)
    , m_mp4Writer(nullptr)
    , m_statusTimer(nullptr)
    , m_frameCount(0)
    , m_audioCount(0)
{
}

RecordingController::~RecordingController()
{
    stopRecording();
    cleanup();
}

bool RecordingController::startRecording(HWND hwnd, bool fullScreen, const QString& savePath)
{
    if (m_isRecording) {
        qWarning() << "Recording is already in progress";
        Logger::instance()->warning("RecordingController", "Recording is already in progress");
        return false;
    }

    cleanup();

    Logger::instance()->info("RecordingController", QString("Starting recording: fullScreen=%1").arg(fullScreen));

    m_videoCapture = new VideoCaptureDevice(this);
    m_audioCapture = new AudioCaptureDevice(this);
    m_videoEncoder = new EncoderFactory(this);
    m_audioEncoder = new EncoderFactory(this);
    m_mp4Writer = new Mp4Writer(this);

    if (!m_videoCapture->initialize()) {
        emit recordingError("Failed to initialize video capture device");
        Logger::instance()->error("RecordingController", "Failed to initialize video capture device");
        cleanup();
        return false;
    }
    Logger::instance()->info("RecordingController", "Video capture initialized");

    if (!m_audioCapture->initialize()) {
        emit recordingError("Failed to initialize audio capture device");
        Logger::instance()->error("RecordingController", "Failed to initialize audio capture device");
        cleanup();
        return false;
    }
    Logger::instance()->info("RecordingController", "Audio capture initialized");

    QString filePath = savePath;
    if (filePath.isEmpty()) {
        QDateTime now = QDateTime::currentDateTime();
        QString fileName = QString("Capture_%1.mp4").arg(now.toString("yyyyMMdd_HHmmss"));
        filePath = QDir::homePath() + "/" + fileName;
    }

    if (!setupMp4Writer(filePath)) {
        emit recordingError("Failed to setup MP4 writer");
        Logger::instance()->error("RecordingController", "Failed to setup MP4 writer");
        cleanup();
        return false;
    }
    Logger::instance()->info("RecordingController", QString("MP4 writer setup: %1").arg(filePath));

    if (!setupEncoders()) {
        emit recordingError("Failed to setup encoders");
        Logger::instance()->error("RecordingController", "Failed to setup encoders");
        cleanup();
        return false;
    }
    Logger::instance()->info("RecordingController", "Encoders setup completed");

    connect(m_videoCapture, &VideoCaptureDevice::frameCaptured,
            this, &RecordingController::onVideoFrameCaptured);
    connect(m_audioCapture, &AudioCaptureDevice::audioCaptured,
            this, &RecordingController::onAudioCaptured);
    connect(m_videoCapture, &VideoCaptureDevice::captureError,
            this, [this](const QString& error) { emit recordingError(error); });
    connect(m_audioCapture, &AudioCaptureDevice::captureError,
            this, [this](const QString& error) { emit recordingError(error); });

    bool captureStarted = false;
    if (fullScreen) {
        captureStarted = m_videoCapture->startCapture(nullptr, true);
    } else {
        captureStarted = m_videoCapture->startCapture(hwnd, false);
    }

    if (!captureStarted) {
        emit recordingError("Failed to start video capture");
        Logger::instance()->error("RecordingController", "Failed to start video capture");
        cleanup();
        return false;
    }
    Logger::instance()->info("RecordingController", "Video capture started");

    if (!m_audioCapture->startCapture(true, false)) {
        emit recordingError("Failed to start audio capture");
        Logger::instance()->error("RecordingController", "Failed to start audio capture");
        cleanup();
        return false;
    }
    Logger::instance()->info("RecordingController", "Audio capture started");

    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &RecordingController::updateStatus);
    m_statusTimer->start(100);

    m_startTime = getCurrentTimestamp();
    m_duration = 0;
    m_frameCount = 0;
    m_audioCount = 0;
    m_isRecording = true;
    m_currentFilePath = filePath;

    Logger::instance()->info("RecordingController", QString("Recording started: %1").arg(filePath));
    emit recordingStarted();
    return true;
}

void RecordingController::stopRecording()
{
    if (!m_isRecording) {
        return;
    }

    Logger::instance()->info("RecordingController", QString("Recording stopped: frames=%1, audioSamples=%2, duration=%3ms")
                                .arg(m_frameCount).arg(m_audioCount).arg(m_duration));

    m_isRecording = false;

    if (m_statusTimer) {
        m_statusTimer->stop();
        delete m_statusTimer;
        m_statusTimer = nullptr;
    }

    if (m_videoCapture) {
        m_videoCapture->stopCapture();
    }

    if (m_audioCapture) {
        m_audioCapture->stopCapture();
    }

    disconnect(m_videoCapture, &VideoCaptureDevice::frameCaptured,
               this, &RecordingController::onVideoFrameCaptured);
    disconnect(m_audioCapture, &AudioCaptureDevice::audioCaptured,
               this, &RecordingController::onAudioCaptured);

    if (m_videoEncoder) {
        m_videoEncoder->flush();
    }

    if (m_audioEncoder) {
        m_audioEncoder->flush();
    }

    if (m_mp4Writer) {
        m_mp4Writer->close();
    }

    QString filePath = m_currentFilePath;
    emit recordingStopped(filePath);
}

void RecordingController::onVideoFrameCaptured(const QImage& frame, qint64 timestamp)
{
    if (!m_isRecording || !m_videoEncoder || !m_mp4Writer) {
        return;
    }

    QByteArray rawData(reinterpret_cast<const char*>(frame.bits()), frame.bytesPerLine() * frame.height());

    bool keyFrame = false;
    QByteArray encodedData = m_videoEncoder->encodeVideo(rawData, timestamp, keyFrame);

    if (!encodedData.isEmpty()) {
        qint64 mp4Timestamp = timestamp * 1000;
        m_mp4Writer->writeVideoSample(encodedData, mp4Timestamp, keyFrame);
        m_frameCount++;
    }
}

void RecordingController::onAudioCaptured(const QByteArray& audioData, qint64 timestamp)
{
    if (!m_isRecording || !m_audioEncoder || !m_mp4Writer) {
        return;
    }

    QByteArray encodedData = m_audioEncoder->encodeAudio(audioData, timestamp);

    if (!encodedData.isEmpty()) {
        qint64 mp4Timestamp = timestamp * 1000;
        m_mp4Writer->writeAudioSample(encodedData, mp4Timestamp);
        m_audioCount++;
    }
}

void RecordingController::updateStatus()
{
    if (!m_isRecording) {
        return;
    }

    m_duration = getCurrentTimestamp() - m_startTime;
    emit durationChanged(m_duration);

    if (m_outputFile.exists()) {
        qint64 fileSize = m_outputFile.size();
        emit fileSizeChanged(fileSize);
    }
}

bool RecordingController::setupEncoders()
{
    if (!m_videoEncoder || !m_audioEncoder) {
        return false;
    }

    SettingsManager* settings = SettingsManager::instance();
    VideoSettings videoSettings = settings->videoSettings();
    AudioSettings audioSettings = settings->audioSettings();

    QSize captureSize = m_videoCapture->captureSize();
    if (videoSettings.width > 0 && videoSettings.height > 0) {
        captureSize = QSize(videoSettings.width, videoSettings.height);
    }

    VideoCodec codec = VideoCodec::H264;
    if (videoSettings.encoder == QString::fromUtf8("H.265")) {
        codec = VideoCodec::H265;
    }

    qDebug() << "Creating video encoder:"
             << "width=" << captureSize.width()
             << "height=" << captureSize.height()
             << "frameRate=" << videoSettings.frameRate
             << "bitrate=" << videoSettings.bitrate
             << "hwAccel=" << static_cast<int>(videoSettings.hwAccel);

    if (!m_videoEncoder->createVideoEncoder(codec,
                                            captureSize.width(),
                                            captureSize.height(),
                                            videoSettings.frameRate,
                                            videoSettings.bitrate,
                                            videoSettings.hwAccel)) {
        qWarning() << "Failed to create video encoder";
        return false;
    }

    if (!m_audioEncoder->createAudioEncoder(AudioCodec::AAC,
                                            audioSettings.sampleRate,
                                            audioSettings.channels,
                                            audioSettings.bitrate)) {
        qWarning() << "Failed to create audio encoder";
        return false;
    }

    return true;
}

bool RecordingController::setupMp4Writer(const QString& filePath)
{
    if (!m_videoCapture || !m_mp4Writer) {
        return false;
    }

    SettingsManager* settings = SettingsManager::instance();
    VideoSettings videoSettings = settings->videoSettings();
    AudioSettings audioSettings = settings->audioSettings();

    QSize captureSize = m_videoCapture->captureSize();
    if (videoSettings.width > 0 && videoSettings.height > 0) {
        captureSize = QSize(videoSettings.width, videoSettings.height);
    }

    m_outputFile.setFileName(filePath);

    if (!m_mp4Writer->open(filePath,
                           captureSize.width(),
                           captureSize.height(),
                           videoSettings.frameRate,
                           audioSettings.sampleRate,
                           audioSettings.channels,
                           videoSettings.bitrate,
                           audioSettings.bitrate)) {
        qWarning() << "Failed to open MP4 writer";
        return false;
    }

    return true;
}

void RecordingController::cleanup()
{
    if (m_videoCapture) {
        delete m_videoCapture;
        m_videoCapture = nullptr;
    }

    if (m_audioCapture) {
        delete m_audioCapture;
        m_audioCapture = nullptr;
    }

    if (m_videoEncoder) {
        delete m_videoEncoder;
        m_videoEncoder = nullptr;
    }

    if (m_audioEncoder) {
        delete m_audioEncoder;
        m_audioEncoder = nullptr;
    }

    if (m_mp4Writer) {
        delete m_mp4Writer;
        m_mp4Writer = nullptr;
    }

    if (m_statusTimer) {
        delete m_statusTimer;
        m_statusTimer = nullptr;
    }

    m_outputFile.close();
    m_currentFilePath.clear();
    m_duration = 0;
    m_frameCount = 0;
    m_audioCount = 0;
}

qint64 RecordingController::getCurrentTimestamp()
{
    return QDateTime::currentMSecsSinceEpoch();
}
