#include "MainWindow.h"
#include "utils/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDateTime>
#include <QTime>
#include <QFileInfo>
#include <QSpacerItem>
#include <QFileDialog>
#include <QApplication>
#include <QScreen>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_tabWidget(nullptr)
    , m_recordingTab(nullptr)
    , m_screenshotTab(nullptr)
    , m_comboMode(nullptr)
    , m_comboWindow(nullptr)
    , m_btnStartRecording(nullptr)
    , m_btnStopRecording(nullptr)
    , m_labelStatus(nullptr)
    , m_labelDuration(nullptr)
    , m_labelFileSize(nullptr)
    , m_statusTimer(nullptr)
    , m_comboResolution(nullptr)
    , m_comboFrameRate(nullptr)
    , m_comboEncoder(nullptr)
    , m_spinVideoBitrate(nullptr)
    , m_comboHardwareAccel(nullptr)
    , m_comboSampleRate(nullptr)
    , m_comboChannels(nullptr)
    , m_spinAudioBitrate(nullptr)
    , m_btnOpenSaveDir(nullptr)
    , m_btnFullScreenCapture(nullptr)
    , m_btnRegionCapture(nullptr)
    , m_labelScreenshotStatus(nullptr)
    , m_captureController(nullptr)
    , m_recordingController(nullptr)
    , m_settingsManager(nullptr)
    , m_trayIconManager(nullptr)
    , m_windowPicker(nullptr)
    , m_regionSelector(nullptr)
    , m_isRecording(false)
{
    m_availableHwAccel = EncoderFactory::detectAvailableHardwareAccel();

    m_captureController = new CaptureController(this);
    m_recordingController = new RecordingController(this);
    m_settingsManager = SettingsManager::instance();
    m_trayIconManager = new TrayIconManager(this);
    m_windowPicker = new WindowPicker(this);
    m_regionSelector = new RegionSelector(this);
    m_hotkeyManager = new HotkeyManager(this);

    setupUi();
    setupConnections();

    initSettingsValues();
    updateWindowList();
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUi()
{
    setWindowTitle(QString::fromUtf8("屏幕录制工具"));
    setMinimumSize(700, 500);

    m_tabWidget = new QTabWidget(this);
    setCentralWidget(m_tabWidget);

    m_recordingTab = new QWidget();
    m_tabWidget->addTab(m_recordingTab, QString::fromUtf8("录制"));

    m_screenshotTab = new QWidget();
    m_tabWidget->addTab(m_screenshotTab, QString::fromUtf8("截屏"));

    setupRecordingTab();
    setupScreenshotTab();
    setupSettingsTab();

    m_statusTimer = new QTimer(this);
}

void MainWindow::setupRecordingTab()
{
    QHBoxLayout* recordingMainLayout = new QHBoxLayout(m_recordingTab);

    QVBoxLayout* leftLayout = new QVBoxLayout();

    QGroupBox* modeGroup = new QGroupBox(QString::fromUtf8("录制模式"), m_recordingTab);
    QVBoxLayout* modeLayout = new QVBoxLayout(modeGroup);

    QHBoxLayout* modeSelectLayout = new QHBoxLayout();
    QLabel* labelMode = new QLabel(QString::fromUtf8("模式:"), modeGroup);
    modeSelectLayout->addWidget(labelMode);

    m_comboMode = new QComboBox(modeGroup);
    m_comboMode->addItem(QString::fromUtf8("全屏模式"), QVariant(0));
    m_comboMode->addItem(QString::fromUtf8("窗口模式"), QVariant(1));
    modeSelectLayout->addWidget(m_comboMode, 1);
    modeLayout->addLayout(modeSelectLayout);

    leftLayout->addWidget(modeGroup);

    QGroupBox* windowGroup = new QGroupBox(QString::fromUtf8("选择窗口"), m_recordingTab);
    QVBoxLayout* windowLayout = new QVBoxLayout(windowGroup);

    m_comboWindow = new QComboBox(windowGroup);
    m_comboWindow->setEnabled(false);
    windowLayout->addWidget(m_comboWindow);

    QPushButton* btnRefresh = new QPushButton(QString::fromUtf8("刷新窗口列表"), windowGroup);
    windowLayout->addWidget(btnRefresh);
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::updateWindowList);

    leftLayout->addWidget(windowGroup);

    QGroupBox* statusGroup = new QGroupBox(QString::fromUtf8("录制状态"), m_recordingTab);
    QVBoxLayout* statusLayout = new QVBoxLayout(statusGroup);

    m_labelStatus = new QLabel(QString::fromUtf8("状态: 未开始"), statusGroup);
    statusLayout->addWidget(m_labelStatus);

    m_labelDuration = new QLabel(QString::fromUtf8("时长: 00:00:00"), statusGroup);
    statusLayout->addWidget(m_labelDuration);

    m_labelFileSize = new QLabel(QString::fromUtf8("文件大小: 0 KB"), statusGroup);
    statusLayout->addWidget(m_labelFileSize);

    leftLayout->addWidget(statusGroup);

    leftLayout->addStretch();

    QHBoxLayout* recordButtonLayout = new QHBoxLayout();

    m_btnStartRecording = new QPushButton(QString::fromUtf8("开始录制"), m_recordingTab);
    m_btnStartRecording->setMinimumHeight(40);
    recordButtonLayout->addWidget(m_btnStartRecording);

    m_btnStopRecording = new QPushButton(QString::fromUtf8("停止录制"), m_recordingTab);
    m_btnStopRecording->setMinimumHeight(40);
    m_btnStopRecording->setEnabled(false);
    recordButtonLayout->addWidget(m_btnStopRecording);

    leftLayout->addLayout(recordButtonLayout);

    recordingMainLayout->addLayout(leftLayout, 1);

    QVBoxLayout* rightLayout = new QVBoxLayout();

    QGroupBox* videoGroup = new QGroupBox(QString::fromUtf8("视频参数"), m_recordingTab);
    QGridLayout* videoLayout = new QGridLayout(videoGroup);

    QLabel* labelResolution = new QLabel(QString::fromUtf8("分辨率:"), videoGroup);
    videoLayout->addWidget(labelResolution, 0, 0);

    m_comboResolution = new QComboBox(videoGroup);
    populateResolutionComboBox();
    videoLayout->addWidget(m_comboResolution, 0, 1);

    QLabel* labelFrameRate = new QLabel(QString::fromUtf8("帧率:"), videoGroup);
    videoLayout->addWidget(labelFrameRate, 1, 0);

    m_comboFrameRate = new QComboBox(videoGroup);
    m_comboFrameRate->addItem(QString::fromUtf8("15 fps"), 15);
    m_comboFrameRate->addItem(QString::fromUtf8("30 fps"), 30);
    m_comboFrameRate->addItem(QString::fromUtf8("60 fps"), 60);
    videoLayout->addWidget(m_comboFrameRate, 1, 1);

    QLabel* labelEncoder = new QLabel(QString::fromUtf8("编码器:"), videoGroup);
    videoLayout->addWidget(labelEncoder, 2, 0);

    m_comboEncoder = new QComboBox(videoGroup);
    m_comboEncoder->addItem(QString::fromUtf8("H.264"), QString::fromUtf8("H.264"));
    m_comboEncoder->addItem(QString::fromUtf8("H.265"), QString::fromUtf8("H.265"));
    videoLayout->addWidget(m_comboEncoder, 2, 1);

    QLabel* labelVideoBitrate = new QLabel(QString::fromUtf8("比特率:"), videoGroup);
    videoLayout->addWidget(labelVideoBitrate, 3, 0);

    m_spinVideoBitrate = new QSpinBox(videoGroup);
    m_spinVideoBitrate->setRange(1000, 50000);
    m_spinVideoBitrate->setValue(8000);
    m_spinVideoBitrate->setSuffix(QString::fromUtf8(" kbps"));
    videoLayout->addWidget(m_spinVideoBitrate, 3, 1);

    QLabel* labelHwAccel = new QLabel(QString::fromUtf8("硬件加速:"), videoGroup);
    videoLayout->addWidget(labelHwAccel, 4, 0);

    m_comboHardwareAccel = new QComboBox(videoGroup);
    populateHardwareAccelComboBox();
    videoLayout->addWidget(m_comboHardwareAccel, 4, 1);

    rightLayout->addWidget(videoGroup);

    QGroupBox* audioGroup = new QGroupBox(QString::fromUtf8("音频参数"), m_recordingTab);
    QGridLayout* audioLayout = new QGridLayout(audioGroup);

    QLabel* labelSampleRate = new QLabel(QString::fromUtf8("采样率:"), audioGroup);
    audioLayout->addWidget(labelSampleRate, 0, 0);

    m_comboSampleRate = new QComboBox(audioGroup);
    m_comboSampleRate->addItem(QString::fromUtf8("44100 Hz"), 44100);
    m_comboSampleRate->addItem(QString::fromUtf8("48000 Hz"), 48000);
    audioLayout->addWidget(m_comboSampleRate, 0, 1);

    QLabel* labelChannels = new QLabel(QString::fromUtf8("声道数:"), audioGroup);
    audioLayout->addWidget(labelChannels, 1, 0);

    m_comboChannels = new QComboBox(audioGroup);
    m_comboChannels->addItem(QString::fromUtf8("单声道"), 1);
    m_comboChannels->addItem(QString::fromUtf8("立体声"), 2);
    audioLayout->addWidget(m_comboChannels, 1, 1);

    QLabel* labelAudioBitrate = new QLabel(QString::fromUtf8("比特率:"), audioGroup);
    audioLayout->addWidget(labelAudioBitrate, 2, 0);

    m_spinAudioBitrate = new QSpinBox(audioGroup);
    m_spinAudioBitrate->setRange(64, 320);
    m_spinAudioBitrate->setValue(192);
    m_spinAudioBitrate->setSuffix(QString::fromUtf8(" kbps"));
    audioLayout->addWidget(m_spinAudioBitrate, 2, 1);

    rightLayout->addWidget(audioGroup);

    rightLayout->addStretch();

    m_btnOpenSaveDir = new QPushButton(QString::fromUtf8("打开保存目录"), m_recordingTab);
    m_btnOpenSaveDir->setMinimumHeight(35);
    rightLayout->addWidget(m_btnOpenSaveDir);

    recordingMainLayout->addLayout(rightLayout, 1);
}

