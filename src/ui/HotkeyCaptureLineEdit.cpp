#include "HotkeyCaptureLineEdit.h"
#include <QApplication>

HotkeyCaptureLineEdit::HotkeyCaptureLineEdit(QWidget* parent)
    : QLineEdit(parent)
{
    setReadOnly(true);
    setPlaceholderText(QString::fromUtf8("点击此处按下快捷键..."));
}

void HotkeyCaptureLineEdit::keyPressEvent(QKeyEvent* event)
{
    Qt::KeyboardModifiers mods = event->modifiers();
    int key = event->key();

    if (isModifierKey(key)) {
        return;
    }

    if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier)) {
        QString hotkey = formatHotkey(mods, key);
        setText(hotkey);
        emit hotkeyCaptured(hotkey);
    }
}

QString HotkeyCaptureLineEdit::formatHotkey(Qt::KeyboardModifiers mods, int key)
{
    QStringList parts;

    if (mods & Qt::ControlModifier) parts << QString::fromUtf8("CTRL");
    if (mods & Qt::AltModifier) parts << QString::fromUtf8("ALT");
    if (mods & Qt::ShiftModifier) parts << QString::fromUtf8("SHIFT");
    if (mods & Qt::MetaModifier) parts << QString::fromUtf8("WIN");

    QString keyStr;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        keyStr = QChar('A' + (key - Qt::Key_A));
    } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        keyStr = QChar('0' + (key - Qt::Key_0));
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        keyStr = QString::fromUtf8("F%1").arg(key - Qt::Key_F1 + 1);
    } else {
        keyStr = QKeySequence(key).toString();
    }

    parts << keyStr;
    return parts.join(QString::fromUtf8("+"));
}

bool HotkeyCaptureLineEdit::isModifierKey(int key)
{
    return key == Qt::Key_Control ||
           key == Qt::Key_Shift ||
           key == Qt::Key_Alt ||
           key == Qt::Key_Meta;
}