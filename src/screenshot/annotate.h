#pragma once
// 截图标注编辑器：在已裁剪的截图上绘制矩形/椭圆/直线/箭头/画笔/高亮，
// 完成后输出标注后的图像（尺寸与输入完全一致）。
//
// 交互规约：
//   拖拽            -> 绘制当前图形（图像坐标，超出图片的部分自动裁掉）
//   工具按钮        -> 切换工具；颜色块/线宽条 -> 切换样式
//   撤销/重做       -> 每次提交的图形为一个历史步（最多 20 步）
//   回车 / 完成     -> 输出标注结果；Esc / 取消 -> 放弃（调用方按"取消截图"处理）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "screenshot/screenshot.h"

// 绘制工具（工具栏顺序；数值会写进 settings.json，新增只能追加到末尾）
enum AnnotateTool
{
    AnnotateRect = 0,      // 矩形（空心）
    AnnotateEllipse,       // 椭圆 / 圆（空心）
    AnnotateLine,          // 直线
    AnnotateArrow,         // 箭头
    AnnotatePen,           // 画笔（自由曲线）
    AnnotateHighlight,     // 高亮笔（半透明粗线）
    AnnotateToolCount
};

// 上次使用的样式（本模块不依赖 settings，由调用方负责读写）
struct AnnotateStyle
{
    int tool  = AnnotateRect;
    int color = 0;
    int width = 1;
};

// 调色板与线宽档位：界面绘制与配置校验同源，不要在别处另写一份
int            AnnotateColorCount();
COLORREF       AnnotateColorAt(int index);
const wchar_t* AnnotateColorName(int index);
int            AnnotateWidthCount();
int            AnnotateWidthPx(int index);   // 显示像素（96dpi 基准）

// 模态运行标注窗口：内部自建窗口并跑消息循环，返回前销毁全部 GDI 资源。
// 返回 false = 用户取消（Esc / 取消按钮），此时 out 不被修改。
// style 为输入输出：进入时按传入值预选，退出时回写用户最后的选择。
bool RunAnnotator(HINSTANCE hinst, const ImageBGRA& src, ImageBGRA& out,
                  AnnotateStyle& style);
