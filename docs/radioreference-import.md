# RadioReference Import

dsd-neo can build a system's trunking data straight from the
[RadioReference.com](https://www.radioreference.com/) database instead of asking you to hand-write
CSVs: browse to a system by zip code, by country/state/county, or by its system ID, pick the site
(a trunked system with only one is picked for you), and it generates the talkgroup list and — where
the protocol needs one — the channel map, in the same formats `docs/csv-formats.md` describes.

Both frontends have it, over one shared runtime client (`include/dsd-neo/runtime/radioreference.h`)
and one shared policy module (`include/dsd-neo/runtime/radioreference_import.h`):

- **Android / Qt.** The generated files land in the imported-files library like any other import, and the
  add-system wizard opens with the frequency, the decode flag and both files already filled in.
- **Terminal UI.** The generated files land in the imports directory next to your config file, and the
  import is applied to the running session immediately. See [Terminal UI](#terminal-ui).

## Site position metadata

The runtime SOAP client exposes site `lat` and `lon` in degrees and `range_mi` in miles.
`has_position` is true when latitude and longitude are present and valid: latitude within
[-90, 90] and longitude within [-180, 180]. The `(0,0)` placeholder means no position;
zero latitude or longitude alone is valid. Missing, nil, malformed or out-of-range coordinates
become zero and clear `has_position`, while the site's frequencies remain available for import.
Coverage range is optional: absent or invalid values become zero without discarding valid
coordinates. Consumers must check `has_position` before using the coordinates as a position.

Decimals use an optional sign, digits, and an optional dot followed by fraction digits,
independent of the process locale. Exponents, commas, whitespace and nonfinite values are
not accepted. This metadata does not change site selection or saved-system grouping.

## Requirements

- **Your own RadioReference premium subscription.** Every user authenticates with their own
  credentials. Nothing is shared or pooled, and dsd-neo does not subsidize accounts.
- **An application key** (see below).
- A build with **libcurl and expat** available. Both are auto-detected; the Android and Windows
  presets require them (`DSD_REQUIRE_CURL=ON`, `DSD_REQUIRE_EXPAT=ON`). Where either is missing the
  feature reports itself unavailable and every entry point is hidden rather than failing later.

## The application key

RadioReference issues one key per *application*, separately from your account. dsd-neo can carry a
project key baked in at build time, and **the baked key is authoritative**: a build that carries one
never asks the user for a key and never lets one be substituted. The key fields stay hidden in both
the import screen and Settings, sign-in is just the RadioReference username and password, and an
auth failure names only those two as possible culprits.

In a build without a baked key the import screen and Settings both offer the key field, and the
entered key is stored in the app's settings.

The two do not mix. A key stored under a keyless build is *ignored*, not merged, once a keyed build
is installed over it — the app's settings are shared between builds, and a leftover override that
outranked the working baked key would fail every request with no in-app way to reach it. It is left
in place rather than erased, so a keyless build run later still finds it.

The trade-off is deliberate: in a keyed build there is no in-app recovery if the project key is
revoked or exhausts its quota. Recovering means shipping a build with a new key.

Baking one in:

```bash
DSD_RR_APP_KEY=<key> cmake --preset dev-release      # or any other preset
DSD_RR_APP_KEY=<key> cmake --preset android-app
```

`DSD_RR_APP_KEY` is a CMake cache variable with an environment-variable fallback. Prefer the
environment form shown above: `-DDSD_RR_APP_KEY=<key>` writes the value in plaintext into
`build/<preset>/CMakeCache.txt`, while the environment route keeps it out of the build tree. The
value is substituted into a generated C source file, so it must match `^[A-Za-z0-9._~-]+$`;
configure fails otherwise. Leave it unset and the app prompts for a key instead.
`DSD_RR_APP_KEY` is read at the top level of `CMakeLists.txt` with no preset and no platform guard, so it
applies to any preset — a terminal-only build bakes a key exactly the same way the Android one does.

**A baked key is not a secret.** It is extractable from any shipped binary with `strings`. Keeping
it out of the repository protects the repository, not the artifact.

CI takes the key from the GitHub repository secret `RADIOREFERENCE_APP_KEY`. Today only the
`Configure (android-app)` step is given it, because that is the only job that ships an artifact; any other
job could take it the same way. An unset secret expands to the empty string, which is a valid build that
prompts.

To request a key, sign in and apply at
<https://www.radioreference.com/account/api/apply>. An active premium subscription is required on
the applying account, and RadioReference staff review each request by hand, so allow lead time.
Describe your use concretely — a tool a user runs alongside their scanner, each user authenticating
with their own premium credentials, is squarely the sanctioned case.

## Credentials

- The **username** and, in a build without a baked key, the **application key** persist. The Qt app keeps
  them in its own settings store (`Settings → Account → RadioReference`); the terminal UI keeps them in the
  config file under `[radioreference]` (`docs/config-system.md`). The key row is offered only where the
  user is the one who has to supply a key — see [The application key](#the-application-key).
- The **password is held in memory only**, for the app session. dsd-neo never writes it to disk, never
  logs it, and it never reaches a status line or an error message. Clearing it is best effort: the
  app scrubs the native copies it owns, but the copies Qt and the Android runtime make on the way
  (QML and Java strings) cannot be wiped on demand and go when the runtime frees them.

### The two sign-in forms (Qt and Android)

The import screen's credentials card and **Settings → Account → RadioReference** are the same sign-in
form: username, password (with **Show**), the application key only in a build without a baked one,
**Check account**, and a result line under it. The Settings row names the fields that build asks for:
"Username and password", or "Username, password and application key".

Both forms share one session password, which the app keeps and neither form shows:

- The password field starts empty each time its screen opens and is cleared and masked again when the
  screen closes. While it is empty and a password is held, it reads **Entered for this session**.
- Leaving the field empty keeps the session password; typing a new one replaces it. **Check account**
  with the field empty checks the session password.
- **Show** reveals what you typed and keeps you in the field; it never submits a check.

The account is checked (`getUserData`) when a field you finish makes the sign-in complete, or when you
tap **Check account**. A field you leave unchanged does not check again, and one tap sends one check.
An expired premium subscription is reported as such, rather than surfacing later as an opaque failure.
The result line shows only the verdict for the current username and password (it resets when either
changes, and a later search that RadioReference refuses changes it too); on the import screen a failure
is reported in the notice line at the top instead, and the credentials card stays open until the check
has an answer.

### Password managers (Android)

Both forms are offered to the device's autofill service (a password manager such as Bitwarden,
KeePassDX, Google Password Manager or 1Password), as a username and a password field. The application
key is never offered. This has been verified with KeePassDX. Bitwarden, Google Password Manager and
1Password are expected to work, based on how their documentation says they read a form, but this has
not been proven yet.

- Tap the username or password field and the service offers its saved logins. Picking one fills both
  fields and checks the account once.
- After RadioReference accepts a sign-in the service may offer to save it, or to update a saved one. That
  copy is the password manager's, kept under its own rules; dsd-neo still writes nothing.
- The suggestions appear in the service's own dropdown, not as chips in the keyboard: the keyboard only
  offers suggestions for the field it is typing into, and Qt's text input is not one Android can
  describe to it.
- Logins are matched to the app (`androidapp://io.github.arancormonk.dsdneo`), not to
  radioreference.com, because the app cannot prove it speaks for that website. A login saved from the
  website is not matched automatically; pick it through the service's own search or vault entry.
- An open dropdown moves with its field when the screen rotates, the form scrolls or the app's window moves,
  once the field stops moving.

The desktop Qt app has the same forms without the password-manager link.

## Finding a system

The import screen is a drill-down with two stages, and shows one or the other, never both:

- **Find** — the credentials form, the search controls (zip / browse / system ID) and the results
  list. Searching again from here replaces the results.
- **The system** — its sites, talkgroup summary, import options and preview. Loading a system
  replaces the find stage; a `‹ All systems` row (`‹ Search` when no list was fetched) is the way
  back to it. The results list survives that, so importing several systems from one search is a
  tap each.

A zip code or system ID you typed stays in its field when you leave the screen (its Back, the system
Back) and is there on the next visit, with the zip or system ID tab still selected; a loaded system
and the browse picks do not carry over. A finished import clears the query.

## Saved sites in Qt and Android

For trunked systems, enable **Import each selected site as its own system**, select the sites, and import.
The setup wizard saves one row per site with its own frequency and CSV files, using the source and tuner
settings you choose. Frequency, protocol and channel/group CSV edits in that wizard apply to the first
site; the other sites retain their individual import settings. A single-site import records the same
site identity. Identity uses RadioReference's database site ID, not the RF site number.

An import fills the wizard's **Channel map** and **Talkgroups** rows with the files it generated. When it
generates none for a row (a TETRA system that lists no talkgroups, a single conventional repeater), that
row is emptied if it held a file an earlier import generated, so a system never keeps another system's
files. A file you picked yourself stays only while you are editing that saved system; a new system and
each site of a per-site import keep only the files their own import generated. A bandwidth that cannot
decode TETRA is emptied too, so a TETRA import runs at 72 kHz.

Home groups sibling sites under one card with an **n sites ›** chooser. **Use my location** requests a
foreground fix, names the nearest eligible saved site in this system, and orders the available sites by
distance. Avoided sites appear last; sites without coordinates are explained and cannot be ranked.
Distances appear in miles by default. Enable **Settings → Units → Use metric units** to display kilometers.
The preference updates distances and location accuracy immediately. The chooser labels a saved fix and
shows its reported accuracy; when that accuracy could change the nearest-site ordering, it says so.
Missing `(0, 0)` coordinates and nonfinite or out-of-range values never participate in distance calculations.
The avoid switch is saved locally. Re-import older saved sites to pick up corrected position metadata.

**Listen to nearest site** starts while Idle. While listening, **Stop and switch to nearest** uses the
named site; it indicates when you are already listening to that site. Each site's **Stop and switch**
also remains available. A switch stops the session, waits for confirmed Idle, and starts the
selected saved UID with that site's files. Any intervening pointer, keyboard, shortcut or wheel action
cancels the pending restart. There is no live automatic hop: tuning alone would retain the old site's
channel and group files, including site-specific DMR LCN maps.

Site location requests accept fixes at most 30 seconds old and finish without reverse geocoding.
An accurate recent cache can complete immediately; otherwise Android requests a fresh fix and allows
up to 20 seconds for a more accurate provider before using the best current result. Precise foreground
location is offered when the site chooser first requests permission, with approximate access still usable.
If approximate access was already granted, it is respected; precise location can be enabled in Android's
app permission settings. GPS participates when precise access is granted. No background location is requested.
During a lookup the nearest-site action is disabled. A failed refresh labels any saved location still in use.

Editing a saved site's frequency, decode flag or CSV selections clears its RadioReference grouping IDs.
Renaming or avoiding a site preserves them. Older imports with provenance only on the library file show
**Refresh from RadioReference to enable site grouping** and stay ungrouped; re-import the site to create
the saved-row identity. Refreshing only a library CSV cannot infer which saved row represents which site.
Manual rows never group.

## Terminal UI

Open the menu overlay with `Enter`, then **Trunking -> Channels & groups -> Import from
RadioReference...**. The wizard asks for your username, password and — in a keyless build — your
application key, verifies the account once, and then offers the same search modes as the Qt screen.
Selecting a system opens one modal panel with the site list, the three option toggles, and a preview of
the plan that is recomputed on every keystroke.

A trunked system is imported one site at a time, and a statewide network is usually worth importing
several times over — once per county you want to be able to tune. So an import does not close the
wizard: it writes its files, applies them to the session, releases the site selection and stays on the
site list, with a status line naming what it just wrote. Picking another site and pressing `Enter`
again imports that one too, with no second fetch. `Esc` closes the wizard when you are done.

The RadioReference rows are hidden in a build without libcurl or expat.

### Imported Systems browser

Once you have imported one or more systems, **Trunking -> Channels & groups -> Imported RadioReference
systems...** lists them — one row per stored import, not one per file, with the group list and channel
map an import wrote folded back together by the file name they share. Several rows can name the same
system: one per site, which is the point.

Each row shows the system name, the site it covers, its protocol, and its start frequency (or
`<freq> scan` for a conventional multi-repeater list, or `-` when the system carries no saved
settings). A `*` in the left gutter marks a row the running session is decoding. The two text columns
are measured against your terminal on every open, so a wide one shows more and an 80-column one
truncates with `..` rather than losing the frequency. The site column is dropped entirely when nothing
in the directory records one.

The site cell is blank for a file imported before dsd-neo recorded sites; a **Refresh from
RadioReference** fills it in.

Selecting a system offers three actions:

- **Use this system** re-applies it to the running session — decode mode, trunking, the channel map and
  group list, and the starting frequency — exactly as the original import did, without fetching anything.
  It works offline. A system imported before this feature landed (or one a newer build wrote) has no saved
  settings; its row reads **Load these files**, which loads whichever CSVs exist and leaves the decode mode
  unchanged.
- **Refresh from RadioReference** re-fetches the system and rebuilds its files in place, refreshing both
  halves in turn — for the site that row was built from, not for the system as a whole. This needs your
  account, like an import.
- **Delete imported files** removes that import's CSVs and their sidecars from disk after a confirmation
  that names the system *and* the site, since several rows can share a system name.
  A running session that already loaded a deleted file keeps decoding its in-memory copy; a saved config
  that references the deleted path will fail to load it on the next launch.

"Use this system" is possible because each generated file's sidecar now records *how* the import was
applied — the protocol, the start frequency, and the trunking, scanning, simulcast and ESK answers — next
to the *where it came from* provenance a refresh already uses. Both actions check the session first, as the
wizard does, and post nothing when it cannot take them; the status line says why. **Use this system** is
checked for the stored system's own decode mode (see [Receiver requirements](#receiver-requirements)), a
running trunk scan, and a trunk scan waiting for the next start. **Load these files** leaves the decode mode
alone, so it is checked only for a running trunk scan and, when it loads a channel map, a trunk scan waiting
for the next start.

### Picking a CSV to import

**Trunking -> Channels & groups -> Import channel map CSV...** and **Import group list CSV...** now
open a chooser of the imports directory's files of that kind — RadioReference file names carry spaces and
are tedious to type — with a final **Enter a path...** row that falls back to typing a path. When the
imports directory holds no matching file, the path prompt opens directly, so nothing changes for a user
with no imports.

### Where the files go

Generated files land in an `imports` directory beside your config file:

| Platform | Directory |
|---|---|
| Linux / macOS, `XDG_CONFIG_HOME` set | `$XDG_CONFIG_HOME/dsd-neo/imports/` |
| Linux / macOS, otherwise | `$HOME/.config/dsd-neo/imports/` |
| Windows | `%APPDATA%\dsd-neo\imports\` |

If none of those variables is set there is nowhere to write, and the import is blocked with a message
naming the variable for your platform.

The names are derived from the system name and the site, and are deterministic — `<system> - <site>
group.csv` and `<system> - <site> chan.csv`. Re-importing the *same site of the same system* overwrites
in place, so any config that references the path stays valid; importing a *different* site writes a new
pair beside it, which is how one statewide system ends up stored once per county. A conventional import
of several repeaters is named `<N> repeaters` rather than for a place.

The system name is cut to 40 bytes and the site to 24, so a long system name cannot squeeze the site
out of the name. Anything else already holding a candidate name — another system, or a file of your own
— pushes the import onto ` (2)`, ` (3)` and so on, up to nine tries before it refuses; both halves of one
import always share a name. A file with no readable sidecar is treated as yours and is never
overwritten.

A refresh rewrites a file **in place** and never renames it, so a site that RadioReference has since
renamed shows its new name in the browser while its file keeps the old one.

### What applying does, and what a save keeps

A finished import is applied to the running session immediately: decode mode, trunking, the channel map and
group list, and the starting frequency. It is **not** written to your config file. Persisting it means
**Config -> Save Config**, which keeps everything the import applied:

| Setting the import applies | Kept by Config -> Save | Key |
|---|---|---|
| Decode mode | Yes | `[mode] decode` |
| Trunking on/off | Yes | `[trunking] enabled` |
| Channel map (`-C`) and group list (`-G`) paths | Yes | `[trunking] chan_csv` / `group_csv` |
| Simulcast QPSK demodulation | Yes | `[mode] demod` |
| Conventional scanning (`-Y`) | Yes | `[trunking] scanner` |
| P25 learned-candidate preference (`-^`) | Yes | `[trunking] p25_prefer_candidates` |
| EDACS extended addressing and ESK | Yes | `[mode] edacs_addressing` / `edacs_esk` |

The last three had no INI key before this feature landed, which meant a saved config for a
multi-repeater Conventional Networked system, or for an EDACS system with ESK, decoded differently on the
next launch. They persist now. `edacs_addressing` (older files: `edacs_ea`) and `edacs_esk` refine the EDACS
decode preset without changing it — they are applied after `[mode] decode`, which resets both — in the same
way `dmr_mono` refines the DMR one.

Limits worth knowing:

- Applying is asynchronous and its outcome is reported by a status message from the decoder, not by the
  wizard: the wizard's `Imported <site>.` means the files were written and the apply was sent. Before it writes
  anything, the wizard checks the session the way the decoder will, and the preview shows `Blocked:` with the
  reason: the receiver for the system's decode mode (see [Receiver requirements](#receiver-requirements)), a
  running trunk scan (it manages its own channel maps), and a trunk scan waiting for the next start beside the
  channel map or the `-Y` scanner the import would set. A refused import stays on the site list with the
  system still loaded, so once the session can take it (the trunk scan stopped, the DSP bandwidth set), `Enter`
  imports it. The decoder checks again when the apply arrives; only it can see a SoapySDR device's rate.
- An import that has nothing to write (a TETRA system with no talkgroup a TETRA group list can carry writes no
  group list, and TETRA has no channel map) is still sent to the session, and the wizard says
  `Imported <site>; no files to save.` Such an import has no row in the Imported Systems browser, so it cannot
  be re-applied offline: import it again from RadioReference.
- The decoder checks everything it can before it changes anything: the receiver (see
  [Receiver requirements](#receiver-requirements)), a trunk scan waiting for the next start, the width an AM
  or Analog mode needs, whether a running I/Q replay can run the mode, and each CSV file. A refused import
  changes nothing (the call, a running `-Y` scan, the mode and the files all stay, and Config -> Save writes
  what it would have before), and the status message gives the reason, for example
  `Failed: RR import -> TETRA needs 72 kHz DSP bandwidth; set it and import again`.
- Something can still fail after those checks. A mode change that fails (a replay that does not reopen at the
  new rate, say) keeps its own message, which names the reason, for example
  `Failed: P25 Phase 1 -> the replay did not start at the new rate (see log)`, and the import goes no further.
  A CSV file that cannot be loaded by then (deleted in the meantime) is reported as a partial apply that names
  what did take, for example `Failed: RR import -> channel map not loaded; mode and talkgroups applied`.
- The tune to the system's frequency reports how it went. A tune that fails is a partial apply
  (`Failed: RR import -> tune to 851.012500 MHz failed; mode applied`). A tuner that is busy (for example
  while auto-PPM trains) takes nothing: the rest applies and the message asks for a retry
  (`Deferred; retry: RR import -> tune to 851.012500 MHz deferred (tuner busy); mode applied`); a manual tune
  still waiting to land keeps the receiver. A tune that is still on its way says so at the end of the import's
  message (`...; tuning to 851.012500 MHz`), and when it ends the message changes: `...; tuned to 851.012500
  MHz`, the failure (with what else the import applied), or `Interrupted: RR import -> tune to 851.012500 MHz; a
  newer tune took over` (or `; the radio input stopped`). A failure is always shown; a landing or an
  interruption only replaces the import's own message while it is still shown, never a newer one. When trunking
  or the scanner takes the receiver on from there (a control-channel hunt, a grant, a scan step, or the P25
  Return-to-CC and Candidate cycle actions), nothing more is said. A session with no tuner (a WAV file, UDP
  audio) says nothing about a tune.
- An import on an I/Q replay never tunes, even with a rigctl radio connected: the capture decides the
  frequency.
- `-Y` scanning needs a live radio input (RTL-SDR, rtl_tcp, SoapySDR or Airspy) or a rigctl-controlled radio.
  On a WAV, UDP or TCP source, or an I/Q replay, the scanner never steps and the session will look stuck on the
  first frequency. The generator emits that warning and the panel shows it.

#### Receiver requirements

AM needs an I/Q radio input. TETRA needs a receiver that can decode it; when the session has none, the import
is refused with the reason:

| Input | TETRA import |
|---|---|
| RTL-SDR, rtl_tcp, SoapySDR or Airspy at a 72 kHz DSP bandwidth | Accepted |
| RTL-SDR, rtl_tcp, SoapySDR or Airspy at any other DSP bandwidth | Refused: set the DSP bandwidth to 72 kHz (or start with `-fT`), then import again. The import does not change it itself, because reopening the radio would restart an `--iq-capture` from the beginning |
| I/Q replay | Accepted; the replay is re-planned to a TETRA rate, or the import is refused when the capture cannot be resampled to one |
| Symbol capture (`.bin`, `.raw`, `.sym`) | Accepted |
| PulseAudio, WAV, TCP/UDP audio, stdin | Refused: TETRA needs an I/Q radio input or a symbol capture |
| A session started with `--tetra-uplink` | Refused: restart without `--tetra-uplink` |

In the Android and Qt app, a saved TETRA system is checked when it starts, as an AM one is. It starts on a USB
RTL-SDR, rtl_tcp or Airspy radio whose bandwidth is 72 kHz or left empty (an empty field runs TETRA at 72 kHz,
whatever the default bandwidth in Settings). At any other bandwidth it is refused with the reason, because it would
start and never decode. A bandwidth the radio does not offer counts as the bandwidth it runs at: 48 kHz on a USB or
rtl_tcp radio, and on an Airspy the nearest bandwidth it offers (48 kHz for one above 72).

The add-system wizard checks the same thing before it saves: TETRA at a bandwidth other than 72 kHz cannot continue
until you set 72 kHz, empty the field, or pick another decode mode, and the reason shows under the decode chips. The
wizard shows the bandwidth field on an Airspy too, so that fix is always in reach. The bandwidth field's hint shows what
an empty field runs at (72 for TETRA). The import screen says what a TETRA system needs in its preview and again in the
review, including that some systems RadioReference lists as TETRA actually run Tetrapol, which dsd-neo cannot decode.
The terminal wizard says the same in three lines under the site list of a TETRA system: an I/Q radio at a TETRA rate (a
72 kHz DSP bandwidth on an RTL-SDR, rtl_tcp, SoapySDR or Airspy) or a symbol capture, and the Tetrapol caveat.

## What gets generated

The import produces up to two files and a decode flag. Which of them depends on the system type
RadioReference reports, resolved through its own type/flavor/voice tables rather than a baked-in
list — RadioReference adds rows over time.

| System | Decode flag | Channel map |
|---|---|---|
| P25, standard | `-ft -^` | Yes — it is the control-channel hunt list, after a loss and at session start ([Startup control-channel hunt](cli.md#startup-control-channel-hunt)) |
| P25, simulcast (LSM) | `-mq -^` | Yes |
| DMR Connect Plus | `-fs` | Yes, LCN-keyed, with a `999,<cc>` seed row |
| DMR Capacity Plus / Hytera XPT | `-fs` | Yes, LSN-paired (`2n-1`, `2n`) |
| DMR Tier III | `-fs` | Yes, LCN-keyed, with a `999,<cc>` seed row |
| NXDN 4800 / 9600 | `-fi` / `-fn` | When the site publishes channel numbers; it is then the control-channel hunt list, at session start and after a loss ([Startup control-channel hunt](cli.md#startup-control-channel-hunt)) |
| EDACS Standard / Networked / Narrowband | `-fh`, or `-fH` with ESK | Yes, strictly positional |
| EDACS Extended Addressing | `-fe`, or `-fE` with ESK | Yes, strictly positional |
| P25 / DMR / NXDN **Conventional Networked** | see below | Only with two or more repeaters, each row named |
| TETRA | `-fT`, with trunking on | Never — the decoder learns the cell's carriers from its control channel |
| Everything else (LTR, Motorola, OpenSky, iDEN, MPT-1327, SmarTrunk, EDACS SCAT, …) | — | Import is blocked with a message |

The talkgroup list keeps only IDs the system can carry. A talkgroup whose RadioReference ID is
missing, not a number or too large to be one is skipped, and so is a TETRA group above 16777215
(TETRA group IDs are 24 bits; 16777215 itself, the broadcast group, is kept, and so is 0). The
preview counts each kind of skipped row in its own warning, and its talkgroup count, encryption counts and
sample show only the talkgroups the list will hold. When no talkgroup is left, the import
writes no talkgroup list and the warnings say why.

`-^` rides with every P25 import on purpose. Supplying a channel map tells the decoder a user LCN
list exists, which otherwise disables its own learned control-channel candidates; `-^` puts the
learned ones first and leaves the imported list as the backstop.

Two things the preview will warn about rather than silently paper over:

- **The P25 channel map's first column is a placeholder.** RadioReference publishes a small
  sequential index per site, not the 16-bit `(iden << 12) | chan` identifier the decoder looks up,
  so the map's *frequency ordering* is what makes it useful (it is the hunt rotation, including when a session starts on a
  quiet control channel) while the
  channel numbers themselves are inert. They are purged within seconds of control-channel lock,
  when the site's first `IDEN_UP` invalidates that band's slots.
- **Custom band plans are not carried.** When RadioReference publishes one for a P25 system the preview says
  so.
  The decoder takes a band plan as a P25 band plan CSV (`--p25-bandplan`, the Imports library's
  **P25 band plan** kind, or the terminal's **Import P25 band plan CSV...**; format in
  `docs/csv-formats.md`), which is one row per identifier rather than one per channel. Generating
  that file from the RadioReference plan is a follow-up: RadioReference publishes base, spacing and
  offset per entry but no identifier number or channel type.

## Conventional Networked systems

This flavor behaves unlike every other supported system and the UI treats it differently.

There is no trunking and no control channel. One RadioReference "site" is **one repeater on one
frequency**, so the unit of choice is the repeater, not the site — the picker becomes a
multi-select list, sorted by name, showing each repeater's frequency and colour code.

- **Two or more repeaters** produce a scanner frequency list (`-C`) and add `-Y`, which walks it
  after `trunk_hangtime` elapses with no sync. Each row's third column is that repeater's
  RadioReference site description, sanitised and capped the same way a talkgroup name is (collapsed
  whitespace, commas rewritten to `/`, control bytes dropped, 49 bytes) — this is what lets the
  scan mode display and call history show which repeater is being heard. A name that sanitises away
  to nothing is left off the row entirely rather than written empty, and shortening any name adds a
  warning to the preview.
- **Exactly one repeater** produces no file at all. The session simply tunes that frequency. This
  is deliberate: a one-entry scan list makes the scanner retune to the frequency it is already on
  at every hangtime expiry.
- Duplicate frequencies are dropped, with a warning: two repeaters can share one output.
- **Scanning needs a live radio input (RTL-SDR, rtl_tcp, SoapySDR or Airspy) or a rigctl-controlled
  radio.** On a WAV, UDP or TCP source, or an I/Q replay, the scanner never steps, so the session looks stuck
  on one frequency. The preview says so.

The repeater's **colour code is shown but never written to a file**. `dsd_opts` has no colour-code
field, because DMR reads it off the air; it is in the list only to help you recognise a repeater.

## TETRA systems

A TETRA import sets `-fT` with trunking on and writes at most one file, the talkgroup list. RadioReference's
decimal talkgroup ID is the group's TETRA ID, so the list works as it is. There is no channel map: every carrier of
a TETRA cell says where the cell's control channel is, so the decoder learns that from the air. Many TETRA systems
on RadioReference list no talkgroups at all; such an import writes no file (see
[What applying does](#what-applying-does-and-what-a-save-keeps)).

- **The start frequency.** As for every trunked system, the session starts on the frequency RadioReference marks
  as the site's control channel, else on the first frequency it lists, with a warning. Many TETRA sites mark none.
  The start frequency should be one the cell sends on (its downlink). RadioReference does not always list that:
  some systems list the frequencies the radios send on (the uplink), some list rounded values, and some list
  carrier numbers instead of frequencies. dsd-neo skips a value that is not a frequency it can tune, but it does
  not correct the others: check the frequency against the system's own listing.
- **Moving to the main carrier.** Calls are set up on the cell's main carrier. A session that starts on another
  carrier of the same cell moves to the main carrier by itself, once it has decoded a block there whose CRC
  passed. That needs a radio dsd-neo can tune (an I/Q replay stays where it is), and it only works from a carrier
  dsd-neo can already decode. dsd-neo does not search for a control channel: a start frequency that is silent,
  an uplink, or another system's stays where it is, and so does a session whose main carrier it cannot receive.
  Tune the cell's downlink yourself (in the app, edit the saved system's frequency), or pick another site. The
  TETRA entry under "Modes & Decoders" in `docs/cli.md` gives the details and the log lines.
- **The receiver.** See [Receiver requirements](#receiver-requirements). Any radio input decodes TETRA at a 72 kHz
  DSP bandwidth, an Airspy included.
- **Tetrapol.** Some systems RadioReference lists as TETRA actually run Tetrapol, which dsd-neo cannot decode.
  Nothing in RadioReference's data tells the two apart, so such a system imports like any other and then decodes
  nothing.
- **Trunk scan.** TETRA is not a `--trunk-scan` target type; follow one site at a time.

## Refreshing

A generated file remembers where it came from — the system ID, the selected sites, and which kind of
file it is. `Imported files → (tap a row) → Refresh from RadioReference` re-fetches the system and
replaces that file in place, keeping its stored path so every saved system referencing it stays
valid.

In the terminal UI refresh lives inside the **Imported Systems** browser (above): pick a system, then
**Refresh from RadioReference**. Provenance travels in a plain-text sidecar written next to each generated
file (`<file>.rr`) recording the system ID, the selected sites, the partial-encryption answer and — new
with the Imported Systems browser — the re-apply recipe. A file with no sidecar is not a managed import, so
it is left out of the browser rather than offered and refused; when nothing in the directory has one, the
browser reports that no imports were found there.

- The staging copy is validated **before** the stored copy is touched, so a fault page or a
  truncated response cannot destroy working local data.
- Sites are matched by RadioReference `siteId`, not by position and not by RF site number: the site
  list is free to be reordered, and a system can number several sites the same.
- A refreshed file is pushed into a running session only when that session is actually using it.
  The terminal and the app send the session a reload naming the refreshed file, and the decoder
  decides when it applies it: it reloads the file only if it is the channel map or group list in use
  at that moment, so a session that moved to another file, or a `-Y` row's own list, is left alone.
  Applying a channel map replaces the live one wholesale and discards what the decoder had learned
  on the air — see `docs/csv-formats.md`.
- A talkgroup-list refresh in which none of RadioReference's talkgroups has a usable ID keeps the
  stored list and says so, rather than replacing it with an empty one.
- The "treat partly encrypted as encrypted" answer is recorded with the file, so a refresh
  regenerates the talkgroup list with the answer the original import was given rather than the
  UI default.

## Terms of service

Sourced from RadioReference's
[Database Web Service API article](https://support.radioreference.com/hc/en-us/articles/18844460198932-Database-Web-Service-API):

- Each end user authenticates with **their own credentials** and must hold **their own active
  premium subscription**. Credentials are never shared or pooled.
- The application key is issued **per application**.
- The API may not be used to reproduce, mirror, or substitute for the RadioReference.com website.
  Non-radio-programming uses require a separate commercial licence.

RadioReference publishes no rate limit. dsd-neo is conservative by construction: one worker thread
per client, so a client has at most one request in flight at a time.

### Use my location (Android)

In **Find a system**, **Use my location** requests approximate foreground location.
Granting coarse location permission is optional; ZIP, Browse and system ID searches
remain available after denial. After permission is granted, location acquisition and
reverse geocoding each have a separate 20-second timeout. Lookup can reuse the newest
usable cached fix from an enabled provider if it is at most ten minutes old and its reported
accuracy is within 10 km. Cache age uses Android's monotonic clock; the original fix
timestamp is retained. This avoids waiting for a fresh update when Android throttles
approximate location. Android 12+ tries both the platform fused and network providers
when no usable cached fix exists; older versions use the network provider.
A successful US five-digit postal code enters the existing ZIP
lookup. Other countries, missing postal codes and geocoding errors direct you to Browse.
Desktop hosts hide this button by default.

A newer search or Browse request cancels the pending location lookup; late results
cannot replace the newer results. Android Activity teardown also cancels it. A valid
fix is retained privately (latitude, longitude, accuracy in metres and timestamp in
milliseconds), even if geocoding fails, for later nearby-site selection. It expires
after 24 hours using the existing preference expiry timer and load/read checks.
Coordinates are not added to diagnostics or exports.

Nearby lookup and the site chooser share one location request owner. Leaving
RadioReference cancels its pending lookup; asking for location in either screen
retires the previous caller, and late replies cannot replace the current request.

### Android encryption review

New Qt/Android imports default to keeping encrypted and partly encrypted talkgroups
enabled. The review can instead exclude fully encrypted groups, or fully and partly
encrypted groups. Exclusion disables the entire group, including its clear calls;
installing keys does not override that listening policy. Refresh retains the policy
stored with each import, including legacy exclusion choices. The terminal wizard
and legacy generation API retain their previous default behavior.

Country/state/county and site/repeater pickers support search. Selection is retained
while filtering, and the final review lists sites, modulation, source requirements,
policy and generated files (or that there are none) before import. Cancelling a
request keeps earlier search results available. Location denial leaves ZIP, Browse
and direct system ID usable.
