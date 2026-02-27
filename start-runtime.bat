@echo off
echo Starting CiRA Runtime...
cd /d "D:\CiRA Claw\cira-edge\runtime\build"
test_stream.exe -m "D:\CiRA Claw\models" -p 8080
