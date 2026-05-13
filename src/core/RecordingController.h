#ifndef RECORDINGCONTROLLER_H
#define RECORDINGCONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QFile>
#include <windows.h>
#include "capture/VideoCaptureDevice.h"
#include "capture/AudioCaptureDevice.h"
#include "encoding/EncoderFactory.h"
#include "encoding/Mp4Writer.h"

/**
 * @class RecordingController
 * @brief 录制控制器类，整合视频捕获、音频捕获和MP4写入
 * @details 负责管理整个录制流程，包括初始化捕获设备、编码器工厂和MP4写入器，
 *          实现音视频同步录制功能
 */
class RecordingController : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param parent 父对象指针
     */
    explicit RecordingController(QObject* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~RecordingController();

    /**
     * @brief 开始录制
     * @param hwnd 目标窗口句柄（全屏录制时为nullptr或桌面句柄）
     * @param fullScreen 是否全屏录制
     * @param savePath 保存路径
     * @return 成功返回true，失败返回false
     */
    bool startRecording(HWND hwnd, bool fullScreen, const QString& savePath);

    /**
     * @brief 停止录制
     */
    void stopRecording();

    /**
     * @brief 检查是否正在录制
     * @return 正在录制返回true
     */
    bool isRecording() const { return m_isRecording; }

    /**
     * @brief 获取录制时长
     * @return 录制时长（毫秒）
     */
    qint64 recordingDuration() const { return m_duration; }

    /**
     * @brief 获取当前文件路径
     * @return 当前录制文件的完整路径
     */
    QString currentFilePath() const { return m_currentFilePath; }

signals:
    /**
     * @brief 录制开始信号
     */
    void recordingStarted();

    /**
     * @brief 录制停止信号
     * @param filePath 录制文件的完整路径
     */
    void recordingStopped(const QString& filePath);

    /**
     * @brief 录制错误信号
     * @param error 错误信息
     */
    void recordingError(const QString& error);

    /**
     * @brief 录制时长变化信号
     * @param duration 当前录制时长（毫秒）
     */
    void durationChanged(qint64 duration);

    /**
     * @brief 文件大小变化信号
     * @param size 当前文件大小（字节）
     */
    void fileSizeChanged(qint64 size);

private slots:
    /**
     * @brief 视频帧捕获处理槽函数
     * @param frame 捕获的视频帧图像
     * @param timestamp 时间戳（毫秒）
     */
    void onVideoFrameCaptured(const QImage& frame, qint64 timestamp);

    /**
     * @brief 音频捕获处理槽函数
     * @param audioData 捕获的音频数据（PCM格式）
     * @param timestamp 时间戳（毫秒）
     */
    void onAudioCaptured(const QByteArray& audioData, qint64 timestamp);

    /**
     * @brief 更新录制状态定时器处理
     */
    void updateStatus();

private:
    /**
     * @brief 设置编码器工厂
     * @return 成功返回true，失败返回false
     */
    bool setupEncoders();

    /**
     * @brief 设置MP4写入器
     * @param filePath 输出文件路径
     * @return 成功返回true，失败返回false
     */
    bool setupMp4Writer(const QString& filePath);

    /**
     * @brief 清理所有资源
     */
    void cleanup();

    /**
     * @brief 获取当前时间戳
     * @return 时间戳（毫秒）
     */
    qint64 getCurrentTimestamp();

private:
    bool m_isRecording;                    ///< 是否正在录制
    qint64 m_startTime;                    ///< 录制开始时间（毫秒）
    qint64 m_duration;                     ///< 录制时长（毫秒）
    QString m_currentFilePath;             ///< 当前录制文件路径
    QFile m_outputFile;                    ///< 输出文件对象

    VideoCaptureDevice* m_videoCapture;    ///< 视频捕获设备
    AudioCaptureDevice* m_audioCapture;    ///< 音频捕获设备
    EncoderFactory* m_videoEncoder;        ///< 视频编码器工厂
    EncoderFactory* m_audioEncoder;        ///< 音频编码器工厂
    Mp4Writer* m_mp4Writer;                ///< MP4写入器

    QTimer* m_statusTimer;                 ///< 状态更新定时器
    int m_frameCount;                       ///< 已捕获帧数
    int m_audioCount;                       ///< 已捕获音频样本数
};

#endif