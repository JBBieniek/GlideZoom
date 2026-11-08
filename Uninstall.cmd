@echo off
setlocal
set "GLIDEZOOM_SCRIPT=%~dp0scripts\Uninstall.ps1"
if exist "%~dp0Uninstall.ps1" set "GLIDEZOOM_SCRIPT=%~dp0Uninstall.ps1"
set "GLIDEZOOM_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "GLIDEZOOM_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%GLIDEZOOM_SCRIPT%" (
  echo Missing Uninstall.ps1. Use the complete archive or the installed Uninstall.cmd.
  pause
  exit /b 1
)
"%GLIDEZOOM_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; try { $q = [char]34; $line = '-NoProfile -ExecutionPolicy Bypass -File ' + $q + $env:GLIDEZOOM_SCRIPT + $q; $hostPath = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'; $p = Start-Process -FilePath $hostPath -Verb RunAs -ArgumentList $line -PassThru -Wait; exit $p.ExitCode } catch { Write-Host ('Uninstallation could not start: ' + $_.Exception.Message); exit 1 }"
set "GLIDEZOOM_RESULT=%ERRORLEVEL%"
if not "%GLIDEZOOM_RESULT%"=="0" (
  echo Uninstallation did not complete. Exit code: %GLIDEZOOM_RESULT%
  pause
)
exit /b %GLIDEZOOM_RESULT%
