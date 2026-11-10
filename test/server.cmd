@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -Mode server %*
if errorlevel 1 pause
