#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QTimer>
#include <QSpinBox>
#include <QTabWidget>
#include "core/CaptureController.h"
#include "core/RecordingController.h"
#include "config/SettingsManager.h"
#include "ui/TrayIconManager.h"
#include "ui/WindowPicker.h"
#include "ui/RegionSelector.h"
#include "encoding/EncoderFactory.h"
#include "ui/HotkeyCaptureLineEdit.h"
#include "config/HotkeyManager.h"
#include "ui/ScreenshotOptionsDialog.h"

/**
 * @class MainWindow
 * @brief 主窗口类，提供录制和截屏功能的标签页界面
 *
 * 主窗口包含两个标签页：录制标签页和截屏标签页。
 * 录制标签页提供视频录制功能，包括全屏录制和窗口录制。
 * 截屏标签页提供全屏截屏和区域截屏功能。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父窗口指针
     */
    explicit MainWindow(QWidget *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~MainWindow();

private slots:
    void onStartRecording();
    void onStartRecordingFullscreen();
    void onStartRecordingWindow();
    void onStopRecording();
    void onWindowSelected(HWND hwnd);
    void updateRecordingStatus();
    void onRecordingDurationChanged(qint64 duration);
    void onRecordingError(const QString& error);

    void onResolutionChanged(int index);
    void onFrameRateChanged(int index);
    void onEncoderChanged(int index);
    void onVideoBitrateChanged(int value);
    void onHardwareAccelChanged(int index);
    void onSampleRateChanged(int index);
    void onChannelsChanged(int index);
    void onAudioBitrateChanged(int value);
    void onOpenSaveDirClicked();

    void onFullScreenCaptureClicked();
    void onRegionCaptureClicked();
    void onRegionSelected(const QRect& region);
    void onRegionCancelled();

    void onTrayScreenshotRequested(const QString& type);
    void onTrayRecordingRequested(const QString& type);

    void onApplyHotkeysClicked();
    void onBrowseScreenshotPath();
    void onBrowseRecordingPath();
    void onApplyStoragePaths();

private:
    void setupUi();
    void setupRecordingTab();
    void setupScreenshotTab();
    void setupSettingsTab();
    void setupConnections();
    void updateWindowList();
    QString getDefaultSavePath() const;
    QString getScreenshotSavePath() const;
    QString getRecordingSavePath() const;
    QString generateRecordingFilePath() const;
    void initSettingsValues();
    void populateResolutionComboBox();
    void populateHardwareAccelComboBox();
    void startRecordingHelper(HWND hwnd, bool fullScreen, const QString& errorContext);

private:
    QTabWidget* m_tabWidget;
    QWidget* m_recordingTab;
    QWidget* m_screenshotTab;
    QWidget* m_settingsTab;

    HotkeyCaptureLineEdit* m_editScreenshotHotkey;
    HotkeyCaptureLineEdit* m_editRegionCaptureHotkey;
    HotkeyCaptureLineEdit* m_editFullscreenRecordHotkey;
    HotkeyCaptureLineEdit* m_editWindowRecordHotkey;
    QPushButton* m_btnApplyHotkeys;

    QComboBox* m_comboMode;
    QComboBox* m_comboWindow;
    QPushButton* m_btnStartRecording;
    QPushButton* m_btnStopRecording;
    QLabel* m_labelStatus;
    QLabel* m_labelDuration;
    QLabel* m_labelFileSize;
    QTimer* m_statusTimer;

    QComboBox* m_comboResolution;
    QComboBox* m_comboFrameRate;
    QComboBox* m_comboEncoder;
    QSpinBox* m_spinVideoBitrate;
    QComboBox* m_comboHardwareAccel;
    QComboBox* m_comboSampleRate;
    QComboBox* m_comboChannels;
    QSpinBox* m_spinAudioBitrate;
    QPushButton* m_btnOpenSaveDir;

    QPushButton* m_btnFullScreenCapture;
    QPushButton* m_btnRegionCapture;
    QLabel* m_labelScreenshotStatus;

    QLineEdit* m_editScreenshotPath;
    QPushButton* m_btnBrowseScreenshotPath;
    QLineEdit* m_editRecordingPath;
    QPushButton* m_btnBrowseRecordingPath;
    QPushButton* m_btnApplyStoragePaths;

    CaptureController* m_captureController;
    RecordingController* m_recordingController;
    SettingsManager* m_settingsManager;
    TrayIconManager* m_trayIconManager;
    WindowPicker* m_windowPicker;
    RegionSelector* m_regionSelector;

    static const int FULLSCREEN_CAPTURE_HOTKEY_ID = 1;
    static const int REGION_CAPTURE_HOTKEY_ID = 2;
    static const int FULLSCREEN_RECORD_HOTKEY_ID = 3;
    static const int WINDOW_RECORD_HOTKEY_ID = 4;
    HotkeyManager* m_hotkeyManager;

    bool m_isRecording;

    QList<HardwareAccelType> m_availableHwAccel;
};

#endif