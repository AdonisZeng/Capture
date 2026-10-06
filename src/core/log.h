#pragma once
// 轻量文件日志
// - 每次程序启动在 log 目录下新建一个 TXT，文件名为启动时刻（年月日_时分秒）
// - log 目录由 core/util.cpp 的 DataSubDir(L"log") 定位，与配置文件同源：
//   优先 exe 同级（便携模式），exe 目录不可写时回退 %APPDATA%\Capture
// - 文件编码 UTF-8（带 BOM），线程安全，不向终端输出
// - 每条日志写入后立即 flush，程序崩溃也不丢已打印内容
// - 最近 20 份，超出的按文件名（字典序即时间序）自动清理

void LogInit();      // 程序入口最先调用
void LogShutdown();  // 程序退出前调用

// fmt 为宽字符格式串，用法同 wprintf（%s 对应 wchar_t*）
void LogWrite(const wchar_t* fmt, ...);

#define LOG_INFO(...)  LogWrite(L"[I] " __VA_ARGS__)
#define LOG_WARN(...)  LogWrite(L"[W] " __VA_ARGS__)
#define LOG_ERR(...)   LogWrite(L"[E] " __VA_ARGS__)
