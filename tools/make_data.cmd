@echo off
rem Windows helper: drag the Carmageddon II folder onto this file,
rem or run:  make_data.cmd "C:\GOG Games\Carmageddon 2"
if "%~1"=="" (
  echo Usage: make_data.cmd "path\to\Carmageddon II"
  pause
  exit /b 1
)
where py >nul 2>nul && (set PY=py -3) || (set PY=python)
%PY% "%~dp0make_data.py" "%~1" "%~dp0..\site" %2 %3 %4
pause
