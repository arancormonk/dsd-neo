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

## Replay Pacing And Decode-Time Windows

`--iq-replay-rate realtime` pins the replay thread to `start + samples_written / sample_rate`, so air time and wall
clock advance together. The default `fast` mode applies no pacing at all: throughput is bounded only by ring
backpressure and decode speed, so a capture's worth of air time completes in considerably less wall-clock time.

That matters because there is currently **no decode-derived clock** in the decoder. Protocol layers time call state
against `dsd_time_now_monotonic_s()` (wall-clock monotonic), and any call site that passes `observed_m = 0.0` falls
back to the same source. Under `fast` replay every wall-clock-measured interval in the canonical call state is
therefore compressed relative to the air time it is meant to describe:

- Gaps look shorter than they were, so the call reacquisition window (`DSD_CALL_REACQUIRE_GAP_S`, which decides
  whether a sync-loss-interrupted transmission is one history row or two) coalesces more readily than it would live.
- Hangtime and staleness timers in the trunking state machines expire later in air-time terms than they would live.

Use `--iq-replay-rate realtime` when reproducing or asserting on any of that timing. The same caveat applies to
non-`.bin` file input under `-r`/WAV replay, which is likewise unthrottled; only `.bin` symbol-capture replay is paced.

## Operational Limits

- `--iq-capture` and `--iq-replay` are mutually exclusive in one invocation.
- Single-segment v1 captures continue to replay unchanged.
- CU8 metadata with `combine_rotate_enabled: false` selects the two-pass byte rotation and bias-128 widening so captures
  made under that transform policy replay identically.
- Retuned captures are replayable only when they include a v2 event timeline. Older retuned v1 captures with
  `contains_retunes: true` and no `events` array are rejected because they do not preserve enough ordering data to replay
  safely.
- `RETUNE` events update replay-visible center frequency state. `RESET` events apply the same demod reset/purge/output
  handling used by live retunes. `MUTE` events emit no samples, but advance replay phase accounting and realtime virtual
  sample time by `duration_bytes`.
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