void MainWindow::setupScreenshotTab()
{
    QVBoxLayout* screenshotMainLayout = new QVBoxLayout(m_screenshotTab);
    screenshotMainLayout->addStretch();

    m_btnFullScreenCapture = new QPushButton(QString::fromUtf8("全屏截屏"), m_screenshotTab);
    m_btnFullScreenCapture->setMinimumHeight(60);
    m_btnFullScreenCapture->setMinimumWidth(200);
    screenshotMainLayout->addWidget(m_btnFullScreenCapture, 0, Qt::AlignCenter);

    screenshotMainLayout->addSpacing(20);

    m_btnRegionCapture = new QPushButton(QString::fromUtf8("区域截屏"), m_screenshotTab);
    m_btnRegionCapture->setMinimumHeight(60);
    m_btnRegionCapture->setMinimumWidth(200);
    screenshotMainLayout->addWidget(m_btnRegionCapture, 0, Qt::AlignCenter);

    screenshotMainLayout->addSpacing(20);

    m_labelScreenshotStatus = new QLabel(QString::fromUtf8("点击按钮开始截屏"), m_screenshotTab);
    m_labelScreenshotStatus->setAlignment(Qt::AlignCenter);
    screenshotMainLayout->addWidget(m_labelScreenshotStatus, 0, Qt::AlignCenter);

    screenshotMainLayout->addStretch();
}

