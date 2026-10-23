#pragma once
// 文字识别：Windows.Media.Ocr（系统自带、离线，无需第三方库）
// 识别在工作线程执行，主循环用 PollOcr 取结果，避免阻塞 UI。
#include <string>
#include "screenshot/screenshot.h"

struct OcrOutcome
{
    bool         ok = false;   // false = 识别失败，error 为原因
    std::wstring text;         // 识别出的文字（可能为空：画面里没有可识别文字）
    std::wstring error;        // 失败原因（含语言包缺失指引）
};

// 启动识别；img 会被复制一份交给工作线程。上一次仍在识别时返回 false。
bool StartOcr(const ImageBGRA& img);
// 识别完成后返回 true 并取走结果（每次识别只返回一次）
bool PollOcr(OcrOutcome& out);
bool OcrBusy();
// 进程退出前收尾：等待工作线程结束
void ShutdownOcr();
