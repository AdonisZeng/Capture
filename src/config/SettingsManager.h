#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QSettings>
#include <QString>
#include <QDir>

/**
 * @brief 硬件加速类型枚举
 */
enum class HardwareAccelType {
    None,       ///< 软件编码（无硬件加速）
    NVIDIA,     ///< NVIDIA NVENC
    AMD,        ///< AMD AMF
    Intel       ///< Intel QuickSync
};

struct VideoSettings {
    int frameRate;              ///< 帧率
    int width;                  ///< 视频宽度
    int height;                 ///< 视频高度
    QString encoder;            ///< 编码器类型 (H.264/H.265)
    int bitrate;                ///< 比特率
    HardwareAccelType hwAccel;  ///< 硬件加速类型
};

struct AudioSettings {
    int sampleRate;             ///< 采样率
    int channels;               ///< 声道数
    int bitrate;                ///< 比特率
    QString systemAudioDevice;  ///< 系统音频设备
    QString microphoneDevice;   ///< 麦克风设备
    bool enableSystemAudio;     ///< 是否启用系统音频
    bool enableMicrophone;      ///< 是否启用麦克风
};

struct GeneralSettings {
    QString savePath;           ///< 保存路径
    QString screenshotFormat;   ///< 截屏格式
    QString screenshotPath;     ///< 截屏保存路径
};

class SettingsManager : public QObject
{
    Q_OBJECT
public:
    static SettingsManager* instance();

    VideoSettings videoSettings() const;
    void setVideoSettings(const VideoSettings& settings);

    AudioSettings audioSettings() const;
    void setAudioSettings(const AudioSettings& settings);

    GeneralSettings generalSettings() const;
    void setGeneralSettings(const GeneralSettings& settings);

    QString screenshotHotkey() const;
    void setScreenshotHotkey(const QString& hotkey);

    QString regionCaptureHotkey() const;
    void setRegionCaptureHotkey(const QString& hotkey);

    QString fullscreenRecordHotkey() const;
    void setFullscreenRecordHotkey(const QString& hotkey);

    QString windowRecordHotkey() const;
    void setWindowRecordHotkey(const QString& hotkey);

    void sync();

signals:
    void settingsChanged();

private:
    SettingsManager();
    ~SettingsManager();
    Q_DISABLE_COPY(SettingsManager)

private:
    static SettingsManager* s_instance;
    QSettings* m_settings;
};

#endif
