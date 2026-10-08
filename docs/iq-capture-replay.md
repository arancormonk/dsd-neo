# IQ Capture And Replay

This guide covers `dsd-neo` RF/baseband I/Q capture and replay.

Capture writes a raw I/Q data file plus a metadata sidecar. Replay reads that metadata/data pair and feeds the same
RTL demodulation path used by live radio input.

## Quick Commands

```bash
dsd-neo -i rtl:0:851.375M:22:0:48:0:2 --iq-capture p25-control.iq --frontend terminal
dsd-neo --iq-info p25-control.iq.json
dsd-neo --iq-replay p25-control.iq.json -f1 --frontend terminal
```

## CLI Flags

- `--iq-capture <path>`: enable I/Q capture.
- `--iq-capture-format <cu8|cf32>`: requested capture format (default `cu8`).
- `--iq-capture-max-mb <n>`: size limit in MiB (`0` means unlimited). Decode continues after capture writer stops.
- `--iq-replay <path>`: replay a capture file pair.
- `--iq-replay-rate <fast|realtime>`: replay pacing mode (default `fast`).
- `--iq-loop`: loop replay at EOF.
  Without it, a replay ends once the decoder has read everything the capture delivered. A capture that cannot be read to
  its end (an I/O error partway, or a data file cut short while it replays, which ends before the bytes measured when
  the replay opened it) ends the same way after the samples already read, logs the error, and makes `dsd-neo` exit
  with status 1.
- `--iq-info <path>`: print metadata/size/alignment summary and exit.

Path handling:

- If the supplied capture path ends in `.json`, it is treated as metadata path and data path becomes the same name
  without `.json`.
- Otherwise the supplied capture path is the data path, and metadata path becomes `<data path>.json`.
- If the final component of that data path carries no extension, `.iq` is added first: `--iq-capture mycap` writes
  `mycap.iq` and `mycap.iq.json`. A dot in a directory name does not count as an extension, and a leading dot belongs
  to the name (`.hidden` gains `.iq`). The suffix is always `.iq` regardless of `--iq-capture-format`; the sample
  format is recorded in the sidecar.
- `--iq-replay` and `--iq-info` accept the metadata path, the data path, or the bare name given to `--iq-capture`.
  A bare name resolves `<name>.json` first and then `<name>.iq.json`, so captures written before `.iq` was added
  still replay.
- The `.json` suffix is matched case-insensitively, so `mycap.iq.JSON` is recognised as a sidecar on Windows.

## Format Notes

Metadata is JSON with `format: "dsd-neo-iq"`.

- `version: 1` identifies historical single-segment captures with no replay event timeline.
- `version: 2` is used when the capture contains a replay event timeline.

The writer records:

- sample format (`cu8` or `cf32`), sample rate, and tuned centers.
- capture-time transform policy (`fs4_shift_enabled`, `offset_tuning_enabled`, `combine_rotate_enabled`). CU8 captures
  record the active `DSD_NEO_COMBINE_ROT` selection: the combined transform by default, or the supported two-pass
  equivalent when explicitly disabled.
- replay rate-chain fields (`base_decimation`, `post_downsample`, `demod_rate_hz`).
- source identity (`source_backend`, `source_args`).
- finalized byte/counter fields (`data_bytes`, `capture_drops`, `capture_drop_blocks`, `input_ring_drops`).
- `size_limit_reached`: `true` when the capture ended because `--iq-capture-max-mb` was reached rather than being cut
  short. Reaching the limit is not data loss and is not counted in `capture_drops`, so this is what distinguishes a
  completed capped capture from one that is still running. Absent from metadata written by older builds, where it reads
  back as `false`.
- retune fields (`contains_retunes`, `capture_retune_count`).
- for v2 captures, an `events` array describing scheduled replay events.

The v2 `events` array is ordered by capture-data `byte_offset`, not wall-clock time. Replay dispatches every event at an
offset when the reader reaches that byte position in the data stream. Replay preserves the order of equal-offset events
as stored in metadata; generated captures may place a `RETUNE` before a same-offset `MUTE` record to preserve
retune/reset semantics.

Event objects contain:

