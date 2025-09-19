@echo off
setlocal ENABLEDELAYEDEXPANSION

REM Resolve script directory
set "ROOT=%~dp0"
set "BIN=%ROOT%bin"
set "SHARE=%ROOT%share"
REM If invoked from repository layout (packaging\windows-cygwin), hop two levels up for bin
if not exist "%BIN%\mintty.exe" (
  if exist "%ROOT%..\..\bin\mintty.exe" (
    set "ROOT=%ROOT%..\..\"
    set "BIN=%ROOT%bin"
  )
)

set "PATH=%BIN%;%PATH%"

REM Ensure ncurses can find terminfo in portable tree; default TERM if unset
if not defined TERM set "TERM=xterm"
if exist "%SHARE%\terminfo" set "TERMINFO=%SHARE%\terminfo"

if exist "%BIN%\mintty.exe" (
  echo Launching in mintty...
  start "dsd-neo" "%BIN%\mintty.exe" -i /Cygwin-Terminal.ico -e "%ROOT%run-dsd-neo.bat" %*
) else (
  echo mintty.exe not found. Falling back to default launcher.
  call "%ROOT%run-dsd-neo.bat" %*
)

endlocal
