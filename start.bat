@echo off
setlocal enabledelayedexpansion
title Claude Usage Server
cd /d "%~dp0"

where pythonw >nul 2>nul
if errorlevel 1 (
  echo Error: pythonw not found. Install Python with "Add to PATH".
  pause
  exit /b 1
)

if exist server.pid (
  set /p OLDPID=<server.pid
  tasklist /fi "PID eq !OLDPID!" 2>nul | find "!OLDPID!" >nul
  if not errorlevel 1 (
    echo Server already running ^(PID !OLDPID!^). Run stop.bat first.
    pause
    exit /b 1
  ) else (
    echo Stale server.pid found ^(PID !OLDPID! not running^) - cleaning up.
    del server.pid 2>nul
  )
)

start "" /b pythonw server.py
echo Starting server in background...
timeout /t 3 >nul

if exist server.pid (
  echo.
  echo  Server running.
  echo  URL:  http://localhost:8080/
  echo  Log:  %CD%\server.log
  echo.
) else (
  echo Server failed to start. Last log lines:
  if exist server.log (
    powershell -Command "Get-Content server.log -Tail 20"
  )
  pause
)

timeout /t 2 >nul
endlocal