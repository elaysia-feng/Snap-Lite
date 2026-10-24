<#
.SYNOPSIS
    Snap-Lite 的唯一本地构建入口。

.DESCRIPTION
    自动定位 Visual Studio Build Tools、导入 vcvars64 环境、调用 CMake + Ninja
    完成配置与构建。目的：消除 build/ 与 build_release/ 两套目录、两套工具链并存
    导致"本地能过、CI 挂"的问题（审计报告 P1-24）。

.EXAMPLE
    pwsh -File scripts/build_local.ps1
    pwsh -File scripts/build_local.ps1 -Tests
    pwsh -File scripts/build_local.ps1 -Clean -Tests
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$Config = 'Release',
    [string]$PythonExe = '',
    [switch]$Tests,
    [switch]$Clean,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repoRoot $BuildDir

function Import-VcVars {
    param([string]$VsRoot)
    $vcvars = Join-Path $VsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "找不到 vcvars64.bat：$vcvars"
    }
    # 让 cmd 执行 vcvars64 并导出结果环境变量，再逐条导入当前进程
    $lines = & cmd.exe /d /s /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "vcvars64.bat 执行失败（退出码 $LASTEXITCODE）"
    }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

function Resolve-Python {
    foreach ($candidate in @('python3', 'python')) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($found) { return $found.Source }
    }
    # PATH 里没有时，探测常见安装位置
    $probes = @(
        'C:\Python312\python.exe',
        'C:\Python311\python.exe',
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe",
        'E:\develop\python_3.11\python.exe'
    )
    foreach ($probe in $probes) {
        if (Test-Path $probe) { return $probe }
    }
    throw '找不到 Python 3（构建期代码生成需要它）。可用 -PythonExe 指定绝对路径。'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "找不到 vswhere.exe：$vswhere"
}
$vsRoot = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw '找不到带 MSVC C++ 工具集的 Visual Studio 安装（需要 Microsoft.VisualStudio.Component.VC.Tools.x86.x64）'
}
Write-Host "[toolchain] VS Build Tools: $vsRoot"

Import-VcVars -VsRoot $vsRoot
Write-Host "[toolchain] cl: $((Get-Command cl.exe -ErrorAction SilentlyContinue).Source)"

$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $ninja)) {
    throw "找不到 ninja.exe：$ninja"
}
$python = if ($PythonExe) { $PythonExe } else { Resolve-Python }
Write-Host "[toolchain] ninja: $ninja"
Write-Host "[toolchain] python: $python"

if ($Clean -and (Test-Path $buildPath)) {
    # 递归删除前确认目标确实位于仓库内，避免 -BuildDir 指向仓库外时被误删。
    $resolved = [System.IO.Path]::GetFullPath($buildPath).TrimEnd('\', '/')
    $root = [System.IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/')
    if (-not $resolved.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "构建目录必须位于仓库内，拒绝删除：$resolved"
    }
    Write-Host "[clean] 移除 $resolved"
    Remove-Item -Recurse -Force $resolved
}

$cmakeArgs = @(
    '-S', $repoRoot,
    '-B', $buildPath,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DPython3_EXECUTABLE=$python"
)
if ($Tests) {
    $cmakeArgs += '-DSNAPLITE_BUILD_INTERACTION_TESTS=ON'
}
if ($PSCulture -eq 'zh-CN') {
    $cmakeArgs += '-DSNAPLITE_MSVC_SHOWINCLUDES_PREFIX=注意: 包含文件:  '
}

Write-Host "[configure] cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败（退出码 $LASTEXITCODE）"
}
if ($ConfigureOnly) {
    return
}

Write-Host "[build] $Config"
& cmake --build $buildPath --config $Config
if ($LASTEXITCODE -ne 0) {
    throw "构建失败（退出码 $LASTEXITCODE）"
}

$exe = Join-Path $buildPath "SnapLite.exe"
if (Test-Path $exe) {
    Write-Host "[ok] $((Get-Item $exe).Length) 字节 -> $exe"
} else {
    Write-Host "[warn] 未找到产物 $exe"
}

if ($Tests) {
    Write-Host '[test] ctest'
    & ctest --test-dir $buildPath -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "测试失败（退出码 $LASTEXITCODE）"
    }
}
