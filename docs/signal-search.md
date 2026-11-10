<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> -->

# Signal search

Choose **Signal search** on the **Explore** tab of the Qt/Android Listen screen. Select
USB, Airspy or RTL-TCP, add or edit frequency ranges, and choose **Start search**.
Search measures RF power across the radio's capture bandwidth without drawing a
spectrum. It visits strong candidates to identify and monitor them, then continues
through the remaining candidates and ranges.

Signal search opens on the radio the last search started on, else the one Explore
last used, else a USB dongle. On Android, a remembered RTL-SDR dongle opens as the
Airspy when only an Airspy is plugged in, and the reverse; with both kinds or
neither plugged in, the remembered radio stays selected, and an RTL-TCP server is
never replaced. Explore's start, its setup screen and the radio named on its Home
card follow the same rule. **Start search** remembers the radio it starts on
separately from Explore, so a search never changes where Explore resumes; both
travel with a settings backup.

RF strength does not measure distance. Antenna, gain, propagation and other
transmitters affect discovery. This is not hardware Close Call capture, and crowded
RF conditions can mask weaker signals. A short burst may end before a listening
visit starts.

## Ranges and reception

Ranges are enabled and processed in their displayed order. Their inclusive start
and end frequencies use MHz; channel step and channel bandwidth use Hz. Fractional
steps have an integer numerator and denominator: **25,000 / 3** preserves exact
8⅓ kHz spacing. Each channel is rounded from the range origin independently.
Overlapping ranges retain their order; frequencies already visited in a pass are
suppressed until the next pass.

| Setting | Default and behavior |
| --- | --- |
| RF sensitivity | 15 dB above the local RF noise estimate; adjustable from 3–30 dB |
| Minimum RF level | Optional dBFS threshold; when enabled, both RF conditions must pass |
| NFM bandwidth / listening squelch | 16,000 Hz / `noise+10`; existing Auto fallback on narrow channels |
| AM bandwidth / listening squelch | 6,000 Hz / `auto+10` |
| Digital identification | Explicit Auto for NFM, or a selected existing profile; analog-only is available |
| Probe duration | 3 seconds after fresh tuned samples arrive; adjustable from 2 to 10 seconds |
| Quiet time | Resume after 500 ms without relevant carrier or validated digital activity; an analog visit counts it from the monitor's first verdict on the landed channel (at most 3 seconds after landing) |
| Maximum visit | 30 seconds; adjustable through 600 seconds, or 0 to disable |
| Gain | Inherit the session, or use the backend's range controls; Airspy has its own gain controls. When the search warns that the receiver is overloaded, Pause, lower this, then Resume |

The range editor closes with **Save range** or **Cancel**. Once a field differs
from what the editor opened with, Cancel, Back, Escape or a tap beside the panel
asks before discarding the change. While no search runs, leaving the screen with
a different radio or RTL-TCP host or port asks the same way; the radio the screen
opened on, including the attached receiver it chose, is not a change.

RF detection sensitivity and listening squelch are separate. Changing listening
squelch does not change which channels pass the RF detector. An Auto or Noise
listening squelch takes a margin of 3 to 30 dB, the range the receiver applies;
a range with a margin outside it is refused rather than saved, and a range saved
earlier with 0 to 2 dB loads with the 3 dB the receiver applied. AM and analog-only
NFM ranges open their monitor directly. Digitally enabled NFM ranges first run
the selected profile. An Auto probe tries the symbol profiles in the order that
reaches the common systems first: 4800 symbols/s four-level (P25 Phase 1, DMR,
NXDN96, YSF, M17) for about 1.1 seconds, 6000 symbols/s (P25 Phase 2) for 0.3
seconds, held for as long as Phase 2 bursts keep arriving, then 2400 symbols/s
(NXDN48, dPMR) for about 2.3 seconds, 9600 symbols/s (ProVoice) from about 3.7
seconds and 4800 symbols/s binary (D-STAR) after that. A profile that decodes
frames is held while they keep coming, and the first confirmed protocol ends the
probe; the rest of the visit runs the ordinary rotation. The three-second default
leaves time for P25 Phase 1 and 2, DMR, NXDN, dPMR, YSF and M17. In replays of the
test captures landing at several points of a transmission, Phase 1 control
channels, DMR, NXDN96, YSF and M17 confirmed within 0.6 seconds and Phase 1 voice
usually within 1.6 (one capture whose first frames are not decodable confirmed
only on a later landing); P25 Phase 2 (with its scrambling code recovered on the
channel), NXDN48 joined mid-call and dPMR confirmed within 2.4 seconds, and real
Phase 2 voice with added noise within 2.9. ProVoice needs about five seconds and
D-STAR about seven, so a range that should identify them needs a longer probe.
Ranges saved before this release keep their two-second probe, which leaves P25
Phase 2 and NXDN48 joined mid-call often unresolved: raise them to three seconds or
more in the range editor.

A detection displays frequency and measured strength immediately, while its
listening tune is still pending. While listening, the screen also shows the
channel's current RF level (**Now … dBFS**), measured on the same detector scale
roughly four times a second; history keeps the detection-time level. Live level
needs the receiver's Fs/4 offset tuning, which keeps the landed channel away from
hardware DC; inputs without it show only the detection level. A steady transmitter
reads steady: on a constant-level FM carrier successive readings differ by a few
hundredths of a dB, so the displayed tenth of a dB often does not change, while a
change in the signal's level shows within about a second. On a paced loopback
rtl_tcp source, 6, 12 and 18 dB steps in the transmitter's level each showed at
the next reading, on NFM, digital-identification and decoded DMR visits. When the receiver cannot measure the
channel for about two seconds (the capture keeps changing, drops samples or clips),
the screen stops showing **Now** and shows only the detection level until a fresh
measurement arrives; a visit's live level also ends with the visit. Clipping
during a visit does not end the visit or skip anything. A small dot
beside **Now** brightens briefly at each fresh measurement, including one that
reads the same level as the last, so a steady reading is visibly a live one; it
is hidden while no live level is shown.

While a search runs, the screen is laid out for what is on the air now:

- A status line under the title names the pass, the range being searched with
  its bounds, and the channel, for example
  **Pass 2 · Air band · 118.000–137.000 MHz · channel 81 of 801**. It names the
  range the search is in, not the range of the last detection, and a range
  edited while paused shows there once **Resume** applies it. A range name too
  long for the line is shortened so the pass and channel count always show
  whole; on a narrow phone with the largest text the line keeps three lines.
  The Now panel's actions take three columns, or two, or one where a label's
  word would not fit half the width, so no label is cut.
