<#
    构建后生成 sha256sum 格式的校验文件，放在 exe 旁边（<exe 名>.exe.sha256）。

    由 Capture.vcxproj 的 Release|Win32 与 Release|x64 两个配置在 PostBuildEvent
    里调用，所以编译完打开输出目录就能直接拿到成对的文件，不用再手工跑一遍。

    格式：<64 位小写十六进制><两个空格><文件名><换行>
      - 十六进制必须连续 64 位、不能被换行拆开：src/core/update.cpp 的
        ParseSha256Text 取「首个长度 >= 64 的连续十六进制段」，其余一律跳过。
      - 十六进制之后的文件名对解析器无影响，可以写构建时的原始名字。发布时把
        .exe 与本文件一起改名成 Capture-<tag>-<arch>.exe[.sha256] 即可，内容不用动。
      - 用 ASCII 且无 BOM 落盘（WriteAllText + Encoding.ASCII），避免 UTF-8 BOM
        混进文件开头影响人工核对。

    两个必须记住的坑（都是实测踩出来的，改动前先读完）：

    1. **本文件必须存为带 BOM 的 UTF-8。** 调用方是 Windows PowerShell 5.1 的
       powershell.exe，它对无 BOM 的 .ps1 一律按 ANSI（中文系统=GBK 936）解码，
       中文注释被解成乱码，其中某些字节序列会连行尾的引号/换行一起吞掉，报的却是
       莫名其妙的语法错误（实测炸在下一行赋值语句的引号上）。与 C++ 侧「MSVC 必须
       带 /utf-8」是同一类问题：编译器/解释器按默认代码页解无 BOM 的 UTF-8。
    2. **不能用 Get-FileHash。** 它是 Microsoft.PowerShell.Utility 模块导出的，
       靠模块自动加载；而 MSBuild 的 PostBuildEvent 环境里 PSModulePath 是坏的，
       自动加载失败，报「无法将"Get-FileHash"项识别为 cmdlet」（同样是实测）。
       故下面直接用 System.Security.Cryptography，只依赖 BCL。

    失败时以非 0 退出码结束 —— PostBuildEvent 会让整个构建失败。这是故意的：
    发布产物缺校验文件恰好是「应用内自动更新静默失效」的主因，宁可构建红掉。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ExePath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    throw "gen-sha256: 找不到 exe '$ExePath'"
}

$full = (Resolve-Path -LiteralPath $ExePath).Path

# 只依赖 BCL，理由见文件头第 2 条
$algo = [System.Security.Cryptography.SHA256]::Create()
$stream = [System.IO.File]::OpenRead($full)
try {
    $digest = $algo.ComputeHash($stream)
}
finally {
    $stream.Dispose()
    $algo.Dispose()
}

# x2 = 每字节两位十六进制且补零，小写，与 GNU sha256sum 输出一致
$hash = -join ($digest | ForEach-Object { $_.ToString('x2') })

if ($hash.Length -ne 64) {
    throw "gen-sha256: SHA256 长度异常（$($hash.Length)，应为 64）"
}

$out = "$full.sha256"
# 用字符串拼接而不是 "{0}`n" 双引号格式串：反引号转义只在双引号里生效，
# 单引号里的 `n 会变成字面的反引号加 n，被当成十六进制后面的「垃圾」写进文件。
$line = $hash + '  ' + [System.IO.Path]::GetFileName($full) + [char]10
[System.IO.File]::WriteAllText($out, $line, [System.Text.Encoding]::ASCII)

Write-Host "[sha256] $out"
Write-Host "         $hash"

exit 0