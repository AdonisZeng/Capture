#pragma once
// 开机自启动：写入 HKCU\Software\Microsoft\Windows\CurrentVersion\Run，
// 仅当前用户权限、不弹 UAC；值名固定为 "Capture"，命令为带引号的当前 exe 路径

// 注册表值是否存在且指向当前 exe（不存在、指向旧位置均视为未启用）
bool AutostartIsEnabled();
// 写入/覆盖注册表值（始终指向当前 exe）；失败返回 false
bool AutostartEnable();
// 删除注册表值（值不存在也视为成功）；失败返回 false
bool AutostartDisable();