- The **Now** panel keeps one height for the whole session: frequency, strength,
  identification, two lines of detail, saved-frequency names and the actions.
  The strength line shows the live and detection levels
  (**Now -47.2 · detected -32.1 dBFS**, on two lines with the largest text);
  the range is named in the status line. The two detail lines read the checked
  site, the access code and tone, the call (**Slot 2 · Target 100 · Source 2001**,
  the slot on two-slot systems only), then its encryption
  (**WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2 · NAC 293 · Target 2401 · Source 1234567**;
  **TIII Small · Net 13 · Site 4 · SYS 1183 · CC 1 · Slot 2 · Target 100 · Source 2001 ·
  ALG 21 · KID 0001**), breaking lines only between values. Values that do not
  fit the two lines are left off whole, the detail then ending in **· …**. Other
  lines that do not fit end in an ellipsis, and the full text is available to
  screen readers. When several
  saved frequencies match, the panel names two and counts the rest
  (**Saved: A / B +1**); the detection's history row lists them all. Between
  detections the panel reads **Waiting for a signal**, shows the phase
  (**Searching Air band…**, **Tuning…**, **Paused**) and the hint **Strong
  signals open here to be identified and monitored.** (while searching, tuning
  or paused; a stopped or failed search shows only its phase and message), and
  keeps the actions' space empty, so nothing below it moves as detections come
  and go. The six actions (**Hold**, **Skip**, **Avoid this session**, **Avoid
  permanently**, **Save as system** and **More**) sit in two columns, or three
  where every label fits. **More** offers **Explore here** and **Start** for
  each saved system the detection matched.
