Portable Linux packaging via AppImage

This repository builds a portable AppImage that bundles required
runtime libraries (excluding core system libs like glibc).

Key bits:
- AppDir layout with prefix /usr
- .desktop file at packaging/appimage/dsd-neo.desktop
- Icon name: dsd-neo; CI derives a 256x256 PNG from images/dsd-neo.png or images/dsd-neo_const_view.png
- Built in an Ubuntu 20.04 container (glibc 2.31) for broad compatibility
- Packaging builds require RTL-SDR and SoapySDR at configure time

End users:
- Download dsd-neo-linux-<arch>-portable-<version>.AppImage or dsd-neo-linux-<arch>-portable-nightly.AppImage
- chmod +x and run: ./dsd-neo-linux-*-portable-*.AppImage -h
- Radio use still depends on host device access. RTL-SDR requires the usual USB permissions/udev setup, and non-RTL
  SoapySDR devices require compatible host Soapy modules and hardware drivers discoverable through the AppImage's
  `SOAPY_SDR_PLUGIN_PATH` hook or standard host Soapy module paths.
