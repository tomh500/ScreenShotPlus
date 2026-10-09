param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $taskVswhere)) { throw '未找到 VS Installer / vswhere。' }
$taskMsbuild = & $taskVswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$taskMsbuild) { throw '未找到 MSBuild。' }
Push-Location $taskRoot
try {
    & $taskMsbuild ScreenShotPlus.sln /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw '应用构建失败。' }
    $taskOutput = Join-Path $taskRoot "x64\$Configuration\ScreenShotPlus.exe"
    Write-Output "构建输出：$taskOutput"
    Write-Output "文件版本：$((Get-Item -LiteralPath $taskOutput).VersionInfo.FileVersion)"
    if ($Test) {
        & $taskMsbuild tests\ScreenShotPlus.Tests.vcxproj /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /nologo /verbosity:minimal
        if ($LASTEXITCODE -ne 0) { throw '测试构建失败。' }
        & ".\build\tests\$Configuration\ScreenShotPlus.Tests.exe"
        if ($LASTEXITCODE -ne 0) { throw '测试失败。' }
    }
    if ($Package) {
        if ($Configuration -ne 'Release') { throw '仅允许打包 Release。' }
        $taskDist = Join-Path $taskRoot 'dist\ScreenShotPlus'
        New-Item -ItemType Directory -Force -Path $taskDist | Out-Null
        Copy-Item -LiteralPath (Join-Path $taskRoot 'x64\Release\ScreenShotPlus.exe') -Destination $taskDist
        $taskPackageFiles = @((Join-Path $taskDist 'ScreenShotPlus.exe'))
        foreach ($taskDoc in @('README.md', 'THIRD_PARTY_NOTICES.md', 'VERIFICATION.md')) {
            Copy-Item -LiteralPath (Join-Path $taskRoot $taskDoc) -Destination $taskDist
            $taskPackageFiles += Join-Path $taskDist $taskDoc
        }
        Compress-Archive -LiteralPath $taskPackageFiles -DestinationPath (Join-Path $taskRoot 'dist\ScreenShotPlus-x64.zip') -Force
        Write-Output "发布目录：$taskDist"
    }
} finally { Pop-Location }
