#include "SettingsManager.h"
#include <QCoreApplication>
#include <QStandardPaths>

SettingsManager* SettingsManager::s_instance = nullptr;

SettingsManager::SettingsManager()
    : QObject(nullptr)
    , m_settings(new QSettings(QSettings::IniFormat, QSettings::UserScope,
                               "Capture", "ScreenRecorder", this))
{
}

SettingsManager::~SettingsManager()
{
    sync();
}

SettingsManager* SettingsManager::instance()
{
    if (s_instance == nullptr) {
        s_instance = new SettingsManager();
    }
    return s_instance;
}

VideoSettings SettingsManager::videoSettings() const
{
    VideoSettings settings;
    settings.frameRate = m_settings->value("video/frameRate", 30).toInt();
    settings.width = m_settings->value("video/width", 1920).toInt();
    settings.height = m_settings->value("video/height", 1080).toInt();
    settings.encoder = m_settings->value("video/encoder", "H.264").toString();
    settings.bitrate = m_settings->value("video/bitrate", 8000000).toInt();
    int hwAccelInt = m_settings->value("video/hwAccel", static_cast<int>(HardwareAccelType::None)).toInt();
    settings.hwAccel = static_cast<HardwareAccelType>(hwAccelInt);
    return settings;
}

void SettingsManager::setVideoSettings(const VideoSettings& settings)
{
    m_settings->setValue("video/frameRate", settings.frameRate);
    m_settings->setValue("video/width", settings.width);
    m_settings->setValue("video/height", settings.height);
    m_settings->setValue("video/encoder", settings.encoder);
    m_settings->setValue("video/bitrate", settings.bitrate);
    m_settings->setValue("video/hwAccel", static_cast<int>(settings.hwAccel));
    emit settingsChanged();
}

AudioSettings SettingsManager::audioSettings() const
{
    AudioSettings settings;
    settings.sampleRate = m_settings->value("audio/sampleRate", 44100).toInt();
    settings.channels = m_settings->value("audio/channels", 2).toInt();
    settings.bitrate = m_settings->value("audio/bitrate", 192000).toInt();
    settings.systemAudioDevice = m_settings->value("audio/systemAudioDevice", "").toString();
    settings.microphoneDevice = m_settings->value("audio/microphoneDevice", "").toString();
    settings.enableSystemAudio = m_settings->value("audio/enableSystemAudio", true).toBool();
    settings.enableMicrophone = m_settings->value("audio/enableMicrophone", false).toBool();
    return settings;
}

void SettingsManager::setAudioSettings(const AudioSettings& settings)
{
    m_settings->setValue("audio/sampleRate", settings.sampleRate);
    m_settings->setValue("audio/channels", settings.channels);
    m_settings->setValue("audio/bitrate", settings.bitrate);
    m_settings->setValue("audio/systemAudioDevice", settings.systemAudioDevice);
    m_settings->setValue("audio/microphoneDevice", settings.microphoneDevice);
    m_settings->setValue("audio/enableSystemAudio", settings.enableSystemAudio);
    m_settings->setValue("audio/enableMicrophone", settings.enableMicrophone);
    emit settingsChanged();
}

GeneralSettings SettingsManager::generalSettings() const
{
    GeneralSettings settings;
    QString defaultSavePath = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                   .filePath("Capture");
    settings.savePath = m_settings->value("general/savePath", defaultSavePath).toString();
    settings.screenshotFormat = m_settings->value("general/screenshotFormat", "PNG").toString();
    settings.screenshotPath = m_settings->value("general/screenshotPath", defaultSavePath).toString();
    return settings;
}

void SettingsManager::setGeneralSettings(const GeneralSettings& settings)
{
    m_settings->setValue("general/savePath", settings.savePath);
    m_settings->setValue("general/screenshotFormat", settings.screenshotFormat);
    m_settings->setValue("general/screenshotPath", settings.screenshotPath);
    emit settingsChanged();
}

LogSettings SettingsManager::logSettings() const
{
    LogSettings settings;
    settings.enabled = m_settings->value("logging/enabled", true).toBool();
    return settings;
}

void SettingsManager::setLogSettings(const LogSettings& settings)
{
    m_settings->setValue("logging/enabled", settings.enabled);
    emit settingsChanged();
}

QString SettingsManager::screenshotHotkey() const
{
    return m_settings->value("hotkeys/screenshot", "Ctrl+Alt+S").toString();
}

void SettingsManager::setScreenshotHotkey(const QString& hotkey)
{
    m_settings->setValue("hotkeys/screenshot", hotkey);
    emit settingsChanged();
}

QString SettingsManager::regionCaptureHotkey() const
{
    return m_settings->value("hotkeys/regionCapture", "Ctrl+Alt+R").toString();
}

void SettingsManager::setRegionCaptureHotkey(const QString& hotkey)
{
    m_settings->setValue("hotkeys/regionCapture", hotkey);
    emit settingsChanged();
}

QString SettingsManager::fullscreenRecordHotkey() const
{
    return m_settings->value("hotkeys/fullscreenRecord", "Ctrl+Alt+F").toString();
}

void SettingsManager::setFullscreenRecordHotkey(const QString& hotkey)
{
    m_settings->setValue("hotkeys/fullscreenRecord", hotkey);
    emit settingsChanged();
}

QString SettingsManager::windowRecordHotkey() const
{
    return m_settings->value("hotkeys/windowRecord", "Ctrl+Alt+W").toString();
}

void SettingsManager::setWindowRecordHotkey(const QString& hotkey)
{
    m_settings->setValue("hotkeys/windowRecord", hotkey);
    emit settingsChanged();
}

void SettingsManager::sync()
{
    m_settings->sync();
}
