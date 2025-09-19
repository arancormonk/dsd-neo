@echo off
setlocal ENABLEDELAYEDEXPANSION

REM Optional: allow disabling the final pause with --no-pause
set "DO_PAUSE=1"
if /I "%~1"=="--no-pause" (
  set "DO_PAUSE=0"
  shift
)

REM Resolve script directory and expected layout
set "ROOT=%~dp0"
set "BIN=%ROOT%bin"
set "ETC=%ROOT%etc"

REM If invoked from repository layout (packaging\windows-cygwin), hop two levels up
if not exist "%BIN%\dsd-neo.exe" (
  if exist "%ROOT%..\..\bin\dsd-neo.exe" (
    set "ROOT=%ROOT%..\..\"
    set "BIN=%ROOT%bin"
    set "ETC=%ROOT%etc"
  )
)

REM Validate expected binary exists
if not exist "%BIN%\dsd-neo.exe" (
  echo Error: Could not find "%BIN%\dsd-neo.exe".
  echo Make sure you run this from the top-level of the unzipped package.
  echo Expected layout: this .bat next to "bin\" and "etc\" directories.
  if "%DO_PAUSE%"=="1" pause
  exit /b 1
)

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

REM If no arguments were provided, show help by default so users see something
set "DEFAULT_ARGS="
if "%~1"=="" set "DEFAULT_ARGS=-h"

echo Launching dsd-neo...
"%BIN%\dsd-neo.exe" %DEFAULT_ARGS% %*
set "ERR=%ERRORLEVEL%"

if not "%ERR%"=="0" (
  echo dsd-neo exited with code %ERR%.
)

REM Optionally stop PulseAudio here; by default, leave it running for reuse
REM taskkill /IM pulseaudio.exe /F >nul 2>nul

if "%DO_PAUSE%"=="1" pause

endlocal & exit /b %ERR%
