# 发布流程（Release Checklist）

本文是发一个 Capture 版本的完整清单。**发布窗口的产物命名是应用内
自动更新能工作的硬性前提**——名字对不上，用户点「检查更新」
会得到「无法解析 GitHub 返回的发布信息」。

## 0. 前置条件

- 本机工具链在 D 盘（见 `AGENTS.md` 的「本机工具链位置」）
- 仓库远程指向 GitHub：`git remote -v` 应为 `https://github.com/AdonisZeng/Capture.git`
- 已登录 `gh` CLI（若用命令行发版）

## 1. 决定版本号（SemVer）

只改**一个文件**：`src/core/version.h`。

| 位 | 何时递增 | 例子 |
| --- | --- | --- |
| MAJOR | 不兼容变更：`settings.json` 结构破坏性调整、删除既有功能、默认值语义变化 | `2.0.0` |
| MINOR | 新增功能，向后兼容 | `1.1.0` |
| PATCH | 缺陷修复，向后兼容 | `1.0.1` |

该文件同时驱动：exe 属性里的 `FileVersion`/`ProductVersion`、界面左下角显示、
以及与 GitHub Release tag 的新旧比较。**不存在第二份需要手工同步的版本号。**

> RC 编译器不支持字符串化（`#`），所以 `CAPTURE_VERSION_STR` 是显式写出来的字符串。
> `src/core/update.cpp` 顶部有 `static_assert` 校验它与三个整数宏一致，
> 改一处忘另一处会直接编译失败。

## 2. 更新 CHANGELOG.md

在 `CHANGELOG.md` 顶部加一节，标题写新版本号，正文从本次改动整理。
仓库里的 `AGENTS.md`「变更记录」是原始素材，但只记功能级变更，粒度比 CHANGELOG 粗。

## 3. 提交

```powershell
git add -A
git commit -m "release: v1.1.0"
```

## 4. 编译

四个配置全部重建（`/t:Rebuild`，别用增量——版本资源改动容易不被检测到）：

```powershell
$msb = 'D:\Software\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
foreach ($c in 'Release','Debug') { foreach ($p in 'x64','Win32') {
    & $msb Capture.vcxproj /p:Configuration=$c /p:Platform=$p /t:Rebuild /v:minimal /nologo
} }
```

发布只取 Release 产物（文件名即发布名，不用再改名）：

| 配置 | 输出 |
| --- | --- |
| Release \| x64 | `x64\Release\Capture.exe` → 发布为 `Capture-x64.exe` |
| Release \| Win32 | `Release\Capture.exe` → 发布为 `Capture-win32.exe` |

拷出来改个名即可，没有校验文件、不用算哈希。
Debug 配置的产物不发布。

## 5. 冒烟测试（对着真实 Release 产物做）

改的是同一个 exe，务必确认这几件事：

1. 启动不崩，exe 属性（属性 → 详细信息）里版本号是新版本号
2. 设置页「版本与更新」显示当前版本正确，且点「检查更新」不报解析错误
     （下载进度条与下方「启动时自动检查更新」开关的右缘应与卡片内其他控件对齐）
3. 录一段 10 秒，确认录制链路没被影响
4. 截一张图，确认 WIC 存盘正常

### 5.1 必查：产物没有运行库依赖

发布物是「下载即用」的，任何外部 DLL 依赖都会让用户在别人机器上起不来。
**Release 必须静态链接 CRT**（`vcxproj` 里两个 Release 配置都写了
`<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`，别删）。链接完当场验一次：

```powershell
$dumpbin = 'D:\Software\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe'
foreach ($e in 'x64\Release\Capture.exe','Release\Capture.exe') {
  $crt = & $dumpbin /nologo /dependents $e | Select-String 'MSVCP|VCRUNTIME|api-ms-win-crt'
  if ($crt) { throw "$e 仍依赖 VC++ 运行库: $($crt -join ', ')" }
  "OK $e"
}
```

> 曾经踩过：Release 没写 `RuntimeLibrary`，默认 `/MD`，产物导入
> `MSVCP140.dll` / `VCRUNTIME140.dll` / `VCRUNTIME140_1.dll`。
> 那三个 DLL 只随「Visual C++ Redistributable」分发，**系统默认不带**，
> 本机开发因为装过 VS 所以一直正常，用户从 GitHub 下载就会直接失败。

### 5.2 必查：清单已合入

同样两个产物，确认清单在位（缺 `supportedOS` 时系统会按 Win7 处理进程）：

```powershell
$mt = 'C:\Program Files (x86)\Windows SDK\10\bin\10.0.26100.0\x64\mt.exe'
& $mt '-nologo' '-inputresource:x64\Release\Capture.exe;#1' '-out:m.xml'
Select-String -Path m.xml -Pattern 'supportedOS|PerMonitorV2|longPathAware|activeCodePage|SegmentHeap'
Remove-Item m.xml
```

