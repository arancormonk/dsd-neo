# DSD-neo Privacy Policy

**Effective date: August 8, 2026**

This policy covers the DSD-neo Android app (package `io.github.arancormonk.dsdneo`)
distributed on Google Play. The same practices apply to the desktop builds of DSD-neo.

## Summary

DSD-neo does not collect, store, or transmit any personal data. The app contains no
analytics, no advertising, no user accounts, and no tracking. It includes no third-party
SDKs that communicate with the developer or anyone else.

## Everything runs on your device

DSD-neo is a digital-voice decoder. All signal processing — receiving, decoding, and
playing back radio traffic — happens locally on your device. Nothing you tune to, listen
to, or record is ever sent to the developer.

## Network connections you configure

The app only opens network connections that you explicitly set up, and only to servers you
choose:

- **rtl_tcp input** — streaming I/Q samples from an SDR server you specify.
- **UDP audio output** — sending decoded audio to a host you specify.
- **rigctl** — controlling a receiver at an address you specify.
- **rdio-scanner export** — optionally uploading decoded calls to an rdio-scanner server
  you run or designate.

None of these features are enabled by default, and none of them involve the developer's
infrastructure. The developer never receives any of this traffic.

## Files on your device

Recordings, logs, and configuration created by the app are stored locally on your device
and never uploaded anywhere by the app. Android's cloud backup is disabled for this app
(`allowBackup` is off), so this data does not enter device backups either. Uninstalling
the app removes its private data.

## Permissions

- **Internet** — used only for the user-configured connections listed above.
- **Foreground service and notifications** — keep decoding running while the app is in the
  background and show its status.
- **Wake lock** — keep the device awake during long decode sessions.
- **USB host** (optional hardware feature) — talk to an RTL-SDR dongle plugged into the
  device. The app installs and works without USB support.

## Children's privacy

DSD-neo is not directed at children and, as described above, collects no data from anyone.

## Third-party services

None. The app uses no third-party analytics, advertising, or data-collection services.

## Changes to this policy

Updates to this policy are published at this same URL. The full revision history is
available in the repository's git history.

## Contact

Questions about this policy can be raised on the project's issue tracker:
<https://github.com/arancormonk/dsd-neo/issues>
