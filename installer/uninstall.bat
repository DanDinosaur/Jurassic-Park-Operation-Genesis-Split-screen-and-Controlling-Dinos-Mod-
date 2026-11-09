@echo off
REM  JPOG Co-op v2 - UNINSTALL: removes the mod and restores every original file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
echo.
pause
