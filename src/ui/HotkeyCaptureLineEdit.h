#ifndef HOTKEYCAPTURELINEEDIT_H
#define HOTKEYCAPTURELINEEDIT_H

#include <QLineEdit>
#include <QKeyEvent>

class HotkeyCaptureLineEdit : public QLineEdit
{
    Q_OBJECT
public:
    explicit HotkeyCaptureLineEdit(QWidget* parent = nullptr);

signals:
    void hotkeyCaptured(const QString& hotkey);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QString formatHotkey(Qt::KeyboardModifiers mods, int key);
    bool isModifierKey(int key);
};

#endif