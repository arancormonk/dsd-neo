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

REM Tell PulseAudio client to use TCP instead of Unix sockets/shm
set "PULSE_SERVER=tcp:127.0.0.1:4713"

REM PulseAudio modules directory - check portable locations
set "PA_MODULES="
if exist "%ROOT%lib\pulseaudio\modules" set "PA_MODULES=%ROOT%lib\pulseaudio\modules"

REM Start PulseAudio server if available
if not exist "%BIN%\pulseaudio.exe" goto :skip_pulseaudio

echo Starting PulseAudio...

REM Check if PulseAudio is already running
tasklist /FI "IMAGENAME eq pulseaudio.exe" 2>nul | findstr /I "pulseaudio.exe" >nul
if not errorlevel 1 (
  echo   PulseAudio already running.
  goto :pulseaudio_check
)

if "%PA_MODULES%"=="" (
  echo   ERROR: PulseAudio modules directory not found!
  echo   Expected: %ROOT%lib\pulseaudio\modules
  echo   Please ensure the portable package includes the PulseAudio modules.
  dir /B "%ROOT%lib" 2>nul
  goto :skip_pulseaudio
)

echo   Modules: %PA_MODULES%
echo   Listing modules directory:
dir /B "%PA_MODULES%" 2>nul
echo.

REM Convert Windows paths to Cygwin POSIX paths for PulseAudio
set "PA_CONFIG_POSIX="
set "PA_CONFIG_FILE=%ETC%\pulse\default.pa"
if exist "!PA_CONFIG_FILE!" (
  if exist "%BIN%\cygpath.exe" (
    for /f "delims=" %%P in ('"%BIN%\cygpath.exe" -u "!PA_CONFIG_FILE!" 2^>nul') do set "PA_CONFIG_POSIX=%%P"
  ) else (
    REM Fallback: manual conversion C:\path -> /cygdrive/c/path
    set "PA_TMP=!PA_CONFIG_FILE:\=/!"
    set "PA_DRIVE=!PA_TMP:~0,1!"
    set "PA_REST=!PA_TMP:~2!"
    set "PA_CONFIG_POSIX=/cygdrive/!PA_DRIVE!!PA_REST!"
  )
)
echo   Config file: !PA_CONFIG_FILE!
echo   Config (POSIX): !PA_CONFIG_POSIX!

echo   Starting PulseAudio daemon...
if defined PA_CONFIG_POSIX (
  "%BIN%\pulseaudio.exe" -n --daemonize=1 --exit-idle-time=-1 --use-pid-file=0 --disable-shm=1 -p "%PA_MODULES%" -F "!PA_CONFIG_POSIX!"
) else (
  "%BIN%\pulseaudio.exe" --daemonize=1 --exit-idle-time=-1 --use-pid-file=0 --disable-shm=1 -p "%PA_MODULES%"
)
echo   PulseAudio exit code: %ERRORLEVEL%

:pulseaudio_check
REM Give PulseAudio a moment to initialize
timeout /t 2 /nobreak >nul 2>nul

REM Check if PulseAudio is now running
tasklist /FI "IMAGENAME eq pulseaudio.exe" 2>nul | findstr /I "pulseaudio.exe" >nul
if errorlevel 1 (
  echo   WARNING: PulseAudio does not appear to be running!
) else (
  echo   PulseAudio is running.
)

:skip_pulseaudio

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
