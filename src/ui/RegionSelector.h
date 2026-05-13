#ifndef REGIONSELECTOR_H
#define REGIONSELECTOR_H

#include <QWidget>
#include <QRect>
#include <QPoint>

/**
 * @class RegionSelector
 * @brief 区域选择器组件，用于在屏幕上选择矩形区域
 *
 * 该组件提供一个全屏半透明遮罩，允许用户通过鼠标拖拽选择屏幕上的矩形区域。
 * 选择完成后会发出 regionSelected 信号，取消选择则发出 selectionCancelled 信号。
 */
class RegionSelector : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父窗口指针
     */
    explicit RegionSelector(QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~RegionSelector();

    /**
     * @brief 开始区域选择
     *
     * 显示全屏选择窗口，进入选择模式等待用户操作。
     */
    void startSelection();

signals:
    /**
     * @brief 区域选择完成信号
     * @param region 用户选择的矩形区域
     */
    void regionSelected(const QRect& region);

    /**
     * @brief 选择取消信号
     *
     * 当用户按下 ESC 键或取消选择时发出
     */
    void selectionCancelled();

protected:
    /**
     * @brief 绘制事件处理
     * @param event 绘制事件对象
     *
     * 绘制半透明遮罩和选中区域的高亮边框
     */
    void paintEvent(QPaintEvent* event) override;

    /**
     * @brief 鼠标按下事件处理
     * @param event 鼠标事件对象
     *
     * 记录选择区域的起始点
     */
    void mousePressEvent(QMouseEvent* event) override;

    /**
     * @brief 鼠标移动事件处理
     * @param event 鼠标事件对象
     *
     * 更新选择区域并触发重绘
     */
    void mouseMoveEvent(QMouseEvent* event) override;

    /**
     * @brief 鼠标释放事件处理
     * @param event 鼠标事件对象
     *
     * 完成区域选择，发出 regionSelected 信号并关闭窗口
     */
    void mouseReleaseEvent(QMouseEvent* event) override;

    /**
     * @brief 键盘按下事件处理
     * @param event 键盘事件对象
     *
     * ESC 键取消选择，发出 selectionCancelled 信号
     */
    void keyPressEvent(QKeyEvent* event) override;

private:
    /**
     * @brief 初始化窗口属性
     *
     * 设置全屏、无边框、置顶等窗口属性
     */
    void initWindow();

    /**
     * @brief 规范化矩形区域
     * @return 规范化后的矩形区域
     *
     * 确保矩形的宽高为正值
     */
    QRect normalizedRect() const;

private:
    QPoint m_startPoint;        ///< 选择起始点
    QPoint m_endPoint;          ///< 选择结束点
    bool m_isSelecting;         ///< 是否正在选择中
};

#endif
