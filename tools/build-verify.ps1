<#
.SYNOPSIS
    构建并校验 ScreenShotPlus，输出到独立目录后同步到标准输出路径。

.DESCRIPTION
    当 x64\<Configuration>\ScreenShotPlus.exe 被正在运行的程序占用时，链接器无法覆盖它。
    本脚本始终先构建到 build\verify\<Configuration>\，验证通过后再把可执行文件同步到
    x64\<Configuration>\，因此构建结果不会因为程序正在运行而失败。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-verify.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-verify.ps1 -Configuration Debug
#>
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $taskVswhere)) { throw '未找到 VS Installer / vswhere。' }
$taskMsbuild = & $taskVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$taskMsbuild) { throw '未找到 MSBuild。' }

$taskVerify = if ($OutDir) { $OutDir } else { Join-Path $taskRoot "build\verify\$Configuration" }
$taskCanonicalDir = Join-Path $taskRoot "x64\$Configuration"
$taskBuilt = Join-Path $taskVerify 'ScreenShotPlus.exe'
$taskTarget = Join-Path $taskCanonicalDir 'ScreenShotPlus.exe'
$taskLocked = Join-Path $taskCanonicalDir 'ScreenShotPlus.locked.exe'

Push-Location $taskRoot
try {
    & $taskMsbuild ScreenShotPlus.sln /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 `
        "/p:OutDir=$taskVerify\" /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw '应用构建失败。' }
    if (!(Test-Path -LiteralPath $taskBuilt)) { throw "构建未生成 $taskBuilt。" }

    # 同步到标准输出路径；被占用的旧文件移到一旁而不是删除失败。
    New-Item -ItemType Directory -Force -Path $taskCanonicalDir | Out-Null
    if (Test-Path -LiteralPath $taskTarget) {
        try { Remove-Item -LiteralPath $taskTarget -Force -ErrorAction Stop }
        catch { Rename-Item -LiteralPath $taskTarget -NewName 'ScreenShotPlus.locked.exe' -ErrorAction Stop }
    }
    Copy-Item -LiteralPath $taskBuilt -Destination $taskTarget -Force
    if (Test-Path -LiteralPath $taskLocked) { Remove-Item -LiteralPath $taskLocked -Force -ErrorAction SilentlyContinue }

    Write-Output "构建通过：$taskBuilt"
    Write-Output "已同步到：$taskTarget"
} finally { Pop-Location }
