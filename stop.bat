@echo off
setlocal enabledelayedexpansion
title Stop Claude Usage Server
cd /d "%~dp0"

REM ── 1. Stop the Python usage server ──────────────────────────────────────────
if not exist server.pid (
  echo No server.pid found - server may not be running.
) else (
  set /p PID=<server.pid

  REM Only kill if this PID is actually a python process, so we never kill an
  REM unrelated program that happened to reuse the PID after a crash.
  tasklist /fi "PID eq !PID!" /fo csv 2>nul | findstr /i "python" >nul
  if errorlevel 1 (
    echo PID !PID! is not a running python process - removing stale server.pid.
    del server.pid 2>nul
  ) else (
    taskkill /pid !PID! /f >nul 2>&1
    del server.pid 2>nul
    echo Usage server stopped ^(PID !PID!^).
  )
)

REM ── 2. Kill any Claude CLI windows (cmd windows titled "Claude CLI") ─────────
REM   These are the windows started by start.bat with title "Claude CLI".
REM   We kill the whole window group by title via taskkill on the cmd host.
taskkill /fi "WINDOWTITLE eq Claude CLI" /f >nul 2>&1
if not errorlevel 1 (
  echo Claude CLI window closed.
)

REM ── 3. Kill claude.exe / claude processes directly (Claude Code CLI) ─────────
tasklist /fo csv 2>nul | findstr /i "claude.exe" >nul
if not errorlevel 1 (
  taskkill /im claude.exe /f >nul 2>&1
  echo Claude Code process stopped.
)

REM Also catch Node-based Claude Code runner (claude is often a node script)
REM We look for node.exe processes whose command line contains "claude"
powershell -NoProfile -Command ^
  "Get-WmiObject Win32_Process | Where-Object { $_.Name -match 'node' -and $_.CommandLine -match 'claude' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; Write-Host ('Stopped Node/Claude PID ' + $_.ProcessId) }" 2>nul

echo.
echo All Claude processes stopped.
timeout /t 2 >nul
endlocal