- A refusal or error (**The avoid list is full**, a radio failure) shows under
  the **Now** panel, so it never moves the panel as it appears or clears; with
  no search running it shows above the setup instead. The overload warning is
  the last line there while the search runs or is paused, for example
  **Part of “Marine” was skipped because the receiver is overloaded. Pause, then
  lower the range's receiver gain.** Paused, it ends **Lower the range's receiver
  gain, then Resume.** With an Airspy it would ask for a lower Airspy gain for
  the range instead (**Use Airspy gain settings for this range** in the range
  editor), but an Airspy's overload cannot be seen, so the warning does not
  appear for one (see the Airspy paragraph under [Scope and
  implementation](#scope-and-implementation)). It is not shown once the search
  fails or stops. A screen reader hears
  the warning at once as it appears or clears; a new range name in it is read at
  most every five seconds, so two ranges that take turns clipping do not
  interrupt every pass.
- **Recent detections** shows the five newest; **Show more** adds 25. Under its
  verdict a row shows the site, access code, tone and encryption the detection
  last had, in the Now panel's order without the call
  (**TIII Small · Net 13 · Site 4 · SYS 1183 · CC 1 · Encrypted**;
  **CTCSS 127.3 Hz** or **DCS D023N / D047I** on an analog channel). A value
  the detection never had is left out. The site, access code and encryption are
  checked signalling; the tone is what the analog monitor heard (the tone
  paragraph under [Ranges and reception](#ranges-and-reception) says which tone
  a detection keeps). These are the detection's last values, not a lasting list
  of each frequency's codes. Each row's actions button
  (**Actions for 463.5521 MHz**) opens what can be done with that
  detection: **Explore here**, **Start “<saved system>”** for each saved system
  it matched, **Save as system**, **Avoid this session** (while a search runs,
  unless the frequency is a permanent avoid or the search failed), and **Avoid
  permanently**, or **Remove permanent avoid** when it already is.
  A matched system whose site is avoided is listed but cannot start: remove the
  avoid in its site list first. **Export detections…** at the end of the section
  saves the whole detection history as a CSV file (see [Exporting detections and
  avoids](#exporting-detections-and-avoids)).
- A row whose detection kept call recordings says how many under its details
  (**3 recordings**), and its actions start with **Recordings (3)**. That opens
  the detection's recordings without playing any: the player under the
  frequency is on the one selected (the newest when it opens), and the list
  below it shows each recording newest first with its start time and length,
  then the call it names (**Target 1234 · Source 5678**, **NFM**), or **No
  target or source recorded** when it names none, such as audio no call owned.
  The selected one has a heavier border and a play mark. Tap a recording to
  select and play it. **Call details** opens the selected recording's History
  details while History still holds its call; their Hold is not offered, as a
  search takes no receiver command, and Back returns to the search. When History
  does not hold the call (never logged, trimmed or cleared), Call details stays
  in its place, disabled, with **This call is not in History** under it. The list
  follows the recordings: when the selected one is deleted or removed by the
  storage limit, the one that takes its place (the next older) is selected, or
  the one before it when it was the oldest; the sheet closes when none is left,
  and a recording on storage that is away stays listed and says so. Delete
  removes only the audio; History and the detection are unchanged. With
  **Record calls** off, a row that kept no recordings ends its actions with
  **Recordings** disabled, saying that turning it on in Settings keeps the
  audio of later detections. With it on, a detection that kept none (nothing
  was decoded on it, as for an RF-only detection) offers no recordings action.
- **Search ranges · 3 enabled** is collapsed; tap it to show the ranges. While
  paused, or before a search starts, the ranges are always shown with their
  edit controls. A range list opened by hand stays open until you leave the
  screen or a new session starts.

Analog monitoring starts its quiet timing only once the monitor has read the
landed channel. A receiver that is still preparing its listening squelch (the NFM
noise squelch designs its filters the first time a channel width is used) delivers
no samples for a moment, and a closed carrier then is not a quiet channel. Only validated protocol content can add a
confirmed digital identity, access code or call details. A protocol reports its
confirmation only after decoding the content that passed the check, so one
validated frame, such as a single M17 link setup or D-STAR header, is enough for
its details to reach the detection. When a digital probe
expires, the panel says **NFM · digital unresolved** (screen readers hear
**NFM monitoring · digital identification unresolved**): the channel is monitored
as NFM because no digital identification succeeded. That fallback does not
establish that the transmission is analog.

P25 Phase 1 is confirmed by a control block whose CRC passes (TSBK, MBT or LCCH MAC)
or by a call whose identity resolves. A carrier that sends only NIDs, such as a run
of terminators, or voice whose link control never decodes, is not confirmed: the
list-decoded NID accepts several percent of random words and the voice link
control's soft Reed-Solomon decode about a fifth, so neither tells such a carrier
from noise. That carrier is monitored as NFM with the display above.

A detection's tone (the CTCSS tone or DCS code on its detail line) is what the analog
monitor heard, so only an NFM detection has one: a confirmed digital detection is
monitored digitally and shows no tone. The analog monitor decodes no digital, and a
digital carrier's repeated frames can read there as a false tone or code (measured cases
under Digital carriers in [Received tone](cli.md#received-tone-ctcss-on-the-analog-monitor)).
So a tone on an NFM detection can be false when the carrier is digital and Signal search
could not confirm it: on an analog-only range, under a profile without that protocol, a
signal too weak to confirm, or TETRA, which Signal search does not identify. The
detection shows the latest tone until one has been held for 1 s, and from then on the
last tone held that long; the ended detection keeps it. Holding 1 s does not make a tone
real: a false tone held that long becomes the detection's tone, and a real tone heard
after it for less than 1 s does not replace it.

With a receive filter on (the slot, DCC, RAN and NAC filters in
[Receive filters and service names](cli.md#receive-filters-and-service-names)), traffic the
filter rejects, or whose code or slot it is still checking, neither confirms a detection nor
keeps the search listening. A probe that heard traffic the filter rejects, and none it
admits, leaves the detection when its window ends, unless it is held: monitored as NFM, the
rejected transmission's own audio would keep the search on that channel until the maximum
visit, or for good with no maximum. The detection ends unconfirmed, like an RF-only one, and
the search goes on. A probe that heard only traffic still being checked, a probe that heard
nothing, and a held probe fall back to NFM as above. One limit: with a receive filter on, a
short transmission whose access code is proven only after its last checked frame ends
unconfirmed at the probe deadline; a continuous carrier sends more checked frames and
confirms normally. A detection's site and access code never come from traffic whose access
code the filter rejects: when a transmitter on a blocked colour code takes over the channel
after the detection was confirmed, the detection keeps the site and code it showed, and that
transmitter's broadcasts heard before the confirmation never join it. A slot filter alone
does not keep them off, since the site and the code belong to the whole channel.

P25 Phase 2 is confirmed by any Phase 2 MAC message whose CRC passes: on a voice
channel's FACCH or SACCH (a call's push-to-talk or active message, its hangtime, idle
or end, among others) as on a control channel's LCCH. A message let through with a
failed CRC by `-F` confirms nothing. A Phase 2 voice channel usually scrambles these
messages with its system's code (WACN, SYSID and NAC), so a visit must have the right
code before anything on the channel decodes.

Each visit starts from the session's own P25 site: the code, the frequency tables
(IDEN) and the channel map the decoder held when the search started, including a code
set with `-X` and an imported channel map. Whatever a visit learned about its system is
dropped when the search leaves the channel, so one system's code or frequency tables
never reach the next channel, and a code proven on one channel is never taken as proof
on another. The session instead keeps the last 16 Phase 2 codes it confirmed, from
network status broadcasts whose CRC passed and from codes recovered from a channel's
own signalling, and each digital visit tries them on its channel. A new search starts
with none; Pause and Resume keep them. A code set with `-X` in the decoder's extra
arguments wins: every visit uses it, and the search neither tries a learned code nor
recovers another, so only that system's Phase 2 channels decode.

**Explore here** and **Save as system** carry no Phase 2 scrambling code from the
search: not the code a visit used, and not one the session learned. The new session
learns the code from the air again, unless its decoder arguments set one with `-X`.
On a weak voice channel, whose code comes from its own signalling, that can wait for
a clean FACCH or SACCH burst, so a channel that decoded in the search may stay silent
for a while after Explore here or in the saved system.

A Phase 2 channel stays unresolved, and is monitored as NFM, when the probe hears no
Phase 2 MAC message that decodes with the right code: a voice channel whose code the
session has not learned and whose own signalling gives none to recover in time (code
recovery needs clean FACCH or SACCH bursts, so a weak channel may not get there), a
subscriber's inbound (uplink) frequency, whose H-CPM modulation dsd-neo does not
decode, or a code set with `-X` that is not the channel's. Encrypted voice confirms like
clear voice, from its signalling; its audio stays encrypted.

Each Auto visit starts on the 4800 symbols/s C4FM profile, whatever profile or
modulation the decoder held when the search started. A modulation lock (`-m`) in the
decoder's extra arguments is kept as given, and it confines identification to the
profiles that share its timing at the receiver's output rate: at the usual 48 kHz, a
lock such as `-mq` or `-mc` held on 4800 symbols/s never reaches P25 Phase 2, NXDN48,
dPMR or ProVoice, and `-m2` holds 6000 symbols/s, where only P25 Phase 2 can be
identified. Under a lock the probe keeps the lock's own walk and dwell, not the order
above.

A digital detection also shows the site it identified, from checked broadcasts heard
during that visit only: a P25 network status (WACN and SYSID) or RFSS status (SYSID,
RFSS and site) from a TSBK, MBT or Phase 2 MAC whose CRC passed, read as
**WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2**; a DMR Tier III identity from a C_ALOHA or a
C_SYS_PARMS/P_SYS_PARMS short LC that passed its checks, read as
**TIII Small · Net 13 · Site 4 · SYS 1183** (or, with the numbers as sent, as
**TIII Large · Capacity Max · Net 1 · Site 1 · SYS 2101** on a network recognised as
Motorola Capacity Max); or the NXDN location of a CRC-checked site,
service or control channel message once the transmission is confirmed, read as
**Site 12 · Sys 3**. A P25 RFSS status joins only the system its SYSID names, and a
network status of another system replaces what was shown. Voice link control (whose
Reed-Solomon check accepts random input), a Phase 2 broadcast let through with a failed
CRC by `-F`, a secondary control channel or adjacent site broadcast, and any value the
decoder keeps from earlier, from a hard-set system or from a cache never set it. Each
detection starts with no site, a site is shown only once the detection's protocol is
identified and matches it, and a change to another protocol clears it; the last broadcast
before the channel falls silent is kept on the ended detection, and in its history row.

A digital detection's call details -- talkgroup or destination, source, callsigns, the
timeslot and the encryption -- come together from one checked call record: the call the
decoder published after its own check (a DMR LC or PI header, a P25 link control, grant,
MAC_PTT, HDU, LDU2 or ESS, an NXDN call, a dPMR, M17, YSF or D-STAR frame that confirmed
the transmission), never pieced together from two calls. On a two-slot system the
detection shows the call that began most recently, with its own slot (**Slot 1** or **Slot
2** for DMR and P25 Phase 2), and a later call on the same detection replaces the earlier
one's details whole, on either slot. A call whose record carries a CRC failure, such as a
link control accepted as RAS under `-F`, does not set them; the IDs a protocol reports
with its confirmation stand in only until a call record names the detection, and only when
the content that carried them passed its own check, so a RAS repeater's talkgroup and
source never reach the detection. The encryption shows **ALG 84 · KID 0001** with only the
parts received over the air for that call: a P25 HDU, LDU2, ESS or MAC_PTT tuple (not one
held back as a contradiction of clear service, and not an HDU, LDU2 or ESS whose
Reed-Solomon decode needed soft erasures, so a weak signal whose service options say
encrypted shows **Encrypted**; service options from a link control that needed them
count for nothing), a DMR PI header (its key ID only where the header carries one; Kirisun's
is derived), an NXDN call's cipher and key once corroborated and carried by the VCALL
itself, and an M17 link setup's encryption type. **Encrypted** alone means the call's own
checked signalling says it is encrypted and carried no ALG ID or key ID: a DMR link
control's privacy bit, P25 service options or an ALG ID other than 80, an NXDN VCALL or
SACCH-2 cipher, a dPMR scrambled frame, an M17 encryption type. An ALG ID forced with
`--dmr-force-algid` or inferred from a loaded key or from late entry adds nothing to it,
and a forced NXDN scrambler (`-ns`) on a call whose VCALL says clear shows nothing:
decryption still uses it, but nothing on the air said the call was encrypted. A clear call
shows nothing. A DMR PI header decoded while no call is active (the voice
header was lost) is held for the call the next checked link control opens on that slot
within two seconds, and forgotten at that transmission's end, a carrier change or a new
visit. A call that fades and is reacquired keeps what it had.

When a terminator is missed and the next transmission's link control replaces the call
(DMR, or conventional P25 Phase 1), the decoder keeps decrypting with what it holds,
exactly as before, but the new call -- its detection, its history row and the Monitor --
shows none of the previous call's ALG ID and key ID: only what the new call's own
signalling supplies after its link control (a DMR PI or late-entry identifier, or a P25
LDU2, ESS or MAC_PTT), or a forced or key-inferred algorithm the decoder chose for it,
even when that is the same value the previous call showed. Two limits follow. A new call
whose own PI, HDU, LDU2 or DMR late-entry identifier decoded before its link control (its
header lost, or its first embedded link control failing its check) shows no ALG ID or key
ID until a later one of its own decodes, and that earlier signalling, decoded while the
previous call was still active, is attributed to the previous call. A conventional P25
Phase 1 call's HDU always precedes its first LDU1, so after a missed TDU its HDU is the
previous call's. History keeps the call details, slot and encryption the detection last
showed.

## Controls, history and saved systems

- **Hold** overrides quiet release and the maximum visit. A held probe still falls
  back when its identification window expires, even one that heard only traffic a
  receive filter rejects.
- **Skip** suppresses this frequency for the current pass.
- The search keeps two avoid lists, and a frequency is in one of them at most.
  **Avoid this session** lasts until the session ends. Session avoids are listed
  under **Avoided this session**, and **Stop avoiding** lifts one for the following
  measurements; a frequency already visited in the current pass returns with the next
  pass. A history row's avoid works the same way, and leaves the frequency at once if
  the search is visiting it. **Avoid permanently** saves the frequency for this and
  every later search and moves it to **Permanent avoids**: a running search leaves it
  at once if it is visiting it, and drops it from **Avoided this session** if it was
  there. A row's permanent avoid, and **Remove avoid**, change the saved list whether a
  search runs or not; a running search follows within a moment. Removing a permanent
  avoid lifts it from the running search too. **Stop avoiding** only lifts a session
  avoid, so it never brings back a permanent one, and a row whose frequency is already
  a permanent avoid does not offer **Avoid this session**.
- Each list holds 1,024 frequencies, and permanent avoids take no room in the
  session's list. A full list is said on the screen: **The avoid list is full** for
  this session's list and **The permanent avoid list is full** for the permanent
  one, and nothing changes, not even a skip. A full-list message clears when the
  search tunes to its next detection. A search that failed keeps its own error,
  shows no session list and offers no **Avoid this session**: its session avoids
  ended with it, while permanent avoids stay listed and can be removed. An avoid
  for a detection the search has already left (the panel was a moment old)
  changes neither list and shows nothing: the panel already shows what the
  search is doing now.
- Taps that change the avoids are taken in order, each against both lists as they
  stand when the search runs it (within a moment of the tap), and the screen reads
  both lists from the same place the search does, so what it lists is what the
  search avoids.
- **Pause** ends any call in progress on the channel and permits range edits. A search
  that fails (its input stalls, for example) ends that call too.
  **Resume** applies them and restarts the pass under the same session, retaining
  session avoids; a start after **Stop** is always a new session. **Stop** ends the radio session and leaves Signal
  search open on the same receiver, ready to start again. The session's settings
  return to those it had before the search, frequency included, so the configuration
  it autosaves never keeps a frequency the search visited.
- A start waiting for Android USB permission belongs to the search: leaving Signal
  search, or a later **Start search** it refuses, cancels it, so a permission
  granted afterwards starts nothing.
- **Explore here** and **Start “<saved system>”** leave the search for another
  session. The search's own start is cancelled first (a USB permission granted
  afterwards cannot turn the new session into a search), then the running
  search stops. Until it has, the screen stays and reads **Stopping the
  search…**, with its controls set aside; a stray tap does not drop the
  handoff. A slow stop keeps that state as long as the engine's own restore
  timeouts allow. **Back** cancels the handoff: the search screen and its
  controls come back while the session finishes stopping, nothing new starts,
  and a second Back works as usual. Once the decoder is idle the screen closes
  and the new session starts. If the search fails to stop, nothing starts and
  Home shows the failure.
  Explore here tunes the detection's frequency with its receive mode, gain and
  receiver settings, on the receiver attached now, whether the search is
  running or stopped. An rtl_tcp server stays. When the attached receiver is
  another kind, only the settings that kind takes go with it: a detection heard
  on an RTL dongle starts on an Airspy without the dongle's PPM, bias tee and
  tuner gain, and an Airspy's own settings (its serial among them) never go to a
  dongle. A P25 Phase 2 channel's scrambling code does not go with it (see
  [Ranges and reception](#ranges-and-reception)). Explore here does not change where
  Explore resumes from Home, even through a USB permission request. A saved
  system starts as saved (keys, decoder and trunking). A system whose site is
  avoided is refused with a message on the search screen, and nothing stops.
  Like a refused **Start search**, the message stays until the next start,
  Resume or leaving the screen, and what the search reports meanwhile (a full
  avoid list, for example) shows below it. If
  it became avoided while the search stopped, Home shows the refusal: **Retry**
  asks again and **Edit** opens that system. A recreated Activity restarts the app
  under Qt 6.11.2, which ends the search and the handoff with it (`android/README.md`
  "Known limits").
- **Retry** on a failed Signal search start opens Signal search again and starts
  the search on the same receiver; **Edit** opens Signal search. Both take the
  receiver attached now, as opening Signal search from Home does: a USB dongle
  search that failed, with only an Airspy plugged in, retries on the Airspy. An
  rtl_tcp server stays. **Retry** on a failed Explore here starts it again on
  the receiver attached now (Airspy settings only go with an Airspy); **Edit**
  opens Explore setup on that attempt's receiver and frequency, not on Explore's
  remembered answer. Starting from there keeps the detection's receive mode and
  gain and, like Explore here, does not change where Explore resumes from Home.
- Android Back follows the existing background-listening preference. A recreated
  Activity restarts the app under Qt 6.11.2 and ends the session (`android/README.md`
  "Known limits").

Detection history is separate from call history: an RF detection need not decode
anything. The newest 1,000 detections are retained, with one row updated as details
arrive. Ended, confirmed and RF-only detections remain distinguishable. The private
versioned journal is compacted atomically; write failures are reported while
available records remain in memory. Each record keeps the detection's checked
site, slot and encryption, in a version 2 journal (`signal-search-history.v2`)
beside the earlier releases' `signal-search-history.v1`. The first time no
version 2 journal exists, the earlier journal's detections load without those
details and the next save writes them to the version 2 journal (both versions
keep the access code and tone, so those detections still show theirs); from then
on only the version 2 journal is read. The earlier journal is never changed, so
an earlier release still shows its own history, and detections it records later
do not reach this one. A detection republished with nothing new is not written
again. Detection history is not part of a backup. History and permanent avoids
are saved separately, and a failed one stays reported and is retried by later
saves until it is written.

### Exporting detections and avoids

**Export detections…** (the last button of Recent detections, after **Show more**,
shown while there are detections) and **Export avoids…** (after the Permanent
avoids list, shown while either avoid list has entries) each save a CSV file for a
spreadsheet or a script. Each opens the save picker with a suggested name
(`Signal search detections 2026-11-08.csv`, `Signal search avoids 2026-11-08.csv`),
and one export runs at a time. The result is shown under the button: **Exported 214
detections.**, **Exported 37 avoids.**, **Exported 214 detections to Download/DSD-neo
as** the file's name when the device has no save picker (see `android/README.md`
"Choosing files"), or **The export couldn't be written. Try another location.** If
the avoid lists change while the export is being prepared it says **The avoid lists
were changing. Try again.** Closing the picker without choosing a place saves
nothing and says nothing.

The detections file holds every detection kept in the history (the newest 1,000),
not only the rows shown, newest first. The avoids file holds the permanent avoids,
then the session avoids of the search on screen; a frequency in both lists is
written once, as `permanent`. The files are a copy at the moment you press the
button. They leave out the receiver settings (the rtl_tcp address, PPM, bias tee,
gain and Airspy settings), since those can identify a device or a network. The
columns are listed in [CSV formats](csv-formats.md#files-dsd-neo-writes-not-read-back),
with how to open the files in a spreadsheet. Exports are not part of a backup and
are not read back by DSD-neo.

Files that can't be read. When the version 2 journal or the permanent avoid
list (`signal-search-history.v1.avoids.json`) can't be read whole (it can't be
opened, a read fails, a record is damaged or cut short, or the avoid list isn't
a valid list), the app says so and shows what it read before the problem: the
detections before it, or no permanent avoids. The Signal search screen says it
in its message line ("Signal search history couldn't be read completely, so
some entries may be missing. The file won't be overwritten."), and a card on
Home lists every file the app couldn't read this run until you dismiss it; a
dismissed file shows again only if what is known about it changes. The file
itself is not changed. The first time that file has to be saved, it is renamed
to `<name>.unreadable-1` (or the next free number, up to 99) and a new file is
written beside it, so nothing in it is lost; the screen and the card then name
the kept file ("The file was kept as signal-search-history.v2.unreadable-1.").
A save that fails is said on the screen too, and is retried with the next
change.
A run that saves nothing leaves the file where it is, so a problem that goes
away (a permission fixed, a storage error) reads normally at the next start. A
link at that name is renamed itself, never what it points to. A folder, FIFO or
other special file there is never moved or replaced: saving it stays reported
until it is gone. While a kept version 2 journal exists and no
new one does, the earlier releases' journal is not read again, so an
interrupted save cannot bring old detections back. The earlier releases'
journal is never moved: if it can't be read, the app says so and leaves it as
it is. Leftover temporary files from an interrupted save are removed at start.
Limit: if the app stops between keeping the avoid list and writing the new
one, the next start shows no permanent avoids; the kept file still holds them.
Kept files are never part of a backup or touched by a restore. While the
permanent avoid list can't be read, Export backup stops and says so; once a
change to the permanent avoids has saved the list again, a backup holds only
the avoids the app shows.

A recording belongs to a detection through the call recording journal: each
recording published from audio received on a detection's visit names it there
(the `search` key; see `docs/code_map.md`, "Recording journal"). The detection
journal is unchanged. Limits: a recording made before this release, or one whose
journal an older release compacted, names no detection and is listed under none;
detection history keeps 1,000 detections and the recordings follow their own
storage limit, so a recording can outlive its detection's row (it stays in Call
recordings), and a detection its recordings (the row then shows none); History
keeps 1,000 calls, so an older recording can still play with no Call details.
Orderly stop flushes the journal. Ranges and permanent avoids are also stored privately.

Saved-system labels come from a cached index of saved frequencies, referenced
channel maps and scan lists. Exact matches win; otherwise the nearest frequency
must be within half the smaller applicable spacing or channel width. Equally near
matches are all shown. For example, 463.5521 MHz can match a saved 463.5500 MHz
channel with 12.5 kHz spacing. A label suggests a saved frequency, not proof of
system identity, and never retunes the radio.

**Save as system** opens the normal save wizard from an immutable detection
snapshot. Frequency, receive mode, source, bandwidth and gain are prefilled, and
so are the receiver settings the detection was made with: PPM correction and bias
tee on an RTL-SDR or rtl_tcp source, serial, sample rate, gain and bias tee on an
Airspy. They are saved as explicit values, so a later change to the app-wide
defaults does not change the saved system; a bias tee that was off stays off.
A saved system holds an RTL-SDR or rtl_tcp gain in whole dB, where 0 means the
tuner's automatic gain, so a detection made on automatic gain saves automatic
gain, and a manual gain saves as the nearest whole dB from 1 to 49: a detection
at manual 0 or 0.9 dB saves as manual 1 dB, not automatic, and the tuner then
runs the nearest step it supports. An Airspy keeps its own gain settings.
Confirmed NXDN48 and NXDN96 detections preserve their respective decoder rates.
Unresolved identification stays explicit. The user must choose the normal Save
action; later detections do not change the open draft. A P25 Phase 2 detection's
scrambling code is not saved; the saved system finds it on the air (see
[Ranges and reception](#ranges-and-reception)).

## Scope and implementation

Search owns one live tuner exclusively. The shared backend supports tunable
RTL-SDR, Airspy, rtl_tcp and Soapy inputs; the Qt source chooser offers the existing
Explore sources. PCM, rigctl-only inputs and production I/Q replay cannot search.
Conventional scanning and trunk following must stop first. CLI controls, public
TCP API commands and simultaneous analog/digital reception are not provided.

A stalled radio input (an rtl_tcp stall, for example) does not hold a visit or
end the session: after 100 ms without samples the decoder leaves the frame it was
reading and returns to the coordinator, so controls and visit deadlines still take
effect, and it reads the input again on its next pass.

The frame the decoder leaves, there or when a visit ends or a command arrives
mid-frame, is dropped as on a sync loss: nothing that depends on what it read after
the cut plays, is published or recorded, or counts as evidence for the detection.

A search started with the radio session waits up to 10 seconds for the receiver's
first samples. Samples from an earlier radio session do not count: the measurement
tap forgets a stream when it stops, so a long pause between sessions is not taken
for a stalled input.

Implementation is divided into four reviewable parts:

1. Runtime records and range planner, plus the independent capture-rate FFT
   detector (`runtime/signal_search`, `io/signal_measure`).
2. Decoder-thread coordinator, tagged controller completion and runtime hooks
   (`engine/signal_search`).
3. Native app-control journal, cached frequency index and Qt workflow
   (`app_control/signal_search`, Qt `SignalSearchModel` and search QML screens).
4. Detector/coordinator/persistence/QML regressions, manual hardware benchmark and
   the evidence below. See also [the module map](code_map.md).

The detector integrates Hann-windowed linear FFT power with window-energy
normalization. It requires two complete measurements of at least 10 ms each,
accumulated by sample count across arbitrary input blocks, and at least eight FFT
bins across the smaller channel width/spacing; each channel must pass its threshold
in both. Local noise comes from sidebands 1.5 to 3 channel widths out on either
side, in four blocks each: the mean of the blocks within 6 dB of the third lowest.
A block a transmitter raises by more than that is left out, up to five of the eight;
one it raises by less still counts, so with up to five raised blocks the estimate
reads at most 2.875 times the third lowest block: over a flat floor about 4.6 dB high
(five blocks 4.8 dB above three quiet ones read 3.5 dB high), more where the
uncontaminated blocks themselves differ. Clipping and capture discontinuities invalidate the measurement.
More than 1% of the samples clipped fails it. A sample counts as clipped when either
of its two parts reaches the receiver's clip level. On an RTL-SDR or an rtl_tcp server
that is the dongle's lowest or highest value, and the values one step inside them do
not count. With `DSD_NEO_COMBINE_ROT=0`, or when replaying a cu8 capture recorded with
that older conversion, a capture shifted by a quarter of its rate uses a conversion that
puts the value one step above the lowest as far from the centre as the highest, so after
the quarter-rate shift that value counts too. The shift swaps and mirrors the dongle's
values, so which raw value one step from a limit counts changes from sample to sample;
the lowest and highest always count. Other receivers count 0.999 of full scale. A
failed measurement keeps the request it belonged to and says why it failed: clipped
samples, dropped samples, a changed capture (retune, rate, gain or offset), a sample
that is not a number, or a transform that could not run.
Allocations, FFT size, candidates and total channels are bounded. An unsupported plan
reports "Unsupported FFT measurement plan", and a detector that cannot be allocated
reports "Not enough memory for a measurement".

A span whose measurements keep clipping does not stop the search. When they keep
clipping for one second, or one clipped a second ago and no answer has come since,
the search skips the span and goes on to the next one. It also skips the span when
five seconds have passed since the span's first measurement started, none has
finished, and at least one of them clipped. A skipped span is
measured again on the next pass. Nothing is ever detected from a clipped measurement.
If the only span clips, or every span does, the search keeps going round, and the
pass number keeps climbing. A measurement that fails for another reason, such as
dropped samples or a changed capture, is simply tried again. If none finishes within
five seconds and none of them clipped, the search fails with "No uninterrupted
samples for a measurement". A measurement that finished in time is always used, even
when the search reads it late.

When a span is skipped, the search warns that the receiver is overloaded and names
the range of the first span it skipped in that pass. A later skip in the same pass
does not change the name. The warning stays while spans keep being skipped, and for
the whole of the next pass, so there is time to read it. It clears at the end of a
pass that skipped nothing. It also clears at Resume, including when Resume applies
edited ranges, so a lower gain shows its effect at once; if the span still clips, the
warning comes back a second later. To fix it, Pause, lower the range's receiver gain,
then Resume. The warning is not a refusal or an error: visits, actions and new
detections leave it in place.

The receiver takes in a band at least two to three times wider than the span it
searches at a time, and clipping anywhere in that band spoils every measurement taken
from it. So one strong transmitter can overload the receiver even when it is outside
the range being searched, and the spans near it, a few in a row, are all skipped. A
lower gain, or a filter or attenuator in front of the receiver, keeps it from
clipping.

An Airspy's overload cannot be seen in the I/Q samples libairspy delivers. libairspy
turns the Airspy's real samples into I/Q itself, and that conversion halves their
amplitude and filters one of the two parts, so samples at the Airspy's limits arrive
near half scale and never reach the clip level. Neither Signal search nor the input
level meter shows Airspy clipping. If strong signals are near, lower the Airspy gain.

A strong transmitter's skirt and the window's leakage reach neighbouring channels'
windows, particularly with the default 16 kHz width on a 12.5 kHz grid or on any
grid finer than the width, and would pass their thresholds on their own. So a
transmitter belongs to the grid channel nearest its centre: half a step either side
of the channel's exact position on the range's rational grid, so neighbouring
channels share one boundary however the grid frequencies were rounded. This is
decided once per measurement, on both windows' power together, so a transmitter is
assigned the same way for the whole measurement. It must also hold its own power in
each window (within 10 dB of the other over the bins it occupies), so energy one
window only owes to another transmitter is not sustained. A transmitter whose
modulation differs between the windows (a slow tone half a cycle apart) is still
sustained, as long as the bins it occupies hold its power in both.

Transmitters are located in the spectrum itself, not only among the requested
channels, so one past a range or span boundary counts. Each channel's window climbs
to the transmitter dominating it. That transmitter's segment of spectrum runs out
from its peak to the valley before another transmitter: a dip clearly below the
peak (by the rise threshold below) followed by an equally clear rise, measured on an
envelope that bridges the gaps between an FM transmitter's sideband lines for tones
up to 3 kHz. Across such a valley the segment beyond is taken in when one
transmitter explains the pair better than two (the plateau between a wide FM
transmitter's horns, whose halves are each lopsided), provided the valley stays
within 20 dB of the lower side and the two peaks are within two channel widths. The
centre is the point about which most of the segment's power overlaps its own
mirror image: an internal notch overlaps its mirror notch, a short-term lopsided
spectrum still has a symmetric extent, and a neighbour in the segment overlaps
only the far skirt. A centre with less than 1% of the peak power splits the segment
when two transmitters, each symmetric about its own centre, explain it at least as
well as one (two carriers the envelope bridged), while a carrier its FM modulation
weakens keeps the transmitter whole. With 250 Hz or similarly fine steps the
analysis aggregates bins and refines centres on the FFT bins of the transmitter's
whole body within its channel width, every spectral line taking part, so a tone FM
transmitter whose first sidebands outweigh its carrier is centred on its carrier.

Beside each such transmitter, a separate one is found in two ways. Walking out
from it, the envelope must rise again after the skirt has fallen at least 6 dB, by
a threshold (at least 4.5 dB) that grows with the statistical spread of the averaged
spectrum, and climb above the last ridge before the valley and every ridge passed
since (the transmitter's own outer sideband lines decay one after another). Or,
where its other side is a clean skirt and its spectrum mostly overlaps its mirror
image, the energy beside it stands 10 dB above that side mirrored about its centre,
peaks away from the transmitter's own edge, and makes the envelope itself rise from
a dip (a lopsided transmitter's own skirt only falls, however far above its mirror);
that looks only as far as the first other transmitter, beyond which the mirror says
nothing. Within the transmitter's own
channel width a rise is usually its own spectrum (an FM transmitter's sidebands): it
counts only when the transmitter mostly overlaps its mirror image, the rise stands
10 dB above its own mirror image (sideband lines come in mirrored pairs) and its
valley falls 20 dB below it (a lopsided edge of the transmitter's own modulation
leaves the valley filled). A rise that does not count stays the transmitter's own
until it falls back, even where its envelope reaches past the channel width. Each
separate transmitter is located once: as a transmitter of its own when its own
segment's tallest bin lies beyond the valley; otherwise at the centre of what
remains of it after the mirror (or the valley's level), from where it rose out
through the ripples of its sideband lines to where it falls to a tenth. It is
reported by the channel that owns that centre.

These rules come from measured cases, not a guarantee of detection or uniqueness.
The regressions and sweeps below report every transmitter once at its nearest
channel, including off-grid, wide and noise-like FM, tone FM with carrier nulls,
horns and wide sidebands, CW, AM and 4FSK, on 2.5 to 25 kHz grids, rotation-free
rates and 250 Hz grids, and they report neighbours down to 20 dB below beside each
other. Known limits:

- Two transmitters whose spectra merge or overlap can count as one, at the channel
  of their combined centre (for example a neighbour 3 dB down, 12.5 kHz beside
  5 kHz deviation, at 1.2 to 1.5 kHz bins without rotation, or 5 and 2.5 kHz
  deviation 10 kHz apart), and so can two such transmitters that each fill only one
  window.
- A weak neighbour hidden under a strong transmitter's skirt or splatter, raising
  neither a valley rise nor the envelope (for example 26 dB or more below noise-like
  voice FM, 36 dB below 5 kHz deviation, or 20 dB below a heavily modulated AM
  transmitter), is not reported until that transmitter ends.
- A transmitter whose centre lies outside every channel's window (a window narrower
  than the step, far off-grid) is reported at most once, at the nearest channel its
  sidebands reach, or not at all.
- Within a few hundred hertz of an ownership boundary a short-term lopsided spectrum
  can move a transmitter to the neighbouring channel.
- Two weaker transmitters placed symmetrically within a stronger one's channel width
  mirror each other and are taken for its own spectrum.
- A transmitter whose modulation is lopsided has no symmetric extent about its
  carrier; it is centred where its spectrum is most symmetric, which on 250 Hz
  grids can be a channel or more from its carrier.
- A transmitter whose outer modulation lines grow again after a dip (clipped tone FM)
  can have such a line, beyond its channel width, reported as a neighbour: its pair on
  the other side mirrors it as exactly as a weaker transmitter on either side of a
  strong one mirrors the other, and those are reported.
- A transmitter that sits on a different frequency in each window is not sustained
  for that measurement: a clipped slow tone (45 Hz clipped to 700 Hz on a 2.5 kHz grid)
  whose windows fall on its two dwells matches, window for window, two carriers that
  each fill one window, which must not be sustained. That transmitter is missed at
  about one tone phase in six and found at the others, so a later pass measures it.
- A transmitter wider than the channel width whose slow modulation puts each window
  on a different side of it can be reported a channel from where it sits.
- A weaker transmitter beside a stronger one is located from what rises out of the
  valley between them. In crowded spectrum on 250 or 625 Hz grids that places it
  within its own width, which spans many channels, so it can be reported several
  channels from its carrier; and one wider than the channel width (for example 5 kHz
  deviation with 8 kHz windows) can be reported at its deviation horns, a channel
  either side of its own on a 5 kHz grid.
- A window that only climbs toward a transmitter more than six reaches (twelve
  channel widths) away is that transmitter's skirt and is not reported.
- On a strongly coloured noise floor with 25 kHz widths, local noise can read about
  1 dB below a plain mean; noise alone produced no detections at 3 dB sensitivity in
  the sweeps below.

The coverage planner conservatively uses one usable side of hardware DC per
capture, leaving noise sidebands and edge guards. It explicitly accounts for
capture frequency, rate and Fs/4 ingest rotation. The first remaining channel
anchors each new span, so DC/edge exclusions do not leave gaps. This costs more
captures than an ideal full-bandwidth sweep: the 10 MHz example below uses 22
spans, rather than the planning estimate of 9–10 before exclusions.

## Verification and measured limits

Run the focused native and QML regressions in a Qt-enabled build:

```sh
ctest --preset dev-debug -R 'SIGNAL_(MEASURE|SEARCH|TAP)|UI_QT_QML_SESSION_TOOLS' --output-on-failure
ctest --preset dev-debug -L iq-decode --output-on-failure
```

The probe's order and dwell are measured by `tests/engine/search_schedule_replay.c`
(`DECODE_IQ_SEARCH_SCHEDULE_*`): Signal search refuses an I/Q replay, so it replays a
fixture through the whole decoder with the search's probe forced on from a 4800/4 C4FM
start, landing 0, 350 and 700 ms into the capture, and requires the protocol (on its
profile) within the three-second default. It also takes `--ordinary` (the rotation
without the probe order, for comparison) and `--hold-ms` (keep decoding after the
identification).

The detector tests cover calibrated power, noise, carriers, modulated signals,
bursts, off-grid signals, nearby transmitters, adjacent-channel leakage from a
single strong NFM transmitter (narrow, wide, off-grid, at a range boundary and on a
6.25 kHz grid), weaker separate neighbours (including two either side of a strong
transmitter), clipping (more than 1% of samples at the clip level in either component
fails the measurement, fewer does not, whether the first window or only both together
pass 1%), each RTL conversion's clip level (both rails and the codes next to them,
through the production widening, and a clean tone just under each level measured),
each failure's reason and request ids (from the detector, from the shared tap when a
waiting report is invalidated, the first reason kept, and from a demod thread racing
the requests), non-finite samples mid-frame,
gain/rate/drop boundaries, DC, rotation, edges and fragmented input. They also
cover a neighbour 6 dB down beside 5 kHz deviation on both sides at four bin
alignments; single transmitters on 5 and 6.25 kHz grids at five FFT resolutions
and alignments (tone FM with carrier nulls, horns and wide sidebands, noise-like
FM); an off-grid carrier and low-frame noise-like FM near ownership boundaries;
offset sweeps across boundaries on 5, 6.25 and 12.5 kHz grids and a rational
25000/3 grid, with no gaps or duplicates; a 250 Hz grid of 512 channels, including
a tone FM transmitter whose first sidebands outweigh its carrier; a transmitter's
own decaying sideband lines past its channel width; windows that wrap FFT bin 0,
rotation-free capture and channels at the DC and edge guards; transmitters 25 and
37.5 kHz apart (local noise), equal ones 5 and 10 kHz apart on fine grids, a carrier
6 dB down 5 kHz beside a stronger one within its window, a weak neighbour on an
ownership boundary, transmitters present in only one window each against one whose
45 or 50 Hz tone puts its windows on either side of its carrier, a clipped 45 Hz tone
measured across its changes of side against two carriers at its dwells that each fill
one window, a carrier in one window beside a continuous transmitter, a transmitter with
lopsided modulation whose skirt stands far above its mirror, a two-tone wide FM
transmitter whose lobes stand either side of a valley, and noise alone at 3 dB
sensitivity. The shared tap is tested for
reports delivered only to their own request, cancels and capture changes, with a
feeding thread racing requests, polls and cancels. An I/Q replay stream checks that
stopping a stream leaves the tap with no capture or latest-sample time, and that the
next stream publishes its own. A coordinator regression runs
the real detector and tap on synthetic capture I/Q over three passes at six capture
alignments and checks that only the transmitter's channel is ever visited; it also
starts a search on a tap a stopped stream has emptied, which waits for the first
block and then plans its first span from it. On the same tap, an NFM visit serviced
by the main loop and a digitally confirmed visit serviced only by the in-frame yield
check each follow 12 dB and 6 dB level changes in the live strength within three
intervals, withdraw a level the tap stops refreshing (a producer drop with every
block, or clipped samples) within three intervals and a retry, show a fresh level
once measurements succeed again, and publish no level for an ended visit; a clipped
level on a main-loop visit is withdrawn the same way. A transmitter that overloads the
receiver has its span skipped on every pass with the overload warning and the search
never fails; once its level is back in range it is visited, and the first pass that
skips nothing clears the warning. Coordinator
tests exercise stale reports and controls, deferred/failed tunes, fresh-sample
waits, short probes, fallback, hold during tuning, quiet release, an analog
monitor that has not judged the landed channel yet (and its deadline), visit caps,
live strength, span measurements that clip (a span skipped once clipping lasts a
second, a clip followed by no answer included, or at five seconds when the first clip
came late or clips were mixed with drops; nothing skipped under a second; a single
span and every span clipping keep searching with the pass climbing), failures that
are not clipping, no answer at all and clips of an earlier request (the search fails
at five seconds), a finished report read late (used, not failed or skipped), and the
overload warning (named at the first skip of a pass, kept through visits, actions and
Pause, cleared by a pass that skips nothing, Resume and a restart with edited
ranges), the two avoid lists (1,024 permanent and 1,024 session avoids at once,
a session avoid of a permanent frequency adding nothing, Stop avoiding keeping a
permanent avoid, a permanent avoid added or removed mid-search taking effect at the
next tick, even on a held visit, and session avoids refused after a failure),
pause/restart and scoped restoration, that a
Pause or a failure (a stalled or lost input during a held visit) runs the carrier boundary
once, so a call it interrupts ends there, and that a session ending under a search leaves
the frequency it started from.
Stalled-input regressions check that the live read gives up after 100 ms only while
a search owns the tuner, and that the FSK, monitor and CQPSK symbol readers alike
take that as an interruption rather than the end of the session, then read the
rest of the frame as erasures without waiting again. Cut-frame regressions feed every
digital decoder (D-STAR, ProVoice, P25 Phase 1 and 2, DMR, NXDN, dPMR, YSF, X2-TDMA,
EDACS, M17) a frame interrupted part-way and check that nothing read across the cut
plays, publishes, captures or reports evidence.
Persistence and Qt tests cover bounded history, duplicate suppression, failed
writes/retry (history and avoids each stay owed until their own write succeeds,
including when storage only recovers by close), files that can't be read (each
kept byte for byte by the first save and never touched by a run that saves
nothing: damaged, torn, unopenable and read-error journals, invalid and
NUL-holding avoid lists, a failure after the rename and its retry, a kept
journal or an unknown name blocking the earlier journal, the earlier journal
reported and left in place, folders and FIFOs never moved, leftover temporary
files), recreation, fractional steps,
ambiguous matches, nested channel maps, immutable saves (including each source's own
receiver settings and the detection's gain mode reaching the saved system), phone
text scaling and navigation ownership. They also check that connection and USB-access
failures reported after Start or Resume still reach the search screen beside its own
refusals, that status-only refreshes leave history untouched (it is copied only when its store
revision moves), session avoids (read from the search's own list, shown only for the running
search and never after it failed or stopped), permanent avoids changed by a row
action without the decoder (including at the Stop edge) and loaded once when the
file or a backup repeats one, the aggregate 100,000-channel check before a start
is submitted, and that a start the engine rejects with no search left owns nothing
and is retired when the decoder session ends, so a recreated frontend never restores
the search screen over a later ordinary session (a path Qt 6.11.2 never takes; see
`android/README.md` "Known limits"). A rejected restart of a paused
search keeps that search's phase, session and ownership; only its message changes.
A Resume keeps the paused search's session, while a start after Stop takes a new one
even before the screen's status tick has seen the Stop.
Leaving the search (its Back, the system Back) or a Start it refuses abandons a
start still waiting for USB permission, so the grant starts no session, while a
start Home left waiting survives, and when it is granted with the search screen open
that screen closes rather than present the session as a search; Stop and an arm the screen refuses (missing or
invalid ranges) leave nothing armed. A start the decoder's command queue turned away
because the host reported Running before the decoder took commands (Android raises it
just ahead of opening command admission) stays armed and goes in once, when
initialization is reported or at a later status tick, whichever comes first; a session
that stops, fails or ends before then takes the arm with it, so the next ordinary
session starts no search. A screen reopened over a running search after recreation
(a path Qt 6.11.2 never takes; see `android/README.md` "Known limits") takes its
receiver (USB, Airspy or rtl_tcp server) from the session's options snapshot, whether
that snapshot arrives before or after the screen, offers that receiver's gain
controls in a paused range edit, and keeps it through Stop and a restart.
The overload warning reaches the screen as its own fields, never in the message; the
screen words it per phase and radio as the message's last line, drops it once the
search fails or stops, keeps the Now panel still as it comes and goes (also at 360 px
with large text), and names it at a screen reader's pace: at once as it appears or
clears, a new range name at most every period, and nothing while a sheet covers it.
The app-control provenance regression also checks that a later false sync or
unvalidated code change cannot enrich an already confirmed detection during a
periodic refresh, and that a new detection starts without the preceding details.

Local validation on 2026-10-12 passed all 1,003 tests in the Qt-enabled Debug build
(including 196 `iq-decode` cases), all 748 tests in the radio-off build, and eight
focused ASan/UBSan tests covering the detector, coordinator, history, provenance,
retuning, squelch and trunk scanning. The broad quality preflight passed, including
architecture checks, strict static analysis, a full Clang analyzer rebuild,
repository/dependency checks and fuzz smoke tests.

The manual benchmark opens the explicitly supplied hardware and is intentionally
not registered as an automatic CTest:

```sh
cmake --build --preset dev-debug --target dsd-neo_test_signal_search_bench
DSD_SEARCH_BENCH_MODE=search build/dev-debug/tests/dsd-neo_test_signal_search_bench \
  --frontend none -fA -i rtl:0:450M:0:0:48:0:1 -o null
```

Repeat with `DSD_SEARCH_BENCH_MODE=sweep` and `serial`. All three traverse
450–460 MHz. Serial reads at least 40 ms of fresh NFM samples per 12.5 kHz channel;
sweep uses the existing 3.5-second dwell per capture span without Qt rendering.
`hit` stops at the first RF detection (or reports an empty pass). This harness is
a timing reference, not a production search CLI.

For a reproducible RF detection through the complete live network-input path,
without physical hardware, run the NumPy-based loopback source:

```sh
python3 tools/bench_signal_search_tcp.py build/dev-debug/tests/dsd-neo_test_signal_search_bench
```

It emits paced NFM-modulated I/Q at a fixed RF frequency, responds to rtl_tcp
frequency/rate commands and checks the first detected frequency and the 100 ms RF
publication target. It does not enable searching in production I/Q replay.

Local Linux Debug measurements on 2026-10-12, RTL-SDR USB device 0 at 1.536 Msps,
hardware gain auto, 801 channels, 16 kHz channel width and 15 dB sensitivity:

| Mode | Traversal | Detail |
| --- | ---: | --- |
| Signal search, empty pass | 4.944 s | 22 spans; 4.429 s acquisition, 0.515 s measurement, no identification/listening visits |
| Sweep reference | 25.610 s | 7 capture spans at 3.5 s dwell, no rendering |
| Serial reference | 179.335 s | 801 tunes and 40 ms fresh-sample windows |

This run was 5.18× faster than the headless sweep reference and 36.27× faster than
the serial reference. Including startup/shutdown, this search process used 0.104 s
user CPU and 0.046 s system CPU over 5.899 s, with 48,316 KiB peak RSS. A repeat
after finalizing publication timing took 4.939 s (4.432 s acquisition, 0.507 s
measurement); its maximum report latency from arrival at the measurement tap of
the completing input block to native publication was 2.007 ms. Acquisition
includes tuner settling and buffered transport; the detector cannot inspect
samples that have not arrived.

The paced loopback rtl_tcp run detected the synthetic 163.0125 MHz NFM transmitter
in 155 ms: 129 ms acquisition and 25 ms measurement, with no identification or
listening visit included. RF publication took 1.058 ms after the completing input
block arrived, below the 100 ms target. This validates the live network-input path
with a local synthetic source; it does not measure remote network latency.

The sweep reference does not measure the actual Qt renderer. These are local
observations, not hardware-independent performance guarantees. Qt polls native
status every 250 ms; rendered UI latency, rtl_tcp transport delay, Airspy/Soapy
hardware timings and Android screen-off behavior need device measurements. No
Android device was attached for this run.

Analog receive-path A/B against `a020cbdee`, using `tools/replay_ab.sh` with the
repository's NFM/AM real-speech excerpts, produced identical metrics in 12 repeats
per build: NFM in-band ratio 17.73 dB, audible duration 3940 ms, RMS −25.97 dBFS;
AM 31.90 dB, 6160 ms, −23.93 dBFS. These excerpts have no event realization offsets;
the repeats are deterministic checks of two fixtures, not independent RF trials.
Longer independent real RF captures remain useful acceptance evidence.
