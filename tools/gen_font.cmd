@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gen_font.ps1" %*
echo.
echo Done. Press any key to close.
pause >nul
