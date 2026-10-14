<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> -->

# Signal search

Choose **Signal search** beside **Explore** in the Qt/Android Home screen. Select
USB, Airspy or RTL-TCP, add or edit frequency ranges, and choose **Start search**.
Search measures RF power across the radio's capture bandwidth without drawing a
spectrum. It visits strong candidates to identify and monitor them, then continues
through the remaining candidates and ranges.

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
| Probe duration | 2 seconds after fresh tuned samples arrive; adjustable through 10 seconds |
| Quiet time | Resume after 500 ms without relevant carrier or validated digital activity; an analog visit counts it from the monitor's first verdict on the landed channel (at most 3 seconds after landing) |
| Maximum visit | 30 seconds; adjustable through 600 seconds, or 0 to disable |
| Gain | Inherit the session, or use the backend's range controls; Airspy has its own gain controls |

RF detection sensitivity and listening squelch are separate. Changing listening
squelch does not change which channels pass the RF detector. An Auto or Noise
listening squelch takes a margin of 3 to 30 dB, the range the receiver applies;
a range with a margin outside it is refused rather than saved, and a range saved
earlier with 0 to 2 dB loads with the 3 dB the receiver applied. AM and analog-only
NFM ranges open their monitor directly. Digitally enabled NFM ranges first run
the selected profile. Auto normally takes about six seconds to rotate its symbol
profiles, so the two-second default cannot identify every supported signal.

A detection displays frequency and measured strength immediately, while its
listening tune is still pending. While listening, the screen also shows the
channel's current RF level (**Now … dBFS**), measured on the same detector scale
roughly four times a second; history keeps the detection-time level. Live level
needs the receiver's Fs/4 offset tuning, which keeps the landed channel away from
hardware DC; inputs without it show only the detection level.

Analog monitoring starts its quiet timing only once the monitor has read the
landed channel. A receiver that is still preparing its listening squelch (the NFM
noise squelch designs its filters the first time a channel width is used) delivers
no samples for a moment, and a closed carrier then is not a quiet channel. Only validated protocol content can add a
confirmed digital identity, access code or call details. A protocol reports its
confirmation only after decoding the content that passed the check, so one
validated frame, such as a single M17 link setup or D-STAR header, is enough for
its details to reach the detection. When a digital probe
expires, the display says **NFM monitoring · digital identification unresolved**.
That fallback does not establish that the transmission is analog.

P25 Phase 1 is confirmed by a control block whose CRC passes (TSBK, MBT or LCCH MAC)
or by a call whose identity resolves. A carrier that sends only NIDs, such as a run
of terminators, or voice whose link control never decodes, is not confirmed: the
list-decoded NID accepts several percent of random words and the voice link
control's soft Reed-Solomon decode about a fifth, so neither tells such a carrier
from noise. That carrier is monitored as NFM with the display above.

## Controls, history and saved systems

- **Hold** overrides quiet release and the maximum visit. A held probe still falls
  back when its identification window expires.
- **Skip** suppresses this frequency for the current pass.
- **Avoid this session** lasts until the session ends, including across Activity
  recreation. Session avoids are listed under **Avoided this session**, and
  **Stop avoiding** lifts one for the following measurements; a frequency already
  visited in the current pass returns with the next pass. **Avoid permanently** also saves it for
  future searches; removing a permanent avoid during a search lifts it from the
  running session too.
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
- Android Back follows the existing background-listening preference. The native
  session and history worker continue when the foreground Activity is recreated,
  and the reopened screen takes its receiver (USB, Airspy or rtl_tcp server) from
  the running session.

Detection history is separate from call history: an RF detection need not decode
anything. The newest 1,000 detections are retained, with one row updated as details
arrive. Ended, confirmed and RF-only detections remain distinguishable. The private
versioned journal is compacted atomically; write failures are reported while
available records remain in memory. History and permanent avoids are saved separately,
and a failed one stays reported and is retried by later saves until it is written.
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
action; later detections do not change the open draft.

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
Allocations, FFT size, candidates and total channels are bounded; unsupported plans
report an error.

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

The detector tests cover calibrated power, noise, carriers, modulated signals,
bursts, off-grid signals, nearby transmitters, adjacent-channel leakage from a
single strong NFM transmitter (narrow, wide, off-grid, at a range boundary and on a
6.25 kHz grid), weaker separate neighbours (including two either side of a strong
transmitter), clipping (more than 1% of samples at full scale in either component
fails the measurement, fewer does not), non-finite samples mid-frame,
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
block and then plans its first span from it. Coordinator
tests exercise stale reports and controls, deferred/failed tunes, fresh-sample
waits, short probes, fallback, hold during tuning, quiet release, an analog
monitor that has not judged the landed channel yet (and its deadline), visit caps,
live strength, lifting session avoids, pause/restart and scoped restoration, that a
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
including when storage only recovers by close), recreation, fractional steps,
ambiguous matches, nested channel maps, immutable saves (including each source's own
receiver settings and the detection's gain mode reaching the saved system), phone
text scaling and navigation ownership. They also check that connection and USB-access
failures reported after Start or Resume still reach the search screen beside its own
refusals, that status-only refreshes leave history untouched (it is copied only when its store
revision moves), session avoids, the aggregate 100,000-channel check before a start
is submitted, and that a start the engine rejects with no search left owns nothing
and is retired when the decoder session ends, so a recreated frontend never restores
the search screen over a later ordinary session. A rejected restart of a paused
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
takes its receiver (USB, Airspy or rtl_tcp server) from the session's options
snapshot, whether that snapshot arrives before or after the screen, offers that
receiver's gain controls in a paused range edit, and keeps it through Stop and a
restart.
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
