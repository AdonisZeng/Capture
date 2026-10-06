# Capture

轻量级 Windows 屏幕录制与截图工具。捕获主显示器画面（或指定窗口、框选区域），
可选录制系统声音与麦克风，编码为 MP4（H.264 + AAC）。单文件、免安装、可后台驻留托盘。

![Windows 桌面应用](https://img.shields.io/badge/platform-Windows%2010%201803+-0078D4)
![语言](https://img.shields.io/badge/lang-C%2B%2B20-blue)
![许可](https://img.shields.io/badge/license-MIT-green)

## 功能

**录屏**
- 画面来源可选：显示器 / 指定窗口 / 框选区域
- 系统声音与麦克风双路，各自独立开关、独立选设备、独立增益，自动混音
- 实时预览、电平表、画质预设（帧率 / 分辨率 / 码率）
- 暂停 / 继续，暂停段不会留在成片里
- 延时开始（0 / 3 / 5 / 10 秒）
- H.264 硬件加速优先，无硬编时自动回退软件编码

**截图**
- 全屏遮罩拖拽框选，带 8 倍放大镜与方向键微调
- PNG / JPEG / BMP / TIFF / GIF 五种格式，存盘与复制到剪贴板可同时进行
- 窗口隐藏时快捷键依然可用

**其他**
- 托盘驻留后台运行，录制中托盘图标带红色标记
- 全局快捷键，可自定义并在冲突时自动回退
- 开机自启动
- 深色 / 浅色主题
- **应用内自动更新**：从 GitHub Releases 检查新版本，下载后校验 SHA-256，
  自动替换并重启

## 环境要求

- Windows 10 1803（17763）或更高 —— 依赖 Windows.Graphics.Capture。
  版本不足时程序会在启动时直接提示并退出，不会带着一个看不懂的报错继续运行。
- **无需安装任何运行库**：发布产物静态链接 CRT，直接双击即可运行。
- 编译需 Visual Studio 2022 及以上（MSVC v143 或更高工具集，C++20）

## 下载

从 [Releases](https://github.com/AdonisZeng/Capture/releases) 下载与系统架构对应的一份
（`x64` 用于 64 位系统，`win32` 用于 32 位），连同同名 `.exe.sha256` 一起下载保存即可，
校验文件用于自行核对文件完整性。

## 从源码编译

```powershell
# 用 Visual Studio 打开 Capture.slnx，选 Release | x64 直接编译
# 或命令行：
& 'D:\Software\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  Capture.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
```

产物为单文件 `Capture.exe`，无外部依赖（Dear ImGui 以源码形式内嵌在 `imgui/`）。
Release 配置静态链接 CRT，Debug 用 `/MDd`。

仓库根目录的 `app.manifest` 会由 MSBuild 合并进 exe（`AdditionalManifestFiles`，
四个配置都挂了，漏挂的那个配置就没有），内含 `supportedOS`（Win10/11）、
`PerMonitorV2`、长路径与 UTF-8 代码页声明 —— 缺了这些，系统会把进程按 Win7 处理。

## 使用

1. 运行 `Capture.exe`，主窗口显示主显示器实时预览
2. 「截屏」页选好输出目录与格式，点截图或按 `Ctrl+Alt+S`
3. 「录屏」页选好画面来源、帧率与画质，按 `Ctrl+Alt+R` 开始录制，再按一次结束
4. 默认快捷键（可在设置页修改）：

| 操作 | 快捷键 |
| --- | --- |
| 区域截图 | `Ctrl+Alt+S` |
| 开始 / 停止录制 | `Ctrl+Alt+R` |
| 显示主窗口 | `Ctrl+Alt+C` |

### 配置文件与日志

配置是 UTF-8 JSON，**存放在 exe 同级的 `config/settings.json`**，日志在同级的 `log/`。
首次启动时文件不存在，程序会自动建目录并写入一份默认配置（输出目录取桌面）。

把 exe 和 `config/`、`log/` 放在同一个文件夹里，整个文件夹拷到别的机器即为「绿色版」。

> **只读安装目录的例外**：如果 exe 位于不可写的位置（例如装在 `C:\Program Files`），
> 配置与日志会自动改放 `%APPDATA%\Capture\`（即「应用数据 / 罗aming」）。
> 实际落点会写在启动日志的第一行，删掉 `config/settings.json` 即可恢复默认设置。

已知限制：

- exe **未做代码签名**，首次运行时 SmartScreen 可能提示「未知发布者」，需手动选择「仍要运行」。
- 中文界面依赖系统里的微软雅黑（`msyh.ttc`）。在删除了中文字体的精简版 Windows 上
  界面会显示方块（不影响录制与截图功能本身）。

## 版本与更新

版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)，**单一真源是
[`src/core/version.h`](src/core/version.h)** —— 改这一处即可，
它同时驱动 exe 属性、界面显示、以及与 Release tag 的比较。

用户可在「设置 → 版本与更新」里检查更新，或由程序每天静默检查一次。
更新包来自本仓库的 GitHub Releases，下载后会自动校验 SHA-256。

> exe 目前**未做代码签名**。自动更新的可信度依赖 HTTPS 证书链校验与
> SHA-256 比对两层，请只从官方 Release 获取更新包。

维护者发布流程见 [`docs/RELEASING.md`](docs/RELEASING.md)，
版本变化见 [`CHANGELOG.md`](CHANGELOG.md)，架构与踩坑记录见 [`AGENTS.md`](AGENTS.md)。

## 第三方组件

- [Dear ImGui](https://github.com/ocornut/imgui) —— MIT，源码内嵌于 `imgui/`

许可见 [`LICENSE`](LICENSE) 与 `imgui/LICENSE.txt`。

## 许可

MIT
