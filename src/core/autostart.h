#pragma once
// 开机自启动：写入 HKCU\Software\Microsoft\Windows\CurrentVersion\Run，
// 仅当前用户权限、不弹 UAC；值名固定为 "Capture"，命令为带引号的当前 exe 路径
// + " --minimized"（开机由 Explorer 拉起时只驻留托盘，不弹主窗口）

// 注册表值是否存在且指向当前 exe（新旧命令都认：是否带 --minimized 只影响
// 启动时显隐，不影响「已启用」判定；缺标志的旧值由主入口按需补写迁移）
bool AutostartIsEnabled();
// 当前注册表值是否已带最小化启动标志（缺标志的旧值需要补写迁移）
bool AutostartHasMinimizedFlag();
// 写入/覆盖注册表值（始终指向当前 exe，并带 --minimized）；失败返回 false
bool AutostartEnable();
// 删除注册表值（值不存在也视为成功）；失败返回 false
bool AutostartDisable();
