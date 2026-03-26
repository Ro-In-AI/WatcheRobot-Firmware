@echo off
setlocal

powershell -ExecutionPolicy Bypass -File "%~dp0run-lane.ps1" %*
exit /b %ERRORLEVEL%
