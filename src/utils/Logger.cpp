#include "Logger.h"
#include <QCoreApplication>
#include <QDir>
#include <QDebug>

Logger::Logger()
    : QObject(nullptr)
    , m_file(nullptr)
    , m_stream(nullptr)
    , m_enabled(true)
    , m_initialized(false)
{
    ensureLogDirectoryExists();
    initFile();
}

Logger::~Logger()
{
    if (m_stream) {
        m_stream->flush();
        delete m_stream;
        m_stream = nullptr;
    }
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
}

Logger* Logger::instance()
{
    static Logger inst;
    return &inst;
}

void Logger::setEnabled(bool enabled)
{
    QMutexLocker locker(&m_mutex);
    m_enabled = enabled;
}

bool Logger::isEnabled()
{
    QMutexLocker locker(&m_mutex);
    return m_enabled;
}

QString Logger::generateLogFileName() const
{
    QDateTime now = QDateTime::currentDateTime();
    return now.toString("yyyy-MM-dd-HH-mm-ss") + ".txt";
}

void Logger::ensureLogDirectoryExists()
{
    QString appDir = QCoreApplication::applicationDirPath();
    m_logDir = appDir + "/logs";
    QDir dir(m_logDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
}

void Logger::initFile()
{
    QMutexLocker locker(&m_mutex);

    if (m_initialized) {
        return;
    }

    m_currentFilePath = m_logDir + "/" + generateLogFileName();
    m_file = new QFile(m_currentFilePath);

    if (m_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_stream = new QTextStream(m_file);
        m_initialized = true;
    } else {
        qWarning() << "Failed to open log file:" << m_currentFilePath;
    }
}

QString Logger::currentLogFilePath()
{
    QMutexLocker locker(&m_mutex);
    return m_currentFilePath;
}

void Logger::info(const QString& source, const QString& message)
{
    writeLog("INFO ", source, message);
}

void Logger::warning(const QString& source, const QString& message)
{
    writeLog("WARN ", source, message);
}

void Logger::error(const QString& source, const QString& message)
{
    writeLog("ERROR", source, message);
}

void Logger::writeLog(const QString& level, const QString& source, const QString& message)
{
    QMutexLocker locker(&m_mutex);

    if (!m_enabled || !m_initialized) {
        return;
    }

    QDateTime now = QDateTime::currentDateTime();
    QString timestamp = now.toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString logLine = QString("[%1] [%2] [%3] %4\n")
                          .arg(timestamp)
                          .arg(level)
                          .arg(source)
                          .arg(message);

    if (m_stream) {
        *m_stream << logLine;
        m_stream->flush();
    }
}

void Logger::flush()
{
    QMutexLocker locker(&m_mutex);
    if (m_stream) {
        m_stream->flush();
    }
    if (m_file) {
        m_file->flush();
    }
}