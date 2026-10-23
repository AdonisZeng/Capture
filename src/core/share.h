#pragma once
// 分享到桌面版微信 / QQ：把图片放进剪贴板后，尝试把目标应用切到前台。
// 桌面版微信/QQ 都没有「发送给指定好友」的公开接口，这里只做到
// 「复制图片 + 唤起应用」，选好友与发送由用户在应用内完成。
#include <windows.h>

enum class ShareApp
{
    WeChat,
    QQ,
};

// 已运行则恢复并尝试置前；未运行则按 App Paths / 协议拉起。
// 返回 false 表示既没找到也没能启动（剪贴板内容不受影响）。
bool ActivateShareApp(ShareApp app);
