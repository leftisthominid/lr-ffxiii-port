@echo off
rem Lightning Returns builder: double-click this file. It starts builder.ps1 next to it.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0builder.ps1" %*
rem Exit code 10: the person chose not to go on; the window just closes.
if %ERRORLEVEL% EQU 10 exit /b 0
echo.
pause