void MainWindow::setupSettingsTab()
{
    m_settingsTab = new QWidget();
    m_tabWidget->addTab(m_settingsTab, QString::fromUtf8("设置"));

    QVBoxLayout* settingsMainLayout = new QVBoxLayout(m_settingsTab);

    QGroupBox* hotkeyGroup = new QGroupBox(QString::fromUtf8("热键设置"), m_settingsTab);
    QGridLayout* hotkeyLayout = new QGridLayout(hotkeyGroup);

    QLabel* labelScreenshotHotkey = new QLabel(QString::fromUtf8("全屏截屏热键:"), hotkeyGroup);
    hotkeyLayout->addWidget(labelScreenshotHotkey, 0, 0);

    m_editScreenshotHotkey = new HotkeyCaptureLineEdit(hotkeyGroup);
    m_editScreenshotHotkey->setMinimumWidth(200);
    hotkeyLayout->addWidget(m_editScreenshotHotkey, 0, 1, 1, 2);

    QLabel* labelRegionCaptureHotkey = new QLabel(QString::fromUtf8("区域截屏热键:"), hotkeyGroup);
    hotkeyLayout->addWidget(labelRegionCaptureHotkey, 1, 0);

    m_editRegionCaptureHotkey = new HotkeyCaptureLineEdit(hotkeyGroup);
    m_editRegionCaptureHotkey->setMinimumWidth(200);
    hotkeyLayout->addWidget(m_editRegionCaptureHotkey, 1, 1, 1, 2);

    QLabel* labelFullscreenRecordHotkey = new QLabel(QString::fromUtf8("全屏录屏热键:"), hotkeyGroup);
    hotkeyLayout->addWidget(labelFullscreenRecordHotkey, 2, 0);

    m_editFullscreenRecordHotkey = new HotkeyCaptureLineEdit(hotkeyGroup);
    m_editFullscreenRecordHotkey->setMinimumWidth(200);
    hotkeyLayout->addWidget(m_editFullscreenRecordHotkey, 2, 1, 1, 2);

    QLabel* labelWindowRecordHotkey = new QLabel(QString::fromUtf8("窗口录屏热键:"), hotkeyGroup);
    hotkeyLayout->addWidget(labelWindowRecordHotkey, 3, 0);

    m_editWindowRecordHotkey = new HotkeyCaptureLineEdit(hotkeyGroup);
    m_editWindowRecordHotkey->setMinimumWidth(200);
    hotkeyLayout->addWidget(m_editWindowRecordHotkey, 3, 1, 1, 2);

    QLabel* labelHotkeyHelp = new QLabel(QString::fromUtf8("提示: 点击输入框后按下快捷键组合（如 Ctrl+Alt+S）"), hotkeyGroup);
    labelHotkeyHelp->setStyleSheet(QString::fromUtf8("color: gray;"));
    hotkeyLayout->addWidget(labelHotkeyHelp, 4, 0, 1, 3);

    m_btnApplyHotkeys = new QPushButton(QString::fromUtf8("应用"), hotkeyGroup);
    m_btnApplyHotkeys->setMinimumWidth(100);
    hotkeyLayout->addWidget(m_btnApplyHotkeys, 5, 2, 1, 1, Qt::AlignRight);

    settingsMainLayout->addWidget(hotkeyGroup);

    QGroupBox* storageGroup = new QGroupBox(QString::fromUtf8("存储路径设置"), m_settingsTab);
    QVBoxLayout* storageLayout = new QVBoxLayout(storageGroup);

    QWidget* screenshotPathRow = new QWidget(storageGroup);
    QHBoxLayout* screenshotPathLayout = new QHBoxLayout(screenshotPathRow);
    screenshotPathLayout->setContentsMargins(0, 0, 0, 0);

    QLabel* labelScreenshotPath = new QLabel(QString::fromUtf8("图片保存路径:"), screenshotPathRow);
    screenshotPathLayout->addWidget(labelScreenshotPath);

    m_editScreenshotPath = new QLineEdit(screenshotPathRow);
    m_editScreenshotPath->setPlaceholderText(QString::fromUtf8("默认: 文档/Capture"));
    screenshotPathLayout->addWidget(m_editScreenshotPath);

    m_btnBrowseScreenshotPath = new QPushButton(QString::fromUtf8("浏览..."), screenshotPathRow);
    screenshotPathLayout->addWidget(m_btnBrowseScreenshotPath);

    QWidget* recordingPathRow = new QWidget(storageGroup);
    QHBoxLayout* recordingPathLayout = new QHBoxLayout(recordingPathRow);
    recordingPathLayout->setContentsMargins(0, 0, 0, 0);

    QLabel* labelRecordingPath = new QLabel(QString::fromUtf8("视频保存路径:"), recordingPathRow);
    recordingPathLayout->addWidget(labelRecordingPath);

    m_editRecordingPath = new QLineEdit(recordingPathRow);
    m_editRecordingPath->setPlaceholderText(QString::fromUtf8("默认: 文档/Capture"));
    recordingPathLayout->addWidget(m_editRecordingPath);

    m_btnBrowseRecordingPath = new QPushButton(QString::fromUtf8("浏览..."), recordingPathRow);
    recordingPathLayout->addWidget(m_btnBrowseRecordingPath);

    storageLayout->addWidget(screenshotPathRow);
    storageLayout->addWidget(recordingPathRow);

    m_btnApplyStoragePaths = new QPushButton(QString::fromUtf8("应用"), storageGroup);
    m_btnApplyStoragePaths->setMinimumWidth(100);
    m_btnApplyStoragePaths->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    storageLayout->addWidget(m_btnApplyStoragePaths, 0, Qt::AlignRight);

    settingsMainLayout->addWidget(storageGroup);
    settingsMainLayout->addStretch();
}

