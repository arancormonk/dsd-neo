@echo off
setlocal ENABLEDELAYEDEXPANSION

REM Resolve script directory
set "ROOT=%~dp0"
set "BIN=%ROOT%bin"
set "ETC=%ROOT%etc"

REM Ensure our bin comes first in PATH so cyg*.dll are resolved
set "PATH=%BIN%;%PATH%"

REM Start PulseAudio server if available; ignore errors if already running
if exist "%BIN%\pulseaudio.exe" (
  echo Starting PulseAudio...
  if exist "%ETC%\pulse\default.pa" (
    start "" "%BIN%\pulseaudio.exe" -n --daemonize=1 --exit-idle-time=-1 -F "%ETC%\pulse\default.pa"
  ) else (
    start "" "%BIN%\pulseaudio.exe" -n --daemonize=1 --exit-idle-time=-1
  )
)

echo Launching dsd-neo...
"%BIN%\dsd-neo.exe" %*

REM Optionally stop PulseAudio here; by default, leave it running for reuse
REM taskkill /IM pulseaudio.exe /F >nul 2>nul

endlocal
