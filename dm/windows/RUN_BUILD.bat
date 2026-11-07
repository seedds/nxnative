@echo off
cd /d "%~dp0"
py -3.14 --version >nul 2>nul && (py -3.14 run_all.py %* & goto done)
py -3.13 --version >nul 2>nul && (py -3.13 run_all.py %* & goto done)
echo Python 3.14 was not found. From python.org, install the latest 3.14 for Windows (64-bit), then run this again.
:done
echo.
pause
