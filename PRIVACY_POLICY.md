# DSD-neo Privacy Policy

**Effective date: November 10, 2026**

This policy covers the DSD-neo Android app (package `io.github.arancormonk.dsdneo`)
distributed on Google Play. The same practices apply to the side-loaded Android APK and the
desktop builds of DSD-neo.

## Summary

DSD-neo does not collect any personal data, and the developer never receives anything from
the app. The app contains no analytics, no advertising, no developer accounts, and no
tracking. It includes no third-party SDKs that communicate with the developer or anyone else.

The app only talks to other services when you use a feature that needs one: an SDR server
or audio stream you point it at, RadioReference when you import a system, your own
rdio-scanner server, and your device's location service when you tap **Use my location**.
Each is described below.

## Everything runs on your device

DSD-neo is a digital-voice decoder. All signal processing (receiving, decoding, and
playing back radio traffic) happens locally on your device. Nothing you tune to, listen
to, or record is ever sent to the developer.

## Network connections you configure

The app only opens network connections that you explicitly set up, and only to servers and
addresses you choose:

- **rtl_tcp input:** streaming I/Q samples from an SDR server you specify.
- **Network audio input:** receiving an audio stream over TCP or UDP from a host or port you
  specify.
- **UDP audio output and M17 over UDP:** sending decoded audio or M17 frames to a host you
  specify.
- **rigctl:** controlling a receiver at an address you specify.
- **rdio-scanner export:** optionally uploading decoded calls, with the API key you enter, to
  an rdio-scanner server you run or designate.
- **RadioReference import:** see below.
- **Desktop builds only:** the control and telemetry API and the UDP retune control listen
  for connections on an address and port you choose (only the device itself by default).

None of these features are enabled by default, and none of them involve the developer's
infrastructure. The developer never receives any of this traffic.

## RadioReference

When you import a system from RadioReference.com, the app sends your request to
RadioReference's web service: your RadioReference username and password, the app's
RadioReference application key (or the one you entered), and what you searched for (a ZIP
code, a country, state or county, or a system ID). RadioReference returns the system's
details, and the app builds its channel and talkgroup files on your device. RadioReference
handles this data under its own privacy policy.

The app saves your username (and an application key you entered) on your device. It keeps
your password in memory only, for as long as the app runs, and never writes it to storage or
to its logs. On Android, the sign-in fields are offered to your device's autofill service, so
a password manager you use can fill them in and save them under its own terms.

## Location

Location is optional and only used in the RadioReference screens:

- **Use my location** finds your ZIP code so you don't have to type it. The app asks
  Android for your approximate location, and then asks Android's geocoder to turn it into a
  postal code. The geocoder belongs to your device (on most phones it is provided by
  Google) and may send the location to its provider to do this, under that provider's
  terms. The app sends only the resulting ZIP code to RadioReference, never your
  coordinates.
- **The nearby-site chooser** uses your location to sort a system's sites by distance. This
  happens on your device.

The app asks for location only while you are using it, never in the background. The last
location it got (latitude, longitude, accuracy and time) is kept on your device for 24 hours
to pick nearby sites and then deleted. It is never included in diagnostics, exports or
backups. If you deny location, ZIP code, browse and system ID searches all still work.

## Files on your device

Recordings, logs, call history, and configuration created by the app are stored locally on
your device and never uploaded anywhere by the app. Android's automatic cloud backup is
disabled for this app (`allowBackup` is off), so this data does not enter device backups
either. Uninstalling the app removes its private data.

Settings → Diagnostics keeps a short log on your device to help track down problems. Keys
and your device's location are filtered out of it. It leaves your device only if you copy or share it
yourself.

In the Android app you can export its configuration yourself (Settings → Backup) as a file
encrypted with a passphrase you choose. It holds your saved systems, keys and settings, but
no recordings or call history. The app writes it only to the place you pick in Android's
file picker and never uploads it on its own; if you pick a cloud service such as Google
Drive, that service stores the file under its own terms. The developer never receives it.

## Permissions

- **Internet:** used only for the connections described above.
- **Network state:** lets the app see whether the device is online. It sends nothing.
- **Location** (optional, while the app is in use): only for **Use my location** and the
  nearby-site chooser, as described above.
- **Foreground service and notifications:** keep decoding running while the app is in the
  background and show its status.
- **Wake lock:** keep the device awake during long decode sessions.
- **Storage** (Android 10 only, read-only): lets the app's file browser open files you
  choose. The side-loaded APK from GitHub can also ask for All files access on Android 11
  and newer, for the same browser; the Google Play app never asks for it.
- **USB host** (optional hardware feature): talk to an RTL-SDR dongle or Airspy receiver
  plugged into the device. The app installs and works without USB support.

## Children's privacy

DSD-neo is not directed at children and, as described above, collects no data from anyone.

## Third-party services

The app has no built-in third-party analytics, advertising, or data-collection services.
The services described above (RadioReference, an rdio-scanner server, your device's location
and autofill services) are used only when you choose to use those features, and each
handles your data under its own terms.

## Changes to this policy

Updates to this policy are published at this same URL. The full revision history is
available in the repository's git history.

## Contact

Questions about this policy can be raised on the project's issue tracker:
<https://github.com/arancormonk/dsd-neo/issues>
