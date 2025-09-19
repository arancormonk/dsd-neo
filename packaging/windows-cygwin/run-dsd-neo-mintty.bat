@echo off
setlocal ENABLEDELAYEDEXPANSION

REM Resolve script directory
set "ROOT=%~dp0"
set "BIN=%ROOT%bin"
REM If invoked from repository layout (packaging\windows-cygwin), hop two levels up for bin
if not exist "%BIN%\mintty.exe" (
  if exist "%ROOT%..\..\bin\mintty.exe" (
    set "ROOT=%ROOT%..\..\"
    set "BIN=%ROOT%bin"
  )
)

set "PATH=%BIN%;%PATH%"

if exist "%BIN%\mintty.exe" (
  echo Launching in mintty...
  start "dsd-neo" "%BIN%\mintty.exe" -i /Cygwin-Terminal.ico -e "%ROOT%run-dsd-neo.bat" %*
) else (
  echo mintty.exe not found. Falling back to default launcher.
  call "%ROOT%run-dsd-neo.bat" %*
)

endlocal
