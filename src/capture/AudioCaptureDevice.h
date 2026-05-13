#ifndef AUDIOCAPTUREDEVICE_H
#define AUDIOCAPTUREDEVICE_H

#include <QObject>
#include <QList>
#include <QString>
#include <QMutex>
#include <QByteArray>
#include <QThread>
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>

/**
 * @brief 音频设备信息结构体
 */
struct AudioDeviceInfo {
    QString id;          ///< 设备ID
    QString name;       ///< 设备名称
    bool isDefault;     ///< 是否为默认设备
    bool isSystemAudio; ///< 是否为系统音频设备
    bool isMicrophone;  ///< 是否为麦克风设备
};

/**
 * @class AudioCaptureDevice
 * @brief 音频捕获设备类，使用Windows Core Audio API (WASAPI)实现系统音频和麦克风捕获
 * @details 支持混音捕获，通过WASAPI Loopback捕获系统音频，通过默认音频输入捕获麦克风
 */
class AudioCaptureDevice : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param parent 父对象指针
     */
    explicit AudioCaptureDevice(QObject* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~AudioCaptureDevice();

    /**
     * @brief 枚举所有可用的音频设备
     * @return 音频设备列表
     */
    QList<AudioDeviceInfo> enumAudioDevices();

    /**
     * @brief 初始化音频捕获设备
     * @return 初始化是否成功
     */
    bool initialize();

    /**
     * @brief 开始音频捕获
     * @param systemAudio 是否捕获系统音频
     * @param microphone 是否捕获麦克风
     * @return 启动是否成功
     */
    bool startCapture(bool systemAudio, bool microphone);

    /**
     * @brief 停止音频捕获
     */
    void stopCapture();

    /**
     * @brief 混合两个音频缓冲区的样本
     * @param systemBuffer 系统音频缓冲区
     * @param micBuffer 麦克风音频缓冲区
     * @param frameCount 帧数量
     */
    void mixAudioSamples(BYTE* systemBuffer, BYTE* micBuffer, int frameCount);

    /**
     * @brief 获取混合后的音频样本
     * @return 混合音频数据
     */
    QByteArray getMixedSamples() const;

signals:
    /**
     * @brief 音频数据捕获信号
     * @param audioData 音频数据
     * @param timestamp 时间戳（毫秒）
     */
    void audioCaptured(const QByteArray& audioData, qint64 timestamp);

    /**
     * @brief 捕获错误信号
     * @param error 错误信息
     */
    void captureError(const QString& error);

private:
    /**
     * @brief 初始化设备枚举器
     * @return 初始化是否成功
     */
    bool initDeviceEnumerator();

    /**
     * @brief 创建系统音频客户端
     * @return 创建是否成功
     */
    bool createSystemAudioClient();

    /**
     * @brief 创建麦克风客户端
     * @return 创建是否成功
     */
    bool createMicrophoneClient();

    /**
     * @brief 获取默认设备ID
     * @param dataFlow 数据流方向（eRender或eCapture）
     * @return 设备ID字符串
     */
    QString getDefaultDeviceId(EDataFlow dataFlow);

private:
    IMMDeviceEnumerator* m_deviceEnumerator;   ///< 设备枚举器
    IMMDevice* m_systemAudioDevice;            ///< 系统音频设备
    IMMDevice* m_microphoneDevice;             ///< 麦克风设备
    IAudioClient* m_systemAudioClient;         ///< 系统音频客户端
    IAudioClient* m_microphoneClient;          ///< 麦克风客户端
    IAudioCaptureClient* m_systemCaptureClient;///< 系统音频捕获客户端
    IAudioCaptureClient* m_micCaptureClient;   ///< 麦克风捕获客户端

    bool m_isCapturing;                        ///< 是否正在捕获
    bool m_systemAudioEnabled;                 ///< 系统音频是否启用
    bool m_microphoneEnabled;                  ///< 麦克风是否启用

    QByteArray m_mixedBuffer;                  ///< 混合音频缓冲区
    mutable QMutex m_bufferMutex;              ///< 缓冲区互斥锁
    int m_sampleRate;                          ///< 采样率
    int m_channels;                            ///< 通道数
    int m_bitsPerSample;                       ///< 位深度
    QThread* m_captureThread;                  ///< 捕获线程

private slots:
    void captureLoop();
};

#endif