- `kind`: `RETUNE`, `MUTE`, or `RESET`.
- `byte_offset`: capture-data byte offset where the event applies.
- `reason`: a short event source/reason string.
- `duration_bytes`: required for `MUTE`; omitted muted data duration in capture-data bytes.
- `center_frequency_hz`, `capture_center_frequency_hz`, and `sample_rate_hz`: required for `RETUNE` and `RESET`.

The integrity summary fields remain present. `contains_retunes` and `capture_retune_count` summarize retune activity, while
the v2 `events` array provides the ordering needed for replay.

`data_bytes` is reconciled against the finished file at close, so it reports what is actually on disk rather than what
the writer handed to `fwrite`. Anything the file system refused — a full disk, a size-limited file system — shows up in
`capture_drops` alongside queue overruns, and `notes` says the capture ended early. Because event `byte_offset` is
stamped from bytes accepted rather than bytes written, offsets past the end of a truncated capture are clamped to
`data_bytes` so the surviving part of the capture stays replayable.

`--iq-info` reports:

- metadata bytes vs actual file bytes.
- aligned effective replay bytes and estimated duration.
- event timeline count.
- whether the size limit was reached.
- warnings for interrupted captures (`data_bytes == 0`), metadata/data mismatch, misalignment, and retune-containing
  captures that do not include a replay event timeline.

Replay uses `min(data_bytes, actual_file_size)` rounded down to sample alignment after metadata is finalized. If an
interrupted capture never finalized metadata (`data_bytes == 0`), replay falls back to the actual file size and still
rounds down to sample alignment. Zero effective bytes are rejected for `--iq-replay`.

## Replay Pacing And The Decode Clock

`--iq-replay-rate realtime` pins the replay thread to `start + samples_written / sample_rate`, so air time and wall
clock advance together. The default `fast` mode puts no clock on the reader, so a capture's worth of air time takes
only as long as decoding it does.

In both modes the decoder paces the demodulator, a capture chunk at a time: the demodulator starts on the next chunk
only once the decoder has read everything it made of the last one, and every event and `--iq-loop` rewind waits for the
same point. Both modes therefore hand the decoder the same stream, with each event applied at the same position in it,
and whatever the decoder asks of the front end mid-replay (a symbol profile the sync hunt tries, a CQPSK switch, a
reacquire) lands at the start of the next chunk, however fast the machine is or however loaded. Fast replay is somewhat
slower than it was before issue #572 for the same reason: the front end waits for the decoder instead of running ahead
of it, so the two no longer overlap. A 94 s capture that replayed in 8.4 s took 10.8 s after it.

A chunk is one block of a live receiver's (issue #626): at most 8192 complex samples, a live RTL-SDR transfer, and no
more than such a transfer lasts at the default 1.536 Msps, 5.33 ms of capture (256 samples of a 48 kHz capture), or up
to the next event. Every decision the demodulator makes once per block (the level squelch and its envelope, the
channel power the decoder and the scanner read, adaptive loop gains, metrics) and every request the decoder makes
therefore runs at a live receiver's cadence; with the 64 KiB chunks of earlier builds a 48 kHz capture's block was
683 ms, and a level squelch opened and closed on that grid. The input-level snapshot covers one chunk, a live
transfer's time, as a live stream's covers each transfer. A replay now does the per-block work a receiver does on air:
in a Release build it takes 13 to 27 % more CPU than with 64 KiB chunks, and that 94 s capture's fast replay 2.0 s
against 1.6 s.

The decoder's clock follows the capture as well. Under `--iq-replay`, decode time is the sidecar's
`capture_started_utc` plus the capture time of the sample being decoded, the time `MUTE` events omitted included:

- Timestamps in the decoder's output (the console, event and call history, logs and file names) show when the
  capture was made, in the local time zone as live, not when it was replayed. A `capture_started_utc` before
  2000-01-01T00:00:00Z, such as the `1970-01-01T00:00:00Z` the generated fixtures in `tests/fixtures/iq` carry, starts
  the clock at 2000-01-01T00:00:00Z instead; one that does not parse is logged and starts it there too.
