@echo off
REM ============================================================
REM  JPOG Co-op v3 - ONE-CLICK INSTALLER. Double-click. Re-run any time.
REM  Installs the mod next to SimJP.exe and applies the data tweaks as
REM  text patches to YOUR game files (originals kept as *.prejpogmod.bak).
REM ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
