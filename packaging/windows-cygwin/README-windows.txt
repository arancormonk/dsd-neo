DSD-NEO Portable (Cygwin) for Windows
=====================================

Contents
- bin\dsd-neo.exe              : The decoder executable
- bin\*.dll                    : Cygwin and library runtime dependencies
- bin\pulseaudio.exe           : PulseAudio server (if present)
- etc\pulse\default.pa         : Optional PulseAudio server config
- run-dsd-neo.bat              : Launcher that sets PATH, starts PulseAudio, runs dsd-neo

Usage
1) Extract the ZIP to any folder (avoid protected locations like Program Files).
2) Recommended: double-click run-dsd-neo-mintty.bat for a nicer terminal UI.
   Or use run-dsd-neo.bat directly. The launcher will:
   - add bin\ to PATH so bundled DLLs are used
   - start PulseAudio in the background (if available)
   - launch dsd-neo.exe with any arguments you pass to the BAT

Examples
- Show help:
  run-dsd-neo.bat -h

- Use a WAV file as input:
  run-dsd-neo.bat -i sample.wav

- List PulseAudio devices (from within dsd-neo):
  run-dsd-neo.bat -O

- Use PulseAudio output explicitly (recommended on Windows):
  run-dsd-neo.bat -o pulse

RTL-SDR Support
If this ZIP includes RTL-SDR support, you will also need to install WinUSB
drivers for your dongle (Zadig). See the RTL-SDR project documentation.

Notes
- The build is based on Cygwin and bundles required Cygwin and library DLLs.
- PulseAudio is started automatically to satisfy audio backends used by dsd-neo.
- You can leave PulseAudio running between invocations; or kill it manually.
- If your audio devices do not appear, try starting the launcher from a fresh
  console and verify pulseaudio.exe is running in Task Manager.

Licensing
This distribution includes third-party components under their respective
licenses. dsd-neo is GPL-2.0-or-later. Cygwin runtime and libraries are
licensed per their packages. For source and license details, visit:
- dsd-neo:      https://github.com/arancormonk/dsd-neo
- mbelib-neo:   https://github.com/arancormonk/mbelib-neo
- Cygwin:       https://www.cygwin.com/
