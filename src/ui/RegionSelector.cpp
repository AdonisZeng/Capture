#include "RegionSelector.h"
#include "utils/Logger.h"
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QMouseEvent>

RegionSelector::RegionSelector(QWidget* parent)
    : QWidget(parent)
    , m_startPoint(0, 0)
    , m_endPoint(0, 0)
    , m_isSelecting(false)
{
    initWindow();
}

RegionSelector::~RegionSelector()
{
}

void RegionSelector::initWindow()
{
    // 设置窗口标志：无边框、置顶、工具窗口
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);

    // 设置窗口透明度属性
    setAttribute(Qt::WA_TranslucentBackground);

    // 设置鼠标追踪
    setMouseTracking(true);

    // 获取所有屏幕的总几何区域
    QScreen* primaryScreen = QApplication::primaryScreen();
    if (primaryScreen)
    {
        QRect totalGeometry;
        QList<QScreen*> screens = QApplication::screens();
        for (QScreen* screen : screens)
        {
            totalGeometry = totalGeometry.united(screen->geometry());
        }
        setGeometry(totalGeometry);
    }
}

void RegionSelector::startSelection()
{
    // 重置选择状态
    m_startPoint = QPoint(0, 0);
    m_endPoint = QPoint(0, 0);
    m_isSelecting = false;

    // 显示窗口并激活
    show();
    activateWindow();
    setFocus();
}

void RegionSelector::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    // 绘制半透明黑色遮罩
    painter.fillRect(rect(), QColor(0, 0, 0, 100));

    // 如果正在选择，绘制选中区域
    if (m_isSelecting)
    {
        QRect selectedRect = normalizedRect();

        // 清除选中区域的遮罩，使其清晰可见
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.fillRect(selectedRect, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // 绘制选中区域的边框
        QPen pen(QColor(0, 120, 215), 2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(selectedRect);

        // 绘制尺寸信息
        QString sizeText = QString("%1 x %2").arg(selectedRect.width()).arg(selectedRect.height());
        QFont font = painter.font();
        font.setPointSize(12);
        font.setBold(true);
        painter.setFont(font);

        // 计算文本位置（在选中区域右下角外侧）
        QFontMetrics fm(font);
        QRect textRect = fm.boundingRect(sizeText);
        QPoint textPos(selectedRect.right() - textRect.width() - 5,
                       selectedRect.bottom() + textRect.height() + 5);

        // 确保文本在屏幕内
        if (textPos.x() + textRect.width() > width())
        {
            textPos.setX(selectedRect.right() - textRect.width() - 5);
        }
        if (textPos.y() > height())
        {
            textPos.setY(selectedRect.bottom() - 5);
        }
        if (textPos.y() - textRect.height() < 0)
        {
            textPos.setY(selectedRect.top() + textRect.height() + 5);
        }

        // 绘制文本背景
        QRect bgRect(textPos.x() - 3, textPos.y() - textRect.height() - 3,
                     textRect.width() + 6, textRect.height() + 6);
        painter.fillRect(bgRect, QColor(0, 0, 0, 180));

        // 绘制文本
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(textPos, sizeText);

        // 绘制十字准线
        QPen crossPen(QColor(255, 255, 255, 128), 1, Qt::DashLine);
        painter.setPen(crossPen);

        // 垂直线
        painter.drawLine(selectedRect.left(), 0, selectedRect.left(), height());
        painter.drawLine(selectedRect.right(), 0, selectedRect.right(), height());

        // 水平线
        painter.drawLine(0, selectedRect.top(), width(), selectedRect.top());
        painter.drawLine(0, selectedRect.bottom(), width(), selectedRect.bottom());
    }
}

void RegionSelector::mousePressEvent(QMouseEvent* event)
{
    Logger::instance()->info("RegionSelector", "mousePressEvent called");
    if (event->button() == Qt::LeftButton)
    {
        m_startPoint = event->pos();
        m_endPoint = event->pos();
        m_isSelecting = true;
        Logger::instance()->info("RegionSelector", QString("Selection started at (%1, %2)").arg(event->pos().x()).arg(event->pos().y()));
        update();
    }
}

void RegionSelector::mouseMoveEvent(QMouseEvent* event)
{
    if (m_isSelecting)
    {
        m_endPoint = event->pos();
        Logger::instance()->info("RegionSelector", QString("Mouse move to (%1, %2)").arg(event->pos().x()).arg(event->pos().y()));
        update();
    }
}

void RegionSelector::mouseReleaseEvent(QMouseEvent* event)
{
    Logger::instance()->info("RegionSelector", "mouseReleaseEvent called");
    if (event->button() == Qt::LeftButton && m_isSelecting)
    {
        m_isSelecting = false;
        QRect selectedRect = normalizedRect();
        Logger::instance()->info("RegionSelector", QString("Selection ended: rect=(%1,%2,%3,%4) size=%5x%6")
            .arg(selectedRect.x()).arg(selectedRect.y())
            .arg(selectedRect.width()).arg(selectedRect.height())
            .arg(selectedRect.width()).arg(selectedRect.height()));

        // 确保选择的区域有效（宽高都大于0）
        hide();
        if (selectedRect.width() > 0 && selectedRect.height() > 0)
        {
            Logger::instance()->info("RegionSelector", "Emitting regionSelected signal");
            emit regionSelected(selectedRect);
            Logger::instance()->info("RegionSelector", "Signal emitted successfully");
        }
        else
        {
            Logger::instance()->info("RegionSelector", "Emitting selectionCancelled (invalid rect)");
            emit selectionCancelled();
        }
    }
}

void RegionSelector::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        m_isSelecting = false;
        emit selectionCancelled();
        hide();
    }
    else
    {
        QWidget::keyPressEvent(event);
    }
}

QRect RegionSelector::normalizedRect() const
{
    return QRect(m_startPoint, m_endPoint).normalized();
}
