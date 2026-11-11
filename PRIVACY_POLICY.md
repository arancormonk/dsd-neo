# DSD-neo Privacy Policy

**Effective date: November 11, 2026**

This policy covers the DSD-neo Android app (package `io.github.arancormonk.dsdneo`)
distributed on Google Play. The same practices apply to the side-loaded Android APK and the
desktop builds of DSD-neo.

## Summary

The developer of DSD-neo receives nothing from the app. It contains no analytics, no
advertising, no developer accounts, and no tracking, and it includes no third-party SDKs that
report to the developer or anyone else.

Data leaves your device only when you use a feature that needs another service, and only to
the service you choose: an SDR server or audio stream you point the app at, RadioReference
when you sign in, import a system or update its places, an rdio-scanner server you upload
calls to, and your device's location service when you use location. Each is described below,
with what it receives.

## Everything runs on your device

DSD-neo is a digital-voice decoder. All signal processing (receiving, decoding, and
playing back radio traffic) happens locally on your device. Nothing you tune to, listen
to, or record is ever sent to the developer.

## Network connections you configure

The app only opens network connections that you explicitly set up:

- **rtl_tcp input:** streaming I/Q samples from an SDR server you specify.
- **Network audio input:** receiving an audio stream over TCP or UDP from a host or port you
  specify.
- **UDP audio output:** sending decoded audio to a host you specify.
- **M17 over UDP:** receiving M17 voice and data on a port you specify, or sending them to a
  host you specify. What you send includes the voice, the callsigns you set, and any text
  you add.
- **rigctl:** controlling a receiver at an address you specify.
- **rdio-scanner upload:** sending each call's recording to an rdio-scanner server you run or
  choose, with the API key you enter and the call's details: time, talkgroup and its name,
  the radio ID that talked, frequency, and whether it was encrypted.
- **UDP retune control:** if you turn it on (an option on desktop, or in Extra decoder
  arguments on Android), the app listens on a port you choose for retune commands. It
  listens only on the device itself unless you choose another address.
- **Control and telemetry API (desktop builds only):** if you turn it on, the app listens on
  a port you choose and sends connected programs what it decodes: radio IDs, talkgroups and
  their names, aliases, times, decoded positions and text messages. It listens only on the
  device itself by default; any other address requires a shared token, and the connection is
  not encrypted.
- **RadioReference:** see below.

None of these features are enabled by default, and none of them involve the developer's
infrastructure. The developer never receives any of this traffic.

Your system can add to these connections. A host name you enter is looked up through your
system's name resolver, web requests follow a proxy your system is set to use, and on desktop
your audio system (PulseAudio) or a SoapySDR driver can connect to a server you have set up
in their own settings.

## RadioReference

The app talks to RadioReference's web service (radioreference.com) when you sign in, check
your account, browse or search for a system, import a system, refresh an import or update a
saved system's places. Each request carries your RadioReference username and password, the
app's RadioReference application key (or the one you entered), and what you asked for: a ZIP
code, a country, state or county, a system ID, or a site. Updating places asks for a state's
name and counties by its RadioReference number, and re-reads the sites of systems you saved
from RadioReference. After a system loads in the RadioReference import screen, the app also
asks, with the same account, for the names of that system's states, so a system you save
from it shows its place. Your location is not part of either. RadioReference returns the
system's details, and the app builds its channel and talkgroup files on your device.
RadioReference handles this data under its own privacy policy.

The app saves your username (and an application key you entered) on your device. It keeps
your password in memory only, while the app runs, and never writes it to storage or to its
logs. On Android, the sign-in fields are offered to your device's autofill service, so a
password manager you use can fill them in and save them under its own terms.

## Location

Location is optional and only used in the RadioReference screens and on Listen's Systems tab:

- **Use my location** finds your ZIP code so you don't have to type it. The app asks Android
  for your location, then asks Android's geocoder to turn it into a postal code. It sends
  only the resulting ZIP code to RadioReference, never your coordinates.
