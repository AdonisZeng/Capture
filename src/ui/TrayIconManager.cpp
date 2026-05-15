#include "TrayIconManager.h"
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QCoreApplication>
#include <QGuiApplication>

const QString TrayIconManager::TYPE_FULLSCREEN = QStringLiteral("fullscreen");
const QString TrayIconManager::TYPE_REGION = QStringLiteral("region");
const QString TrayIconManager::TYPE_WINDOW = QStringLiteral("window");

TrayIconManager::TrayIconManager(QObject* parent)
    : QObject(parent)
    , m_trayIcon(nullptr)
    , m_menu(nullptr)
    , m_actionScreenshotFullscreen(nullptr)
    , m_actionScreenshotRegion(nullptr)
    , m_actionRecordingFullscreen(nullptr)
    , m_actionRecordingWindow(nullptr)
    , m_menuScreenshot(nullptr)
    , m_menuRecording(nullptr)
    , m_actionSettings(nullptr)
    , m_actionQuit(nullptr)
    , m_isRecording(false)
{
    createTrayIcon();
}

TrayIconManager::~TrayIconManager()
{
    if (m_trayIcon) {
        m_trayIcon->deleteLater();
    }
    if (m_menu) {
        m_menu->deleteLater();
    }
}

void TrayIconManager::createTrayIcon()
{
    m_menu = new QMenu();

    // 截屏子菜单
    m_menuScreenshot = new QMenu(tr("截屏(&S)"), m_menu);
    m_menuScreenshot->setIcon(QIcon::fromTheme("camera-photo", QIcon(":/icons/screenshot.png")));

    m_actionScreenshotFullscreen = new QAction(tr("全屏截图"), m_menuScreenshot);
    m_actionScreenshotFullscreen->setData(TYPE_FULLSCREEN);
    m_menuScreenshot->addAction(m_actionScreenshotFullscreen);
    connect(m_actionScreenshotFullscreen, &QAction::triggered, this, [this]() {
        emit signalScreenshotRequested(TYPE_FULLSCREEN);
    });

    m_actionScreenshotRegion = new QAction(tr("区域截图"), m_menuScreenshot);
    m_actionScreenshotRegion->setData(TYPE_REGION);
    m_menuScreenshot->addAction(m_actionScreenshotRegion);
    connect(m_actionScreenshotRegion, &QAction::triggered, this, [this]() {
        emit signalScreenshotRequested(TYPE_REGION);
    });

    m_menu->addMenu(m_menuScreenshot);

    // 录制子菜单
    m_menuRecording = new QMenu(tr("录制(&R)"), m_menu);
    m_menuRecording->setIcon(QIcon::fromTheme("media-record", QIcon(":/icons/record.png")));

    m_actionRecordingFullscreen = new QAction(tr("全屏录制"), m_menuRecording);
    m_actionRecordingFullscreen->setData(TYPE_FULLSCREEN);
    m_menuRecording->addAction(m_actionRecordingFullscreen);
    connect(m_actionRecordingFullscreen, &QAction::triggered, this, [this]() {
        emit signalRecordingRequested(TYPE_FULLSCREEN);
    });

    m_actionRecordingWindow = new QAction(tr("窗口录制"), m_menuRecording);
    m_actionRecordingWindow->setData(TYPE_WINDOW);
    m_menuRecording->addAction(m_actionRecordingWindow);
    connect(m_actionRecordingWindow, &QAction::triggered, this, [this]() {
        emit signalRecordingRequested(TYPE_WINDOW);
    });

    m_menu->addMenu(m_menuRecording);

    m_menu->addSeparator();

    m_actionSettings = new QAction(tr("设置(&P)"), m_menu);
    m_actionSettings->setIcon(QIcon::fromTheme("preferences-system", QIcon(":/icons/settings.png")));
    m_menu->addAction(m_actionSettings);
    connect(m_actionSettings, &QAction::triggered, this, &TrayIconManager::signalSettingsRequested);

    m_menu->addSeparator();

    m_actionQuit = new QAction(tr("退出(&Q)"), m_menu);
    m_actionQuit->setIcon(QIcon::fromTheme("application-exit", QIcon(":/icons/quit.png")));
    m_menu->addAction(m_actionQuit);
    connect(m_actionQuit, &QAction::triggered, this, &TrayIconManager::signalQuitRequested);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setContextMenu(m_menu);

    QIcon appIcon(":/icons/Capture.ico");
    if (appIcon.isNull()) {
        appIcon = QIcon(":/icons/app.png");
    }
    if (appIcon.isNull()) {
        appIcon = QGuiApplication::windowIcon();
    }
    if (appIcon.isNull()) {
        QPixmap defaultIcon(64, 64);
        defaultIcon.fill(Qt::darkGray);
        appIcon.addPixmap(defaultIcon);
    }
    m_trayIcon->setIcon(appIcon);

    m_trayIcon->setToolTip(QCoreApplication::applicationName());

    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &TrayIconManager::onIconClicked);

    m_trayIcon->show();
}

void TrayIconManager::updateTooltip()
{
    QString baseName = QCoreApplication::applicationName();
    if (m_isRecording) {
        m_trayIcon->setToolTip(baseName + " - " + tr("录制中"));
    } else {
        m_trayIcon->setToolTip(baseName + " - " + tr("就绪"));
    }
}

void TrayIconManager::setRecordingState(bool isRecording)
{
    m_isRecording = isRecording;
    updateTooltip();

    QIcon icon;
    if (isRecording) {
        QPixmap basePixmap(64, 64);
        basePixmap.fill(Qt::transparent);

        QIcon appIcon(":/icons/Capture.ico");
        if (appIcon.isNull()) {
            appIcon = QIcon(":/icons/app.png");
        }
        if (appIcon.isNull()) {
            appIcon = QGuiApplication::windowIcon();
        }

        QPixmap origPixmap;
        if (!appIcon.isNull()) {
            origPixmap = appIcon.pixmap(64, 64);
        } else {
            origPixmap = QPixmap(64, 64);
            origPixmap.fill(Qt::darkGray);
        }

        QPainter painter(&basePixmap);
        painter.drawPixmap(0, 0, origPixmap);

        QColor redColor(255, 0, 0, 200);
        painter.setBrush(redColor);
        painter.setPen(Qt::NoPen);
        QPointF center(48.0, 48.0);
        painter.drawEllipse(center, 12.0, 12.0);

        icon.addPixmap(basePixmap);
    } else {
        QIcon appIcon(":/icons/Capture.ico");
        if (appIcon.isNull()) {
            appIcon = QIcon(":/icons/app.png");
        }
        if (appIcon.isNull()) {
            appIcon = QGuiApplication::windowIcon();
        }
        if (appIcon.isNull()) {
            QPixmap defaultIcon(64, 64);
            defaultIcon.fill(Qt::darkGray);
            appIcon.addPixmap(defaultIcon);
        }
        icon = appIcon;
    }

    m_trayIcon->setIcon(icon);
}

void TrayIconManager::showMessage(const QString& title, const QString& msg)
{
    if (m_trayIcon && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(title, msg, m_trayIcon->icon(), 3000);
    }
}

void TrayIconManager::onIconClicked(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
        case QSystemTrayIcon::Trigger:
            emit signalSettingsRequested();
            break;
        case QSystemTrayIcon::DoubleClick:
            emit signalScreenshotRequested(TYPE_FULLSCREEN);
            break;
        case QSystemTrayIcon::MiddleClick:
            emit signalRecordingRequested(TYPE_FULLSCREEN);
            break;
        default:
            break;
    }
}
