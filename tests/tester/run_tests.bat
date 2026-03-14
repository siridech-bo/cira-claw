@echo off
REM CiRA CLAW Tester - Windows Runner
REM Runs black-box HTTP tests against runtime

setlocal

REM Configuration
set RUNTIME=..\..\runtime\build\test_stream.exe
set MODEL=..\..\test_signal_model
set PORT=8089

REM Check if runtime exists
if not exist "%RUNTIME%" (
    echo ERROR: Runtime not found: %RUNTIME%
    echo.
    echo Please build the runtime first:
    echo   cd runtime/build
    echo   cmake --build .
    exit /b 1
)

REM Check if model exists
if not exist "%MODEL%" (
    echo ERROR: Test model not found: %MODEL%
    echo.
    echo Please generate test model first:
    echo   python tools/make_test_signal_model.py test_signal_model
    exit /b 1
)

REM Check if Python dependencies are installed
python -c "import requests" 2>nul
if errorlevel 1 (
    echo Installing Python dependencies...
    pip install -r requirements.txt
    if errorlevel 1 (
        echo ERROR: Failed to install dependencies
        exit /b 1
    )
)

REM Run tests
echo.
echo ========================================
echo CiRA CLAW HTTP Tester
echo ========================================
echo Runtime: %RUNTIME%
echo Model:   %MODEL%
echo Port:    %PORT%
echo ========================================
echo.

python tester.py --runtime "%RUNTIME%" --model "%MODEL%" --port %PORT%
set EXIT_CODE=%ERRORLEVEL%

endlocal
exit /b %EXIT_CODE%
