@echo off
setlocal enabledelayedexpansion
title Stop Claude Usage Server
cd /d "%~dp0"

if not exist server.pid (
  echo No server.pid found - server may not be running.
  timeout /t 2 >nul
  exit /b 1
)

set /p PID=<server.pid

REM Only kill if this PID is actually a python process, so we never kill an
REM unrelated program that happened to reuse the PID after a crash.
tasklist /fi "PID eq !PID!" /fo csv 2>nul | findstr /i "python" >nul
if errorlevel 1 (
  echo PID !PID! is not a running python process - removing stale server.pid.
  del server.pid 2>nul
  timeout /t 2 >nul
  exit /b 0
)

taskkill /pid !PID! /f >nul 2>&1
del server.pid 2>nul

echo Server stopped ^(PID !PID!^).
timeout /t 2 >nul
endlocal