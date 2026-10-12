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
| Quiet time | Resume after 500 ms without relevant carrier or validated digital activity |
| Maximum visit | 30 seconds; adjustable through 600 seconds, or 0 to disable |
| Gain | Inherit the session, or use the backend's range controls; Airspy has its own gain controls |

RF detection sensitivity and listening squelch are separate. Changing listening
squelch does not change which channels pass the RF detector. AM and analog-only
NFM ranges open their monitor directly. Digitally enabled NFM ranges first run
the selected profile. Auto normally takes about six seconds to rotate its symbol
profiles, so the two-second default cannot identify every supported signal.

A detection displays frequency and measured strength immediately, while its
listening tune is still pending. Only validated protocol content can add a
confirmed digital identity, access code or call details. When a digital probe
expires, the display says **NFM monitoring · digital identification unresolved**.
That fallback does not establish that the transmission is analog.

## Controls, history and saved systems

- **Hold** overrides quiet release and the maximum visit. A held probe still falls
  back when its identification window expires.
- **Skip** suppresses this frequency for the current pass.
- **Avoid this session** lasts until the session ends, including across Activity
  recreation. **Avoid permanently** also saves it for future searches. Permanent
  avoids can be removed when stopped.
- **Pause** permits range edits. **Resume** applies them and restarts the pass,
  retaining session avoids. **Stop** ends the radio session.
- Android Back follows the existing background-listening preference. The native
  session and history worker continue when the foreground Activity is recreated.

Detection history is separate from call history: an RF detection need not decode
anything. The newest 1,000 detections are retained, with one row updated as details
arrive. Ended, confirmed and RF-only detections remain distinguishable. The private
versioned journal is compacted atomically; write failures are reported while
available records remain in memory. Orderly stop flushes the journal. Ranges and
permanent avoids are also stored privately.

Saved-system labels come from a cached index of saved frequencies, referenced
channel maps and scan lists. Exact matches win; otherwise the nearest frequency
must be within half the smaller applicable spacing or channel width. Equally near
matches are all shown. For example, 463.5521 MHz can match a saved 463.5500 MHz
channel with 12.5 kHz spacing. A label suggests a saved frequency, not proof of
system identity, and never retunes the radio.

**Save as system** opens the normal save wizard from an immutable detection
snapshot. Frequency, receive mode, source, bandwidth and gain are prefilled.
Confirmed NXDN48 and NXDN96 detections preserve their respective decoder rates.
Unresolved identification stays explicit. The user must choose the normal Save
action; later detections do not change the open draft.

## Scope and implementation

Search owns one live tuner exclusively. The shared backend supports tunable
RTL-SDR, Airspy, rtl_tcp and Soapy inputs; the Qt source chooser offers the existing
Explore sources. PCM, rigctl-only inputs and production I/Q replay cannot search.
Conventional scanning and trunk following must stop first. CLI controls, public
TCP API commands and simultaneous analog/digital reception are not provided.

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
bins across the smaller channel width/spacing. Local noise uses sidebands outside
the channel and guard region. Clipping and capture discontinuities invalidate the
measurement. Allocations, FFT size, candidates and total channels are bounded;
unsupported plans report an error.

The coverage planner conservatively uses one usable side of hardware DC per
capture, leaving noise sidebands and edge guards. It explicitly accounts for
capture frequency, rate and Fs/4 ingest rotation. The first remaining channel
anchors each new span, so DC/edge exclusions do not leave gaps. This costs more
captures than an ideal full-bandwidth sweep: the 10 MHz example below uses 22
spans, rather than the planning estimate of 9–10 before exclusions.

## Verification and measured limits

Run the focused native and QML regressions in a Qt-enabled build:

```sh
ctest --preset dev-debug -R 'SIGNAL_(MEASURE|SEARCH)|UI_QT_QML_SESSION_TOOLS' --output-on-failure
ctest --preset dev-debug -L iq-decode --output-on-failure
```

The detector tests cover calibrated power, noise, carriers, modulated signals,
bursts, off-grid signals, nearby transmitters, clipping, gain/rate/drop boundaries,
DC, rotation, edges and fragmented input. Coordinator tests exercise stale reports
and controls, deferred/failed tunes, fresh-sample waits, short probes, fallback,
hold during tuning, quiet release, visit caps, pause/restart and scoped restoration.
Persistence and Qt tests cover bounded history, duplicate suppression, failed
writes/retry, recreation, fractional steps, ambiguous matches, nested channel maps,
immutable saves, phone text scaling and navigation ownership.
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
