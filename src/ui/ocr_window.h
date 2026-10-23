#pragma once
// 文字识别结果小窗：置顶工具窗，只读多行文本（可选中/滚动）+ 复制文字 + 关闭
#include <windows.h>
#include <string>

// 显示结果窗；已存在时只更新内容并置前。
// busy = true 时复制按钮禁用（识别进行中）；copyEnabled 控制复制按钮是否可用。
void ShowOcrResultWindow(HWND owner, const std::wstring& text, bool busy, bool copyEnabled);
void SetOcrResultWindowText(const std::wstring& text, bool busy, bool copyEnabled);
bool OcrResultWindowVisible();
void CloseOcrResultWindow();
