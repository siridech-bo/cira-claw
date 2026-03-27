@echo off
echo ============================================
echo   CiRA Edge System Startup
echo ============================================
echo.

:: Check and install dependencies if needed
if not exist "%~dp0node_modules" (
    echo [0/3] Installing Gateway dependencies...
    cd /d "%~dp0"
    call npm install
    echo.
)

if not exist "%~dp0dashboard\node_modules" (
    echo [0/3] Installing Dashboard dependencies...
    cd /d "%~dp0dashboard"
    call npm install
    echo.
)

:: Start C++ Runtime
echo [1/3] Starting C++ Runtime...
start "CiRA Runtime" "%~dp0start-runtime.bat"
timeout /t 2 /nobreak > nul

:: Start Node.js Backend
echo [2/3] Starting Node.js Gateway...
start "CiRA Gateway" "%~dp0start-gateway.bat"
timeout /t 3 /nobreak > nul

:: Start Vue Dashboard
echo [3/3] Starting Vue Dashboard...
start "CiRA Dashboard" "%~dp0start-dashboard.bat"

echo.
echo ============================================
echo   All services started!
echo ============================================
echo.
echo   Runtime:   http://localhost:8080
echo   Gateway:   http://localhost:18790
echo   Dashboard: http://localhost:3000
echo.
echo Press any key to close this window...
pause > nul
