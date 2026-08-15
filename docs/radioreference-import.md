# RadioReference Import

The Android app can build a system's trunking data straight from the
[RadioReference.com](https://www.radioreference.com/) database instead of asking you to hand-write
CSVs: browse to a system by zip code, by country/state/county, or by its system ID, pick the site,
and the app generates the talkgroup list and — where the protocol needs one — the channel map, in
the same formats `docs/csv-formats.md` describes. The generated files land in the imported-files
library like any other import, and the add-system wizard opens with the frequency, the decode flag
and both files already filled in.

The runtime half is a UI-agnostic C API (`include/dsd-neo/runtime/radioreference.h`), so the
terminal UI can adopt it later; today the Qt Quick frontend is the only consumer.

## Requirements

- **Your own RadioReference premium subscription.** Every user authenticates with their own
  credentials. Nothing is shared or pooled, and dsd-neo does not subsidize accounts.
- **An application key** (see below).
- A build with **libcurl and expat** available. Both are auto-detected; the Android and Windows
  presets require them (`DSD_REQUIRE_CURL=ON`, `DSD_REQUIRE_EXPAT=ON`). Where either is missing the
  feature reports itself unavailable and every entry point is hidden rather than failing later.

## The application key

RadioReference issues one key per *application*, separately from your account. dsd-neo can carry a
project key baked in at build time, and you can override it with your own.

Baking one in:

```bash
DSD_RR_APP_KEY=<key> cmake --preset android-app
```

`DSD_RR_APP_KEY` is a CMake cache variable with an environment-variable fallback. Prefer the
environment form shown above: `-DDSD_RR_APP_KEY=<key>` writes the value in plaintext into
`build/<preset>/CMakeCache.txt`, while the environment route keeps it out of the build tree. The
value is substituted into a generated C source file, so it must match `^[A-Za-z0-9._~-]+$`;
configure fails otherwise. Leave it unset and the app prompts for a key instead.

**A baked key is not a secret.** It is extractable from any shipped binary with `strings`. Keeping
it out of the repository protects the repository, not the artifact.

CI takes the key from the GitHub repository secret `RADIOREFERENCE_APP_KEY`, passed through the
`Configure (android-app)` step's environment. An unset secret expands to the empty string, which is
a valid build that prompts.

To request a key, sign in and apply at
<https://www.radioreference.com/account/api/apply>. An active premium subscription is required on
the applying account, and RadioReference staff review each request by hand, so allow lead time.
Describe your use concretely — a tool a user runs alongside their scanner, each user authenticating
with their own premium credentials, is squarely the sanctioned case.

## Credentials

- The **username** and an optional **application-key override** persist in the app's settings
  (`Settings → RadioReference account`).
- The **password is held in memory only** and is asked for once per app session. It is never
  written to disk, never logged, and never reaches a status line or an error message.

After the password is entered the app verifies the account once (`getUserData`) and reports an
expired premium subscription as such, rather than letting it surface later as an opaque failure.

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
- The list is **capped at 26 entries** — the runtime's positional frequency array holds 26 — and the
  screen shows a running count against that ceiling. Selecting more truncates, with a warning.
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

- The staging copy is validated **before** the stored copy is touched, so a fault page or a
  truncated response cannot destroy working local data.
- Sites are matched by site *number*, not by position: RadioReference is free to reorder its site
  list.
- A refreshed file is pushed into a running session only when that session is actually using it.
  Applying a channel map replaces the live one wholesale and discards what the decoder had learned
  on the air — see `docs/csv-formats.md`.
- The "treat partly encrypted as encrypted" answer is not recorded with the file, so a refresh
  regenerates the talkgroup list with that option on, its default.

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