- Call durations, the call reacquisition window (`DSD_CALL_REACQUIRE_GAP_S`, which decides whether a
  sync-loss-interrupted transmission is one history row or two), protocol windows and staleness timers measure air
  time, as they would live, in both modes.
- Decode time stamps an output about 1.54 ms later than the capture time of the signal in it, on the default
  1.536 Msps chain: each block's output carries the capture span of the chunk it came from, while the front end's
  filters centre its first outputs on samples they held back from the chunk before (their look-ahead, below). The
  offset is the same on every replay and far below any decode window.

So, for one build, configuration and machine, every replay of a capture decodes the same way, fast or realtime, loaded
or idle: the same decoder lines with the same timestamps. Another machine can differ, since the front end's SIMD paths
agree with each other only to within about 1e-5. `docs/testing.md` ("Replay determinism") describes the cases that hold
it to that. Lines measured or paced on real time are not part of it: the audio-sink statistics and the decode loop's
`Runtime:` total. The input-level gain warnings are, since their cooldown runs on decode time, and the stream ends only
on the decoder's read after the last samples, so the level they read holds while the decoder decodes those samples, fast
or realtime. These replays are outside it altogether:

- **Trunking (`-T`).** The P25 trunking state machine's watchdog thread checks its hangtime and
  return-to-control-channel timers at a real-time cadence, so where those checks fall among the replayed samples follows
  the wall clock, and a trunked replay can decide differently from run to run. Replay also refuses the retunes trunking
  asks for.
- **Conventional scanning (`-Y`).** Replay refuses the scanner's retunes (`NOTICE: Retune ignored during IQ replay.`,
  logged at most once a second of real time), so its hangtime and visit timers step a scan that never leaves the
  capture's channel, and how often the notice prints follows the replay's pace.
- **A replay restarted mid-run.** Only a replay started as the run's input runs on the capture's clock, from its first
  sample. When an interactive frontend restarts the stream (a gain, device or DSP bandwidth change, or a stream
  restart) or switches the input away, the decode clock goes back to the system clock, and a replay that starts again
  within the same run decodes on the system clock. A restart that replays the same capture logs this once, as "IQ
  replay restarted mid-run; decode timestamps now follow the system clock."
- **A replay on a reused decoder state.** Only a run on a decoder state no earlier run has used moves onto the
  capture's clock. An embedding host can run again on the state its last run used: the Android service does when a
  start races the previous run's teardown. A replay started as that run's input then decodes on the system clock, and
  the log says so once: "IQ replay: this run reuses a decoder state an earlier run used; replay timing stays on the
  system clock for this run, and decode times are not anchored to the capture." The earlier run's stamps are on the
  system clock, and moving the clock back to an older capture's time would hold its throttles and timers until the
  capture caught up.

