@echo off
echo Stopping CiRA Edge System...
echo.

:: Kill go2rtc (WebRTC streaming service)
taskkill /F /IM go2rtc-windows-amd64.exe 2>nul
if %errorlevel%==0 (
    echo go2rtc stopped.
) else (
    echo No go2rtc process found.
)

:: Kill Node.js processes (gateway and dashboard)
taskkill /F /IM node.exe 2>nul
if %errorlevel%==0 (
    echo Node.js processes stopped.
) else (
    echo No Node.js processes found.
)

:: Kill the runtime
taskkill /F /IM test_stream.exe 2>nul
if %errorlevel%==0 (
    echo Runtime stopped.
) else (
    echo No runtime process found.
)

echo.
echo All services stopped.
pause
