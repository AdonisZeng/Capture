#ifndef MP4WRITER_H
#define MP4WRITER_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

class Mp4Writer : public QObject
{
    Q_OBJECT
public:
    explicit Mp4Writer(QObject* parent = nullptr);
    ~Mp4Writer();

    bool open(const QString& filePath, int width, int height, int frameRate,
              int sampleRate, int channels, int videoBitrate, int audioBitrate);
    void close();

    bool writeVideoSample(const QByteArray& data, qint64 timestamp, bool keyFrame);
    bool writeAudioSample(const QByteArray& data, qint64 timestamp);

    bool isOpen() const { return m_isOpen; }

signals:
    void error(const QString& error);
    void bytesWritten(qint64 bytes);

private:
    bool writeSample(DWORD streamIndex, const QByteArray& data, qint64 mfTimestamp);

private:
    bool m_isOpen;
    QString m_filePath;

    IMFSinkWriter* m_sinkWriter;
    DWORD m_videoStreamIndex;
    DWORD m_audioStreamIndex;

    qint64 m_videoTimestampOffset;
    qint64 m_audioTimestampOffset;
    qint64 m_lastVideoTime;
    qint64 m_lastAudioTime;
};

#endif
