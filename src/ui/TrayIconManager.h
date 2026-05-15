#ifndef TRAYICONMANAGER_H
#define TRAYICONMANAGER_H

#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <functional>

class TrayIconManager : public QObject
{
    Q_OBJECT
public:
    explicit TrayIconManager(QObject* parent = nullptr);
    ~TrayIconManager();

    void setRecordingState(bool isRecording);
    void showMessage(const QString& title, const QString& msg);

    static const QString TYPE_FULLSCREEN;
    static const QString TYPE_REGION;
    static const QString TYPE_WINDOW;

signals:
    void signalScreenshotRequested(const QString& type);
    void signalRecordingRequested(const QString& type);
    void signalSettingsRequested();
    void signalQuitRequested();

public slots:
    void onIconClicked(QSystemTrayIcon::ActivationReason reason);

private:
    void createTrayIcon();
    void updateTooltip();

private:
    QSystemTrayIcon* m_trayIcon;
    QMenu* m_menu;

    QAction* m_actionScreenshotFullscreen;
    QAction* m_actionScreenshotRegion;
    QAction* m_actionRecordingFullscreen;
    QAction* m_actionRecordingWindow;

    QMenu* m_menuScreenshot;
    QMenu* m_menuRecording;

    QAction* m_actionSettings;
    QAction* m_actionQuit;
    bool m_isRecording;
};

#endif