#ifndef WINDOWPICKER_H
#define WINDOWPICKER_H

#include <QObject>
#include <QWindow>
#include <QList>
#include <QString>
#include <QRect>
#include <QPixmap>
#include <windows.h>

/**
 * @struct WindowInfo
 * @brief 窗口信息结构体，包含窗口句柄、标题、类名和几何信息
 */
struct WindowInfo {
    HWND hwnd;       /**< 窗口句柄 */
    QString title;   /**< 窗口标题 */
    QString className; /**< 窗口类名 */
    QRect geometry;  /**< 窗口几何区域 */
};

/**
 * @class WindowPicker
 * @brief 窗口选择器类，用于枚举Windows窗口并获取窗口信息
 * @details 使用Windows API枚举桌面上的顶层窗口，过滤不可见窗口，
 *          支持窗口高亮、缩略图获取等功能
 */
class WindowPicker : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param parent 父对象指针，默认为nullptr
     */
    explicit WindowPicker(QObject* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~WindowPicker();

    /**
     * @brief 枚举所有可录制的窗口
     * @return WindowInfo列表，包含所有符合条件的窗口信息
     * @note 会过滤掉不可见窗口、托盘窗口、隐形窗口等
     */
    QList<WindowInfo> enumWindows();

    /**
     * @brief 高亮指定窗口
     * @param hwnd 要高亮的窗口句柄
     * @details 在窗口周围绘制高亮边框以标识选中状态
     */
    void highlightWindow(HWND hwnd);

    /**
     * @brief 清除所有高亮
     */
    void clearHighlight();

    /**
     * @brief 获取窗口标题
     * @param hwnd 窗口句柄
     * @return 窗口标题字符串
     */
    QString getWindowTitle(HWND hwnd) const;

    /**
     * @brief 获取窗口缩略图
     * @param hwnd 窗口句柄
     * @param maxWidth 最大宽度，默认为200
     * @param maxHeight 最大高度，默认为150
     * @return 窗口缩略图QPixmap对象
     */
    QPixmap getWindowThumbnail(HWND hwnd, int maxWidth = 200, int maxHeight = 150);

signals:
    /**
     * @brief 窗口悬停信号
     * @param hwnd 悬停的窗口句柄
     */
    void windowHovered(HWND hwnd);

    /**
     * @brief 窗口选中信号
     * @param hwnd 选中的窗口句柄
     */
    void windowSelected(HWND hwnd);

protected:
    /**
     * @brief 枚举窗口的回调函数
     * @param hwnd 枚举到的窗口句柄
     * @param lParam 传递给EnumWindows的LPARAM参数
     * @return BOOL TRUE继续枚举，FALSE停止枚举
     */
    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);

private:
    QList<WindowInfo> m_windows;           /**< 窗口列表 */
    QList<HWND> m_highlightedWindows;      /**< 已高亮的窗口列表 */
};

#endif