The last 1.5 ms or so of a capture is never decoded. The front end's filters hold back their look-ahead (about 74
samples at 48 kHz on the default 1.536 Msps chain, 67 on a 48 kHz capture), and nothing flushes it when the capture
ends, so a frame that ends inside those samples is lost: the `edacs` fixture's last message fails its BCH check for
this reason. A `RESET` drops what the filters held the same way, as a live retune does. On a real capture that is a
small, steady cost at the dwell boundaries: a 593 s P25 Phase 1/2 capture with 36 retunes between the control channel
and Phase 2 voice channels, replayed under `-f2 -mq -X` without `-T` over 16 realizations (`docs/testing.md`, "Replay
determinism"), decodes 3.88 +/- 0.18 fewer of its 677 Phase 2 syncs and 2.44 +/- 0.75 fewer of its 1040 voice frames
than the build before the streaming filters, with audio errors equal (+0.19 +/- 0.40). The whole loss comes from the
channel filter's streaming change, whose held tail is 67 samples, 1.4 ms at 48 kHz; the half-band decimators add
nothing measurable.

- **At a dwell's end.** Diffing the two decodes at the recorded alignment, the missing lines are almost all frames that
  straddle a dwell's closing `RESET`, decoded partly from the old channel and partly from the new: `DUID ERR`,
  `CRC12 ERR`, `R-S ERR` and a few partial `2V`/`4V` frames. The build before made the closing frame from the
  filters' padded tail and the new channel's first samples. Now the held tail is dropped, so those frames are not
  produced: 10 such error-type frames against 18.
- **At a dwell's start.** About one call-opening `MAC_ACTIVE` a run is missed (paired -1.9 +/- 0.2). At the recorded
  alignment that is TG 50651 at 07:00:49, whose talker alias then prints `TG: UNK`.

Both are the measured cost of dropping the tail at a `RESET`, which is the default: flushing it there would rebuild the
padded, made-up samples the streaming filters removed. Against `main`, replayed realtime twice (identical runs), the
branch at the recorded alignment decodes 1039 voice frames, 673 syncs and 307 `MAC_ACTIVE` against 1036, 672 and 305,
with 10 error-type frames against 15 and 15 unattributed aliases on both. It is level or ahead on every count except
audio errors, 25 against 24.

File input under `-r` or WAV replay has no capture clock: it decodes on the system clock and is unthrottled (only `.bin`
symbol-capture replay is paced), so its call gaps, hangtime and staleness windows look shorter than they were on air.

## Analog Monitor Replay

`-fA` replays a capture through the analog FM monitor, the same RTL audio-monitor path live NFM uses:

```bash
dsd-neo -fA --iq-replay tests/fixtures/iq/nfm_ctcss_real.iq.json --iq-replay-rate realtime
```

`-fM` replays one through the native AM receiver instead, the envelope detector live AM uses (see
[Native AM](cli.md#native-am--fm) in `docs/cli.md`; `--am-bandwidth-hz` sets its channel width here too):

```bash
dsd-neo -fM --iq-replay tests/fixtures/iq/am_airband_real.iq.json --iq-replay-rate realtime
```

- The monitor runs at the capture's `demod_rate_hz` (48 kHz for every committed fixture). The capture dictates the
  rate chain, so the RTL DSP bandwidth option has no effect on replay. A capture at another rate, such as the
  78,125 Hz an Airspy at 2.5 MS/s forces, is resampled to the 48 kHz output once, as live.
- AM needs a capture whose sidecar has `post_downsample` 1 (every committed fixture has): one that decimates after the
  demodulator would run the AM channel filter at a multiple of the demod rate, so the stream start refuses it with
  `...; AM needs a capture with post_downsample 1`. FM replays such a capture as before.
- The power squelch, the monitor's voice filters (`-v`) and its gain stage (`-n`, the AGC by default) apply as they do
  live; `-o null` discards the audio.
- The monitor plays a replayed signal at the level it plays at live: replay runs the FM discriminator output scale a
  live stream runs (1/pi). Before, replayed FM monitor audio came out pi times (9.9 dB) louder than live.
- Use `realtime` pacing to listen. `fast` replay delivers the same samples and is right for scoring.
- Under `-fA` and `-fM` the modulation auto-switch stands down, so a carrier within a few hertz of 0 Hz
  (`am_airband_real`), which votes for CQPSK, no longer moves the front end to the P25 CQPSK path, and the monitor
  delivers the whole capture. No other symbol profile the sync hunt requests reaches the RTL front end in analog-only
  mode either. `dsd-neo_test_analog_replay` warns if the front end ever delivers CQPSK symbols instead of monitor
  audio. `docs/testing.md` has the details.
- `tests/fixtures/iq` carries short analog captures (`nfm_*`, and for AM `am_airband_real`, `am_tone_synth` and
  `am_adjacent_synth`), and `dsd-neo_test_analog_replay` scores the monitor's audio from any capture.
  `docs/testing.md` covers both, the `tools/replay_ab.sh --metric analog` procedure, and the listen-test sign-off.
- Two-channel u8 or s16 PCM WAV recordings of I/Q (the SDR# style) are not replayable directly.
  `tools/build_iq_fixtures.py` shows the conversion the analog fixtures use: sample-exact WAV read, carrier centring,
  frequency-domain resampling to 48 kHz and a cu8 sidecar pair (`read_wav_iq`, `centre_carrier`,
  `resample_to_fixture_rate`, `write_fixture`).

## Operational Limits

- `--iq-capture` and `--iq-replay` are mutually exclusive in one invocation.
- Single-segment v1 captures continue to replay unchanged.
- CU8 metadata with `combine_rotate_enabled: false` selects the two-pass byte rotation and bias-128 widening so captures
  made under that transform policy replay identically.
- Retuned captures are replayable only when they include a v2 event timeline. Older retuned v1 captures with
  `contains_retunes: true` and no `events` array are rejected because they do not preserve enough ordering data to replay
  safely.
- `RETUNE` events update replay-visible center frequency state. `RESET` events apply the same demod reset/purge/output
  handling used by live retunes, from the center the `RETUNE` before them left, as the live retune did. `MUTE` events
  emit no samples, but advance replay phase accounting, realtime virtual sample time and the decode clock by
  `duration_bytes`.
- Replay applies an event, and an `--iq-loop` rewind, only once the decoder has read everything demodulated before it.
  The decoder then meets each event at the same point in fast and realtime replay, and a `RESET` drops nothing a slow
  decoder has not read. It decodes from the first sample demodulated after a `RESET`, or after a symbol profile or
  CQPSK change it asked for, where a live stream discards the samples read across such a change.
- `--iq-loop` rewinds the event cursor and replay timing so the event schedule repeats each pass. A pass that handed
  the demodulator no samples is not rewound: the replay logs a warning and ends there, as it does at the end without
  `--iq-loop`.
- Only the metadata-scheduled replay events retune a replay. A manual tune, a frequency entry and the tuner release are
  refused during one, from the terminal, Qt/Android and the control API alike, with "An I/Q replay cannot retune.",
  rather than reported as a tune that never lands; the retunes trunking and the conventional scanner ask for are
  ignored, as described above.
- The tuned frequency the frontends show (the terminal `FRQ:` field, the Qt Spectrum label, the Android notification)
  follows the capture's recorded `RETUNE` events: it reads the centre the samples being decoded were captured on, the
  capture's opening centre until the first `RETUNE` and again after an `--iq-loop` rewind. The centre option the
  trunking paths read as the current channel keeps the opening centre, as before, so replay decoding is unchanged.
- Event timelines currently require a constant sample rate through the capture. Metadata with event sample-rate changes is
  rejected until segment-rate replay is supported.
- Replay converts `cu8`, and `cf32` captured as the driver delivered it (`capture_stage: "post_driver_cf32_pre_ring"`).
  `--iq-replay` refuses any other capture when it opens it, naming the sample format and stage, and `dsd-neo` exits with
  status 1: a `cs16` sidecar, or a `cf32` one stamped with another stage. `--iq-info` still describes such a capture and
  reports `Replay compatible: no`.
- Direct `-i iqreplay:...` is intentionally rejected; use `--iq-replay <path>`.
- Replay currently feeds the RTL radio path and reuses existing demod processing/state handling.
- `base_decimation` is capped at 1024 (10 half-band passes); metadata requesting more is rejected.
- The capture file dictates the replay rate chain, so a capture whose demod rate yields a non-integer
  samples-per-symbol (for example 62,500 Hz at 4800 sym/s) is resampled to the resampler target (48,000 Hz by
  default) under the default `digital_resample = "auto"` policy. Decode output for such captures can therefore
  differ from releases that fed the raw demod rate through; set `digital_resample = "off"` to restore the
  previous behavior. See `docs/soapysdr.md` for the full policy.

## Backend Notes

- Capture is available on live radio inputs only: RTL USB, RTL-TCP, and SoapySDR when the active Soapy stream is `CF32`.
- RTL USB / RTL-TCP captures are `cu8`.
- `--iq-capture-format cf32` is only valid when the active backend stream is native `cf32` (for example Soapy CF32).
- Soapy drivers that only provide `CS16` can be used for live decode, but are not currently accepted by the IQ capture
  CLI.
- The metadata parser and public sample-format helpers recognize `cs16` metadata with 4-byte sample alignment, but live
  capture and replay accept only `cu8` and `cf32`; `--iq-replay` refuses a `cs16` sidecar at open (Operational Limits).
- If requested capture format does not match the active backend stream format, startup fails with a clear error.
