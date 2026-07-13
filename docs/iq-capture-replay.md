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
- Otherwise the supplied capture path is treated as data path and metadata path becomes `<path>.json`.
- `--iq-replay` and `--iq-info` accept either metadata path or data path.

## Format Notes

Metadata is JSON with `format: "dsd-neo-iq"`.

- `version: 1` is used for legacy single-segment captures with no replay event timeline.
- `version: 2` is used when the capture contains a replay event timeline.

The writer records:

- sample format (`cu8` or `cf32`), sample rate, and tuned centers.
- capture-time transform policy (`fs4_shift_enabled`, `offset_tuning_enabled`). New captures always record the combined
  CU8 rotate-and-widen transform.
- replay rate-chain fields (`base_decimation`, `post_downsample`, `demod_rate_hz`).
- source identity (`source_backend`, `source_args`).
- finalized byte/counter fields (`data_bytes`, `capture_drops`, `capture_drop_blocks`, `input_ring_drops`).
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

The legacy summary fields remain present. `contains_retunes` and `capture_retune_count` summarize retune activity, while
the v2 `events` array provides the ordering needed for replay.

`--iq-info` reports:

- metadata bytes vs actual file bytes.
- aligned effective replay bytes and estimated duration.
- event timeline count.
- warnings for interrupted captures (`data_bytes == 0`), metadata/data mismatch, misalignment, and retune-containing
  captures that do not include a replay event timeline.

Replay uses `min(data_bytes, actual_file_size)` rounded down to sample alignment after metadata is finalized. If an
interrupted capture never finalized metadata (`data_bytes == 0`), replay falls back to the actual file size and still
rounds down to sample alignment. Zero effective bytes are rejected for `--iq-replay`.

## Operational Limits

- `--iq-capture` and `--iq-replay` are mutually exclusive in one invocation.
- Single-segment v1 captures continue to replay unchanged.
- Historical CU8 metadata with `combine_rotate_enabled: false` still selects the former two-pass byte rotation and
  bias-128 widening so those captures replay identically. This private compatibility decoder can be removed when support
  for pre-unconditional-transform v1/v2 CU8 captures is retired.
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

## Backend Notes

- Capture is available on live radio inputs only: RTL USB, RTL-TCP, and SoapySDR when the active Soapy stream is `CF32`.
- RTL USB / RTL-TCP captures are `cu8`.
- `--iq-capture-format cf32` is only valid when the active backend stream is native `cf32` (for example Soapy CF32).
- Soapy drivers that only provide `CS16` can be used for live decode, but are not currently accepted by the IQ capture
  CLI.
- The metadata parser and public sample-format helpers recognize `cs16` metadata with 4-byte sample alignment, but live
  capture and replay demod conversion currently accept only `cu8` and `cf32`.
- If requested capture format does not match the active backend stream format, startup fails with a clear error.
