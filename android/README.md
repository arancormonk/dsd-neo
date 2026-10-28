<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DSD-neo on Android

An arm64-v8a app: a Qt Quick UI (shared with the future desktop GUI, `src/ui/qt/`),
a foreground service that owns the decoder, and a small JNI surface between them.
The engine is linked into the app process as native code, so JNI carries only
lifecycle, platform glue, and one read-only status record (the scanner readout the
service renders into its notification).

Supported inputs: a directly attached RTL-SDR or native Airspy R2/Mini over USB-OTG,
`rtl_tcp`, UDP PCM, TCP PCM, and local files. See [native Airspy setup](../docs/airspy.md).

## Saved encryption keys

Saved systems store `encKeyType` (`""`, `basic`, `hex`, `rc4`, or `scrambler`),
`encKeyValue`, and `encForceKey` (0 normal, 1 force privacy, 2 force RC4).
Keys rest **unencrypted** in the app-private systems store, `saved_systems.json`. Android's own backup is off
(`allowBackup=false`), so they leave the phone only inside a backup you export, encrypted under your passphrase (see
[Backup and restore](#backup-and-restore)). On desktop the same file is under Qt's `AppDataLocation`, with
owner-only read/write permissions on POSIX at creation and replacement. Retained keys are
resolved and passed to the host entirely in C++; QML validation/start results contain no key arguments.
QString/QML copies cannot promise erasure. Native command payloads, parsed key
sets, and owned JNI argv allocations are securely erased after use.

Session assembly refuses a direct key together with a key CSV. Direct values
become discrete `-b`/`-H`/`-1`/`-R` arguments; force modes use `-4`/`-0`.
Startup and live commands share validation and overlay semantics: basic changes
K, RC4 changes R/RR, scrambler changes R, and hex changes the Hytera/AES block.
Other keys remain installed. Any valid value, including zero, arms decryption;
encrypted-lockout policy still decides whether a call plays. Direct-key CLI
messages and command toasts never echo key values, including with `--show-keys`.
During a keyed scan row, a live direct-key command updates the saved global key
baseline; the row's own key remains effective until rotation restores globals.
Force mode similarly updates configuration while an explicit row override wins.
The encryption wizard and live entry sheet are supplied by a later UI package.

### Editing and saving talkgroups

Long-press a talkgroup card to rename it, change Listening/Not tuned, or set
priority (0/25/50/100 presets and ±5 steps). **Apply** changes only the fields you
edited; renaming or changing priority preserves custom audio/record/stream policy.
Priority changes also update current calls immediately, preserving their dwell and cooldown ages.
Preempt is available above priority zero: a preempt talkgroup interrupts a lower-priority call when the system
announces it on the channel being received (P25, TETRA, DMR trunking, NXDN; see
[Priority and preemption](../docs/csv-formats.md#priority-and-preemption)). The sheet says so, and says that a Not tuned
row's priority does nothing. The card shows `P<n>` and a lightning badge when preempt is set. Heard-only rows offer
**Add to list**; listed rows require two taps on **Remove**. An accepted submit closes the sheet; a decoder refusal
appears in the screen's message line. Over-the-air radio aliases learned while the sheet is open do not make an edit
stale; a list reload, an edit from another frontend or a scan rotation does, and then the sheet must be reopened.

When the running session has no group file, **Save talkgroup list** exports the
canonical policy, including aliases and ranges, to app-owned Imports storage.
The UI waits for the decoder's retained successful result before registering the
file and setting the saved system's group-list path. Later edits write that same
file. If the saved system was deleted or acquired another group file while saving,
the export remains in Imports without changing that system. Explore sessions save
to Imports for later selection in a saved system. Scan-row exports are refused by
the decoder. If completion fails or the session ends, the pending save is cleared.
After 15 seconds without a result, **Try again** allows a new request. A retry uses
a new path; late results from abandoned requests never associate a file.

## The two-mode UI

The shell has two modes, switched on `DecoderHost::sessionState` — a phone cannot
do both at once:

- **Idle** (`Idle`, `Failed`) — the tab shell: saved systems on Home (one tap to
  listen, long-press to edit or remove), the persistent call log on History,
  Settings, plus the add-system wizard and a failure banner when a start was
  abandoned.
- **Monitor** (`Starting`, `Running`, `Stopping`) — the live session takes the
  screen: the hero call, mute, hold, skip and avoid, the signal strip, and the session's
  recent calls. On a radio source the signal strip opens with **FREQ**, the frequency
  the receiver is tuned to: the saved system's frequency, a scan target's park
  frequency, or the voice channel while a trunked target follows a call. It shows
  while scanning and holding as well, and an I/Q replay shows the frequency each part
  of the capture was recorded on.

On a two-slot TDMA carrier (DMR, P25 Phase 2, X2-TDMA) the second slot's strip
stays in place under the hero card, reading **SLOT 2 · idle** (or **SLOT 1 · idle**
while slot 2 leads) between that slot's calls, so the screen below it does not jump
when the other slot keys up or drops. It appears once the decoder has decoded a
checked frame of a two-slot protocol and goes on a retune, a scan step, a decode-mode
change or a P25 Phase 1 frame; an FDMA carrier shows it only for a call on the other
slot.

**Skip** leaves this call without changing the talkgroup list. On P25 a group-call
skip lasts while the receiver keeps seeing the talkgroup's call: control-channel
grant updates, or the call on the voice channel it is parked on for the other slot.
It expires 15 s after the receiver last saw it and after 10 minutes at most. Quick
replies and new calls within that 15 s window are skipped together. Following a
call on another carrier for longer than 15 s lets the skipped call be followed
again when next seen; tap **Skip** again. DMR, NXDN, EDACS and P25 private-call skips
last 15 s from the tap in this version; ProVoice has no skip.

**Avoid TG** blocks the talkgroup for the session, until stop or list reload/replacement.
**Lock out** saves the block to the configured talkgroup list. **Settings → Listening
→ Save avoided talkgroups** selects **Lock out** when on (the default) and **Avoid TG**
when off. Without a group file, or on a scan row's own list, lockouts stay in memory.
**Skip** is unaffected by this setting and never saves. Avoids and unexpired call
skips survive scan-target revisits; **Clear temporary avoids and call skips — current
list** clears both in the active list. List reload/replacement or stopping the decoder
also clears them; reopening the Activity while the service runs does not.

The Monitor's decode-quality row sits below the tuner strip and also works with
PCM, network, and file inputs. `CC FEC` and P2 `RS` show successful blocks as a
percentage; no blocks (0/0) means no reading. `VOICE x.x err/fr` is the average
corrected-error count per voice frame, using only populated samples in the current
call's ring (P1, or the lead P2 slot). It is not BER. A fresh call waits for its
first voice sample; an unsampled slot never borrows the other slot's average.
Non-P25 calls show the lead slot's last-frame `ERR a/b` instead. The row wraps
inside the scrolling Monitor body on short screens and clears with the session.
Protocol-specific true BER and DMR/NXDN windowed error rings remain follow-up work.

Both layers stay instantiated and cross-fade, so nothing typed or scrolled is
lost when a session ends. The live readings exist *only* in the monitoring view:
nothing upstream invalidates the published snapshot on stop, so
`MetricsModel::clear()` is what stops the last live SNR and carrier lock from
sitting on screen for a decoder that is no longer running. `UiController` drives
it from the session-state edges. The call history is deliberately different — it
is persistent and never cleared on session boundaries, only fed. Its two stores
are rewritten whole, so their saves are paced by size: a small history saves 3 s
after a change, a full one about once a minute (at most 16 KiB/s of flash; it
was ~1 GB/hour on a busy system before issue #745). A stop, a failed session, the
app going to the background and quitting all save at once, so a hard crash or a
phone restart can lose at most the last minute of rows.

The service state machine, the native `g_running` atomic and the failure path are
folded into that one phase by `session_state_map.h`, which is deliberately free of
Qt and JNI so `UI_QT_SESSION_STATE` can test it on the host. Each accepted start
has a monotonically increasing session id. `nativeLifecycleStatus` retains the
post-initialization edge and terminal reason (pending/completed/cancelled/failed),
run return code, and native USB open/claim error after cleanup. It also carries why
the run ended (the first cause accepted: the user's Stop, a USB detach, the receiver's
committed ending, a teardown) and the session's configured receiver, published while
the run's cleanup still goes, so a lost receiver moves the UI to Stopping at once and
holds it there until that session ends. The service folds
these into one `lifecycleStatus` record, including `lastError`; the existing UI
tick reads it without consuming a decoder snapshot. Failures remain visible even
when the run ends between polls or fails well after startup. While a native terminal
result is present but the service is still RUNNING/STOPPING, the UI stays Stopping.
Tapping **Stop listening** shows Stopping (the button reads "Stopping…" and is
disabled) at once, before the service has handled the intent; the RUNNING polls
that arrive before its STOPPING keep it. If the service still reports RUNNING
two seconds after the tap, the stop is taken as lost: Running comes back with the
button enabled, and the diagnostics log records it.
For a configured run, the app now signals native cancellation synchronously before
dispatching the intent. Delayed service callbacks, a missing Activity context, or a
rejected intent do not prevent that cancellation request. The deferred intent carries its
session id so it cannot stop a later run; a start still being configured is stopped
through ordered intent delivery, after configuration has reset the native flags.
Notification Stop also signals cancellation before updating notifications. Diagnostics
record the session, stop cause and native result, and any dispatch failure.
Any stop or restart you start from the app does not wait for rdio-scanner uploads
still pending: Stop listening (in the app or the notification), switching
systems, retrying after a failure, leaving Signal search or its Stop, Back or
closing the window with background listening off, and "Dismiss failure and choose
a site". Removing the app's task from Recents also stops through the service, without
waiting for Activity destruction. The transfer in flight is abandoned, queued uploads
are not sent (one log line gives the count), and their recordings and sidecars stay on
disk. A session that ends on its own, or whose receiver is unplugged, still waits for
its uploads, as does an Activity or service teardown. A later user Stop can abandon
an ongoing drain.
Restart remains disabled until the service releases its wake lock and publishes
IDLE; the host also checks a fresh service record before accepting a retry.
USB claim error -6
is surfaced as `DeviceBusy` without discarding the original code.

`sessionInitialized()` comes from the engine lifecycle callback after initialization,
not from `g_running`. Saved-system recency and `prefs.lastStartedKind/Uid` are
written only on that edge. `nativeRun()` raises `g_running` before
`dsd_app_frontend_runtime_start()` opens command admission, so Running alone does not
mean the decoder takes commands: Signal search keeps a start refused there and submits it
again on this edge. Saved systems carry stable UUIDs, so deleting another
row during startup cannot stamp the wrong system. Starting/Idle/Failed and
trunk-scan target changes clear live model caches; history remains persistent.

The shared host provides location requests/cancellation, content-based diagnostics
sharing, device-attach signals and a diagnostic sink. Location and sharing default to
unsupported until their platform packages supply implementations. USB/lifecycle diagnostics
enter the process capture directly, including when no Activity exists. Before Qt initialization
installs the tap, bounded redacted host records wait in memory. The optional last location fix
uses milliseconds since epoch and expires after 24 hours. Android's `LocationSupport.kt`
brokers **Use my location** for RadioReference with coarse foreground permission on
the Android main thread. API 30+ uses `getCurrentLocation`; API 29 uses
`requestSingleUpdate` and removes its listener on completion/cancellation. After
permission is granted, it checks enabled providers for a cached fix up to ten minutes
old with reported accuracy within 10 km, using monotonic time for freshness and retaining
the original fix timestamp. API 31+ requests both platform fused and network providers
when no usable cached fix exists, accepting the first usable result and cancelling the
remaining requests; API 29–30 uses the network provider. Fix acquisition and worker-thread
reverse geocoding each have a separate 20-second timeout. Request IDs and
separate fix/geocode status preserve a usable fix when geocoding fails and prevent
late responses from replacing a newer search. Activity teardown cancels the request.
The Sites chooser uses a separate coordinate-only request: at most 30-second-old fixes,
an accurate recent cache or fresh provider updates, and no reverse-geocoding wait. It offers
fine and coarse foreground permission together when permission is first needed, respects
existing approximate access, and adds GPS when fine permission is granted. A coarse result
waits for a more accurate pending provider, up to the 20-second deadline. Accuracy and saved
location are visible in the chooser. Existing users can enable precise location in Android's
app permission settings; background location is never requested.
Desktop location defaults to unsupported.
USB/lifecycle diagnostics use the runtime log surface, including when no Activity
exists. The optional last location fix includes accuracy in metres, uses milliseconds since epoch and is deleted
after 24 hours, on load/read and by a foreground timer. Location producers use
`AppPrefs::setLocationFix` to publish the tuple with one coherent notification; the coordinate
and timestamp properties are read-only to QML. Coordinates and direct
key fields must never be included in diagnostic exports. QString/QML secret
copies cannot guarantee erasure; command-owned key bytes are securely erased.

The Monitor keeps its hero fixed and scrolls the rows below it; recent calls have
a minimum pane height. Modal sheets constrain their height to the space above the
keyboard, scroll their contents, and reveal the focused field.

## Imported files (trunking CSVs)

The C core only opens real filesystem paths (`src/runtime/path_policy.c` requires
a regular file, `O_NOFOLLOW`), so SAF content URIs are always materialized into a
copy first. `AppSupport.importDocumentToFiles` makes that copy in `files/imports/`,
both for the wizard's audio/capture "File" source and for the CSV library (channel
maps, talkgroup lists, key files, P25 band plans, source ID lists). These copies must
survive restarts — saved systems reference the path, and the engine appends learned
talkgroup rows to the group list in place — so none of them goes to the evictable
cache. Display-name collisions are unique-ified (`chan.csv` → `chan (2).csv`); updates
stage a temp file and rename over the target so a half-copied CSV is never observable.

Embedded `single_key_dec`/`single_key_hex` values in a channel map travel with that copied file and therefore work on
Android. The companion selection flow copies referenced files into a private bundle
and rewrites the stored paths. A target CSV additionally includes its channel maps,
band plans, scoped policy/mapping files, and dependencies inside channel maps.

Both uses are reached through `DecoderHost` virtuals (`importContentUri` /
`importDocument`); the desktop default of `importDocument` implements the same
semantics under `QStandardPaths::AppDataLocation`, which is what
`UI_QT_IMPORTED_FILES` tests on the host. The library itself (one JSON-persisted
row per stored file, with dry-run validation counts from
`<dsd-neo/core/csv_validate.h>`) is `src/ui/qt/imported_files_model.{h,cpp}`,
surfaced as Settings → Imported files and the wizard's Trunking data pickers.
The imported-file kinds include `chan`, `group`, `keysDec`, `keysHex`, `p25Bandplan`,
`src` (the **Radio IDs** picker), and `trunkTargets` (**Trunk scan targets**). Source
ID lists use `id,name[,tags]` and supply labels only; they can be applied or cleared in a running session. A group
list and a source ID list cannot be distinguished by content: validation uses the kind selected by the user.
No storage permissions are involved; SAF needs none.

## Backup and restore

Settings → **Backup** saves your setup as one encrypted file, to keep a copy or to move it to another phone.

A backup carries your saved systems (with their saved keys), scan lists, imported files (channel maps, talkgroup lists,
key files, band plans, source ID lists, trunk scan targets, and the companion files bundled with them), decryption
profiles with their keys, Signal search ranges and permanent avoids, and your settings, including the app-wide Extra
decoder arguments and the RadioReference username and application-key override.

It does not carry: call history, recordings (replay files), Signal search's detection history, diagnostics, the last
location fix and first-run state. The RadioReference password is never saved, so it is never in a backup.
Generated scan-list CSVs and compiled decryption files are not carried either; the restoring phone makes them again. A
file that a saved system, scan list or profile names outside DSD-neo's imported files cannot be carried: the export
leaves that reference out and lists it under **Not included** on the Settings card.

### Exporting

**Export backup** works while listening too. Type a passphrase of at least 8 characters twice (**Show passphrases**
reveals both fields), then **Encrypt**. Android's save picker opens with the name `dsd-neo-backup-YYYY-MM-DD.dsdbak`;
keep DSD-neo open until Settings reads **Backup saved.** The passphrase is the only way to open the backup. DSD-neo
keeps no copy of it, and a forgotten passphrase cannot be recovered or reset. A running session keeps adding to its
talkgroup lists; if a file changes while the backup is being made, the export stops with **Your setup changed while the
backup was being made. Try again.**

### Restoring

**Restore from backup** replaces this phone's setup with the backup's, so stop listening first (the row reads **Stop
listening to restore** until then). Pick the file in Android's file picker (DSD-neo checks the file's contents, not its
name) and enter its passphrase. A wrong passphrase reads the same as a file damaged on the way (**Wrong passphrase, or
the file is damaged.**). When the file itself is the problem (not a DSD-neo backup, made by a newer DSD-neo, too large,
damaged, or unreadable), **Choose another file** opens the picker again. DSD-neo checks the whole backup before it
changes anything. An imported file this phone cannot read (a channel map whose companion files it refuses, say) is left
out of the restore rather than refusing the whole backup; whatever named it loses that reference. Then it asks **Replace
your setup?** with the backup's date, app version and counts, how many replay systems will need their recording chosen
again, how many systems carry extra decoder options, and what is **Not included** (what the export could not carry, and
files left out on this phone).

**Replace and close** closes DSD-neo. If DSD-neo is still stopping a session, the confirmation says so and stays open:
confirm again in a moment, or **Cancel** to drop the restore. Open DSD-neo again: the restore is applied before
anything is loaded, and Home shows the result until you **Dismiss** it:

- **Backup restored**, with what the phone now has and what was not included.
- **Restore not applied. Nothing was changed.** Your previous setup is intact.
- The decoder was busy, or there was not enough free storage: the restore waits for the next start, and **Discard
  restore** drops it instead.
- **Restore couldn't finish.** Neither finishing nor undoing it worked, and DSD-neo tries again at the next start. Free
  some storage and reopen DSD-neo.

While a confirmed restore waits (DSD-neo could not close itself, or the restore was put off or could not finish),
Settings → Backup shows **Restore is ready** with **Cancel restore**. Once a restore has started changing files, nothing
can stop it: neither **Cancel restore** nor **Discard restore** is offered, Settings and Home say the restore is
part-way through, and reopening DSD-neo finishes or undoes it.

A backup never carries recordings, so each replay system arrives without its file: open it with Edit and choose the
recording again (until then, starting it says its recording is missing). A restore replaces the old setup as a whole:
its saved systems, scan lists, decryption profiles, settings, Signal search ranges and permanent avoids, and the
imported files it used (generated scan-list CSVs and compiled decryption files are made again). It never deletes a
recording or any other file the old setup did not use. When such a file holds a name an incoming file needs, the
incoming file takes a free name instead (`group (1).csv`) and the restored lists point to it.

**Restore only backups you made or trust.** A backup carries every setting, and extra decoder options run exactly as
typed: a crafted backup could, for example, send decoded audio to someone else's server. The confirmation therefore
counts the systems that pass options of their own (anything in their Extra decoder arguments, or a decode mode the app
itself never writes) and says when the app-wide Extra decoder arguments are set. Review them after restoring a backup
you did not make.

Limits:

- Paths typed into Extra decoder arguments are restored as typed; DSD-neo does not adjust them for another phone.
- Paths written inside CSV files are restored exactly as they are. Companion files that an import bundled are named by
  relative paths, so they keep working; a channel map that names a companion by its full path is listed under **Not
  included** when you export.
- The file is encrypted with AES-256-GCM under a key that scrypt derives from the passphrase (N = 2^16, r = 8, p = 2,
  about 64 MiB of memory per attempt). That slows guessing; it cannot protect a short or common passphrase.
- The sealed backup file is at most 64 MiB. Inside it: up to 4096 files of up to 32 MiB each, and up to 4096 entries
  of each kind (saved systems, scan lists, imported files, profiles, ranges).
- Google Drive has not been tested as a place to save or restore: it cannot be exercised on the test emulators. If it
  fails, save the backup to the phone first, or copy it there before restoring.
- If DSD-neo reopens while its decoder is still running in the background, a confirmed restore waits for a start
  without a running decoder. If that restore had already started changing files (it was interrupted, or could not
  finish), the setup you use meanwhile is partly restored, and changes you make to it can be overwritten when the
  restore completes.
- If DSD-neo cannot delete the restore's own "ready to apply" marker once the new setup is in place, the restore
  counts as couldn't finish rather than being undone: undoing it would end by deleting that same marker, so it could
  not finish either. The next start applies the restore again, which changes nothing that is already in place, and
  deletes the marker then.
- If a part-way restore's undo record is damaged (only tampering or storage corruption can do that, and the settings it
  keeps are checked too), every start says the restore couldn't finish and nothing can cancel it. The way out is to
  clear DSD-neo's storage in Android's app settings, which erases all of its data, and then restore the backup again.
- A channel map you updated with companion files keeps its own file beside the folder of companions and names that
  folder in its contents, which a restore never rewrites, so the folder cannot take a free name. If a folder of the
  same name, left by an earlier import, is already on the phone, the restore fails, changing nothing (**A folder left
  by an earlier import is in the way of a restored channel map.**). Clearing DSD-neo's storage in Android's app
  settings removes that folder, along with all of DSD-neo's data (call history and recordings included); then restore
  the backup again.
- A restore always fails, changing nothing, while one of DSD-neo's own list files is a symbolic link. DSD-neo never
  makes one; only something with access to its private storage (root, or `adb` on a debuggable build) can, and
  clearing DSD-neo's storage removes it.

## Build

Prerequisites:

- Android SDK with build-tools and platform 36
- NDK r28c (`28.2.13676358`), retained by this tree's CI; Qt 6.11's published
  support matrix recommends r27c, so a further NDK upgrade needs separate validation
- JDK 21 for the build tools (Java source/target compatibility remains 17)
- Qt 6.11.2 `android_arm64_v8a` kit **and** the matching host kit (`gcc_64` on Linux)
- vcpkg (the repo's manifest and overlay ports cross-compile mbe-neo, OpenSSL,
  libsndfile, Codec2 and libcurl)
- a host C compiler: Codec2 builds `generate_codebook` for the build machine and
  runs it to generate its codebooks, so a container without one cannot build it

```sh
export VCPKG_ROOT=$HOME/vcpkg
export ANDROID_SDK_ROOT=/opt/android-sdk
export ANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/28.2.13676358
export QT_ANDROID_ROOT=$HOME/Qt/6.11.2/android_arm64_v8a
export QT_HOST_ROOT=$HOME/Qt/6.11.2/gcc_64

cmake --preset android-app
cmake --build --preset android-app -j          # builds the apk target
```

The APK lands at `build/android-app/android/android-build/dsd-neo-app.apk` and is
unsigned; see [Release signing](#release-signing) for signing and installing it.

CMake is the outer build: vcpkg chainloads Qt's `qt.toolchain.cmake`, which
chainloads the NDK toolchain, and androiddeployqt generates and drives the Gradle
project from `android/package/`. `android/package/build.gradle` is a vendored
copy of Qt's androiddeployqt template — files in `QT_ANDROID_PACKAGE_SOURCE_DIR`
override the Qt template — carrying release R8 optimization and the per-artifact
native-library packaging described under [Google Play](#google-play-the-app-bundle); re-diff it against
`<Qt>/android_arm64_v8a/src/android/templates/build.gradle` whenever the pinned
Qt version changes.

### Release optimization and JNI

Both release APKs and AABs enable R8 code optimization, obfuscation, code shrinking
and Android resource shrinking with `proguard-android-optimize.txt`. Debug builds
remain unminified. R8 affects Java/Kotlin DEX code; the native decoder and Qt `.so`
libraries have their own CMake/compiler optimization settings.

`android/package/proguard-rules.pro` preserves the exact Kotlin methods called by
name from `decoder_host_android.cpp` and the `DsdNative` JNI bindings. Add a matching
rule whenever adding a native upcall. `qt-rules.pro` preserves Qt's native method
registration, `UsedFromNativeCode` annotations and the unannotated bridge types and
methods in Qt 6.11.2. Re-audit it on a Qt upgrade: annotations alone are incomplete.
This includes the `relativePath`, `size` and `isDir` fields of
`QtApkFileEngine.JFileInfo`: native plugin discovery reads them when libraries stay
inside Play's APK splits. Removing or renaming them prevents the Android platform
plugin from loading even when the standalone APK launches successfully.
`res/raw/dsdneo_keep.xml` protects the resources Qt loads by name, including the
library arrays androiddeployqt generates. QML and `.rcc` assets are separate from
Android resource shrinking.

CI checks the actual release DEX definitions against the app's C++ JNI calls,
selected Qt upcalls and instance fields, and Qt class names referenced by the packaged native libraries.
It requires R8 compiler markers, a nonempty mapping, and, for an AAB, the embedded
`BUNDLE-METADATA/com.android.tools/r8.json` with optimization and shrinking enabled.
The check prints total uncompressed DEX bytes and the bundle's R8 statistics:

```sh
python3 tools/check_android_r8.py path/to/release.apk \
  --dexdump "$ANDROID_SDK_ROOT/build-tools/36.0.0/dexdump" \
  --mapping build/android-app/android/android-build/build/outputs/mapping/release/mapping.txt
python3 tools/check_android_r8.py path/to/release.aab \
  --dexdump "$ANDROID_SDK_ROOT/build-tools/36.0.0/dexdump"
```

Keep `mapping.txt` from each distributed APK to retrace Java/Kotlin crash reports
and the main-thread stacks in diagnostics ("Main-thread watchdog"). Release builds
keep line numbers (`-keepattributes SourceFile,LineNumberTable`, with every source
file name replaced by `SourceFile`), which at minSdk 29 costs no DEX size.
CI saves the mapping and other R8 reports as `<APK artifact name>.r8` before building
the AAB (90 days for tags, 30 otherwise); archive release mappings before expiry.
An AAB includes its own mapping at
`BUNDLE-METADATA/com.android.tools.build.obfuscation/proguard.map`.

Before publishing an optimized release, exercise it on a device: cold launch,
start/stop/restart decoding, background notification and screen modes, RTL-SDR and
Airspy USB permission/attachment, file import and backup/restore, diagnostics
sharing, and location permission/fix/cancellation. Artifact checks do not exercise
Android lifecycle behavior or every Qt native upcall.

See Android's [R8 configuration guidance](https://developer.android.com/topic/performance/app-optimization/enable-app-optimization),
[JNI keep rules](https://developer.android.com/topic/performance/app-optimization/keep-rule-examples#java-native-interface-jni)
and [DEX optimization measurements](https://developer.android.com/topic/performance/issues/code-optimization).

### Package staging

androiddeployqt is pointed at a staged copy of that directory
(`build/android-app/android/package`) rather than the source tree, because it
packages nothing else and the APK has to carry the same license/notice set as
every other release asset. `cmake/stage_android_package.cmake` copies
`android/package/` plus `LICENSE`, `COPYRIGHT`, `THIRD_PARTY.md` and the vendored
notices into `assets/doc/dsd-neo/`, and runs on every build, so edits under
`android/package/` reach the APK without re-running CMake. Add new package files
to `android/package/` as usual — never to the staged copy, which is overwritten.

For a headless CLI binary (no UI, no APK) use the `android-arm64-release` preset;
it needs only `ANDROID_NDK_HOME` and `VCPKG_ROOT`.

### App identity

The launcher shows **DSD-neo**, from `@string/app_name` in
`android/package/res/values/strings.xml`. That indirection only works because
`android/CMakeLists.txt` sets `QT_ANDROID_APP_NAME` and `QT_ANDROID_APP_ICON`:
without them androiddeployqt substitutes the CMake target name into
`android:label` and deletes `android:icon` outright, so the app installs as
`dsd-neo-app` with the stock Android robot.

The version surfaces both come from `GIT_TAG`, the same `git describe` string the
CLI banner and terminal UI header print — the Qt UI shows no version of its own,
so `android:versionName` is all the app has:

| build | `versionName` | `versionCode` |
| --- | --- | --- |
| tag `v2.5.1` | `2.5.1` | `20501000` |
| nightly, 6 commits later | `2.5.1-6-gfa336d4` | `20501006` |
| next tag `v2.5.2` | `2.5.2` | `20502000` |

`PROJECT_VERSION` only moves at release time and the tag sits on the bump commit
itself, so deriving the code from it alone would give every nightly in a release
window the same code as the release it follows — installable, but invisible to
anything that compares version codes to detect an update. Scaling the release
code up and adding `git describe`'s commit distance keeps tagged builds round,
lets nightlies increment, and has the next release clear every nightly before it.

Minor and patch are assumed to stay below 100 and a release window below 1000
commits; configure fails loudly if the latter is ever exceeded. The scheme tops
out at major version 210, against the platform's 2100000000 ceiling. A tarball
with no git metadata falls back to the plain release version.

The launcher, themed, notification and splash bitmaps under
`android/package/res/mipmap-*` and `res/drawable-*` are generated from
`images/dsd-neo.png` and committed — the Android CI job has no ImageMagick, and
androiddeployqt packages `res/` as-is. Regenerate them only when the logo
changes:

```sh
tools/gen_android_icons.sh
```

The hand-written pieces live alongside them: `mipmap-anydpi-v26/ic_launcher.xml`
(the adaptive icon, including the Android 13 `<monochrome>` layer),
`values/colors.xml` (the one near-black backdrop the transparent logo is drawn
on), `drawable/splash.xml` and `values{,-v31}/themes.xml`.

The NDK, SDK platform, build-tools and Qt versions CI builds against are pinned in
`tools/ci-dependency-pins.env` (`ANDROID_NDK_VERSION`, `ANDROID_COMPILE_SDK`,
`ANDROID_BUILD_TOOLS_VERSION`, `ANDROID_QT_VERSION`). Newer ones generally work;
those are the combination the tree is known good with.

## Continuous integration

Three jobs keep this path from rotting, none of which needs a device. They run on
pull requests as well as pushes to `main`:

- **`android-ci` / arm64 CLI (NDK cross build)** — cross-compiles the headless
  `android-arm64-release` preset: engine, AAudio backend, and the vendored libusb
  and librtlsdr below. Asserts the binary is AArch64 with 16 KB page alignment.
- **`android-ci` / APK (Qt Quick app)** — builds the `android-app` preset through
  androiddeployqt, signs the APK, then unpacks it and asserts the same alignment
  for every packaged `.so` (ours and Qt's), that `arm64-v8a` is the only ABI
  inside, that every `.so` is Deflate-compressed with the manifest extracting at
  install (the AAB check asserts the inverse for Play — see
  [Google Play](#google-play-the-app-bundle)), that the license files are
  present, and that the launcher identity is intact (label `DSD-neo`, a declared
  icon, a non-placeholder version code and the icon/splash/theme resources). The APK is uploaded as a build
  artifact, and on `main` and release tags the `Publish APK` job attaches it to a
  release (see [Release signing](#release-signing)). On `main` and release tags
  the same `build-apk` job also builds and signs the Play bundle — the `Publish
  APK` job never touches it, and a separate `AAB status` check reports bundle
  failures (see [Google Play](#google-play-the-app-bundle)).
- **`linux-ci` / android shape (headless, forced radio pipeline)** — the same
  option set on the host without an NDK, and deliberately without PulseAudio or
  ncurses installed. This is the only place the Android configuration gets test
  coverage: `--iq-replay` needs the radio pipeline, so the `DECODE_IQ_*` cases
  only register when it is forced on.

`tools/check_android_elf_alignment.sh` is the alignment check and runs locally
against any ELF (it finds `llvm-readelf` in `$ANDROID_NDK_HOME`):

```sh
tools/check_android_elf_alignment.sh build/android-arm64-release/apps/dsd-cli/dsd-neo
```

What CI does not cover is everything that needs hardware — decoding, audio, the
USB descriptor path, and battery/thermal behavior are verified by hand on a
device.

## Release signing

androiddeployqt emits a release APK that Gradle leaves **unsigned**
(`build/outputs/apk/release/android-build-release-unsigned.apk`, copied alongside
it as `dsd-neo-app.apk`), and an unsigned APK cannot be installed. Sign it with
the release keystore before pushing it to a device:

```sh
"$ANDROID_SDK_ROOT/build-tools/$ANDROID_BUILD_TOOLS_VERSION/apksigner" sign \
  --ks /path/to/dsd-neo-release.jks --ks-key-alias "$ANDROID_KEY_ALIAS" \
  --out /tmp/dsd-neo-app.apk \
  build/android-app/android/android-build/dsd-neo-app.apk
adb install -r /tmp/dsd-neo-app.apk
```

The alias is whatever the keystore was generated with — the same value CI passes as
`ANDROID_KEY_ALIAS` — so read it off the keystore rather than assuming, since a wrong
alias fails only after the password prompt:

```sh
keytool -list -keystore /path/to/dsd-neo-release.jks
```

`apksigner` prompts for the keystore password when no `--ks-pass` is given, which
keeps it out of shell history; `--ks-pass env:VAR` works for scripted signing.
Prefer `env:` over `file:` — `file:` sources are read sequentially, so pointing
both `--ks-pass` and `--key-pass` at one file fails with "end of file reached"
unless the password is repeated on a second line.
Use the release key rather than `~/.android/debug.keystore` even locally, so a
device tracking local builds keeps upgrading in place instead of hitting a
signature mismatch against published APKs. Switching an already-installed
debug-signed build over is a one-time
`adb uninstall io.github.arancormonk.dsdneo` first — Android refuses an in-place
update across a signing identity change.

CI signs the same way with the project release key and publishes the result as
`dsd-neo-android-arm64-app-<version>.apk` (`-nightly` off `main`). It needs four
signing secrets, plus one optional secret for the RadioReference import:

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | `base64 -w0 release.jks` of the release keystore |
| `ANDROID_KEYSTORE_PASSWORD` | keystore password |
| `ANDROID_KEY_ALIAS` | key alias inside the keystore |
| `ANDROID_KEY_PASSWORD` | key password |
| `RADIOREFERENCE_APP_KEY` | RadioReference application key, baked into the APK |

`RADIOREFERENCE_APP_KEY` reaches the build through the `Configure (android-app)`
step's environment as `DSD_RR_APP_KEY`, which keeps it out of
`build/android-app/CMakeCache.txt`. It is optional in every sense: unset, the
secret expands to the empty string, the build succeeds, and the app asks the
user for a key instead. It is also not a secret in the shipped artifact --
`strings` recovers it from any APK. Set it the same way as the others,
`gh secret set RADIOREFERENCE_APP_KEY` reading from stdin, so it stays out of
shell history. See `docs/radioreference-import.md`.

Generate the keystore once, outside the working tree so no `git add` can reach
it, and back it up somewhere durable before uploading anything. Android has no
key rotation: losing the keystore or its password means every installed copy has
to be uninstalled before an upgrade will apply, and there is no recovery path.

```sh
keytool -genkeypair -v -keystore dsd-neo-release.jks -alias dsd-neo \
  -keyalg RSA -keysize 4096 -validity 10000
chmod 600 dsd-neo-release.jks
base64 -w0 dsd-neo-release.jks | gh secret set ANDROID_KEYSTORE_BASE64
```

Piping into `gh secret set` keeps the encoded key out of shell history and off
disk; the three remaining secrets read from stdin the same way (`gh secret set
ANDROID_KEYSTORE_PASSWORD`, …) rather than taking `--body`. `keytool` accepts an
empty key password to reuse the store password, in which case
`ANDROID_KEY_PASSWORD` is that same value — `apksigner` still wants both.

Without the secrets the workflow still builds and uploads the APK as a build
artifact, but it stays unsigned, logs a warning, and the publish job is skipped;
on a release tag the missing keystore is a hard failure instead, because a
release must not ship an APK nobody can install. CI deliberately does not re-run
`zipalign`: the Gradle output is already 16 KB page aligned and `apksigner`
preserves that, so it only verifies the alignment (`zipalign -c -P 16 4`) after
signing.

## Google Play: the app bundle

Play will not accept an APK for a new app, so releases also produce an Android
App Bundle. Qt's Android macros already define a global `aab` target next to
`apk`; it sits outside `ALL`, so it has to be named explicitly, and the
`android-app` build preset pins its targets to `apk`. Point the build at the
directory instead:

```sh
export QT_ANDROID_KEYSTORE_PATH=/path/to/dsd-neo-release.jks
export QT_ANDROID_KEYSTORE_ALIAS="$ANDROID_KEY_ALIAS"
export QT_ANDROID_KEYSTORE_STORE_PASS=...
export QT_ANDROID_KEYSTORE_KEY_PASS="$QT_ANDROID_KEYSTORE_STORE_PASS"

cmake --preset android-app -DQT_ANDROID_SIGN_AAB=ON
cmake --build build/android-app --target aab -j
find build/android-app -name '*.aab'
```

`QT_ANDROID_SIGN_AAB` only adds `--sign` to the `aab` target — the APK target
stays governed by `QT_ANDROID_SIGN_APK` — but building `aab` re-runs
androiddeployqt with `--sign`, which also apksigner-signs the APK and overwrites
the build-tree `dsd-neo-app.apk` with the signed copy in passing. That overwrite
is also *Play-shaped*: the bundle invocation packages the in-passing APK with
uncompressed libraries and `extractNativeLibs=false` (see
[Native libraries](#native-libraries-sideload-apk-vs-play-bundle)), roughly
doubling it. A plain rebuild of the apk target will not undo this — the aab
build changes no inputs the apk target's depfile tracks, so it is a no-op.
Delete the build-tree APK first to force androiddeployqt to re-run:

```sh
rm build/android-app/android/android-build/dsd-neo-app.apk
cmake --build --preset android-app -j
``` Signing
happens in the build because androiddeployqt signs a bundle with `jarsigner`;
`apksigner` cannot sign an AAB at all, so the post-hoc recipe above does not
transfer. The four `QT_ANDROID_KEYSTORE_*` variables keep the passwords out of
your shell history and off the command line *you* type, but androiddeployqt
still forwards them to `jarsigner` as `-storepass`/`-keypass` arguments, so they
are visible in the local process table while the bundle signs.

CI builds the bundle in the same `build-apk` job, reusing the native libraries
the APK build already produced, and uploads it as the workflow artifact
`dsd-neo-android-arm64-app-<tag>.aab` (`-nightly` off `main`). Nothing consumes
it automatically, so retention is sized for hand-fetching: 90 days for a tag
bundle, 30 for a nightly. Fetch a release bundle within that window — re-running
the tag workflow after expiry rebuilds against the current toolchain rather than
restoring the same bytes. The bundle path runs on `main` as well as on tags so
it is exercised every merge rather than for the first time on a release, and
every AAB step is `continue-on-error`: a bundle-only failure surfaces through
the separate `AAB status` check instead of failing `build-apk` and taking the
APK publish down with it. Building consumes no `versionCode`, only an upload to
a Play track does. Without the signing secrets a tag fails hard at the shared
keystore step and a nightly just warns and skips, matching how the APK is
treated. Every bundle gets an SPDX SBOM artifact, and a tag bundle additionally
gets GitHub provenance/SBOM attestations — verify with `gh attestation verify`
before handing it to the Play Console
(see `docs/release-verification.md`). It is deliberately **not** a release asset:
an AAB cannot be installed, so beside the APK it only misleads, and under Play
App Signing the uploaded bundle carries the upload key while Google re-signs what
users install — it corresponds to no shipped binary. Download it by hand when
updating a Play track.

### Native libraries: sideload APK vs Play bundle

The app's 79 `.so` files (the monolithic app library plus Qt) total ~67 MiB
uncompressed, so how they are packaged dominates artifact size, and the two
artifacts deliberately differ. The bundle keeps them stored uncompressed
(`extractNativeLibs=false`): Play's delta patching diffs the raw ELF bytes, so
updates download only what changed, and the installed app loads the libraries
straight out of the APK with no second on-disk copy. The sideload APK instead
uses legacy packaging — Deflate-compressed entries with
`android:extractNativeLibs="true"` — which roughly halves the GitHub release
download (~77 MB → ~31 MB) at the cost of the platform extracting the ~67 MB of
libraries at install. Sideload updates are full downloads either way, so
download size wins there; Play installs never pay the extraction cost.

The switch lives in the vendored `android/package/build.gradle`, keyed on the
Gradle invocation androiddeployqt hardcodes: `assembleRelease` (the apk target)
gets legacy packaging, `assembleRelease bundle` (the aab target) honors the
non-legacy default. CI asserts both sides — every `.so` in the published APK is
compressed, and the AAB's base manifest keeps `extractNativeLibs=false`.

Two consequences worth knowing before enrolling in Play App Signing:

- The release keystore becomes the *upload* key. Play installs and GitHub release
  APKs then have different signatures and cannot upgrade into each other, so a
  tester on a sideloaded build has to uninstall first.
- A `versionCode` is consumed permanently the moment a bundle reaches any track
  and can never be re-uploaded. Whether a nightly bundle is safe to upload
  depends on when it was built, because the version part of the code moves at
  the version-bump commit while the distance part counts from the previous tag:
  - **After a release, before the next version bump** (most nightlies): the code
    is the last release's round thousand plus the commit distance —
    `20600042` sits above `v2.6.0` but below `v2.6.1`'s `20601000`, so the next
    release still outranks it. Uploading one to a closed testing track works
    and burns nothing the release will need.
  - **After the version bump, before its tag**: the code is the *upcoming*
    release's round thousand plus a distance that has not reset — a nightly
    after the 2.6.1 bump was `20601001`, above `v2.6.1`'s `20601000`. Uploading
    that bundle to any track permanently burns a code above the release and
    makes the release unuploadable.

  The safe habit is to upload only release-tag bundles; before uploading a
  nightly, check its `versionCode` is below the next release's round thousand.
  A respin is: once `v2.6.0`'s `20600000` is on a track, correcting that
  release means cutting `v2.6.1`, not rebuilding the tag.

## USB-OTG: native receiver descriptors

An Android application cannot open `/dev/bus/usb` nodes, so librtlsdr cannot
enumerate or open a dongle the way it does everywhere else. The descriptor is
obtained in Java and injected:

1. `UsbSourceManager.kt` matches an attached device against the RTL2832U and Airspy
   vendor/product table (`UsbReceiverKind.kt`), requests permission if it does not already have it, calls
   `UsbManager.openDevice()` and takes `UsbDeviceConnection.getFileDescriptor()`.
2. That descriptor goes to `DsdNative.nativeSetUsbFd(fd, resetFirst)` →
   `rtl_device_publish_preopened_fd()` (`src/io/radio/rtl_device.cpp`), with
   whether the attachment owes a USB reset (below).
3. `rtl_device_create()` sees the injected descriptor and calls `rtlsdr_open_fd()`
   instead of `rtlsdr_open()`. That entry point — added by the patch below — turns
   libusb device discovery off and wraps the descriptor with
   `libusb_wrap_sys_device()`, then runs the normal claim/probe sequence.
4. Because discovery is off, `rtlsdr_get_device_count()` reports zero. The engine
   would read that as "no supported devices" and abort the run, so
   `dsd_engine_setup_enumerate_rtl_devices()` short-circuits to a single device at
   index 0 whenever a descriptor is set. The app is what selected the device.

For Airspy, the broker recognizes `1d50:60a1` and routes the descriptor through
`nativeSetAirspyUsbFd()` to the native adapter's `airspy_open_fd()` path. Source
kind and an explicit serial are checked before a session starts. Readiness and
native descriptor-in-use reporting cover both drivers; switching between USB
receivers requires the current session to have ended (one still configured, or
still holding its descriptor, counts). Both drivers share the vendored
libusb target, and Java retains descriptor ownership until native transfers end.

### Receiver status on Listen

Listen keeps the receiver's status in a pill beside the title, in a header that
stays put while the list scrolls (the desktop has no pill and its title scrolls
as before). One shared reading (`DeviceStatus.qml`) names the receiver —
RTL-SDR, Airspy, or "receiver" when it cannot tell — for Listen, Onboarding and
the Monitor:

| Pill | Meaning | Tap |
| --- | --- | --- |
| `RTL-SDR READY` | connected and permitted | — |
| `CONNECT RTL-SDR` / `CONNECT RECEIVER` | plugged in, not opened yet | Connect |
| `RTL-SDR WAITING` | Android's permission prompt is up | — (Connect returns after 60 s without an answer) |
| `NO RTL-SDR` / `NO RECEIVER` | the requested receiver (or any) is not attached | Retry / Connect |
| `RTL-SDR NO ACCESS` | permission denied | Retry |
| `RTL-SDR ERROR` | Android could not open or claim it | Retry |
| `RTL-SDR LOST` | it was unplugged or stopped sending samples during the last session | Retry |

An Airspy reads `AIRSPY` in place of `RTL-SDR`. Which receiver is named: the
connected one, else the one the last session lost, else the one last requested,
else the only kind on the bus. With both kinds attached and nothing requested it
reads `CONNECT RECEIVER`. The pill and the details card under it only ask for
access; they start nothing, and they act only while no session runs. When the
label does not fit beside the title (narrow screens, large fonts) the pill moves
under it and wraps instead of cutting the label off. Onboarding words its card
from the same reading ("Airspy detected", "No receiver detected", …).

### When a receiver ends a session

A session whose receiver is unplugged, or stops delivering samples (its reader
returns, or no samples arrive for 5 s), ends by itself. The Monitor's LIVE pill
turns to `LOST` at once, while the session still saves and uploads its last calls;
tapping it explains that the session is finishing and offers **Stop now**, which
drops the uploads still waiting. On a short landscape Monitor, where the header
scrolls away, a fixed `RTL-SDR LOST` line above Stop does the same. A stalled
RTL-SDR is reset and reopened in place, once per stall, as on the desktop
(`docs/cli.md`, "Stall recovery"), and the Monitor shows "RTL-SDR stopped sending
samples; reset and reopened" for 5 s while the session goes on (#782). The app
resets it through its own USB descriptor, after the 2 s grace below lets a detach
broadcast own an unplug; the descriptor's own VID:PID and strings identify the
dongle, so no unique serial is needed. Only when that does not bring samples back
(or the dongle stalls again within 60 s) does the session end; an Airspy, which is
never reset, ends as before. If the descriptor's identity cannot be read, the open
logs `RTL-SDR: stall recovery is off for this receiver: its descriptor's USB
identity could not be read (errno <n>)` and a stall ends the session.

Once the session has ended, Listen shows a notice in place of the receiver card:
"RTL-SDR was unplugged, so <system> stopped. Plug it back in, then tap Reconnect."
or "RTL-SDR stopped sending samples, so <system> stopped. Tap Reconnect to reset it
and start again." (an Airspy's: "… Tap Reconnect to start again."). **Reconnect**
runs the usual USB access (a permission prompt if one is needed) and then starts
the same system, scan list or Explore session; the RTL-SDR's open resets it first.
With Signal search open, its screen says to plug the receiver back in and start
the search again. **Dismiss** clears the notice and cancels a Reconnect still
waiting. After the Activity is recreated (for example after Android reclaimed
it in the background) the app cannot prove which saved system the service was
running, so Reconnect there only asks for access and the notice ends "…tap
Reconnect, then start it again."

A stop you ask for first wins: Stop and then an unplug while the session winds
down shows no notice; an unplug and then Stop now keeps it. The receiver's own
end is detected natively — see `docs/code_map.md` "USB reader exit" and "USB
sample-progress monitor" — with a 2 s grace that lets the detach broadcast own an
unplug, so an unplug reads "unplugged" rather than "stopped sending samples".

An ending acknowledged by a new start, or by an access request (Reconnect, the
pill) once its session ended, stays acknowledged after a recreation: the service
keeps the mark (`DecoderService.acknowledgeReceiverEnding()`, published as
`endingAcknowledged` in its lifecycle record), so the pill does not read `LOST`
again over a receiver that is connected and ready.

Known limits: the notice names only the receiver's kind, so after a recreation
with two Airspys attached Reconnect may ask for the other one; the pill names, and
Connect asks for, the last requested kind before the kind on the bus, so after an
RTL-SDR request with only an Airspy plugged in it reads `NO RTL-SDR` and Retry
answers "No RTL-SDR attached" until an Airspy system is started; **Dismiss** hides
the notice for that Activity only, as it does any failure, so a recreated Activity
shows a dismissed (but not acknowledged) notice again; a healthy session
reattached after a recreation shows LIVE with no receiver sheet (LOST still shows
if it ends).

### What a failed start says

A start that fails says why on Home's failure card (#782), and the card's own
failure stays until the next start, Retry or Dismiss, so a poll of an older
session can never put that session's failure back:

- **The previous session is still stopping:** "The previous session is still
  stopping. Tap Retry to start this one once it has stopped."
- **The decoder service did not take the start within 2 s:** "The decoder did not
  respond in time, so the session was not started. Tap Retry, or open Details if
  this keeps happening."
- **The receiver's USB permission was revoked:** "RTL-SDR needs USB permission
  again, so the session was not started. Tap Retry, then allow access when asked."
- **Android could not open the receiver again for this session:** "RTL-SDR could
  not be reopened, so the session was not started. Tap Retry, or unplug it and plug
  it back in if this keeps happening."
- **The receiver is no longer connected:** "RTL-SDR is no longer connected, so the
  session was not started. Plug it back in, then tap Retry."
- **The RTL-SDR or Airspy failed to open in the decoder:** "The receiver could not
  be opened or stopped receiving. Check its USB connection and settings."

An Airspy is named "Airspy", and a start the app cannot tie to a receiver says "The
receiver". For the three USB cases, Signal search's own screen says the search was
not started and to start it again. A start that timed out can never run later:
Dismiss gives it up, and Retry or another start replaces it. Settings → Diagnostics
keeps the line behind each (`Start attempt N refused: the decoder service is
STOPPING`, `Start was not delivered to the decoder service within N ms (service
state X)`, `usb: <device> is no longer attached; not reopened`, ...).

### Permission requests and detaches

`UsbSourceManager.kt` keeps its request bookkeeping in the plain-Kotlin
`UsbRequestState.kt`. Each permission prompt is a new generation: its
`PendingIntent` uses the generation as request code (`FLAG_CANCEL_CURRENT`) and
carries it as an extra, and a reply for another generation or device is ignored.
The Airspy serial search's hop to the next device keeps the request's generation
and restarts its wait. A prompt left unanswered for 60 s expires on the host's
250 ms status poll: the record says "No answer to the permission request",
Connect comes back, and nothing prompts again until asked; a late answer is
ignored. Unplugging the requested device retires its prompt and says "Device
detached: <device>", so a re-plug can prompt at once. While a prompt is
outstanding, auto-start waits.

A plain Connect asks for the last requested receiver (kind and serial), then the
connected kind, then the first known receiver on the bus. An attachment replaces
the card's text only when nothing is connected and either nothing was requested or
it is the requested receiver (an RTL-SDR plugged in never clears "Requested Airspy
serial not found"); a request
that finds its receiver already open rewrites "Ready: <device>". Asking for
another receiver while a session runs (the other kind, or another Airspy serial)
is refused with "Stop the current receiver before switching USB devices", which is
request feedback, not a device failure.

A detach of the connected receiver (or one whose device Android does not name)
releases the connection; unplugging some other device releases nothing. It stops
the engine only when the session uses that receiver: configured or running on a
local RTL-SDR or Airspy (`nativeSessionUsesLocalReceiver()`), or holding its
descriptor (`nativeIsUsbFdInUse()`). A detach during a start is handled after
configure returns, by this same path: configure and the detach broadcast both run
on the main thread, and by then the native record names the session's receiver.
Unplugging a retained receiver no session uses, or one during an rtl_tcp session,
stops nothing.

One serialized owner holds the broker's state (#782): changes run on the main
thread, where the USB broadcasts and the service's start arrive, and the Qt
thread's Connect, Retry and pill requests are posted there. Two reads of the Qt
thread's poll also change state, each in one step under the broker's lock:
`deviceStatus()` retires a permission request whose answer never came, and
`takeAttachment()` takes the attach edge. Otherwise the lock only makes the record
readable from the poll, and it is never held across a binder call: the bus listing
and the serial reads come first, and the poll's own listing for an attachment is
dropped when an attach or detach was applied while it was taken, so it can never
clear a newer attachment. While a session reopens its receiver, the poll reads the
record from before the reopen, never the moment between the old connection's close
and the new one's publish as a receiver that is not ready.

Each session gets a connection of its own (#782). The first session after an open
takes that connection; every later start of a local receiver, on the main thread
between STARTING and `nativeConfigure()`, closes the connection the last session
used, opens the same `UsbDevice` again (no prompt while permission holds) and
publishes the new descriptor, so a descriptor a session left unusable (the
reporter's "Failed to open rtlsdr device from descriptor") is never handed on.
A start finding the previous session still configured or still holding its
descriptor is refused as "previous session still stopping". If the device is no
longer listed (or another serial now carries its name), permission was revoked, or
Android cannot open it, the start fails before configure (`usb_gone`,
`usb_permission`, `usb_open_failed`), the broker reads not ready, and Connect or
Retry takes the normal request path. A switch to another receiver is refused while
a session is configured, not only while it runs. The broker's record names the
ready connection (`device`, `epoch`: the attach it came from, `connection`), each
session's record names the connection it took, and the host shows a session's claim
error only while that same connection is ready: two dongles with the same or no
serial, or a replacement connection to the same dongle, never show another's.
libusb's own error and warning lines reach the diagnostics through its log callback
(`rtl_usb_log_install()`, set before every descriptor open).

A stalled RTL-SDR owes a USB reset before its next open (#782): reopening the same
dongle does not clear a wedged bulk stream, and `USBDEVFS_RESET` on the app's own
descriptor does. The engine sets the obligation for the descriptor it holds when
the stall is detected (before the grace, so a Stop then keeps it), or when its
reader gives up by itself, and clears it once samples arrive after that (also in a
session whose descriptor came owing one and was refused its reset) or a reset
succeeds (`nativeUsbResetOwed()`). So every "stopped sending samples" ending of an
RTL-SDR leaves it owing the reset its notice promises. The broker records it on the
physical attachment (`UsbResetObligations.kt`: the device name, with VID:PID and
serial; a device keeps its name while it stays attached, so only a detach ends the
attachment) after each run, and before it lets a connection go, and hands it with
the next descriptor of that attachment; the open resets first. A refused reset keeps
it; a detach drops it. Selecting another receiver never applies one attachment's to
another. The obligations are kept in app-private storage
(`noBackupFilesDir/usb-reset-owed`), read and written on a storage thread of their
own, in order, never on the main thread; the record in memory is authoritative. The
read starts at process start, and the first open applies it (waiting up to 1 s for
the read if it has not finished) before it publishes a descriptor; nothing is
written before it was applied. An entry whose device is no longer attached is
dropped then, and one still attached is honoured on its first open (the worst a
stale one costs is one reset that was not needed).

`ANDROID_USB_BROKER_JVM` runs `UsbRequestState.kt` and the real
`UsbSourceManager.kt` on the JVM against stubs of the USB service and its prompts.
The request-code identity itself is checked against the Android documentation and
these stubs only: the emulator exposes no USB device to ask permission for.

### USB diagnostics and Retry

Onboarding and Home's receiver card show the USB diagnostic and a **Retry**
button. If `UsbManager.openDevice()` returns null, the app rechecks the device
list and permission: the result is `detached`, `permission`, or `open_failed`,
in that order. A null return with permission still granted says
“Android could not open <device> (open_failed).” Android supplies no numeric error
or owner information for that return, so it cannot establish that another app
holds the dongle. Only a native librtlsdr claim failure with libusb code `-6`
is classified as `DeviceBusy` and says: “Android could not claim <device>. It may
be held by another SDR app, or the OTG port may be under-powered — try a powered
hub.” Other native failures say “Android could not open or claim <device>
(code <code>).” Retry rechecks USB access (and can request permission again);
after access is ready, tap Play to retry the native claim. The original run
failure remains available in session diagnostics. Retry is disabled until the
session has stopped. Detach and permission failures take precedence over an old
native claim diagnostic; a successful new session clears that native error.

Java keeps ownership of the descriptor throughout: `libusb_wrap_sys_device()` does
not take it over. Closing the `UsbDeviceConnection` while the engine still has the
descriptor wrapped is a use-after-close, so `UsbSourceManager.release()` clears the
native slot immediately — that only affects the *next* open — and then hands the
close to a background thread. That thread waits only while the engine holds that
connection's own descriptor (`nativeHeldUsbFds()`, #782): native marks a descriptor
held in the same locked step that takes it from the slot, so once the slot is
cleared a session that has not taken it never will, and a later session on a
replacement connection, holding another descriptor, keeps nothing waiting. An
rtl_tcp, UDP or file session never takes it, so a receiver unplugged during one is
closed at once. Both callers (a detach broadcast, a source
switch while stopped) therefore return without blocking. If the engine never lets
go within 10 s, the connection is deliberately leaked: one
descriptor held for the rest of the process's life beats pulling it out from under
an in-flight USB transfer.

Note that discovery does not come back. `rtlsdr_open_fd()` sets libusb's
process-global `LIBUSB_OPTION_NO_DEVICE_DISCOVERY`, which has no counterpart —
and costs nothing here, since an app cannot enumerate `/dev/bus/usb` anyway.

`rtlsdr_open_fd()` exists only in the vendored tree, so configuring an Android
build with `DSD_ENABLE_RTLSDR=ON` and `DSD_ANDROID_VENDORED_RTLSDR=OFF` fails at
configure time rather than at link time or on the device.

Attaching a listed dongle launches the app through the manifest's
`USB_DEVICE_ATTACHED` filter, which is the only way Android grants device
permission without a prompt. `res/xml/device_filter.xml` holds the same ids as the
Kotlin table in `UsbReceiverKind.kt`; add rebadged dongles to both.

Before any permission is asked, `UsbSourceManager.attachedSource()` reads the kind of
receiver plugged in (`usb`, `airspy`, `both`, or empty) from `UsbManager.deviceList`,
which needs no permission, counting only devices in that table. The host polls it into
`DecoderHost::attachedDeviceSource`, and Explore and Signal search start on the
attached kind when the remembered USB kind is not plugged in.

## Auto-start on USB attachment

Settings → Listening → **Start when a dongle is attached** is off by default and
appears only on hosts that broker local USB access ("Resume the last
USB-receiver system or scan list"). When enabled, an attachment resumes the last
successfully initialized saved system or scan list if it still exists, uses an
RTL-SDR or an Airspy, and is on the kind of receiver that just attached: an
Airspy plugged in resumes an Airspy target and never an RTL-SDR one, whatever
else is on the bus. Explore and network/file sources are never selected. The
app must have completed onboarding, be Idle (Failed does not qualify), and have
no overlay or USB permission request open; a permission request that expires
unanswered lifts the block. A suppressed attachment is consumed;
closing a sheet or enabling the preference does not retry it. Existing start and
permission failures use the normal banner. Last-started identity changes only
on confirmed session initialization, never when the service queues a start.

Activity cold-start/new intents and receiver broadcasts share one dedupe tracker.
It validates deviceName and VID/PID against the current USB device list, retains
one pending attachment (newest wins), and forgets suppression on detach. Polling
consumes that pending identity, including on the first host refresh. App-specific
intent input supplies no decoder configuration or bypass of the opt-in policy.

Host regressions: `UI_QT_AUTO_START_POLICY`, `UI_QT_CONTROLLER`,
`UI_QT_PERSISTENCE`, `UI_QT_QML_CALL_LISTS` and `UI_QT_QML_MAIN_RECEIVER_NOTICE`.
The Android-free Kotlin tracker reports each attachment as `<kind>:<device>` and
runs on the JVM in `ANDROID_USB_BROKER_JVM`; verify the rest on a device:

- Cold launch from the USB intent followed by the receiver's duplicate report
  starts exactly once; an Activity recreation or duplicate new intent does not
  restart it. Detach and reattach permits one new start.
- Mismatched deviceName or VID/PID, unknown devices, and a device removed before
  polling produce no start. With two devices, only the newest pending attachment
  is delivered; detaching it cancels the pending delivery.
- Opt-out, incomplete onboarding, Starting/Running/Stopping/Failed, an open
  overlay, a pending permission request, a deleted target, an Explore,
  network or file target, and a target on the other receiver kind suppress the
  start. Removing the blocker alone does not retry.
- Saved RTL-SDR and Airspy systems and scan lists resume through the existing permission
  flow. Denial, invalid configuration, or failed tuner initialization shows the
  existing banner and leaves last-started identity unchanged; successful
  initialization updates it. Test both cold and already-running Activity intents.

## Power

An RTL-SDR draws roughly 300 mA, and many phones cap what they will supply over
OTG — some silently, some by dropping the device mid-stream. If the dongle
enumerates but the run dies or never starts, try a **powered OTG hub** before
suspecting the software. A powered hub is the recommended setup for anything
longer than a quick test; it also keeps the phone from discharging into the
dongle. Bias-tee power for an external LNA comes out of the same budget — the UI
exposes it as a checkbox on the USB source, and it is off by default.

### Screen

Settings → Listening → **Screen** sets what the display does while DSD-neo is
open. The row appears only where the host supports it (the Android app):

- **System default** — the phone's own screen timeout, as if DSD-neo set
  nothing.
- **Always on** — the screen stays on while DSD-neo is on screen, listening or
  not. This is what the old **Keep screen awake** switch did.
- **Dim between calls** — while listening, the screen dims to minimum
  brightness after the delay with no audible audio or touch, and audio or a tap
  brightens it. While DSD-neo is on screen it never turns off, and it never
  wakes.
- **Off between calls** — while listening, the screen stays on through audible
  audio and for the delay after it, then dims and is left to the phone's own
  timeout. New audible audio turns it back on, under the wake rules below.

**Dim after** (or **Turn off after**, in Off between calls) sets the delay for
the two between-calls modes: 10 s, 30 s, 1 min, 2 min or 5 min, 30 s by default.
With the decoder stopped, both modes behave like System default.

**Audible** means audio DSD-neo actually plays. A muted Monitor, a slot that is
switched off, a blocked, locked-out or skipped talkgroup, and an encrypted call
with no usable key (unless a setting plays it anyway) never count, and neither
do tones or audio with no output to play it on (the `null` output). On the
analog monitor (FM or AM) a channel counts while its carrier is open and passes
the tone filter (CTCSS/DCS); with the squelch off and no tone filter the monitor
plays all the time, so it always counts. An EDACS analog call counts while its
squelch is open, and all the time when the call runs no squelch.

Timing:

- The delay runs from the last audible audio, touch, decoder start, return to
  DSD-neo, or change to these settings.
- In Off between calls the screen goes dark at the later of two points: the end
  of the delay, and the phone's own screen timeout after the last touch or the
  last automatic wake (Android counts a wake as user activity: on an API 34
  emulator the screen went off 15 s, the phone's timeout, after an automatic
  wake). On most phones the lock screen applies its own, shorter timeout.
- The decoder's status is read once a second, so a wake follows the audio by up
  to a second. Audio more than 2 s old when it is read never wakes the screen.
- Audio heard just after the screen goes off with DSD-neo in front, before
  Android has both stopped DSD-neo and announced the screen-off, wakes the screen
  once both have happened, if the audio is no more than 2 s old by then.
- The first readable status of a session, usually from the second read (the
  first only wakes the status publishers), never wakes the screen. The audio it
  reports was played in this session, so it counts for the delay and for a
  power-button snooze.

In Off between calls, new audible audio turns the screen on only when:

- DSD-neo was the app in front, with focus, when the display went off. A
  decoder behind another app never wakes the screen, nor does the unfocused
  side of split screen.
- No phone or VoIP call is active or ringing, and Do Not Disturb is off (any of
  its modes blocks wakes).
- Wakes are not snoozed. Turning the screen off with the power button within
  the delay after audible audio snoozes them until a full delay passes with no
  audible audio. A call that begins after the screen went off is never snoozed
  by that press. Touches and starts never snooze.

A wake only turns the screen on: DSD-neo does not show itself over the lock
screen, so a locked phone wakes to its lock screen. The screen then stays on
through the audio and the delay after it, and then goes off on the phone's timer
(on most phones, the lock screen's own shorter timeout); a phone left alone
wakes again for the next call. A delayed screen-on broadcast still belongs to
the successful wake that caused it; broadcast latency does not stop repeat wakes.
Anything other than DSD-neo's own wake that turns
the screen on stops wakes at once: you, another app or a notification, an
incoming call, or on some phones plugging in the charger. So does unlocking. A
call that arrives in the moment between such a screen-on and Android telling
DSD-neo of it changes nothing: DSD-neo finds the screen already on, leaves it
alone, and does not count that screen-on as its own.
After a wake, so does a screen that goes off more than 5 s after the later of
two points: the end of DSD-neo's hold, and the wake plus the phone's screen
timeout setting. The lock screen's own timeout plays no part; keeping the screen
on by touching the lock screen is what can push the screen-off that late. Either
way, wakes stay stopped until DSD-neo is back in front and the display goes off
there again. That sleep in front arms wakes afresh, however long DSD-neo was in
use after the wake (or, on a phone with no screen lock, after the wake showed
it).

The lock-screen readout is the **Decoder** notification. Allow DSD-neo's
notifications and enable lock-screen display of silent notifications in Android
settings; the channel is deliberately silent. Android can hide silent
notifications on the lock screen even when notifications are allowed, leaving
only the clock after a wake. Channel and lock-screen privacy settings still
control which call details appear. Waking the display does not override them.

Dimming happens only while DSD-neo's window has focus and is not in split screen
or another multi-window mode, because the brightness an app sets applies to the
whole display. Otherwise the screen stays on undimmed (Dim) or goes off on the
phone's timer (Off). The first tap on a dimmed screen only brightens it: the
whole gesture is swallowed, so the tap does not press what is under it. With
TalkBack's touch exploration on, a touch still brightens the screen but is never
swallowed.

If Android refuses a wake and the screen stays off, Settings → Diagnostics
records "Screen wake refused by Android; calls cannot turn the screen on" once
per session, and DSD-neo makes no further wake attempt until a full delay passes
with no audible audio.

Two platform limits are accepted rather than solved:

- Android delivers lifecycle callbacks asynchronously. Home followed quickly by
  Power, with the Home processed late on the main thread, looks exactly like
  DSD-neo going to sleep in front, so later calls wake the screen.
- Quick use of the secure camera from the lock screen soon after a wake gives
  DSD-neo no callback at all, so the next call wakes the screen again.

The wake is a `SCREEN_BRIGHT_WAKE_LOCK` with `ACQUIRE_CAUSES_WAKEUP`. At
targetSdk 36 it needs no permission beyond `WAKE_LOCK`: on an API 34 emulator
the platform logged "Allowing device wake-up without
android.permission.TURN_SCREEN_ON" for this app and turned the screen on. AOSP's
`REQUIRE_TURN_SCREEN_ON_PERMISSION` compatibility change is not enabled for apps
targeting SDK 36, as of Android 16 (API 36); a target SDK bump that enables it
will need `android.permission.TURN_SCREEN_ON` in the manifest and the user's
grant, or every wake is refused.

An install that used **Keep screen awake** reads as **Always on** (switch on) or
**System default** (switch off) until a mode is chosen. The old setting is never
rewritten, so an older build finds the switch as it was.

`ANDROID_SCREEN_POLICY_JVM` runs the policy on the JVM, and `UI_QT_PERSISTENCE`
and `UI_QT_QML_CALL_LISTS` the settings. What only a phone shows, verify on a
device, with a source that has known calls (an I/Q replay works):

- System default and Always on behave as the phone and the old switch did, with
  and without a session.
- Dim between calls: the screen dims after the delay with no audio; a call or a
  tap brightens it; the first tap presses nothing; it never turns off.
- Off between calls: the screen stays on through a call and the delay, then
  goes off at the later of the delay and the phone's timeout; the next call
  wakes it to the lock screen within about a second, and again after the screen
  times out untouched.
- Repeat the wake cycle with delayed `SCREEN_ON` delivery (for example, a test
  build that stalls the Android main looper for 3 s just after a successful
  pulse). The screen must still stay on for the remaining audio delay and wake
  on subsequent calls. A later manual wake must still disarm it.
- Allow notifications but hide silent notifications on the lock screen: a wake
  shows only the clock. Enable silent lock-screen notifications: the Decoder
  readout appears, subject to the phone's privacy settings.
- Each delay choice, with a phone timeout shorter and longer than the delay.
- Power pressed during or just after a call snoozes wakes until a full delay of
  quiet; pressed after a quiet delay, it does not.
- Swipe, PIN and no screen lock: what each wake shows; unlocking stops wakes
  until DSD-neo is in front again.
- Wake, then unlock, use DSD-neo and let it time out in front: the next call
  wakes the screen. With no screen lock, use DSD-neo after the wake shows it,
  then let it time out: the next call wakes.
- A screen-on DSD-neo did not cause (the power button, a notification, a
  charger plug-in where the phone turns the screen on for it) stops wakes.
- Keep touching the lock screen after a wake until it stays on more than 5 s
  past both DSD-neo's hold and the phone's timeout from the wake: further wakes
  stop; a shorter touch does not stop them.
- Split screen: no dimming; the unfocused side never arms wakes.
- Rotation while dimmed, while the screen is held on, and before a sleep:
  nothing sticks or resets.
- TalkBack: a touch on a dimmed screen brightens it and still acts.
- Do Not Disturb, and a phone or VoIP call: no wakes.
- Mute on the Monitor: nothing counts as audible, so no wakes, and the screen
  dims and goes off as between calls.
- Leaving DSD-neo (Home, Recents, another app) before the display goes off: no
  wakes.
- Migration from a build with Keep screen awake on, and off.

## Vendored third-party code

`third_party/` carries trimmed snapshots of three upstream projects. They are **not**
under `src/third_party/`: that perimeter has repo-wide guardrails, and these build
privately for the Android app only.

| Project | Version | Upstream commit | What is vendored |
| --- | --- | --- | --- |
| libusb | v1.0.30 | `87a55632db62c9bdc58cd31d3ccfa673f1bb017f` | The exact source list upstream's own `android/jni/libusb.mk` builds — the `linux_usbfs` backend plus the POSIX event/thread shims — with upstream's `android/config.h` |
| librtlsdr | v2.0.3 (osmocom) | `797f8143266d983c56d8f35d2d442527529dd8a5` | The library only: `librtlsdr.c`, the five tuner drivers and `include/`; the `rtl_*` command-line tools and their helpers are dropped |
| libairspy | 1.0.12 (airspyone_host) | `bd15be38e91ebaa3e0bebb1e320255bde4ccf059` | The library only: `libairspy/src` (`airspy.c`, the IQ converters and their headers); the `airspy_*` command-line tools are dropped |

librtlsdr is GPL-2.0-or-later and libusb is LGPL-2.1-or-later; both upstream
license texts travel with the snapshots (`*/COPYING`) and both are listed in the
repository's `THIRD_PARTY.md`. libairspy (BSD-3-Clause and MIT) keeps its notice
in `libairspy/LICENSE`, also listed there.

These trees keep upstream formatting and are excluded from every repo tool
(`.clang-format-ignore`, `tools/format.sh`, `.githooks/pre-push`), so do not
reformat or "fix" them — the point is that they stay diffable against the release
they came from.

### The librtlsdr patches

The `*-librtlsdr-*` patches in `third_party/patches/` are already applied to the
vendored copy, in order; they are kept as files so the delta stays reviewable and can be re-applied
when the snapshot is refreshed.

- `0001-librtlsdr-add-rtlsdr_open_fd.patch` adds `rtlsdr_open_fd()` and splits the
  claim/probe tail of `rtlsdr_open()` into a shared helper so both open paths run
  identical initialisation.
- `0002-librtlsdr-check-async-buffer-allocation.patch` makes the async reader check
  every transfer and buffer allocation: a failure frees what was obtained and
  `rtlsdr_read_async()` returns `-ENOMEM` instead of submitting NULL transfers.
  `VENDORED_LIBRTLSDR_ASYNC_ALLOC` (a desktop test) fails each allocation in turn.
- `0003-librtlsdr-account-for-every-transfer.patch` makes the async reader count
  the transfers libusb owns. A transfer lost to a failed completion or
  resubmission no longer leaves the reader waiting forever: once none is left it
  marks the device lost and `rtlsdr_read_async()` returns `-1`. Every exit
  (cancel, lost device, event error, failed submission) cancels the transfers and
  keeps handling events until each has been called back, and only then frees
  transfers and buffers; the first error is the return value, even when the
  device is lost while the reader drains.
  `VENDORED_LIBRTLSDR_TRANSFERS` (a desktop test) completes, fails and cancels
  transfers from a second thread.

To move to a newer librtlsdr:

```sh
git clone https://gitea.osmocom.org/sdr/rtl-sdr.git /tmp/rtlsdr
git -C /tmp/rtlsdr checkout <new-tag>
cd /tmp/rtlsdr && for p in <repo>/android/third_party/patches/*-librtlsdr-*.patch; do patch -p1 < "$p"; done
# then copy src/librtlsdr.c, src/tuner_*.c and include/* over the vendored tree,
# regenerate the patches against the new baseline, and update the table above
```

### The libairspy patch

`0001-libairspy-wait-for-cancelled-transfers.patch` is already applied to the
vendored copy, like the librtlsdr ones.

- It makes the device count the transfers libusb owns. A stop cancels them and
  keeps handling events until each has been called back, so `airspy_close()`
  never frees, and a restart (a retune's stop and start) never resubmits, a
  transfer libusb still owns. A start whose submission fails gives back the slots
  of the transfers it never submitted, and the stop after it waits for the rest.
  A stream that ended by itself is reaped the same way before a start or a
  packing change reuses its transfers. `streaming` and `stop_requested` become
  C11 atomics, since the library's own threads share them.
  `VENDORED_LIBAIRSPY_TRANSFERS` (a desktop test) drives the library's threads
  against a stub libusb whose cancellations call back late.

To move to a newer libairspy:

```sh
git clone https://github.com/airspy/airspyone_host.git /tmp/airspy
git -C /tmp/airspy checkout <new-commit>
cd /tmp/airspy && for p in <repo>/android/third_party/patches/*-libairspy-*.patch; do patch -p1 < "$p"; done
# then copy libairspy/src/* over the vendored tree, regenerate the patches against
# the new baseline, and update the table above, README.dsd-neo and the upstream
# hashes in tests/tools/test_airspy_packaging.py
```

## Codec2, libcurl and expat

All three come from vcpkg and all three are required by the Android presets
(`DSD_REQUIRE_CODEC2`, `DSD_REQUIRE_CURL`, `DSD_REQUIRE_EXPAT`), so a detection
regression fails configure rather than producing an APK that quietly decodes no
M17 voice or has lost the RadioReference import. The triplet links statically,
so they end up inside `libdsd-neo-app_arm64-v8a.so` — there is no extra `.so` to
package and no `System.loadLibrary` change.

libcurl backs the rdio-scanner upload path and the RadioReference client, both
through `src/runtime/curl_common.c`. `android.permission.INTERNET` is already
declared, and because OpenSSL's compiled-in `/etc/ssl/certs` does not exist on
Android, that helper supplies the trust store itself.

`CURLOPT_CAPATH` alone does not work here, which cost a device debugging session
to establish: the path is set, the directory exists and is readable, no SELinux
denial is logged, and the same store verifies the same chain on a desktop with
the same OpenSSL — yet in the app process the hashed-directory lookup resolves
nothing and every `https://` request fails with `CURLE_PEER_FAILED_VERIFICATION`.
So the system roots (`/apex/com.android.conscrypt/cacerts`, falling back to
`/system/etc/security/cacerts`) are read once into a single PEM blob and handed
to libcurl through `CURLOPT_CAINFO_BLOB`; only the `BEGIN..END` block of each
file is taken, because Android's are a certificate followed by a text dump.
`CURLOPT_CAPATH` is still set, but only reaches a build older than libcurl
7.77.0 — anywhere `CAINFO_BLOB` exists, the blob is what makes TLS work.

expat parses the RadioReference SOAP responses. It is a pure-C port with no Qt
module behind it, so androiddeployqt has nothing extra to package.

## Known limits

- **arm64-v8a only.** armv7 would run scalar-only DSP and is out of scope.
- **One decoder per process.** The service rejects a start unless it is idle.
- **SoapySDR is not supported** and is not planned.
- **No mic input in the UI.** The AAudio backend implements capture, but
  `RECORD_AUDIO` is deliberately not requested.
- **An open output stream is never idle.** Once a chunk of real audio has played,
  `conceal_has_good` stays set for the life of the stream, so the pump keeps waking
  on its 20 ms cadence and topping the device up with concealment whenever the ring
  is empty and the device queue falls below `DSD_AAUDIO_CONCEAL_LOW_WATER_MS`. That
  is what stops a decoder gap from becoming an audible stutter, and the writes are
  paced by the blocking `AAudioStream_write`, but it does mean the pump plus the
  AAudio mixer stay resident for as long as a run lasts rather than only while a
  call is on air. This is the first thing to measure in the battery soak below.

## Diagnostics

Settings → Diagnostics shows current-process decoder and host diagnostics plus a
bounded redacted persistent tail from previous runs, including how Android says
recent app processes ended. It captures no crash stacks or tombstones; the one
stack it records is the main thread's during a stall (see "Main-thread watchdog").
The process ring holds at most 2000 entries, each at most 512 UTF-8 bytes. Pause
freezes the displayed list while capture continues; Copy and Share
include the current ring, after the host records (see below). Clear removes the ring, the host records, pending earlier
records and both persistent stores; records captured after Clear remain available, and each saved store starts again
with the running process's `process:` line (never the cleared session's). Diagnostics deliberately survive decoder
Starting/Idle/Failed transitions.

Host records arriving before the first decode start do not depend on the runtime sink or
stderr pump. Tap installation drains their bounded buffer to the ring and the tail at
Qt's initialized application-data location. The log itself (its directory and both saved
stores) is loaded on a thread of its own, so the service's `nativeInit` on the main thread
does not wait for flash. Until it is ready, host and decoder records wait in that bounded
buffer, a startup-window request is applied when it is ready, and the teardown flush waits
for it only within its own 500 ms bound.

A process-lifetime runtime log tap, the stderr pump, and host messages all enter
`DiagnosticsLog::submit()`. It filters sensitive records before ring insertion and
queues only redacted bytes to one asynchronous writer. Oversized stderr lines
are omitted rather than split into potentially unlabelled secret fragments, and the
stderr lines after a dropped line that could still be its value (one token, key
characters only, or the rest of a quoted string) are dropped with it.
librtlsdr's, libusb's and dsd-neo's own rtl/usb error lines, and the service's
`engine run returned` and `engine configuration returned` lines, keep their negative codes
(`usb_claim_interface error -1`): a record that is wholly one of those producers' fixed
lines is exempt from the key-option rule.

### The persistent tail

The writer keeps the newest 256 KiB as two append-only segments in
`files/diagnostics/`: `tail.prev.log`, then `tail.log`, at most 128 KiB each.
- **Writing:** records are appended, never rewritten. The writer drains every
  250 ms and syncs at most once a second, so a trunked session that prints about
  70 lines a second writes those lines and nothing more. Before issue #745 the
  whole 256 KiB tail was rewritten and synced for every line, about 12 MB/s on a
  P25 trunk.
- **Rotation:** a full segment is synced and becomes the previous one.
- **Limits:** a segment untouched for seven days is deleted at startup. The bounded
  queue drops its oldest records under sustained storage backpressure, and disk
  failures never stop decoding.
- **Recovery at load:** the loader drops a record a crash left half-written,
  deletes temporaries a kill left behind, and splits an older build's single
  `tail.log` at line starts.

### Host and session records

A fast trunk scan prints hundreds of decoder lines a minute, which push the
`[host]`, `[session]` and `[process]` records out of the 2000-entry ring within
about 90 seconds and out of the persistent tail soon after (issue #782). Those
records, the service's lifecycle, the USB steps and how the previous run ended, are
what explains a failure, so they are also kept in a store of their own:
- **Live:** a ring of at most 500 records, each at most 512 bytes and redacted like
  every other record.
- **Saved:** `host.prev.log`, then `host.log`, in `files/diagnostics/`, at most
  32 KiB each (64 KiB together). The same writer appends them on the same drain and
  sync cadence. They rotate, start each segment with the `process:` and
  `session starting:` lines, recover at load and expire after seven days exactly as
  the tail's segments do.

Copy and Share start with these records under `--- host and session records ---`,
the previous run's first under `--- previous run ---`, then give the full log under
`--- all records ---`. The records therefore appear twice when the ring still holds
them. The screen shows the full log only. Clear removes this store with the rest.
The watchdog, teardown and screen records below are host records too. Written
from Kotlin, they can come before the native library is ready; up to 64 wait for
it in order (later ones are counted and reported as `diagnostics: N host records
dropped while waiting for native diagnostics`), and each also goes to logcat at
once under the `dsd-neo` tag.

**Durable start.** A phone that restarts while listening loses whatever was not
yet on flash. So a start records itself before it touches the device:
- **When:** a USB access request records a `usb access requested:` line first.
  Before the decoder starts, a `session starting:` line records the source, the
  boot count and, for a dongle, its product and VID:PID. Every Play, the same
  source again included, gets its own `session starting:` line.
- **The window:** for the next 10 s every record is written at once and synced.
  The window opens again at `UsbManager.openDevice` and when the service starts
  decoding, and the steps in between log on entry and on return: the foreground
  promotion, engine configuration, the USB open and the first AAudio open and
  start. A start that never returned from one of them still shows how far it got.
- **Best effort:** the start waits at most 200 ms for its line to reach flash.
  Listening never waits longer. A store that refuses to sync is retried once a
  second, never in a loop.
- **Leaving:** quitting and the Activity's teardown flush the tail, with a bounded
  wait.

**How the previous run ended.** Each run starts with a `process:` line: pid,
Android version, boot count (`Settings.Global.BOOT_COUNT`), manufacturer and
model, app version. Android 11 and later add up to three `exit:` lines from
`ActivityManager.getHistoricalProcessExitReasons()`, newest first, each with time,
pid, reason (`CRASH_NATIVE`, `LOW_MEMORY`, `ANR`, `SIGNALED`, ...), importance,
PSS and Android's description. Each exit is logged once: a launch skips the ones
an earlier launch logged (their time and pid are kept in
`noBackupFilesDir/exit-facts-logged`, written only once the lines are synced to
the host store, so a launch that dies first does not lose them), so the host
store does not fill with the same history. Each new
segment starts again with the current `process:` and `session starting:` lines.
The screen, Copy and Share keep the previous run's and this run's `process:` and
`session starting:` lines at the top, however many records the 2000-row limit
drops after them. Each run keeps its own lines, even when two runs started the
same source and wrote identical ones. So a shared report always says which
process, boot and session its lines belong to.

If the boot count changed since the previous run, the run adds `previous run:
Android has restarted since (boot 5 -> 6)`. That is a fact, not a cause. Android
records no app exit when the phone itself restarts, and the boot count rises for
a full reboot and for a restart of the Android framework alike. Read it together
with the previous run's last lines: a session start or decode lines followed by
nothing, then a higher boot count, means Android restarted while or after that
session ran. An `exit:` line timed inside that run means the app process ended
first, for the reason given. The app cannot read why the device restarted; an
`adb bugreport` taken right after the restart has the boot reason.

Sharing creates a temporary text file in `cacheDir/diagnostics/`, exposed through
our separate, non-exported diagnostics FileProvider with a temporary read grant.
Files older than one hour are removed on the next share. The API accepts text,
not paths; systems, preferences, and imported CSVs are outside that provider.
Qt's own provider paths are unchanged.

The app's argv token gate rejects `--show-keys` (including assignment syntax).
`session_args_extra_safe()` is the shared gate for future scan-list generation.
Malformed CLI key diagnostics report the expected shape, never the value.
QString/QML copies cannot promise erasure; only filtered content enters diagnostics.

### Main-thread watchdog

The records in this and the next two sections were added to diagnose two reports
in issue #782: the dim between calls not lifting, and a relaunch waiting on the
splash screen after the app was closed. They change nothing the app does.

A thread of its own (`dsd-main-watchdog`) posts one ordinary message to the main
thread every 500 ms and waits for it to run. One that waits more than 1.5 s is a
stall, and the first stall in any 30 s is recorded as an incident of up to eight
records, each `watchdog: stall <id>.<n> of <total>: ...`:
- **Header:** how long the main thread has been blocked and in which thread state
  (`BLOCKED`, `WAITING`, `TIMED_WAITING`, or `RUNNABLE`, which a native or binder
  call also shows as), how many touch events Android delivered since the message
  was posted, where the activity stood (`activity 2 resumed, window 3 attached`),
  and how many stack frames follow.
- **Screen:** the screen policy's inputs as last applied, and how long ago.
- **Main stack:** up to six records of the main thread's stack, top frame first,
  numbered (`main stack 1-3: ...`); when not every frame fits, the header says how
  many were recorded (`stack of 120 frames, top 31 recorded`).

When the main thread runs the message again, one more record says `main thread
resumed after N ms` and how many touches arrived while it waited. Touches during a
stall mean input still ran while ordinary messages did not. Stalls the 30 s limit
kept out are counted in the next incident's header. When Android freezes the whole
app in the background, the watchdog is frozen too; that time is not counted as a
stall, and a resume names it (`the watchdog itself was held for N ms, not
counted`). The cost is two short messages every 500 ms, for as long as the app
process lives.

### Teardown markers

When the app's activity is destroyed for good (closing the app, not a rotation or
another configuration change), each stage of the teardown is recorded with how long
it took, so a relaunch that waited for the old process shows which stage held it:

```
teardown: activity 2 destroying at uptime 1234.567 s
teardown: engine stop request took 0 ms
teardown: stopService took 3 ms
teardown: diagnostics flush request took 0 ms
teardown: nativeQuitUi took 0 ms
teardown: Qt event loop returned at uptime 1234.890 s
teardown: QML engine and its models started
teardown: ~BackupModel started
teardown: ~BackupModel took 0 ms
teardown: ~CallHistoryModel started
teardown: ~CallHistoryModel took 41 ms
teardown: ~SignalSearchModel started
teardown: ~SignalSearchModel took 2 ms
teardown: ~RadioReferenceModel started
teardown: ~RadioReferenceModel took 1012 ms
teardown: QML engine and its models took 1090 ms
teardown: decoder host started
teardown: decoder host took 1 ms
teardown: main() returning at uptime 1236.010 s; flushing, at most 500 ms
```

The models whose destructors can wait (`~BackupModel`, `~CallHistoryModel`,
`~SignalSearchModel`, `~RadioReferenceModel`) each mark their stage inside the QML
engine's. A stage that started and never took is the one that held the teardown;
when the last record is a stage's `took`, the hold is in an object no marker covers
(other models, the app's settings, the QML objects). A watchdog incident during the
wait names the activity `in Qt teardown`. Uptimes on the Kotlin and native sides
are the same clock, so they line up. The teardown waits for one diagnostics flush
only, `main()`'s final one, bounded at 500 ms; the activity's and the quit's only
start one. No record can follow the final flush durably, so its own time is
reported by the next start of the app's screen: `teardown: the previous process's
final diagnostics flush took N ms`, with `, and timed out` when its bound ran out
or `, and failed to write` when the store could not write or sync it.

### Screen records

Each time the app writes the screen's brightness for the dim, or the dim it wants
changes without a write, it records `screen: change <n>: ...`: dim on or off, the
brightness requested and read back from the window (or why nothing was written:
`brightness already <v>`, `no attached window`), the policy's inputs (mode, delay,
session, visible, focused, top, multi-window, screen on or off, what it wants, and
what engaged it, such as `engaged by touch 300 ms ago`) and the activity. A written
change is followed to the screen: one more record says `traversal after X ms, draw
after Y ms on window W`, `no draw within 2000 ms`, or that a later change replaced
it. So a change that never drew is told apart from one that drew while the panel
stayed dim, which points at the platform. Change records come at most once a
second; of the changes inside that second, only the latest is recorded, at its
end, saying how late and how many changes it replaced.

A touch that comes down while the screen is dimmed is recorded as well, at most
once every 5 s (the next record counts the ones left out): `screen: touch while
dimmed: swallowed, dim off`, `passed on, dim still on`, or `not taken: the screen
policy follows another activity`. A tap that reached the app but did not lift the
dim leaves this record; a tap that never reached the app leaves none.

### Reading a shared report

In a release build the app's own stack frames are obfuscated, with line numbers
kept: retrace them with the `mapping.txt` of that exact APK ("Release optimization
and JNI"). In the watchdog and screen records, a word the key filter would drop the
whole record for is broken with a `*` (`dispatchK*yEvent`, `javax.crypto.A*S`), and
a dash that reads like an option becomes `~`. That is deliberate, not corruption:
without it the filter would drop the record and everything correlated with it.

### Monitor site identity

Tap the site row between the Monitor's call panel and action buttons to open the
site details. The row and the sheet follow the carrier being decoded: they show
only what this carrier sent in a message that passed its own check (CRC, FEC or
a confirmed transmission), never a Phase 2 seed, a `--tetra-net` preset no
checked block proved, a control channel the decoder returns to, or a value
another carrier left. P25 NAC, WACN, SYS, RFSS/SITE and LRA each appear once
received; a conventional Phase 1 NAC alone is useful identity even without the
complete Phase 2 parameter set. The sheet also shows DMR color code/site
text/rest LSN/Hytera XPT free LCN/trunking type/Tier III system code, NXDN RAN
and location codes (Area, site code and Type-D system for IDAS), EDACS site,
system, area and control channel LCN, the dPMR color code or M17 CAN, TETRA
MCC, MNC, color code and location area, the control channel frequency where
this receiver heard the control channel, and the voice frequency being followed.

A DMR carrier confirmed as RAS (Restricted Access to System: its control and data
fail the standard CRC, two bursts with that signature on this carrier) shows
**Access RAS** in the sheet. While Relaxed CRC is off the sheet explains it in the
terminal notice's words and offers **Turn on Relaxed CRC**, which sets the running
session and the app preference below; under a scan row whose own `--strict-crc`
decides for the channel it says so instead. Once Relaxed CRC is on it says the
system's control and data decode.

The sheet's **Phase 2 seed** row shows the WACN.SYS.NAC triple the Phase 2
descrambler runs on (for example `BEE00.348.293`), marked "(set)" when `-X` or
the menu set it by hand. It is configuration, not received identity, so it never
takes the NAC row's place; **Phase 2 parameters** says whether it is complete.

When sync is lost the identity stays as last heard on this carrier: the row
dims and the sheet labels it as retained. Any change of carrier (a retune, a
scan step, an input switch, a trunk-scan target switch) clears it, and the next
carrier shows only what it sends. A trunking follow keeps the system and drops
the access code until the new channel sends its own. Starting, stopping to Idle
and failure clear it too. Unknown fields are omitted; the row is hidden when no
identity fields are available.
Emergency calls carry an EMERGENCY badge on the monitor (including the other TDMA
slot), recent calls, and history. The notification title prefixes the lead call
with EMERGENCY. Native notification records and `DecoderStatus.kt` use wire v3:
each of the two slot records ends with emergency and priority (11 slot fields),
and the header carries the audible-audio stamp and its age at indices 9 and 10,
which the screen modes read (see [Screen](#screen)).
Call-history JSON stores `"em": true` only for emergency rows; older history
without that optional key remains readable. Emergency indication persists when
later fragments enrich a history row. The notification channel remains
`IMPORTANCE_LOW`; an opt-in heads-up channel is a follow-up.
### P25 network announcements

During P25 reception, the Monitor's **P25 Site → Network** entry opens a read-only
sheet with Neighbours, Patches, Affiliations and Radios. Empty sections show
“(none announced)”. Neighbours show the current control channel `[CC]`, candidate
`[C]`, frequency, site identity and CFVA status. Each list shows at most 100 recent
records and refreshes only while the sheet is open. Starting, Idle, Failed and
trunk-scan target changes clear these live records.

The Monitor entry is the WP-F2 fallback until WP-F1's SiteSheet is integrated;
move the entry there with the `siteProtocol === "P25"` guard. There is no Tune
control in v1. The deferred neighbour Tune work must restrict `RTL_SET_FREQ` to a
running single-system P25 trunk session with the same WACN/SYS. CC selection
preserves user lists but clears site metadata; crossing systems requires restart.
### Decryption profiles

Saved systems and scan entries can select shared **Decryption keys** profiles.
Automatic collections select by received key ID where the protocol supports it;
DMR profiles can also map group calls to a stored key ID. Direct channel/stream
keys and vendor modes are explicit alternatives. Formats, storage, migration and
scope are documented in [decryption profiles](../docs/decryption-profiles.md),
including how to [add a P25 AES-256 key](../docs/decryption-profiles.md#add-a-p25-aes-256-key)
and keep a session override after Stop.

Monitor → More → Decryption shows per-slot received identifiers, selected key
source and availability separately from listening policy. Key material availability
is not proof of successful decryption. Live changes remain Pending until the
decoder answers. Applying an unchanged direct draft does nothing. Profile changes
can update session defaults or the active supported scan target; the dialog names
that scope. Saved profile edits take effect on the next start or explicit Apply.

New RadioReference imports keep encrypted and partly encrypted talkgroups enabled.
The review also offers explicit exclusions; those exclude entire talkgroups,
including clear calls. Refresh keeps the import's saved choice (including older
imports' exclusion behavior). Supplying keys never silently unblocks a talkgroup.

### Navigation, accessibility and replay

**Signal search**, beside Explore on Home, searches configured frequency ranges
using internal RF power measurements, then identifies and monitors detections.
Ranges have independent detection sensitivity and listening squelch. The default
digital probe is two seconds, followed by explicitly unresolved NFM monitoring;
Hold, Skip, session/permanent avoids, detection history and Save as system are
available. Search owns the radio exclusively and follows the existing background
listening preference. See [Signal search](../docs/signal-search.md) for controls,
limits, persistence and measured performance.

Android Back dismisses input, then the top dialog, then the pushed screen or
wizard step. Root Back follows the background-listening preference. Monitor's
More menu reaches History, Settings and Diagnostics without stopping the session.
Preferences identify changes that apply at the next start. **Decoding → Voice hang time**
defaults to 2.0 seconds (0.0–30.0); a saved system can override it under Advanced,
or leave its field empty to follow the app default. Extra decoder arguments containing
`-t` take precedence. Channel scanning enabled with `-Y` also uses this value as
its dwell timer; scan lists have separate dwell settings.

**Decoding → Relaxed CRC** (off by default) accepts control and data whose CRC
check failed and marks them; DMR RAS systems need it. Every session the app starts
(a saved system, Explore, a search hit, a scan list) gets `-F`, unless the system's
or the app's Extra decoder arguments contain `-F` or `--strict-crc`, which win. It
also applies to the running session at once: the switch shows that session's value
until it confirms. While a scan row with its own `-F` or `--strict-crc` is on air,
the row decides for its channel and the switch says so (**This channel: Off (scan
row)**); the switch edits the value beneath it.

The wizard's **Auto** decode chip is the default: it decodes every digital mode
except TETRA (NXDN included).

To check P25 timing, Extra decoder arguments must include a writable filename,
for example `--p25-sm-log /data/user/0/io.github.arancormonk.dsdneo/files/p25-sm.log`.
Effective hang time and carrier-release events are appended to that file, not
the Diagnostics screen. Stop listening before retrieving it. On a rooted test
emulator, use `adb root` followed by `adb pull` with that path; a debuggable build
can use `adb exec-out run-as io.github.arancormonk.dsdneo cat files/p25-sm.log > p25-sm.log`.
Production builds on non-rooted devices do not expose app-private files through
adb. Remove the argument after testing to stop the log growing. Argument
construction is also covered by the host tests.

Controls expose named accessibility actions, keyboard focus and 48 dp targets.
Text uses Android's per-size SP conversion, including nonlinear large-text scaling.
The app's effective theme controls system-bar icons. Expanded windows use a
navigation rail; short Monitor windows keep frequent controls above Stop.

Source failures identify the attempted input and offer Retry, Edit source and
Details. RTL-TCP connection establishment is bounded to ten seconds after DNS
resolution and checks cancellation during the connection attempt. Notifications
are explained when background listening first becomes useful; denial leaves the
foreground UI usable. Audio output reports the active device route and follows
Android's routing controls.

New replay picks are copied to durable app-private storage. Existing saved cache
copies are migrated when available; missing files can be reselected through Edit.
File details show the basename, size and modification time. Replay completion is
retained with a Play again action. Progress is not invented when the input backend
cannot report it. History rows open full details, including timestamps and messages.

The Spectrum view is view-only during an I/Q replay: the replay plays the tuning its
capture recorded, so tapping, stepping, **Go to** and **Explore from here** are not
offered, and the view says **An I/Q replay cannot retune.**

The line under the Spectrum view's sync and SNR names the system on the carrier
being decoded, from the same received identity as the Monitor's site row: the
access code first (**CC 1 · Rest LSN 3**, **NAC 293 · SYS 348 · RFSS 3 · SITE 21
· WACN BEE00**, **RAN 5**, **MCC 262 · MNC 1001 · CC 25**). Where it does not
fit, whole parts drop from the end and **…** says so; a part is never cut from
its value. When sync is lost the line keeps what was last heard on this carrier,
dimmed and led by its protocol (**DMR · CC 1**), and any retune clears it. At
large text sizes the strip grows to keep both lines clear of the waterfall, and
the **VIEW ONLY** / **TAP TO TUNE** pill moves under the frequency where the two
no longer fit side by side.

Each call in History and Monitor's Recent calls names the access code of the carrier
it was decoded on right after its talkgroup and source: a DMR or dPMR color code
(**CC 1**), a P25 network access code in hex (**NAC 293**), an NXDN radio access
number (**RAN 5**) or an M17 channel access number (**CAN 0**). The frequency the
call was heard on closes the line, so a narrow phone cuts it off first. The details
sheet spells both out (**Color code 1**, **Frequency 851.0125 MHz**). A call whose
code or frequency is not known leaves it out.

History's search finds a call by its talkgroup, unit, name, message text, channel or
the frequency it was heard on. A frequency needs its decimal point (**851.0125**,
**851.**) or the unit (**851 MHz**): a bare **851** searches talkgroups and units
only, so a talkgroup search on an 800 MHz system does not return every call. Type
as many digits as the row shows, or the six the Site sheet prints. A row the search
found by its frequency leads its line with that frequency.

Tap a row in History or Monitor's Recent calls to **Hold TG** while listening to
the same saved system. The held talkgroup follows the decoder's state; release it
from the details or Monitor. Explore sessions and scans (including scans enabled
through Extra decoder arguments) cannot start a history hold. Hold waits for
the effective session settings to arrive after startup. Rows heard during
those sessions or saved before system identities were recorded remain ineligible,
even after starting a single saved system. A running session can still release an
existing hold, including from a different system's row with the same talkgroup ID.

The other active slot's **SLOT n** strip has its own **Skip** and a **More actions**
menu with **Hold TG** and **Avoid TG**/**Lock out**. Holding that talkgroup replaces any
existing hold and silences the other slot; **Release hold** clears it. The menu closes
if its slot or talkgroup changes or the call ends.

The hero stays on the earliest-started identified active call when the other slot
starts; the notification uses the same rule.

### Scan lists

**Import target CSV** opens Android's document picker. Select the target CSV and
the companion files requested by the import flow, review the read-only target
preview, choose the receiver settings, name the list and Save. Play uses the stored
CSV directly, preserving its supported options and their precedence over session
defaults. Blank target settings inherit. The original documents can be removed
after import; private copies survive process restarts.

The CSV remains authoritative; its rows are not converted into manual entries.
Select another imported target CSV in the list editor or use **Update from file**
in the imports library to change target contents. Replacement and removal require
an idle decoder. Failed replacement keeps the previous file usable. Removing a
referenced target asset marks its lists as drafts; deleting a list keeps shared
assets. Validation reports the failing row/field without displaying key values.

Home → **Scan lists** combines saved systems and bare P25/DMR/NXDN frequencies
into a single trunk-scan session. Use the visible Edit action (or long-press) to edit entries, order, timing,
modulation and gain. Choose one USB or RTL-TCP tuner for the list; it replaces
individual systems' source, endpoint, PPM, bandwidth and bias-tee settings.
Scan lists use the app's voice hang time; per-system hang-time overrides do not apply.
`-t` is separate from idle dwell on every target and the activity hold on conventional
targets. The list editor can select imported group/source CSVs from the existing library.
It has no field for the per-visit cap; add `--scan-max-visit-ms <ms>` to Extra decoder
arguments to cap every target in the session.
Per-target squelch is CSV-only: import a target CSV whose `options` carry
`--squelch-db <dB>` (whole dB from -100 to 0, 0 = off, omitted = inherit). It is not a
decoder argument, so Extra decoder arguments cannot set it. The target preview lists each
target's squelch, or inherit. While such a target is on air, the Radio panel shows its
threshold with a **row** badge and the default beside it; the squelch buttons change that
default, not the target's value.
Per-system groups and keys remain isolated on rotation; source aliases are global
and differing system alias files produce a warning. Unsupported settings are
refused rather than dropped. See [scan-list rules](../docs/trunk-scan.md#qt-and-android-scan-lists).

Validate checks a draft without saving. Save validates first; Save draft retains incomplete lists as **Not ready**, which cannot start. Cancel discards only unapplied edits, and removal asks for confirmation.

USB lists use the same permission gate as saved systems. A failed or cancelled
start does not change last-started preferences or list recency; those update only
after engine initialization. Monitor shows the active target ID, ordinal/count
and hold state. System source-alias paths that the list replaces are not validated or opened; the list's effective alias file must be readable.
Generated CSVs may contain keys, are private app inputs, and must
not be exported or logged. QString/QML key copies cannot guarantee memory erasure.

### Saved RadioReference sites

The Qt import screen can import each selected trunked site as a separate saved system. Each site uses its own
recorded simulcast setting unless an explicit override is chosen. Home groups
these rows behind **n sites ›**. **Use my location** orders sites by distance and names the nearest
eligible saved site. Distances use miles or the metric preference; location accuracy and missing
site coordinates are explained. Avoided sites and sites without coordinates are excluded from
nearest selection. **Listen to nearest site** starts while Idle; while listening, **Stop and switch to nearest**
and each site's **Stop and switch** wait for the
service's Idle acknowledgement before starting the selected site's saved configuration. Any intervening
user input cancels that pending restart. This is a full session restart, with no live automatic hop.
See [RadioReference import](../docs/radioreference-import.md#saved-sites-in-qt-and-android) for migration
and editing behavior.

After a failed run, Retry acknowledges the USB diagnostic and Play can start the
saved system again once the service is inactive. The site chooser also offers
“Dismiss failure and choose a site”; it returns to Idle only after the host confirms
that the service has finished stopping.

### Receive filters and service names

While listening, open **Signal & site → Receive filters** or **Service names**. Filters have an On/Off setting and
separate allow/block lists. Naming rows choose an identifier, its value and a service name. **All channels** edits
the configured default; **This channel**, **Use default** and **List value** edit the captured scan row for this
session. A stale row/session command is refused. Back and Escape protect unsaved changes; validation errors keep
the editor open. History and notifications use received service names while preserving the channel label.
Conventional NFM history records the tone identity and `-P` records each accepted transmission independently of
speaker output. See [receive behavior and limits](../docs/cli.md#receive-filters-and-service-names).