void MainWindow::populateResolutionComboBox()
{
    m_comboResolution->addItem(QString::fromUtf8("4K (3840×2160)"), QSize(3840, 2160));
    m_comboResolution->addItem(QString::fromUtf8("2K (2560×1440)"), QSize(2560, 1440));
    m_comboResolution->addItem(QString::fromUtf8("1080p (1920×1080)"), QSize(1920, 1080));
    m_comboResolution->addItem(QString::fromUtf8("720p (1280×720)"), QSize(1280, 720));
    m_comboResolution->addItem(QString::fromUtf8("480p (854×480)"), QSize(854, 480));

    QScreen* primaryScreen = QApplication::primaryScreen();
    if (primaryScreen) {
        QRect screenGeometry = primaryScreen->geometry();
        QSize screenSize = screenGeometry.size();
        QString screenText = QString::fromUtf8("屏幕分辨率 (%1×%2)").arg(screenSize.width()).arg(screenSize.height());
        m_comboResolution->insertItem(0, screenText, screenSize);
    }
}

void MainWindow::populateHardwareAccelComboBox()
{
    m_comboHardwareAccel->clear();

    for (HardwareAccelType type : m_availableHwAccel) {
        m_comboHardwareAccel->addItem(EncoderFactory::hardwareAccelName(type), static_cast<int>(type));
    }
}

void MainWindow::setupConnections()
{
    connect(m_btnStartRecording, &QPushButton::clicked, this, &MainWindow::onStartRecording);
    connect(m_btnStopRecording, &QPushButton::clicked, this, &MainWindow::onStopRecording);

    connect(m_comboMode, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
        m_comboWindow->setEnabled(index == 1);
    });

    connect(m_windowPicker, &WindowPicker::windowSelected, this, &MainWindow::onWindowSelected);

    connect(m_recordingController, &RecordingController::durationChanged, this, &MainWindow::onRecordingDurationChanged);
    connect(m_recordingController, &RecordingController::recordingError, this, &MainWindow::onRecordingError);
    connect(m_recordingController, &RecordingController::recordingStarted, [this]() {
        m_labelStatus->setText(QString::fromUtf8("状态: 录制中..."));
    });
    connect(m_recordingController, &RecordingController::recordingStopped, [this]() {
        m_labelStatus->setText(QString::fromUtf8("状态: 录制已停止"));
    });

    connect(m_comboResolution, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onResolutionChanged);
    connect(m_comboFrameRate, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onFrameRateChanged);
    connect(m_comboEncoder, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onEncoderChanged);
    connect(m_spinVideoBitrate, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onVideoBitrateChanged);
    connect(m_comboHardwareAccel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onHardwareAccelChanged);
    connect(m_comboSampleRate, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSampleRateChanged);
    connect(m_comboChannels, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onChannelsChanged);
    connect(m_spinAudioBitrate, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onAudioBitrateChanged);
    connect(m_btnOpenSaveDir, &QPushButton::clicked, this, &MainWindow::onOpenSaveDirClicked);

    connect(m_btnFullScreenCapture, &QPushButton::clicked, this, &MainWindow::onFullScreenCaptureClicked);
    connect(m_btnRegionCapture, &QPushButton::clicked, this, &MainWindow::onRegionCaptureClicked);
    connect(m_regionSelector, &RegionSelector::regionSelected, this, &MainWindow::onRegionSelected);
    connect(m_regionSelector, &RegionSelector::selectionCancelled, this, &MainWindow::onRegionCancelled);

    connect(m_btnApplyHotkeys, &QPushButton::clicked, this, &MainWindow::onApplyHotkeysClicked);

    connect(m_btnBrowseScreenshotPath, &QPushButton::clicked, this, &MainWindow::onBrowseScreenshotPath);
    connect(m_btnBrowseRecordingPath, &QPushButton::clicked, this, &MainWindow::onBrowseRecordingPath);
    connect(m_btnApplyStoragePaths, &QPushButton::clicked, this, &MainWindow::onApplyStoragePaths);
}

