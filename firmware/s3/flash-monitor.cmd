@echo off
setlocal

powershell -ExecutionPolicy Bypass -File "%~dp0tools\flash-monitor.ps1" %*
exit /b %ERRORLEVEL%
