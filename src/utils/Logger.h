#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QMutex>
#include <QFile>
#include <QTextStream>
#include <QString>
#include <QDateTime>

/**
 * @class Logger
 * @brief 日志类，单例模式，线程安全
 * @details 将日志写入 exe 所在目录的 logs 文件夹下的 txt 文件
 */
class Logger : public QObject
{
    Q_OBJECT

public:
    static Logger* instance();

    void setEnabled(bool enabled);
    bool isEnabled();

    void info(const QString& source, const QString& message);
    void warning(const QString& source, const QString& message);
    void error(const QString& source, const QString& message);

    void flush();
    QString currentLogFilePath();

    ~Logger();

private:
    Logger();
    void initFile();

    QString generateLogFileName() const;
    void ensureLogDirectoryExists();
    void writeLog(const QString& level, const QString& source, const QString& message);

    QMutex m_mutex;
    QFile* m_file;
    QTextStream* m_stream;
    QString m_logDir;
    QString m_currentFilePath;
    bool m_enabled;
    bool m_initialized;
};

#endif // LOGGER_H