void MainWindow::updateWindowList()
{
    m_comboWindow->clear();

    QList<WindowInfo> windows = m_windowPicker->enumWindows();

    for (const WindowInfo& info : windows)
    {
        QString windowTitle = info.title;
        if (windowTitle.isEmpty())
        {
            windowTitle = QString::fromUtf8("未命名窗口");
        }
        m_comboWindow->addItem(windowTitle, QVariant::fromValue(info.hwnd));
    }
}

QString MainWindow::getDefaultSavePath() const
{
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString capturePath = documentsPath + QString::fromUtf8("/Capture");

    QDir dir(capturePath);
    if (!dir.exists())
    {
        dir.mkpath(capturePath);
    }

    return capturePath;
}

QString MainWindow::getScreenshotSavePath() const
{
    QString customPath = m_settingsManager->generalSettings().screenshotPath;
    if (!customPath.isEmpty()) {
        QDir dir(customPath);
        if (dir.exists() || dir.mkpath(customPath)) {
            return customPath;
        }
    }
    return getDefaultSavePath();
}

QString MainWindow::getRecordingSavePath() const
{
    QString customPath = m_settingsManager->generalSettings().savePath;
    if (!customPath.isEmpty()) {
        QDir dir(customPath);
        if (dir.exists() || dir.mkpath(customPath)) {
            return customPath;
        }
    }
    return getDefaultSavePath();
}

void MainWindow::onBrowseScreenshotPath()
{
    QString dirPath = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择图片保存路径"),
        m_editScreenshotPath->text().isEmpty() ? getDefaultSavePath() : m_editScreenshotPath->text());

    if (!dirPath.isEmpty()) {
        m_editScreenshotPath->setText(dirPath);
    }
}

void MainWindow::onBrowseRecordingPath()
{
    QString dirPath = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("选择视频保存路径"),
        m_editRecordingPath->text().isEmpty() ? getDefaultSavePath() : m_editRecordingPath->text());

    if (!dirPath.isEmpty()) {
        m_editRecordingPath->setText(dirPath);
    }
}

void MainWindow::onApplyStoragePaths()
{
    GeneralSettings settings = m_settingsManager->generalSettings();
    settings.screenshotPath = m_editScreenshotPath->text();
    settings.savePath = m_editRecordingPath->text();
    m_settingsManager->setGeneralSettings(settings);
}

void MainWindow::initSettingsValues()
{
    VideoSettings videoSettings = m_settingsManager->videoSettings();
    AudioSettings audioSettings = m_settingsManager->audioSettings();

    for (int i = 0; i < m_comboResolution->count(); ++i) {
        QSize res = m_comboResolution->itemData(i).toSize();
        if (res.width() == videoSettings.width && res.height() == videoSettings.height) {
            m_comboResolution->setCurrentIndex(i);
            break;
        }
    }

    int frameRateIndex = m_comboFrameRate->findData(videoSettings.frameRate);
    if (frameRateIndex >= 0)
    {
        m_comboFrameRate->setCurrentIndex(frameRateIndex);
    }

    int encoderIndex = m_comboEncoder->findData(videoSettings.encoder);
    if (encoderIndex >= 0)
    {
        m_comboEncoder->setCurrentIndex(encoderIndex);
    }

    m_spinVideoBitrate->setValue(videoSettings.bitrate / 1000);

    for (int i = 0; i < m_comboHardwareAccel->count(); ++i) {
        HardwareAccelType type = static_cast<HardwareAccelType>(m_comboHardwareAccel->itemData(i).toInt());
        if (type == videoSettings.hwAccel) {
            m_comboHardwareAccel->setCurrentIndex(i);
            break;
        }
    }

    int sampleRateIndex = m_comboSampleRate->findData(audioSettings.sampleRate);
    if (sampleRateIndex >= 0)
    {
        m_comboSampleRate->setCurrentIndex(sampleRateIndex);
    }

    int channelsIndex = m_comboChannels->findData(audioSettings.channels);
    if (channelsIndex >= 0)
    {
        m_comboChannels->setCurrentIndex(channelsIndex);
    }

    m_spinAudioBitrate->setValue(audioSettings.bitrate / 1000);

    m_editScreenshotHotkey->setText(m_settingsManager->screenshotHotkey());
    m_editRegionCaptureHotkey->setText(m_settingsManager->regionCaptureHotkey());
    m_editFullscreenRecordHotkey->setText(m_settingsManager->fullscreenRecordHotkey());
    m_editWindowRecordHotkey->setText(m_settingsManager->windowRecordHotkey());

    GeneralSettings generalSettings = m_settingsManager->generalSettings();
    m_editScreenshotPath->setText(generalSettings.screenshotPath);
    m_editRecordingPath->setText(generalSettings.savePath);

    m_hotkeyManager->registerHotkey(FULLSCREEN_CAPTURE_HOTKEY_ID, m_settingsManager->screenshotHotkey());
    m_hotkeyManager->registerHotkey(REGION_CAPTURE_HOTKEY_ID, m_settingsManager->regionCaptureHotkey());
    m_hotkeyManager->registerHotkey(FULLSCREEN_RECORD_HOTKEY_ID, m_settingsManager->fullscreenRecordHotkey());
    m_hotkeyManager->registerHotkey(WINDOW_RECORD_HOTKEY_ID, m_settingsManager->windowRecordHotkey());

    connect(m_hotkeyManager, &HotkeyManager::hotkeyPressed, [this](int id) {
        if (id == FULLSCREEN_CAPTURE_HOTKEY_ID) {
            onFullScreenCaptureClicked();
        } else if (id == REGION_CAPTURE_HOTKEY_ID) {
            m_regionSelector->startSelection();
        } else if (id == FULLSCREEN_RECORD_HOTKEY_ID) {
            if (m_isRecording) {
                onStopRecording();
            } else {
                onStartRecordingFullscreen();
            }
        } else if (id == WINDOW_RECORD_HOTKEY_ID) {
            if (m_isRecording) {
                onStopRecording();
            } else {
                onStartRecordingWindow();
            }
        }
    });
}

