@echo off
rem Opens the live control panel for the mod. Press F10 anywhere to show or hide it.
setlocal
set "ROOT=%~dp0"
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%ROOT%tools\ControlPanel.ps1"
exit /b 0
