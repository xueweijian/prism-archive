# PrismArchive 本地开发环境模板（提交进仓库，供每个 worktree 复制）
#
# 为什么有这个文件：真正的 dev-env.ps1 含本机路径，被 .git/info/exclude 排除，
# 所以**每个新 worktree 都没有它**——2026-11-05 一晚就有三张卡各自造了一份、
# 其中一份把构建目录指错了源码树（见 CONTRACTS D36）。
#
# 用法：
#   Copy-Item tools/dev-env.example.ps1 <worktree>/dev-env.ps1
#   然后按本机实际路径改下面 param 的默认值（或直接传参）
#   . .\dev-env.ps1
#   cmake / ctest / python 都在同一个 pwsh 里继续跑
#
# 注意：local_gates.ps1 需要 PATH 里有 python（否则 preflight/manifest 会报
#       "python 3.8+ not found"），所以务必先 dot-source 本脚本再调它。

param(
  [string]`$PythonHome   = 'E:\DevTools\Python312',
  [string]`$SevenZipHome = 'E:\DevTools\7-Zip',
  [string]`$BuildDir     = 'E:\build\prism',
  [string]`$TempDir      = 'E:\Temp',
  [string]`$VcVars       = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
  [string]`$NinjaDir     = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $VcVars)) { throw "vcvars64.bat not found: $VcVars (is VS 2022 BuildTools installed?)" }

# 1. MSVC + Windows SDK into this process (vcvars only affects cmd)
cmd /c "`"$VcVars`" >nul 2>&1 && set" | ForEach-Object {
    if (`$_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:`$(`$matches[1])" -Value `$matches[2] }
}

# 2. PATH: python / 7-Zip / ninja first (beats the WindowsApps python stub)
$env:PATH = "$PythonHome;$PythonHome\Scripts;$SevenZipHome;$NinjaDir;$env:PATH"

# 3. the engine the exe and the integration tests resolve
if (Test-Path "$SevenZipHome\7z.dll") { $env:PRISM_7Z_DLL = "$SevenZipHome\7z.dll" }

# 4. temp + pip cache off the system drive
New-Item -ItemType Directory -Force -Path $TempDir, "$TempDir\pipcache" | Out-Null
$env:TMP  = $TempDir
$env:TEMP = $TempDir
$env:PIP_CACHE_DIR = "$TempDir\pipcache"

Write-Host "PrismArchive dev env ready" -ForegroundColor Green
Write-Host ("  cl       : " + (Get-Command cl.exe -ErrorAction SilentlyContinue).Source)
Write-Host ("  cmake    : " + (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source)
Write-Host ("  python   : " + (Get-Command python.exe -ErrorAction SilentlyContinue).Source)
Write-Host ("  7z.dll   : " + $env:PRISM_7Z_DLL)
Write-Host ("  build dir: " + $BuildDir)