void MainWindow::onStartRecording()
{
    int modeIndex = m_comboMode->currentIndex();

    HWND targetHwnd = nullptr;
    bool fullScreen = true;

    if (modeIndex == 1)
    {
        targetHwnd = m_comboWindow->currentData().value<HWND>();
        if (!targetHwnd)
        {
            QMessageBox::warning(this, QString::fromUtf8("警告"), QString::fromUtf8("请选择一个窗口进行录制"));
            return;
        }
        fullScreen = false;
    }

    startRecordingHelper(targetHwnd, fullScreen, QString::fromUtf8("录制"));
}

void MainWindow::onStartRecordingFullscreen()
{
    startRecordingHelper(nullptr, true, QString::fromUtf8("全屏录制"));
}

void MainWindow::onStartRecordingWindow()
{
    HWND targetHwnd = GetForegroundWindow();
    if (!targetHwnd)
    {
        QMessageBox::warning(this, QString::fromUtf8("警告"), QString::fromUtf8("无法获取前台窗口"));
        return;
    }
    startRecordingHelper(targetHwnd, false, QString::fromUtf8("窗口录制"));
}

QString MainWindow::generateRecordingFilePath() const
{
    QString savePath = getRecordingSavePath();
    QDateTime dateTime = QDateTime::currentDateTime();
    QString fileName = QString::fromUtf8("Recording_%1.mp4").arg(dateTime.toString(QString::fromUtf8("yyyyMMdd_HHmmss")));
    return savePath + QString::fromUtf8("/") + fileName;
}

void MainWindow::startRecordingHelper(HWND hwnd, bool fullScreen, const QString& errorContext)
{
    QString fullPath = generateRecordingFilePath();

    Logger::instance()->info("MainWindow", QString("Starting recording: fullScreen=%1, path=%2").arg(fullScreen).arg(fullPath));

    if (m_recordingController->startRecording(hwnd, fullScreen, fullPath))
    {
        m_isRecording = true;
        m_btnStartRecording->setEnabled(false);
        m_btnStopRecording->setEnabled(true);
        m_statusTimer->start(1000);
    }
    else
    {
        Logger::instance()->error("MainWindow", QString("Failed to start recording: %1").arg(errorContext));
        QMessageBox::critical(this, QString::fromUtf8("错误"),
            QString::fromUtf8("无法开始%1").arg(errorContext));
    }
}

void MainWindow::onStopRecording()
{
    Logger::instance()->info("MainWindow", "Stopping recording");
    m_recordingController->stopRecording();
    m_isRecording = false;
    m_btnStartRecording->setEnabled(true);
    m_btnStopRecording->setEnabled(false);
    m_statusTimer->stop();
    m_labelDuration->setText(QString::fromUtf8("时长: 00:00:00"));
}

void MainWindow::onWindowSelected(HWND hwnd)
{
    Q_UNUSED(hwnd);
}

void MainWindow::updateRecordingStatus()
{
    if (!m_isRecording)
    {
        return;
    }

    qint64 duration = m_recordingController->recordingDuration();
    QTime time(0, 0, 0);
    time = time.addMSecs(duration);
    QString durationStr = time.toString(QString::fromUtf8("hh:mm:ss"));
    m_labelDuration->setText(QString::fromUtf8("时长: %1").arg(durationStr));

    QString filePath = m_recordingController->currentFilePath();
    QFileInfo fileInfo(filePath);
    if (fileInfo.exists())
    {
        qint64 sizeBytes = fileInfo.size();
        QString sizeStr;
        if (sizeBytes < 1024)
        {
            sizeStr = QString::number(sizeBytes) + QString::fromUtf8(" B");
        }
        else if (sizeBytes < 1024 * 1024)
        {
            sizeStr = QString::number(sizeBytes / 1024.0, 'f', 2) + QString::fromUtf8(" KB");
        }
        else
        {
            sizeStr = QString::number(sizeBytes / (1024.0 * 1024.0), 'f', 2) + QString::fromUtf8(" MB");
        }
        m_labelFileSize->setText(QString::fromUtf8("文件大小: %1").arg(sizeStr));
    }
}

