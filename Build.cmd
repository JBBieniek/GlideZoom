@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Build.ps1" %*
if errorlevel 1 (
  echo Build failed. See the console output above.
  exit /b 1
)
exit /b 0