### 5.3 建议：在干净目录里跑一次

模拟用户首次下载：拷到一个空目录，确认 exe 自己会建出 `config\` 与 `log\`，
且 `config\settings.json` 内容是完整默认配置（不是空文件、不是半截 JSON）。

> 注意：**不要在开发树的 `x64\Release\` 里测这个**。那里已经有旧的 `config\`，
> 程序会直接复用，看起来「没问题」其实没验证到新建逻辑。
>
> 也别忘了 `app.manifest` 是通过 `AdditionalManifestFiles` 挂进工程的，
> **四个配置都要挂**，只挂 Release 会导致 Debug 构不出正确的清单。

## 6. 打 tag 并推送

```powershell
git tag -a v1.1.0 -m "Capture v1.1.0"
git push origin main
git push origin v1.1.0
```

tag 名必须与 `version.h` 一致，且带 `v` 前缀（小写）。

## 7. 改名为发布产物名

产物名固定、不带版本号（带版本号会导致用户手动下载后堆出一堆旧文件，
桌面快捷方式也会因文件名变化而失效；自动更新是原地覆盖，本地文件名从不改变）：

```powershell
# 在仓库根目录执行；产物先拷到一个干净的临时目录，避免带 config/ log/
# （程序运行后会在 exe 旁边建这两个目录，别把它们打进发布包）
$stage = "$env:TEMP\Capture-release-v1.1.0"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item x64\Release\Capture.exe  "$stage\Capture-x64.exe"
Copy-Item Release\Capture.exe      "$stage\Capture-win32.exe"
```

> **架构后缀必须保留。** 同一个 Release 下 x64 与 win32 各一个附件，
> GitHub 不允许重名，所以不能都叫 `Capture.exe`。

## 8. 创建 GitHub Release

两个附件全部上传：

```powershell
gh release create v1.1.0 `
  "$stage\Capture-x64.exe" `
  "$stage\Capture-win32.exe" `
  --title "Capture v1.1.0" `
  --notes-file CHANGELOG.md
```

要点：

- **不要勾选 prerelease / draft。** 应用内检查走 `/releases/latest`，
  它只返回最新的**正式**发布，草稿与预发布都不算数——勾错了等于没发
- `--notes` 写用户看得懂的功能变化（会自动出现在应用内更新弹窗里）。
  直接用 CHANGELOG 全文也行，但建议裁掉内部实现细节
- 上传后**自己验一遍**：用浏览器打开那个 Release 页，确认两个附件都在

## 9. 发布后验证自动更新

这是唯一能证明整条链路通了的办法，别跳：

1. 把上一步的 Release 页面在浏览器里打开，确认附件齐全
2. 在**旧版本**（如 v1.0.0）上点设置页「检查更新」
   → 应提示「发现新版本 v1.1.0」
3. 点「下载更新」→ 进度条走完 → 状态变「已下载完成，重启后生效」
4. 点「重启安装」→ 程序退出、短暂黑屏、新版本自动起来
5. 检查：目录里留下过 `Capture.exe.old` 吗？下次启动应被自动清理；
   如果新版本起不来，把它改名回 `Capture.exe` 即可回滚
6. 看 `log/` 里最新那份日志，确认有「更新完成, 已启动新版本」

## 10. 回滚

发现新版本有严重问题：

- **还没人更新**：把 Release 删掉或改成 draft 即可，`/releases/latest` 立刻回退到上一版
- **已经有人更新**：发一个修好的新版本（PATCH+1）。已安装的用户会收到提示。
  不要试图「撤回」已推送的 tag——那会让一部分用户的本地仓库与 Release 对不上

## 后续：给 exe 做代码签名

当前 exe **没有 Authenticode 签名**，自动更新的可信度只有一层：
WinHTTP 的 HTTPS 证书链校验（下载完整性只核对 Release 元数据里的 size，
不做哈希）。这挡得住传输截断，挡不住文件被替换、CA 被攻破、
或本机被装了根证书的场合。对外不要宣称更新包「已验证来源可信」。

要闭环需要一张代码签名证书（OV/EV），在第 4 步之后、第 7 步之前插入：

```powershell
# 用 signtool 对两个产物签名（证书需含私钥并可导出 PFX）
& 'C:\Program Files (x86)\Windows SDK\10\bin\10.0.26100.0\x64\signtool.exe' sign /fd SHA256 `
   /f cert.pfx /p $env:CERT_PWD /tr http://timestamp.digicert.com /td SHA256 `
   "$stage\Capture-x64.exe"
```

签完还可以在 `core/update.cpp` 的下载完成后加一步
`WinVerifyTrust` 验签，这样才真正闭环。有签名证书之前，现有实现是可接受的。
