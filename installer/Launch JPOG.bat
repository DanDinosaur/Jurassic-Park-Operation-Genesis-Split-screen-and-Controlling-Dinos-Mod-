@echo off
REM  JPOG Co-op v3 - play. Starts Jurassic Park: Operation Genesis with the mod.
setlocal
set "GAME="
if exist "%~dp0game_path.txt" set /p GAME=<"%~dp0game_path.txt"
if not defined GAME ( echo Run install.bat first. & pause & exit /b 1 )
if not exist "%GAME%\SimJP.exe" ( echo SimJP.exe not found in "%GAME%" - run install.bat again. & pause & exit /b 1 )
cd /d "%GAME%"
start "" "SimJP.exe"
endlocal
