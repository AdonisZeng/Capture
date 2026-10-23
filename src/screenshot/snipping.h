#pragma once
// 区域框选遮罩：全屏置顶分层窗口显示冻结帧，鼠标拖拽确定截图区域
//
// 交互规约（与截屏页说明一致）：
//   windowPick = true（截屏）：
//     移动鼠标        -> 自动高亮光标下的窗口
//     单击（位移小于阈值）-> 截取高亮窗口；无高亮窗口（桌面空白）时截整屏
//     Ctrl+单击       -> 无视高亮窗口，始终截整屏
//     按住左键拖出矩形 -> 截取该区域（位移超过阈值即切换到框选）
//   windowPick = false（区域录制等）：
//     单击 -> 整屏；拖拽 -> 选区（保持原有行为）
//   Esc / 右键 -> 取消
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "screenshot/screenshot.h"

// 快捷操作栏动作（actionBar = false 时恒为 None，走原有的"松手即保存/复制"流程）
enum class SnipAction
{
    None = 0,     // 未启用快捷操作栏
    Copy,         // 复制到剪贴板
    Ocr,          // 文字识别
    ShareWeChat,  // 分享到微信
    ShareQQ,      // 分享到 QQ
};

struct SnipSelection
{
    bool ok = false;          // true = 用户已确认；false = 取消或初始化失败
    bool fullScreen = false;  // 单击桌面空白处得到的整屏选区（区域内无窗口时）
    RECT px{};                // 相对帧左上角的像素坐标，左/上含，右/下不含
    SnipAction action = SnipAction::None;  // 快捷操作栏选择的功能
};

// 模态运行框选：内部自建窗口并跑消息循环，返回前已完成窗口与 GDI 资源销毁。
// frame 为遮罩底图，调用方须先隐藏主窗口并取到不含自身窗口的最新帧。
// windowPick = true 时启用「悬停识别窗口 + 单击截窗口」；区域录制传 false 保持原行为。
// actionBar = true 时松手不立即返回，而是在选区右下角显示快捷操作栏，
// 用户点复制/文字识别/分享或 ×（取消）后才返回；区域录制传 false。
bool RunSnipping(HINSTANCE hinst, const ImageBGRA& frame, SnipSelection& out,
                 bool windowPick, bool actionBar = false);
