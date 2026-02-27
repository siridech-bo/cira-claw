@echo off
echo Starting CiRA Edge System...
echo.

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
echo All services started!
echo.
echo   Runtime:   http://localhost:8080
echo   Gateway:   http://localhost:18790
echo   Dashboard: http://localhost:3000
echo.
pause
