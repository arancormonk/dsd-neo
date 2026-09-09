<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DSD-neo on Android

An arm64-v8a app: a Qt Quick UI (shared with the future desktop GUI, `src/ui/qt/`),
a foreground service that owns the decoder, and a small JNI surface between them.
The engine is linked into the app process as native code, so JNI carries only
lifecycle, platform glue, and one read-only status record (the scanner readout the
service renders into its notification).

Supported inputs: a directly attached RTL-SDR over USB-OTG, `rtl_tcp`, UDP PCM,
TCP PCM, and local files.

## Saved encryption keys

Saved systems store `encKeyType` (`""`, `basic`, `hex`, `rc4`, or `scrambler`),
`encKeyValue`, and `encForceKey` (0 normal, 1 force privacy, 2 force RC4).
Keys rest **unencrypted** in the app-private systems store, `saved_systems.json`; Android sets
`allowBackup=false`. On desktop the same file is under Qt's `AppDataLocation`.
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
Preempt is available above priority zero and affects P25 trunking only. The card shows `P<n>` and a lightning badge
when preempt is set. Heard-only rows offer **Add to list**; listed rows require two
taps on **Remove**. Decoder refusal messages remain visible in the sheet. If the
policy changed while the sheet was open (including scan rotation), close and reopen
it before trying again.

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
  screen: the hero call, mute/hold/skip, the signal strip, and the session's
  recent calls.

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
is persistent and never cleared on session boundaries, only fed.

The service state machine, the native `g_running` atomic and the failure path are
folded into that one phase by `session_state_map.h`, which is deliberately free of
Qt and JNI so `UI_QT_SESSION_STATE` can test it on the host. Each accepted start
has a monotonically increasing session id. `nativeLifecycleStatus` retains the
post-initialization edge and terminal reason (pending/completed/cancelled/failed),
run return code, and native USB open/claim error after cleanup. The service folds
these into one `lifecycleStatus` record, including `lastError`; the existing UI
tick reads it without consuming a decoder snapshot. Failures remain visible even
when the run ends between polls or fails well after startup. While a native terminal
result is present but the service is still RUNNING/STOPPING, the UI stays Stopping.
Restart remains disabled until the service releases its wake lock and publishes
IDLE; the host also checks a fresh service record before accepting a retry.
USB claim error -6
is surfaced as `DeviceBusy` without discarding the original code.

`sessionInitialized()` comes from the engine lifecycle callback after initialization,
not from `g_running`. Saved-system recency and `prefs.lastStartedKind/Uid` are
written only on that edge. Saved systems carry stable UUIDs, so deleting another
row during startup cannot stamp the wrong system. Starting/Idle/Failed and
trunk-scan target changes clear live model caches; history remains persistent.

The shared host also reserves location requests/cancellation, content-based
diagnostics sharing, device-attach signals and a diagnostic sink. Location and
sharing default to unsupported until their platform packages supply implementations.
USB/lifecycle diagnostics enter the process capture directly, including when no Activity
exists. Before Qt initialization installs the tap, bounded redacted host records wait in memory. The optional last location fix uses milliseconds since epoch and is deleted
The shared host provides location requests/cancellation, content-based diagnostics
sharing, device-attach signals and a diagnostic sink. Android's `LocationSupport.kt`
brokers **Use my location** for RadioReference with coarse foreground permission on
the Android main thread. API 30+ uses `getCurrentLocation`; API 29 uses
`requestSingleUpdate` and removes its listener on completion/cancellation. A 20-second
timeout covers fix acquisition and worker-thread reverse geocoding. Request IDs and
separate fix/geocode status preserve a usable fix when geocoding fails and prevent
late responses from replacing a newer search. Activity teardown cancels the request.
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
copy first. There are two copy paths, with different lifetimes:

- `AppSupport.copyContentUriToCache` → `cacheDir`, for one-shot opens (the
  wizard's audio/capture "File" source). Android may evict it.
- `AppSupport.importDocumentToFiles` → `files/imports/`, for the CSV library
  (channel maps, talkgroup lists, key files, P25 band plans, source ID lists). These must survive restarts —
  saved systems reference the path, and the engine appends learned talkgroup
  rows to the group list in place. Display-name collisions are unique-ified
  (`chan.csv` → `chan (2).csv`); updates stage a temp file and rename over the
  target so a half-copied CSV is never observable.

Embedded `single_key_dec`/`single_key_hex` values in a channel map travel with that copied file and therefore work on
Android. Referenced `keys_dec_csv`/`keys_hex_csv` companion files are not copied or path-rewritten automatically.

Both are reached through `DecoderHost` virtuals (`importContentUri` /
`importDocument`); the desktop default of `importDocument` implements the same
semantics under `QStandardPaths::AppDataLocation`, which is what
`UI_QT_IMPORTED_FILES` tests on the host. The library itself (one JSON-persisted
row per stored file, with dry-run validation counts from
`<dsd-neo/core/csv_validate.h>`) is `src/ui/qt/imported_files_model.{h,cpp}`,
surfaced as Settings → Imported files and the wizard's Trunking data pickers.
The imported-file kinds are `chan`, `group`, `keysDec`, `keysHex`, `p25Bandplan`, and `src` (the **Radio IDs** picker). Source
ID lists use `id,name[,tags]` and supply labels only; they can be applied or cleared in a running session. A group
list and a source ID list cannot be distinguished by content: validation uses the kind selected by the user.
No storage permissions are involved; SAF needs none.

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
override the Qt template — carrying the per-artifact native-library packaging
described under [Google Play](#google-play-the-app-bundle); re-diff it against
`<Qt>/android_arm64_v8a/src/android/templates/build.gradle` whenever the pinned
Qt version changes.

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

## USB-OTG: how the descriptor gets to librtlsdr

An Android application cannot open `/dev/bus/usb` nodes, so librtlsdr cannot
enumerate or open a dongle the way it does everywhere else. The descriptor is
obtained in Java and injected:

1. `UsbSourceManager.kt` matches an attached device against the RTL2832U
   vendor/product table, requests permission if it does not already have it, calls
   `UsbManager.openDevice()` and takes `UsbDeviceConnection.getFileDescriptor()`.
2. That descriptor goes to `DsdNative.nativeSetUsbFd(fd)` →
   `rtl_device_set_preopened_fd()` (`src/io/radio/rtl_device.cpp`).
3. `rtl_device_create()` sees the injected descriptor and calls `rtlsdr_open_fd()`
   instead of `rtlsdr_open()`. That entry point — added by the patch below — turns
   libusb device discovery off and wraps the descriptor with
   `libusb_wrap_sys_device()`, then runs the normal claim/probe sequence.
4. Because discovery is off, `rtlsdr_get_device_count()` reports zero. The engine
   would read that as "no supported devices" and abort the run, so
   `dsd_engine_setup_enumerate_rtl_devices()` short-circuits to a single device at
   index 0 whenever a descriptor is set. The app is what selected the device.

Java keeps ownership of the descriptor throughout: `libusb_wrap_sys_device()` does
not take it over. Closing the `UsbDeviceConnection` while the engine still has the
descriptor wrapped is a use-after-close, so `UsbSourceManager.release()` clears the
native slot immediately — that only affects the *next* open — and then hands the
close to a background thread that waits for `nativeIsRunning()` to go false. Both
callers (a detach broadcast, `DecoderService.onDestroy()`) therefore return without
blocking. If the engine never stops, the connection is deliberately leaked: one
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
Kotlin table; add rebadged dongles to both.

## Power

An RTL-SDR draws roughly 300 mA, and many phones cap what they will supply over
OTG — some silently, some by dropping the device mid-stream. If the dongle
enumerates but the run dies or never starts, try a **powered OTG hub** before
suspecting the software. A powered hub is the recommended setup for anything
longer than a quick test; it also keeps the phone from discharging into the
dongle. Bias-tee power for an external LNA comes out of the same budget — the UI
exposes it as a checkbox on the USB source, and it is off by default.

## Vendored third-party code

`third_party/` carries trimmed snapshots of two upstream projects. They are **not**
under `src/third_party/`: that perimeter has repo-wide guardrails, and these build
privately for the Android app only.

| Project | Version | Upstream commit | What is vendored |
| --- | --- | --- | --- |
| libusb | v1.0.30 | `87a55632db62c9bdc58cd31d3ccfa673f1bb017f` | The exact source list upstream's own `android/jni/libusb.mk` builds — the `linux_usbfs` backend plus the POSIX event/thread shims — with upstream's `android/config.h` |
| librtlsdr | v2.0.3 (osmocom) | `797f8143266d983c56d8f35d2d442527529dd8a5` | The library only: `librtlsdr.c`, the five tuner drivers and `include/`; the `rtl_*` command-line tools and their helpers are dropped |

librtlsdr is GPL-2.0-or-later and libusb is LGPL-2.1-or-later; both upstream
license texts travel with the snapshots (`*/COPYING`) and both are listed in the
repository's `THIRD_PARTY.md`.

These trees keep upstream formatting and are excluded from every repo tool
(`.clang-format-ignore`, `tools/format.sh`, `.githooks/pre-push`), so do not
reformat or "fix" them — the point is that they stay diffable against the release
they came from.

### The librtlsdr patch

`third_party/patches/0001-librtlsdr-add-rtlsdr_open_fd.patch` is already applied to
the vendored copy; it is kept as a file so the delta stays reviewable and can be
re-applied when the snapshot is refreshed. It adds `rtlsdr_open_fd()` and splits
the claim/probe tail of `rtlsdr_open()` into a shared helper so both open paths run
identical initialisation.

To move to a newer librtlsdr:

```sh
git clone https://gitea.osmocom.org/sdr/rtl-sdr.git /tmp/rtlsdr
git -C /tmp/rtlsdr checkout <new-tag>
cd /tmp/rtlsdr && patch -p1 < <repo>/android/third_party/patches/0001-librtlsdr-add-rtlsdr_open_fd.patch
# then copy src/librtlsdr.c, src/tuner_*.c and include/* over the vendored tree,
# regenerate the patch against the new baseline, and update the table above
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
bounded redacted persistent tail from previous runs. It is not a native-crash or
ANR capture. The process ring holds at most 2000 entries, each at most 512 UTF-8
bytes. Pause freezes the displayed list while capture continues; Copy and Share
include the current ring. Clear clears the process ring/view, not the persistent
tail. Diagnostics deliberately survive decoder Starting/Idle/Failed transitions.

Host records arriving before the first decode start do not depend on the runtime sink or
stderr pump. Tap installation drains their bounded buffer to the ring and the tail at
Qt's initialized application-data location.

A process-lifetime runtime log tap, the stderr pump, and host messages all enter
`DiagnosticsLog::submit()`. It filters sensitive records before ring insertion and
queues only redacted bytes to one asynchronous writer. The writer retains the
newest 256 KiB in `files/diagnostics/tail.log`; a tail untouched for seven days is
deleted at startup. Its bounded queue drops oldest queued records under sustained
storage backpressure. Disk failures do not stop decoding. Oversized stderr lines
are omitted rather than split into potentially unlabelled secret fragments.

Sharing creates a temporary text file in `cacheDir/diagnostics/`, exposed through
our separate, non-exported diagnostics FileProvider with a temporary read grant.
Files older than one hour are removed on the next share. The API accepts text,
not paths; systems, preferences, and imported CSVs are outside that provider.
Qt's own provider paths are unchanged.

The app's argv token gate rejects `--show-keys` (including assignment syntax).
`session_args_extra_safe()` is the shared gate for future scan-list generation.
Malformed CLI key diagnostics report the expected shape, never the value.
QString/QML copies cannot promise erasure; only filtered content enters diagnostics.

### Monitor site identity

Tap the site row between the Monitor's call panel and action buttons to open the
site details. P25 NAC, WACN, SYS and LRA appear independently when known; a
conventional Phase 1 NAC alone is useful identity even without the complete
Phase 2 parameter set. RFSS and SITE appear when nonzero. The sheet also shows
DMR color code/site text/rest LSN, NXDN RAN and location codes (Area for IDAS),
EDACS site information, and known control/voice frequencies.

The row dims and the sheet labels identity as retained when current sync is lost.
Starting, stopping to Idle, failure, and scan-target transitions clear the live
identity through the existing controller lifecycle. Unknown fields are omitted;
the row is hidden when no identity fields are available.
Emergency calls carry an EMERGENCY badge on the monitor (including the other TDMA
slot), recent calls, and history. The notification title prefixes the lead call
with EMERGENCY. Native notification records and `DecoderStatus.kt` use wire v2:
each of the two slot records appends emergency and priority (11 slot fields).
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
### Encryption key entry

The saved-system wizard's Encryption panel accepts one direct key type, with a
password-masked field and Show/Hide control. Choose a direct key or a key CSV;
the wizard refuses to save both. Home marks saved direct keys and key CSVs as
“key configured”. The monitor's Encryption key action applies a key and force
mode to the current session only. It does not change the saved system or show a
live key-status label. Direct-key changes in the wizard take effect at the next start;
use “Apply to this session” for a direct key needed now.

Calls the system's key can decrypt still play when Skip encrypted calls is on.
Saved direct keys rest **unencrypted** in the app-private `systems.json` on
Android (`allowBackup=false`), and under Qt's `AppDataLocation` on desktop.
QString/QML copies cannot promise secure erasure; closing the session sheet
clears its draft input but does not guarantee erasure of all memory copies.
Key values are never shown in labels, toasts, diagnostics, or exports.

### Scan lists

Home → **Scan lists** combines saved systems and bare P25/DMR/NXDN frequencies
into a single trunk-scan session. Long-press a card to edit entries, order, timing,
modulation and gain. Choose one USB or RTL-TCP tuner for the list; it replaces
individual systems' source, endpoint, PPM, bandwidth and bias-tee settings.
The list editor can select imported group/source CSVs from the existing library.
Per-system groups and keys remain isolated on rotation; source aliases are global
and differing system alias files produce a warning. Unsupported settings are
refused rather than dropped. See [scan-list rules](../docs/trunk-scan.md#qt-and-android-scan-lists).

USB lists use the same permission gate as saved systems. A failed or cancelled
start does not change last-started preferences or list recency; those update only
after engine initialization. Monitor shows the active target ID, ordinal/count
and hold state. Generated CSVs may contain keys, are private app inputs, and must
not be exported or logged. QString/QML key copies cannot guarantee memory erasure.
