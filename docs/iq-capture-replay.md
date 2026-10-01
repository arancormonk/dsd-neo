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
  its end (an I/O error partway) ends the same way after the samples already read, logs the error, and makes `dsd-neo`
  exit with status 1.
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
(64 KiB of the capture, or up to the next event) only once the decoder has read everything it made of the last one,
and every event and `--iq-loop` rewind waits for the same point. Both modes therefore hand the decoder the same
stream, with each event applied at the same position in it, and whatever the decoder asks of the front end mid-replay
(a symbol profile the sync hunt tries, a CQPSK switch, a reacquire) lands at the start of the next chunk, however fast
the machine is or however loaded.

The decoder's clock follows the capture as well. Under `--iq-replay`, decode time is the sidecar's
`capture_started_utc` plus the capture time of the sample being decoded, the time `MUTE` events omitted included:

- Timestamps in the decoder's output (the console, event and call history, logs and file names) show when the
  capture was made, in the local time zone as live, not when it was replayed. A `capture_started_utc` before
  2000-01-01T00:00:00Z, such as the `1970-01-01T00:00:00Z` the generated fixtures in `tests/fixtures/iq` carry, starts
  the clock at 2000-01-01T00:00:00Z instead; one that does not parse is logged and starts it there too.
- Call durations, the call reacquisition window (`DSD_CALL_REACQUIRE_GAP_S`, which decides whether a
  sync-loss-interrupted transmission is one history row or two), protocol windows and staleness timers measure air
  time, as they would live, in both modes.

So every replay of a capture decodes the same way, fast or realtime, loaded or idle: the same decoder lines with the
same timestamps. `docs/testing.md` ("Replay determinism") describes the cases that hold it to that. Lines measured or
paced on real time are not part of it: the audio-sink statistics, the decode loop's `Runtime:` total, and the
input-level gain warnings, whose cooldown runs on real time. Two kinds of replay are outside it altogether:

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

The last 1.5 ms or so of a capture is never decoded. The front end's filters hold back their look-ahead (about 74
samples at 48 kHz on the default 1.536 Msps chain, 67 on a 48 kHz capture), and nothing flushes it when the capture
ends, so a frame that ends inside those samples is lost: the `edacs` fixture's last message fails its BCH check for
this reason. A `RESET` drops what the filters held the same way, as a live retune does.

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
- The power squelch and the monitor's voice filters (`-v`) apply as they do live; `-o null` discards the audio.
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
- `--iq-loop` rewinds the event cursor and replay timing so the event schedule repeats each pass.
- User/API retune requests during IQ replay remain ignored; only metadata-scheduled replay events are applied.
- Event timelines currently require a constant sample rate through the capture. Metadata with event sample-rate changes is
  rejected until segment-rate replay is supported.
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
  capture and replay demod conversion currently accept only `cu8` and `cf32`.
- If requested capture format does not match the active backend stream format, startup fails with a clear error.
