@echo off
echo Stopping CiRA Edge System...
echo.

:: Kill Node.js processes
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
