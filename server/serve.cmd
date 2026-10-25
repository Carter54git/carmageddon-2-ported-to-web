@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1" 8080
if errorlevel 1 (
  where python >nul 2>nul && python "%~dp0relay.py" 8080
)
pause
