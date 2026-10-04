#pragma once
// 轻量文件日志
// - 每次程序启动在 log 目录下新建一个 TXT，文件名为启动时刻（年月日_时分秒）
// - log 目录定位：从 exe 所在目录向上最多 5 层查找已存在的 "log" 文件夹，
//   找不到则在 exe 目录旁新建一个
// - 文件编码 UTF-8（带 BOM），线程安全，不向终端输出
// - 每条日志写入后立即 flush，程序崩溃也不丢已打印内容

void LogInit();      // 程序入口最先调用
void LogShutdown();  // 程序退出前调用

// fmt 为宽字符格式串，用法同 wprintf（%s 对应 wchar_t*）
void LogWrite(const wchar_t* fmt, ...);

#define LOG_INFO(...)  LogWrite(L"[I] " __VA_ARGS__)
#define LOG_WARN(...)  LogWrite(L"[W] " __VA_ARGS__)
#define LOG_ERR(...)   LogWrite(L"[E] " __VA_ARGS__)
