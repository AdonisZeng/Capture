#ifndef ENCODERFACTORY_H
#define ENCODERFACTORY_H

#include <QObject>
#include <QList>
#include <QString>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include "config/SettingsManager.h"

/**
 * @brief 视频编码器类型枚举
 */
enum class VideoCodec { H264, H265 };

/**
 * @brief 音频编码器类型枚举
 */
enum class AudioCodec { AAC };

/**
 * @brief 编码器配置结构体
 */
struct EncoderConfig {
    int width;                  ///< 视频宽度
    int height;                 ///< 视频高度
    int frameRate;              ///< 帧率
    int bitrate;                ///< 比特率
    VideoCodec videoCodec;      ///< 视频编码器
    AudioCodec audioCodec;      ///< 音频编码器
    int sampleRate;             ///< 音频采样率
    int channels;               ///< 音频声道数
    int audioBitrate;           ///< 音频比特率
    HardwareAccelType hwAccel;  ///< 硬件加速类型
};

/**
 * @class EncoderFactory
 * @brief 编码器工厂类，支持软件编码和硬件加速编码
 * @details 支持的硬件加速：
 *          - NVIDIA NVENC (需要 NVIDIA 显卡)
 *          - AMD AMF (需要 AMD 显卡)
 *          - Intel QuickSync (需要 Intel 集成显卡)
 */
class EncoderFactory : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param parent 父对象指针
     */
    explicit EncoderFactory(QObject* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~EncoderFactory();

    /**
     * @brief 创建视频编码器
     * @param codec 视频编码器类型
     * @param width 视频宽度
     * @param height 视频高度
     * @param frameRate 帧率
     * @param bitrate 比特率
     * @param hwAccel 硬件加速类型
     * @return 创建成功返回 true
     */
    bool createVideoEncoder(VideoCodec codec, int width, int height, int frameRate, int bitrate,
                           HardwareAccelType hwAccel = HardwareAccelType::None);

    /**
     * @brief 创建音频编码器
     * @param codec 音频编码器类型
     * @param sampleRate 采样率
     * @param channels 声道数
     * @param bitrate 比特率
     * @return 创建成功返回 true
     */
    bool createAudioEncoder(AudioCodec codec, int sampleRate, int channels, int bitrate);

    /**
     * @brief 编码视频帧
     * @param rawData 原始视频数据
     * @param timestamp 时间戳
     * @param keyFrame [out] 是否为关键帧
     * @return 编码后的数据
     */
    QByteArray encodeVideo(const QByteArray& rawData, qint64 timestamp, bool& keyFrame);

    /**
     * @brief 编码音频帧
     * @param rawData 原始音频数据
     * @param timestamp 时间戳
     * @return 编码后的数据
     */
    QByteArray encodeAudio(const QByteArray& rawData, qint64 timestamp);

    /**
     * @brief 刷新编码器
     */
    void flush();

    /**
     * @brief 关闭编码器
     */
    void close();

    /**
     * @brief 获取视频编码器
     * @return 视频编码器指针
     */
    IMFTransform* videoEncoder() const { return m_videoEncoder; }

    /**
     * @brief 获取音频编码器
     * @return 音频编码器指针
     */
    IMFTransform* audioEncoder() const { return m_audioEncoder; }

    /**
     * @brief 检测可用的硬件加速类型
     * @return 可用的硬件加速类型列表
     */
    static QList<HardwareAccelType> detectAvailableHardwareAccel();

    /**
     * @brief 获取硬件加速名称
     * @param type 硬件加速类型
     * @return 名称字符串
     */
    static QString hardwareAccelName(HardwareAccelType type);

signals:
    /**
     * @brief 编码器错误信号
     * @param error 错误信息
     */
    void encoderError(const QString& error);

private:
    /**
     * @brief 创建 MFT 编码器
     * @param clsid 编码器 CLSID
     * @param encoder [out] 编码器指针
     * @return 创建成功返回 true
     */
    bool createMFT(const CLSID& clsid, IMFTransform** encoder);

    /**
     * @brief 创建硬件加速编码器
     * @param codec 视频编码器类型
     * @param hwAccel 硬件加速类型
     * @return 创建成功返回 true
     */
    bool createHardwareEncoder(VideoCodec codec, HardwareAccelType hwAccel);

    /**
     * @brief 设置视频输入类型
     * @return 设置成功返回 true
     */
    bool setVideoInputType();

    /**
     * @brief 设置视频输出类型
     * @param codec 视频编码器类型
     * @return 设置成功返回 true
     */
    bool setVideoOutputType(VideoCodec codec);

    /**
     * @brief 设置音频输入类型
     * @return 设置成功返回 true
     */
    bool setAudioInputType();

    /**
     * @brief 设置音频输出类型
     * @return 设置成功返回 true
     */
    bool setAudioOutputType();

private:
    IMFTransform* m_videoEncoder;           ///< 视频编码器
    IMFTransform* m_audioEncoder;           ///< 音频编码器
    IMFMediaType* m_videoInputType;         ///< 视频输入类型
    IMFMediaType* m_videoOutputType;        ///< 视频输出类型
    IMFMediaType* m_audioInputType;         ///< 音频输入类型
    IMFMediaType* m_audioOutputType;        ///< 音频输出类型

    MFT_OUTPUT_STREAM_INFO m_videoOutputInfo;   ///< 视频输出流信息
    MFT_OUTPUT_STREAM_INFO m_audioOutputInfo;   ///< 音频输出流信息

    QList<QByteArray> m_videoOutputBuffers;    ///< 视频输出缓冲区
    QList<QByteArray> m_audioOutputBuffers;    ///< 音频输出缓冲区

    int m_videoWidth;                       ///< 视频宽度
    int m_videoHeight;                      ///< 视频高度
    int m_videoFrameRate;                   ///< 视频帧率
    int m_videoBitrate;                     ///< 视频比特率
    HardwareAccelType m_hwAccel;            ///< 硬件加速类型
};

#endif