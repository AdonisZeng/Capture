#include "HotkeyManager.h"
#include <QDebug>
#include <QCoreApplication>

HotkeyManager::HotkeyManager(QObject* parent)
    : QObject(parent)
{
    qApp->installNativeEventFilter(this);
}

HotkeyManager::~HotkeyManager()
{
    unregisterAll();
    qApp->removeNativeEventFilter(this);
}

bool HotkeyManager::registerHotkey(int id, const QString& hotkey)
{
    QString key;
    UINT modifiers = parseModifiers(hotkey, key);

    if (key.isEmpty()) {
        qWarning() << "Invalid hotkey:" << hotkey;
        return false;
    }

    UINT vkCode = getVirtualKeyCode(key);
    if (vkCode == 0) {
        qWarning() << "Unknown key:" << key;
        return false;
    }

    if (RegisterHotKey(nullptr, id, modifiers, vkCode)) {
        m_hotkeyIds[id] = vkCode;
        m_modifiers[id] = modifiers;
        qDebug() << "Registered hotkey:" << hotkey << "id:" << id;
        return true;
    }

    qWarning() << "Failed to register hotkey:" << hotkey << "Error:" << GetLastError();
    return false;
}

void HotkeyManager::unregisterHotkey(int id)
{
    if (m_hotkeyIds.contains(id)) {
        UnregisterHotKey(nullptr, id);
        m_hotkeyIds.remove(id);
        m_modifiers.remove(id);
        qDebug() << "Unregistered hotkey id:" << id;
    }
}

void HotkeyManager::unregisterAll()
{
    for (int id : m_hotkeyIds.keys()) {
        UnregisterHotKey(nullptr, id);
    }
    m_hotkeyIds.clear();
    m_modifiers.clear();
    qDebug() << "Unregistered all hotkeys";
}

void HotkeyManager::onHotkeyPressed(int id)
{
    emit hotkeyPressed(id);
}

int HotkeyManager::parseModifiers(const QString& hotkey, QString& key)
{
    UINT modifiers = 0;
    QStringList parts = hotkey.toUpper().split('+');

    if (parts.isEmpty()) {
        return 0;
    }

    key = parts.last().trimmed();

    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString& mod = parts[i].trimmed();
        if (mod == "CTRL" || mod == "CONTROL") {
            modifiers |= MOD_CONTROL;
        } else if (mod == "ALT") {
            modifiers |= MOD_ALT;
        } else if (mod == "SHIFT") {
            modifiers |= MOD_SHIFT;
        } else if (mod == "WIN") {
            modifiers |= MOD_WIN;
        }
    }

    return modifiers;
}

int HotkeyManager::getVirtualKeyCode(const QString& key)
{
    QString upperKey = key.toUpper();

    if (upperKey.length() == 1 && upperKey[0].isLetter()) {
        return upperKey[0].unicode();
    }

    if (upperKey.length() == 1 && upperKey[0].isDigit()) {
        return upperKey[0].unicode();
    }

    static QMap<QString, int> keyMap;
    if (keyMap.isEmpty()) {
        keyMap["F1"] = VK_F1;
        keyMap["F2"] = VK_F2;
        keyMap["F3"] = VK_F3;
        keyMap["F4"] = VK_F4;
        keyMap["F5"] = VK_F5;
        keyMap["F6"] = VK_F6;
        keyMap["F7"] = VK_F7;
        keyMap["F8"] = VK_F8;
        keyMap["F9"] = VK_F9;
        keyMap["F10"] = VK_F10;
        keyMap["F11"] = VK_F11;
        keyMap["F12"] = VK_F12;
        keyMap["SPACE"] = VK_SPACE;
        keyMap["ENTER"] = VK_RETURN;
        keyMap["RETURN"] = VK_RETURN;
        keyMap["TAB"] = VK_TAB;
        keyMap["ESCAPE"] = VK_ESCAPE;
        keyMap["ESC"] = VK_ESCAPE;
        keyMap["BACKSPACE"] = VK_BACK;
        keyMap["DELETE"] = VK_DELETE;
        keyMap["INSERT"] = VK_INSERT;
        keyMap["HOME"] = VK_HOME;
        keyMap["END"] = VK_END;
        keyMap["PAGEUP"] = VK_PRIOR;
        keyMap["PAGEDOWN"] = VK_NEXT;
        keyMap["UP"] = VK_UP;
        keyMap["DOWN"] = VK_DOWN;
        keyMap["LEFT"] = VK_LEFT;
        keyMap["RIGHT"] = VK_RIGHT;
    }

    if (keyMap.contains(upperKey)) {
        return keyMap[upperKey];
    }

    return 0;
}

bool HotkeyManager::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(result);

    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_HOTKEY) {
            int id = static_cast<int>(msg->wParam);
            qDebug() << "Hotkey pressed, id:" << id;
            QMetaObject::invokeMethod(this, "onHotkeyPressed", Qt::QueuedConnection, Q_ARG(int, id));
            return true;
        }
    }

    return false;
}
