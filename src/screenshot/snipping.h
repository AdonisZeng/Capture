#pragma once
// 区域框选遮罩：全屏置顶分层窗口显示冻结帧，鼠标拖拽确定截图区域
//
// 交互规约（与截屏页说明一致）：
//   按住左键拖出矩形 -> 截取该区域
//   直接单击（位移小于阈值）-> 视作整屏
//   Esc / 右键          -> 取消
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "screenshot/screenshot.h"

struct SnipSelection
{
    bool ok = false;          // true = 用户已确认；false = 取消或初始化失败
    bool fullScreen = false;  // 直接单击得到的整屏选区
    RECT px{};                // 相对帧左上角的像素坐标，左/上含，右/下不含
};

// 模态运行框选：内部自建窗口并跑消息循环，返回前已完成窗口与 GDI 资源销毁。
// frame 为遮罩底图，调用方须先隐藏主窗口并取到不含自身窗口的最新帧。
bool RunSnipping(HINSTANCE hinst, const ImageBGRA& frame, SnipSelection& out);
