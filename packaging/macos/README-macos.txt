DSD-NEO Portable for macOS
==========================

Contents
- bin/dsd-neo              : The decoder executable
- lib/*.dylib              : Bundled runtime library dependencies (when available)
- dsd-neo.sh               : Launcher that sets DYLD paths and runs dsd-neo
- dylibs-manifest.txt      : Bundled dylib inventory
- share/terminfo/          : Bundled ncurses terminal descriptions (also falls
                             back to macOS's /usr/share/terminfo)
- share/doc/dsd-neo/       : License and third-party notice files

Usage
1) Open the DMG (double‑click the .dmg file).
2) Drag the “dsd-neo-macos” folder to a location on your disk (e.g., ~/Applications/dsd-neo).
3) Eject the DMG.
4) From Terminal, run inside the copied folder:
   ./dsd-neo.sh -h

Notes
- The launcher sets DYLD_FALLBACK_LIBRARY_PATH to use bundled libs and
  TERMINFO_DIRS to use bundled ncurses terminal descriptions.
- CI produces an arm64 portable DMG from the macos-arm64-release preset; every
  bundled library is built with vcpkg. It needs macOS 26 or later.
- Audio goes through PortAudio to macOS's own Core Audio, so no extra audio
  server is needed. Reading audio from a sound card asks for Terminal's (or your
  terminal app's) microphone permission the first time.
- The portable CI build requires RTL-SDR and SoapySDR at configure time.
  Runtime radio use still requires appropriate device permissions, drivers,
  and Soapy modules for your hardware. The bundled SoapySDR is 0.8.1, which
  loads modules built for SoapySDR 0.8 only (not Homebrew's 0.9 modules); point
  SOAPY_SDR_PLUGIN_PATH at the folder that holds them.

- Gatekeeper/Quarantine: macOS may warn about running binaries from the Internet.
  You can right‑click → Open the first time, or remove quarantine attributes:
    xattr -dr com.apple.quarantine dsd-neo-macos

Examples
- Show help:
  ./dsd-neo.sh -h

- Use a WAV file as input:
  ./dsd-neo.sh -i sample.wav

Licensing
This distribution includes third-party components under their respective
licenses. dsd-neo is GPL-3.0-or-later. For source and license details, visit:
- dsd-neo:    https://github.com/arancormonk/dsd-neo
- mbelib-neo: https://github.com/arancormonk/mbelib-neo
