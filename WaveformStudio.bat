@echo off
rem Launches the DualSense waveform studio, building the player first if needed.
setlocal
set "ROOT=%~dp0"

if not exist "%ROOT%tools\waveform_player.exe" (
    echo Building the waveform player, this only happens once...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%Build-Tools.ps1"
    if errorlevel 1 goto build_failed
    if not exist "%ROOT%tools\waveform_player.exe" goto build_failed
)

start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%ROOT%tools\WaveformStudio.ps1"
exit /b 0

:build_failed
echo.
echo The waveform player could not be built. Visual Studio Build Tools with the
echo C++ workload are required.
echo.
pause
exit /b 1
