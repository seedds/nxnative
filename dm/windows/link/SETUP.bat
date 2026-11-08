@echo off
net session >nul 2>nul || (echo Right-click this file and choose Run as administrator. & pause & exit /b 1)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
echo.
pause
