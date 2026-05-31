@echo off
setlocal
cd /d "%~dp0.."

set "PY=C:\Users\lamme\.platformio\penv\Scripts\python.exe"
set "PIO=C:\Users\lamme\.platformio\penv\Scripts\platformio.exe"

echo Building WebUI.h from src\WebUI.html...
"%PY%" tools\build_web_ui.py
if errorlevel 1 goto fail

echo.
echo Building firmware...
"%PIO%" run
if errorlevel 1 goto fail

echo.
echo Done.
pause
exit /b 0

:fail
echo.
echo Failed.
pause
exit /b 1
