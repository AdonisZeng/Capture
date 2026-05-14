#include <QApplication>
#include <QStyleFactory>
#include <QFile>
#include <QDir>
#include <objbase.h>
#include "ui/MainWindow.h"
#include "config/SettingsManager.h"
#include "config/HotkeyManager.h"
#include "utils/Logger.h"

/**
 * @brief Ӧ�ó�����ڵ�
 * @param argc �����в�������
 * @param argv �����в�������
 * @return Ӧ�ó����˳���
 */
int main(int argc, char *argv[])
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    QApplication app(argc, argv);
    app.setApplicationName("Capture");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Capture");

    app.setStyle(QStyleFactory::create("Fusion"));

    QDir saveDir(SettingsManager::instance()->generalSettings().savePath);
    if (!saveDir.exists()) {
        saveDir.mkpath(".");
    }

    QString cssPath = ":/styles/dark.css";
    QFile cssFile(cssPath);
    if (cssFile.exists()) {
        if (cssFile.open(QFile::ReadOnly)) {
            app.setStyleSheet(cssFile.readAll());
            cssFile.close();
        }
    }

    MainWindow window;
    window.show();

    // 初始化日志系统
    Logger::instance()->setEnabled(SettingsManager::instance()->logSettings().enabled);
    Logger::instance()->info("Main", "Application started");
    Logger::instance()->info("Main", QString("Log file: %1").arg(Logger::instance()->currentLogFilePath()));

    return app.exec();
}