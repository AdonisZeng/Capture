#ifndef CAPTURE_VERSION_H
#define CAPTURE_VERSION_H

// 版本号单一真源（SemVer）。整个仓库只改这一处：
//   - Capture.rc 的 VERSIONINFO（FILEVERSION / PRODUCTVERSION）
//   - src/core/update.cpp 的界面显示与 GitHub Release 版本比较
//   - docs/RELEASING.md 的发布清单会核对这一处
//
// 约定：只允许 ASCII 字符与预处理器宏。Capture.rc 会 include 本文件，
// 而 RC 预处理器不支持 C++ 语法、也不支持字符串化（# 运算符）与
// 相邻字符串字面量拼接，故这里显式给出字符串宏；update.cpp 里有
// static_assert 校验它与三个整数宏一致，改一处忘另一处会直接编译失败。
//
// 语义（GitHub Release 的 tag 必须与此处一致，形如 v1.2.0）：
//   MAJOR  不兼容变更：settings.json 结构破坏性调整、删除既有功能、默认值语义变化
//   MINOR  新增功能，向后兼容
//   PATCH  缺陷修复，向后兼容

#define CAPTURE_VERSION_MAJOR 1
#define CAPTURE_VERSION_MINOR 0
#define CAPTURE_VERSION_PATCH 2

#define CAPTURE_VERSION_STR   "1.0.2"

#endif   // CAPTURE_VERSION_H
