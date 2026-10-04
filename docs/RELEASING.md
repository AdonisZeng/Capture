# 发布流程（Release Checklist）

本文是发一个 Capture 版本的完整清单。**发布窗口的产物命名与 `.sha256`
附件是应用内自动更新能工作的硬性前提**——名字对不上，用户点「检查更新」
会得到「无法解析 GitHub 返回的发布信息」。

## 0. 前置条件

- 本机工具链在 D 盘（见 `AGNETS.md` 的「本机工具链位置」）
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
仓库里的 `AGNETS.md`「变更记录」是原始素材，但只记功能级变更，粒度比 CHANGELOG 粗。

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

发布只取 Release 产物：

| 配置 | 输出 |
| --- | --- |
| Release \| x64 | `x64\Release\Capture.exe` |
| Release \| Win32 | `Release\Capture.exe` |

## 5. 冒烟测试（对着真实 Release 产物做）

改的是同一个 exe，务必确认这四件事：

1. 启动不崩，exe 属性（属性 → 详细信息）里版本号是新版本号
2. 设置页「版本与更新」显示当前版本正确，且点「检查更新」不报解析错误
3. 录一段 10 秒，确认录制链路没被影响
4. 截一张图，确认 WIC 存盘正常

## 6. 打 tag 并推送

```powershell
git tag -a v1.1.0 -m "Capture v1.1.0"
git push origin main
git push origin v1.1.0
```

tag 名必须与 `version.h` 一致，且带 `v` 前缀（小写）。

## 7. 生成 `.sha256` 附件

每个架构一个，文件名固定为 `<exe 名>.sha256`，内容是 `sha256sum` 格式：

```powershell
# 在仓库根目录执行；产物先拷到一个干净的临时目录，避免带 config/ log/
$stage = "$env:TEMP\Capture-release-v1.1.0"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item x64\Release\Capture.exe   "$stage\Capture-v1.1.0-x64.exe"
Copy-Item Release\Capture.exe       "$stage\Capture-v1.1.0-win32.exe"

foreach ($a in 'x64','win32') {
    Push-Location $stage
    # 产出 "<64位十六进制>  <文件名>"（两个空格），与 GNU sha256sum 一致
    Get-FileHash ".\Capture-v1.1.0-$a.exe" -Algorithm SHA256 |
        ForEach-Object { "$($_.Hash.ToLower())  Capture-v1.1.0-$a.exe" } |
        Set-Content -Encoding ascii ".\Capture-v1.1.0-$a.exe.sha256"
    Pop-Location
}
```

> **必须是两个空格，且文件名跟在后面。** 解析器取「首个长度 ≥64 的连续十六进制段」，
> 空格数量与文件名都无所谓，但十六进制不能被换行拆开。
> PowerShell 的 `Get-FileHash` 默认输出大写，已在上面的命令里转小写；
> 其实解析器会归一化大小写，这里保持小写只是为了和 `sha256sum` 输出一致、便于人工核对。

## 8. 创建 GitHub Release

四个附件全部上传（缺 `.sha256` 的那个架构用户就装不了）：

```powershell
gh release create v1.1.0 `
  "$stage\Capture-v1.1.0-x64.exe" `
  "$stage\Capture-v1.1.0-x64.exe.sha256" `
  "$stage\Capture-v1.1.0-win32.exe" `
  "$stage\Capture-v1.1.0-win32.exe.sha256" `
  --title "Capture v1.1.0" `
  --notes-file CHANGELOG.md
```

要点：

- **不要勾选 prerelease / draft。** 应用内检查走 `/releases/latest`，
  它只返回最新的**正式**发布，草稿与预发布都不算数——勾错了等于没发
- `--notes` 写用户看得懂的功能变化（会自动出现在应用内更新弹窗里）。
  直接用 CHANGELOG 全文也行，但建议裁掉内部实现细节
- 上传后**自己验一遍**：用浏览器打开那个 Release 页，确认四个附件都在

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

当前 exe **没有 Authenticode 签名**，自动更新的可信度只依赖两层：
WinHTTP 的 HTTPS 证书链校验 + 发布物 SHA-256 比对。这挡得住传输损坏与降级，
挡不住 CA 被攻破或本机被装了根证书的场合。

要闭环需要一张代码签名证书（OV/EV），在第 4 步之后、第 7 步之前插入：

```powershell
# 用 signtool 对两个产物签名（证书需含私钥并可导出 PFX）
& 'C:\Program Files (x86)\Windows SDK\10\bin\10.0.26100.0\x64\signtool.exe' sign /fd SHA256 `
   /f cert.pfx /p $env:CERT_PWD /tr http://timestamp.digicert.com /td SHA256 `
   "$stage\Capture-v1.1.0-x64.exe"
# 签名后再算 sha256（顺序不能反：签名会改文件内容）
```

签完还可以在 `core/update.cpp` 的下载校验里加一步
`WinVerifyTrust` 验签，这样才真正闭环。有签名证书之前，现有实现是可接受的。
