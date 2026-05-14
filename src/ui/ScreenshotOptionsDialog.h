#ifndef SCREENSHOTOPTIONSDIALOG_H
#define SCREENSHOTOPTIONSDIALOG_H

#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

class ScreenshotOptionsDialog : public QDialog
{
    Q_OBJECT

public:
    enum Action {
        None,
        Copy,
        Save,
        SaveAs
    };

    explicit ScreenshotOptionsDialog(const QImage& image, QWidget* parent = nullptr);
    ~ScreenshotOptionsDialog();

    Action getSelectedAction() const { return m_selectedAction; }

private slots:
    void onCopyClicked();
    void onSaveClicked();
    void onSaveAsClicked();

private:
    void setupUi();

private:
    QImage m_image;
    Action m_selectedAction;
    QLabel* m_labelPreview;
};

#endif