#ifndef HOTKEYMANAGER_H
#define HOTKEYMANAGER_H

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QMap>
#include <windows.h>

class HotkeyManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager();

    bool registerHotkey(int id, const QString& hotkey);
    void unregisterHotkey(int id);
    void unregisterAll();

signals:
    void hotkeyPressed(int id);

public slots:
    void onHotkeyPressed(int id);

protected:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    int parseModifiers(const QString& hotkey, QString& key);
    int getVirtualKeyCode(const QString& key);

private:
    QMap<int, UINT> m_hotkeyIds;
    QMap<int, UINT> m_modifiers;
};

#endif
