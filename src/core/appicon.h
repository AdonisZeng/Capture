#pragma once
// 应用图标统一入口（图标数据来自 exe 资源 IDI_CAPTURE，见 Capture.rc）
// - HICON 形态：窗口类 / 标题栏 / 任务栏（WM_SETICON）、系统托盘
// - BGRA 形态：D3D 纹理（界面左上角标识，交给 Graphics::CreateMemorySRV）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

namespace AppIcon
{

// 加载应用图标（cx/cy 为期望像素尺寸；Capture.ico 内含 16/24/32/48/64/72/80/96/128/256 各档）
// 资源缺失或加载失败返回 nullptr
HICON Load(UINT cx, UINT cy);

// 录制态图标：在应用图标右下角叠加红色徽标（托盘用，便于一眼区分是否正在录制）
HICON LoadRecording(UINT cx, UINT cy);

// 读取应用图标的像素（32bpp BGRA，自上而下紧密排列，每像素 4 字节）
bool LoadBGRA(UINT size, std::vector<BYTE>& out);

// 由 32bpp BGRA 像素创建 HICON（1bpp 掩码全 0，透明信息由 alpha 通道承载）
HICON FromBGRA(const BYTE* bgra, UINT cx, UINT cy);

}   // namespace AppIcon
