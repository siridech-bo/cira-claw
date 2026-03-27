@echo off
echo Starting CiRA Runtime...
cd /d "%~dp0runtime\build"
test_stream.exe -m "%~dp0..\models" -p 8080