void MainWindow::onRecordingDurationChanged(qint64 duration)
{
    QTime time(0, 0, 0);
    time = time.addMSecs(duration);
    QString durationStr = time.toString(QString::fromUtf8("hh:mm:ss"));
    m_labelDuration->setText(QString::fromUtf8("时长: %1").arg(durationStr));
}

void MainWindow::onRecordingError(const QString& error)
{
    QMessageBox::critical(this, QString::fromUtf8("录制错误"), error);
    onStopRecording();
}

void MainWindow::onResolutionChanged(int index)
{
    QSize resolution = m_comboResolution->itemData(index).toSize();
    VideoSettings settings = m_settingsManager->videoSettings();
    settings.width = resolution.width();
    settings.height = resolution.height();
    m_settingsManager->setVideoSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onFrameRateChanged(int index)
{
    int frameRate = m_comboFrameRate->itemData(index).toInt();
    VideoSettings settings = m_settingsManager->videoSettings();
    settings.frameRate = frameRate;
    m_settingsManager->setVideoSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onEncoderChanged(int index)
{
    QString encoder = m_comboEncoder->itemData(index).toString();
    VideoSettings settings = m_settingsManager->videoSettings();
    settings.encoder = encoder;
    m_settingsManager->setVideoSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onVideoBitrateChanged(int value)
{
    VideoSettings settings = m_settingsManager->videoSettings();
    settings.bitrate = value * 1000;
    m_settingsManager->setVideoSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onHardwareAccelChanged(int index)
{
    HardwareAccelType hwAccel = static_cast<HardwareAccelType>(m_comboHardwareAccel->itemData(index).toInt());
    VideoSettings settings = m_settingsManager->videoSettings();
    settings.hwAccel = hwAccel;
    m_settingsManager->setVideoSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onSampleRateChanged(int index)
{
    int sampleRate = m_comboSampleRate->itemData(index).toInt();
    AudioSettings settings = m_settingsManager->audioSettings();
    settings.sampleRate = sampleRate;
    m_settingsManager->setAudioSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onChannelsChanged(int index)
{
    int channels = m_comboChannels->itemData(index).toInt();
    AudioSettings settings = m_settingsManager->audioSettings();
    settings.channels = channels;
    m_settingsManager->setAudioSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onAudioBitrateChanged(int value)
{
    AudioSettings settings = m_settingsManager->audioSettings();
    settings.bitrate = value * 1000;
    m_settingsManager->setAudioSettings(settings);
    m_settingsManager->sync();
}

void MainWindow::onOpenSaveDirClicked()
{
    QString savePath = getDefaultSavePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
}

void MainWindow::onFullScreenCaptureClicked()
{
    Logger::instance()->info("MainWindow", "Fullscreen capture started");
    m_labelScreenshotStatus->setText(QString::fromUtf8("正在截取全屏..."));

    QImage image = m_captureController->captureFullScreenOnly();

    if (image.isNull())
    {
        Logger::instance()->error("MainWindow", "Fullscreen capture failed");
        m_labelScreenshotStatus->setText(QString::fromUtf8("截屏失败"));
        QMessageBox::critical(this, QString::fromUtf8("错误"), QString::fromUtf8("截屏失败"));
        return;
    }

    this->showNormal();
    this->activateWindow();

    ScreenshotOptionsDialog dialog(image, this);
    dialog.exec();

    QString savePath = getScreenshotSavePath();
    QDateTime dateTime = QDateTime::currentDateTime();
    QString fileName = QString::fromUtf8("Screenshot_%1.png").arg(dateTime.toString(QString::fromUtf8("yyyyMMdd_HHmmss")));
    QString fullPath = savePath + QString::fromUtf8("/") + fileName;

    switch (dialog.getSelectedAction())
    {
    case ScreenshotOptionsDialog::Copy:
        m_labelScreenshotStatus->setText(QString::fromUtf8("已复制到剪贴板"));
        break;
    case ScreenshotOptionsDialog::Save:
        if (m_captureController->saveImage(image, fullPath))
        {
            Logger::instance()->info("MainWindow", QString("Fullscreen capture saved: %1").arg(fullPath));
            m_labelScreenshotStatus->setText(QString::fromUtf8("截屏已保存"));
        }
        else
        {
            m_labelScreenshotStatus->setText(QString::fromUtf8("保存失败"));
            QMessageBox::critical(this, QString::fromUtf8("错误"), QString::fromUtf8("保存截屏失败"));
        }
        break;
    case ScreenshotOptionsDialog::SaveAs:
        m_labelScreenshotStatus->setText(QString::fromUtf8("请选择保存位置"));
        break;
    default:
        m_labelScreenshotStatus->setText(QString::fromUtf8("已取消"));
        break;
    }
}

void MainWindow::onRegionCaptureClicked()
{
    Logger::instance()->info("MainWindow", "onRegionCaptureClicked called");
    m_labelScreenshotStatus->setText(QString::fromUtf8("请在屏幕上选择区域..."));
    Logger::instance()->info("MainWindow", "Calling showMinimized()");
    this->showMinimized();
    Logger::instance()->info("MainWindow", "Calling startSelection()");
    m_regionSelector->startSelection();
}

void MainWindow::onRegionSelected(const QRect& region)
{
    Logger::instance()->info("MainWindow", QString("onRegionSelected called with region (%1,%2,%3,%4)")
        .arg(region.x()).arg(region.y()).arg(region.width()).arg(region.height()));

    QImage image = m_captureController->captureRegionOnly(region);

    this->showNormal();
    this->activateWindow();

    if (image.isNull())
    {
        m_labelScreenshotStatus->setText(QString::fromUtf8("截屏失败"));
        QMessageBox::critical(this, QString::fromUtf8("错误"), QString::fromUtf8("区域截屏失败"));
        return;
    }

    ScreenshotOptionsDialog dialog(image, this);
    dialog.exec();

    QString savePath = getScreenshotSavePath();
    QDateTime dateTime = QDateTime::currentDateTime();
    QString fileName = QString::fromUtf8("Screenshot_%1.png").arg(dateTime.toString(QString::fromUtf8("yyyyMMdd_HHmmss")));
    QString fullPath = savePath + QString::fromUtf8("/") + fileName;

    switch (dialog.getSelectedAction())
    {
    case ScreenshotOptionsDialog::Copy:
        m_labelScreenshotStatus->setText(QString::fromUtf8("已复制到剪贴板"));
        break;
    case ScreenshotOptionsDialog::Save:
        if (m_captureController->saveImage(image, fullPath))
        {
            m_labelScreenshotStatus->setText(QString::fromUtf8("截屏已保存"));
        }
        else
        {
            m_labelScreenshotStatus->setText(QString::fromUtf8("保存失败"));
            QMessageBox::critical(this, QString::fromUtf8("错误"), QString::fromUtf8("保存截屏失败"));
        }
        break;
    case ScreenshotOptionsDialog::SaveAs:
        m_labelScreenshotStatus->setText(QString::fromUtf8("请选择保存位置"));
        break;
    default:
        m_labelScreenshotStatus->setText(QString::fromUtf8("已取消"));
        break;
    }
}

void MainWindow::onRegionCancelled()
{
    Logger::instance()->info("MainWindow", "onRegionCancelled called");
    this->showNormal();
    this->activateWindow();
    m_labelScreenshotStatus->setText(QString::fromUtf8("已取消区域选择"));
}

void MainWindow::onApplyHotkeysClicked()
{
    QString screenshotHotkey = m_editScreenshotHotkey->text();
    QString regionCaptureHotkey = m_editRegionCaptureHotkey->text();
    QString fullscreenRecordHotkey = m_editFullscreenRecordHotkey->text();
    QString windowRecordHotkey = m_editWindowRecordHotkey->text();

    if (screenshotHotkey.isEmpty() || regionCaptureHotkey.isEmpty() ||
        fullscreenRecordHotkey.isEmpty() || windowRecordHotkey.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("警告"), QString::fromUtf8("热键不能为空"));
        return;
    }

    m_hotkeyManager->unregisterHotkey(FULLSCREEN_CAPTURE_HOTKEY_ID);
    m_hotkeyManager->unregisterHotkey(REGION_CAPTURE_HOTKEY_ID);
    m_hotkeyManager->unregisterHotkey(FULLSCREEN_RECORD_HOTKEY_ID);
    m_hotkeyManager->unregisterHotkey(WINDOW_RECORD_HOTKEY_ID);

    m_settingsManager->setScreenshotHotkey(screenshotHotkey);
    m_settingsManager->setRegionCaptureHotkey(regionCaptureHotkey);
    m_settingsManager->setFullscreenRecordHotkey(fullscreenRecordHotkey);
    m_settingsManager->setWindowRecordHotkey(windowRecordHotkey);
    m_settingsManager->sync();

    QString errorMsg;
    if (!m_hotkeyManager->registerHotkey(FULLSCREEN_CAPTURE_HOTKEY_ID, screenshotHotkey)) {
        errorMsg += QString::fromUtf8("全屏截屏热键 ");
    }
    if (!m_hotkeyManager->registerHotkey(REGION_CAPTURE_HOTKEY_ID, regionCaptureHotkey)) {
        errorMsg += QString::fromUtf8("区域截屏热键 ");
    }
    if (!m_hotkeyManager->registerHotkey(FULLSCREEN_RECORD_HOTKEY_ID, fullscreenRecordHotkey)) {
        errorMsg += QString::fromUtf8("全屏录屏热键 ");
    }
    if (!m_hotkeyManager->registerHotkey(WINDOW_RECORD_HOTKEY_ID, windowRecordHotkey)) {
        errorMsg += QString::fromUtf8("窗口录屏热键 ");
    }

    if (!errorMsg.isEmpty()) {
        errorMsg += QString::fromUtf8("注册失败，可能与其他应用冲突");
        QMessageBox::warning(this, QString::fromUtf8("警告"), errorMsg);
    }

    QMessageBox::information(this, QString::fromUtf8("提示"), QString::fromUtf8("热键设置已保存"));
}
