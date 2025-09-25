@echo off
setlocal ENABLEDELAYEDEXPANSION

REM --- Double-click friendly relaunch: prefer Windows Terminal, fallback to cmd ---
set "__RELAUNCHED=0"
if /I "%~1"=="--relaunch" (
  set "__RELAUNCHED=1"
  shift
)
if not "%__RELAUNCHED%"=="1" (
  echo %CMDCMDLINE% | findstr /I " /c " >nul
  if not errorlevel 1 (
    REM Launched via double-click (cmd /c). Reopen in persistent terminal.
    if exist "%LocalAppData%\Microsoft\WindowsApps\wt.exe" (
      REM Use correct quoting so arguments are preserved through cmd /k
      start "" "%LocalAppData%\Microsoft\WindowsApps\wt.exe" -w 0 nt -d "%CD%" cmd /k ""%~f0" --relaunch %*"
      exit /b
    ) else (
      REM Fallback to classic cmd with proper argument forwarding
      start "" cmd /k ""%~f0" --relaunch %*"
      exit /b
    )
  )
)

REM Optional: allow disabling the final pause with --no-pause
set "DO_PAUSE=1"
if /I "%~1"=="--no-pause" (
  set "DO_PAUSE=0"
  shift
)
if "%__RELAUNCHED%"=="1" set "DO_PAUSE=0"

REM Resolve script directory and expected layout
set "ROOT=%~dp0"
set "BIN=%ROOT%bin"
set "ETC=%ROOT%etc"
set "SHARE=%ROOT%share"

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

REM Ensure ncurses can find terminfo in portable tree; default TERM if unset
if not defined TERM set "TERM=xterm"
if exist "%SHARE%\terminfo" set "TERMINFO=%SHARE%\terminfo"

REM Start PulseAudio server if available; run hidden (no extra window)
REM Use --daemonize=1 so it detaches immediately; silence output.
if exist "%BIN%\pulseaudio.exe" (
  echo Starting PulseAudio...
  if exist "%ETC%\pulse\default.pa" (
    "%BIN%\pulseaudio.exe" -n --daemonize=1 --exit-idle-time=-1 -F "%ETC%\pulse\default.pa" >nul 2>nul
  ) else (
    "%BIN%\pulseaudio.exe" -n --daemonize=1 --exit-idle-time=-1 >nul 2>nul
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
