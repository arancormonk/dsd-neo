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
  them in its own settings store (`Settings -> RadioReference account`); the terminal UI keeps them in the
  config file under `[radioreference]` (`docs/config-system.md`). The key row is offered only where the
  user is the one who has to supply a key — see [The application key](#the-application-key).
- The **password is held in memory only** and is asked for once per app session. It is never
  written to disk, never logged, and never reaches a status line or an error message.

After the password is entered the app verifies the account once (`getUserData`) and reports an
expired premium subscription as such, rather than letting it surface later as an opaque failure.

## Finding a system

The import screen is a drill-down with two stages, and shows one or the other, never both:

- **Find** — the credentials form, the search controls (zip / browse / system ID) and the results
  list. Searching again from here replaces the results.
- **The system** — its sites, talkgroup summary, import options and preview. Loading a system
  replaces the find stage; a `‹ All systems` row (`‹ Search` when no list was fetched) is the way
  back to it. The results list survives that, so importing several systems from one search is a
  tap each.

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
to the *where it came from* provenance a refresh already uses. A session started with `--trunk-scan`
manages its own channel maps, so the decoder refuses the apply and reports it, the same as a fresh import.

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
| EDACS extended addressing and ESK | Yes | `[mode] edacs_ea` / `edacs_esk` |

The last three had no INI key before this feature landed, which meant a saved config for a
multi-repeater Conventional Networked system, or for an EDACS system with ESK, decoded differently on the
next launch. They persist now. `edacs_ea` and `edacs_esk` refine the EDACS decode preset without changing
it — they are applied after `[mode] decode`, which resets both — in the same way `dmr_mono` refines the DMR
one.

Two more limits worth knowing:

- Applying is asynchronous and its outcome is reported by a status message from the decoder, not by the
  wizard. A session started with `--trunk-scan` manages its own channel maps: the wizard refuses the import
  before writing anything, and the decoder-thread handler refuses the apply again if one ever reaches it.
- `-Y` scanning needs an RTL-SDR or a rigctl-controlled radio. On a WAV, UDP or TCP source the scanner
  never steps and the session will look stuck on the first frequency. The generator emits that warning and
  the panel shows it.

## What gets generated

The import produces up to two files and a decode flag. Which of them depends on the system type
RadioReference reports, resolved through its own type/flavor/voice tables rather than a baked-in
list — RadioReference adds rows over time.

| System | Decode flag | Channel map |
|---|---|---|
| P25, standard | `-ft -^` | Yes — it is the control-channel hunt list |
| P25, simulcast (LSM) | `-mq -^` | Yes |
| DMR Connect Plus | `-fs` | Yes, LCN-keyed, with a `999,<cc>` seed row |
| DMR Capacity Plus / Hytera XPT | `-fs` | Yes, LSN-paired (`2n-1`, `2n`) |
| DMR Tier III | `-fs` | Yes, LCN-keyed, with a `999,<cc>` seed row |
| NXDN 4800 / 9600 | `-fi` / `-fn` | When the site publishes channel numbers |
| EDACS Standard / Networked / Narrowband | `-fh`, or `-fH` with ESK | Yes, strictly positional |
| EDACS Extended Addressing | `-fe`, or `-fE` with ESK | Yes, strictly positional |
| P25 / DMR / NXDN **Conventional Networked** | see below | Only with two or more repeaters |
| Everything else (LTR, Motorola, OpenSky, iDEN, TETRA, MPT-1327, SmarTrunk, EDACS SCAT, …) | — | Import is blocked with a message |

`-^` rides with every P25 import on purpose. Supplying a channel map tells the decoder a user LCN
list exists, which otherwise disables its own learned control-channel candidates; `-^` puts the
learned ones first and leaves the imported list as the backstop.

Two things the preview will warn about rather than silently paper over:

- **The P25 channel map's first column is a placeholder.** RadioReference publishes a small
  sequential index per site, not the 16-bit `(iden << 12) | chan` identifier the decoder looks up,
  so the map's *frequency ordering* is what makes it useful (it is the hunt rotation) while the
  channel numbers themselves are inert. They are purged within seconds of control-channel lock,
  when the site's first `IDEN_UP` invalidates that band's slots.
- **Custom band plans are out of scope.** When RadioReference publishes one the preview says so;
  use the wizard's extra CLI args.

## Conventional Networked systems

This flavor behaves unlike every other supported system and the UI treats it differently.

There is no trunking and no control channel. One RadioReference "site" is **one repeater on one
frequency**, so the unit of choice is the repeater, not the site — the picker becomes a
multi-select list, sorted by name, showing each repeater's frequency and colour code.

- **Two or more repeaters** produce a scanner frequency list (`-C`) and add `-Y`, which walks it
  after `trunk_hangtime` elapses with no sync.
- **Exactly one repeater** produces no file at all. The session simply tunes that frequency. This
  is deliberate: a one-entry scan list makes the scanner retune to the frequency it is already on
  at every hangtime expiry.
- Duplicate frequencies are dropped, with a warning: two repeaters can share one output.
- **Scanning needs an RTL-SDR or a rigctl-controlled radio.** On a WAV, UDP or TCP source the
  scanner never steps, so the session looks stuck on one frequency. The preview says so.

The repeater's **colour code is shown but never written to a file**. `dsd_opts` has no colour-code
field, because DMR reads it off the air; it is in the list only to help you recognise a repeater.

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
  Applying a channel map replaces the live one wholesale and discards what the decoder had learned
  on the air — see `docs/csv-formats.md`.
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