- **The nearby-site chooser** uses your location to sort a system's sites by distance. The
  sorting happens on your device.
- **Listen's Nearest order** uses your location to sort your saved systems by distance. The
  app asks Android for your coordinates only, with no postal-code lookup, and sends nothing
  to RadioReference. The distances are worked out on your device.

The first time you use location, the app asks for approximate location (Use my location) or
for approximate and precise location (the nearby-site chooser and Listen's Nearest order).
The app keeps your location at the precision you allowed, approximate or precise. Once you
have granted precise location, Use my location can use it too. Getting a location and turning
it into a postal code are done by your device's location services (on most phones they are
Google's), which may use Wi-Fi, mobile network and GPS information and contact their provider
to do it, under that provider's terms. The app asks for location only while you are using it,
never in the background.

The last location the app got (latitude, longitude, accuracy and time) is saved on your
device so it can pick nearby sites and order your saved systems by distance. It stops being
used after 24 hours and is removed the next time the app checks it, while it is running or
when you open it again. The app does not write it to its diagnostics log, and it is not part
of the app's own backup file. If you deny location, ZIP code, browse and system ID searches
all still work.

## Files and settings on your device

Recordings, call history, logs, and configuration created by the app are stored on your
device in the app's private storage. The app sends them anywhere only through a feature you
turn on (such as rdio-scanner upload) or when you share or export them yourself.

That configuration includes things you enter that are sensitive: your saved systems and the
decryption keys you add to them, the rdio-scanner API key, your RadioReference username and
any application key you entered. On desktop these are in DSD-neo's configuration files. The
app stores them as they are, protected by your device's app sandbox or your account's file
permissions rather than by a passphrase. They stay until you delete them in the app or
uninstall it.

Android's automatic cloud backup is turned off for this app (`allowBackup` is off), so this
data does not go into your Google backups. Some Android 12 and newer devices can still copy
app data during a phone-to-phone transfer, which the app cannot prevent. Uninstalling the app
removes its private data.

In the Android app you can export its configuration yourself (Settings → Backup) as a file
encrypted with a passphrase you choose. It holds your saved systems, keys and settings, but
no recordings, call history or location. The app writes it only to the place you pick in
Android's file picker and never uploads it on its own; if you pick a cloud service such as
Google Drive, that service stores the file under its own terms. The developer never receives
it.

Settings → Diagnostics keeps a short log on your device to help track down problems. Lines
that could hold a key are removed from it. It can contain your device's maker and model, its
Android version, and what the app decoded, such as talkgroups, radio IDs and positions sent
over the air. It leaves your device only if you copy or share it yourself.

## Permissions

- **Internet:** used only for the connections described above.
- **Network state:** lets the app see whether the device is online. It sends nothing.
- **Location** (optional, approximate or precise, only while the app is in use): only for Use
  my location, the nearby-site chooser and Listen's Nearest order, as described above.
- **Foreground service and notifications:** keep decoding running while the app is in the
  background and show its status.
- **Wake lock:** keep the device awake during long decode sessions.
- **Storage** (read-only): lets the app's file browser open files you choose. The Google Play
  app asks for it on Android 10 only. The side-loaded APK from GitHub asks for it up to
  Android 12, and can also ask for All files access on Android 11 and newer, for the same
  browser; the Google Play app never asks for All files access.
- **USB host** (optional hardware feature): talk to an RTL-SDR dongle or Airspy receiver
  plugged into the device. The app installs and works without USB support.

## Children's privacy

DSD-neo is not directed at children. The developer collects no data from anyone.

## Third-party services

The app has no built-in third-party analytics, advertising, or data-collection services.
The services described above (RadioReference, an rdio-scanner server, your device's location,
geocoding and autofill services, and any server you connect the app to) are used only when
you choose to use those features, and each handles your data under its own terms.

## Changes to this policy

Updates to this policy are published at this same URL. The full revision history is
available in the repository's git history.

## Contact

Questions about this policy can be raised on the project's issue tracker:
<https://github.com/arancormonk/dsd-neo/issues>
