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

signals:
    void signalScreenshotRequested();
    void signalRecordingRequested();
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
    QAction* m_actionScreenshot;
    QAction* m_actionRecording;
    QAction* m_actionSettings;
    QAction* m_actionQuit;
    bool m_isRecording;
};

#endif