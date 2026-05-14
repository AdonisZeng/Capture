#include "ScreenshotOptionsDialog.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFileDialog>
#include <QGroupBox>
#include <QDialogButtonBox>

ScreenshotOptionsDialog::ScreenshotOptionsDialog(const QImage& image, QWidget* parent)
    : QDialog(parent)
    , m_image(image)
    , m_selectedAction(None)
    , m_labelPreview(nullptr)
{
    setupUi();
    setWindowTitle(QString::fromUtf8("截图选项"));
    setMinimumSize(500, 400);
    setModal(true);
}

ScreenshotOptionsDialog::~ScreenshotOptionsDialog()
{
}

void ScreenshotOptionsDialog::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    QGroupBox* previewGroup = new QGroupBox(QString::fromUtf8("截图预览"), this);
    QVBoxLayout* previewLayout = new QVBoxLayout(previewGroup);

    m_labelPreview = new QLabel(this);
    m_labelPreview->setAlignment(Qt::AlignCenter);
    m_labelPreview->setBackgroundRole(QPalette::Dark);
    m_labelPreview->setMinimumSize(400, 300);
    m_labelPreview->setScaledContents(true);

    QPixmap pixmap = QPixmap::fromImage(m_image);
    int maxWidth = 460;
    int maxHeight = 300;
    if (pixmap.width() > maxWidth || pixmap.height() > maxHeight) {
        pixmap = pixmap.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    m_labelPreview->setPixmap(pixmap);

    previewLayout->addWidget(m_labelPreview);
    mainLayout->addWidget(previewGroup);

    QGroupBox* actionGroup = new QGroupBox(QString::fromUtf8("操作"), this);
    QHBoxLayout* actionLayout = new QHBoxLayout(actionGroup);

    QPushButton* btnCopy = new QPushButton(QString::fromUtf8("复制"), actionGroup);
    btnCopy->setMinimumSize(100, 40);
    QPushButton* btnSave = new QPushButton(QString::fromUtf8("保存"), actionGroup);
    btnSave->setMinimumSize(100, 40);
    QPushButton* btnSaveAs = new QPushButton(QString::fromUtf8("另存为"), actionGroup);
    btnSaveAs->setMinimumSize(100, 40);

    actionLayout->addWidget(btnCopy);
    actionLayout->addWidget(btnSave);
    actionLayout->addWidget(btnSaveAs);
    actionLayout->addStretch();

    mainLayout->addWidget(actionGroup);

    connect(btnCopy, &QPushButton::clicked, this, &ScreenshotOptionsDialog::onCopyClicked);
    connect(btnSave, &QPushButton::clicked, this, &ScreenshotOptionsDialog::onSaveClicked);
    connect(btnSaveAs, &QPushButton::clicked, this, &ScreenshotOptionsDialog::onSaveAsClicked);
}

void ScreenshotOptionsDialog::onCopyClicked()
{
    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setImage(m_image);
    m_selectedAction = Copy;
    accept();
}

void ScreenshotOptionsDialog::onSaveClicked()
{
    m_selectedAction = Save;
    accept();
}

void ScreenshotOptionsDialog::onSaveAsClicked()
{
    QString filePath = QFileDialog::getSaveFileName(
        this,
        QString::fromUtf8("另存为"),
        QString("Screenshot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        QString::fromUtf8("图片文件 (*.png)"));

    if (!filePath.isEmpty()) {
        m_selectedAction = SaveAs;
        m_image.save(filePath, "PNG", 100);
        accept();
    }
}