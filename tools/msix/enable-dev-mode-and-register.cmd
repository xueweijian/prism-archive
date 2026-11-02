@echo off
REM Enable Windows Developer Mode (one time, needs the UAC prompt) and register
REM the PrismArchive sparse package by loose-file registration.
REM
REM Why: the Windows 11 modern context menu is only offered to PACKAGED apps.
REM Packaged apps normally need a code-signing certificate; loose registration
REM ("Add-AppxPackage -Register") needs no certificate at all, but Windows
REM refuses unsigned packages unless Developer Mode or sideloading is on.
REM Both switches live under HKLM and therefore need one elevation -- once.
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Elevating...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" /t REG_DWORD /f /v AllowDevelopmentWithoutDevLicense /d 1
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" /t REG_DWORD /f /v AllowAllTrustedApps /d 1
echo Developer Mode enabled.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0register-loose.ps1"
pause
