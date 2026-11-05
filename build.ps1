# 构建 看看 SeeSee —— 用 Visual Studio Build Tools 自带的环境
# 用法:  powershell -ExecutionPolicy Bypass -File build.ps1 [-Run]

param(
    [switch]$Run,
    [string]$Target = "seesee"
)

$ErrorActionPreference = 'Stop'
$root = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $root) { $root = (Get-Location).Path }
$bt = "D:\DevTools\VS2022BuildTools"

$vcvars = Join-Path $bt "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $bt "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if (-not (Test-Path $vcvars)) { throw "找不到 vcvars64.bat: $vcvars" }
if (-not (Test-Path $cmake)) { throw "找不到 cmake.exe: $cmake" }

# 图标（可选，缺了就用默认图标）
$ico = Join-Path $root "resources\seesee.ico"
if (-not (Test-Path $ico)) {
    python (Join-Path $root "tools\make_icon.py")
}

$cmd = @(
    "call `"$vcvars`" >nul 2>&1",
    "`"$cmake`" -S `"$root`" -B `"$root\build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=`"$ninja`"",
    "`"$cmake`" --build `"$root\build`" --target $Target"
) -join " && "

$sw = [Diagnostics.Stopwatch]::StartNew()
cmd /c $cmd
$code = $LASTEXITCODE
$sw.Stop()

if ($code -ne 0) { throw "构建失败 (exit $code)" }

$exe = Join-Path $root "bin\seesee.exe"
if (Test-Path $exe) {
    $size = (Get-Item $exe).Length
    Write-Host ("构建成功  {0:N1} KB  {1:N0} ms  ->  {2}" -f ($size / 1kb), $sw.Elapsed.TotalMilliseconds, $exe) -ForegroundColor Green
    if ($Run) { Start-Process $exe }
} else {
    Write-Host "构建完成，但没找到 $exe" -ForegroundColor Yellow
}
