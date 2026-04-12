# IQ Capture And Replay

This guide covers `dsd-neo` RF/baseband I/Q capture and replay.

Capture writes a raw I/Q data file plus a metadata sidecar. Replay reads that metadata/data pair and feeds the same
RTL demodulation path used by live radio input.

## Quick Commands

```bash
dsd-neo -i rtl:0:851.375M:22:0:48:0:2 --iq-capture p25-control.iq -N
dsd-neo --iq-info p25-control.iq.json
dsd-neo --iq-replay p25-control.iq.json -f1 -N
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

Metadata is JSON with `format: "dsd-neo-iq"` and `version: 1`.

The writer records:

- sample format (`cu8` or `cf32`), sample rate, and tuned centers.
- capture-time transform policy (`fs4_shift_enabled`, `combine_rotate_enabled`, `offset_tuning_enabled`).
- replay rate-chain fields (`base_decimation`, `post_downsample`, `demod_rate_hz`).
- source identity (`source_backend`, `source_args`).
- finalized byte/counter fields (`data_bytes`, `capture_drops`, `capture_drop_blocks`, `input_ring_drops`).
- retune fields (`contains_retunes`, `capture_retune_count`).

`--iq-info` reports:

- metadata bytes vs actual file bytes.
- aligned effective replay bytes and estimated duration.
- warnings for interrupted captures (`data_bytes == 0`), metadata/data mismatch, misalignment, and retune-containing
  captures.

Replay uses `min(data_bytes, actual_file_size)` rounded down to sample alignment. Zero effective bytes are rejected for
`--iq-replay`.

## Operational Limits

- `--iq-capture` and `--iq-replay` are mutually exclusive in one invocation.
- Replay retune events are not supported in this format version. Captures with `contains_retunes: true` are rejected.
- Direct `-i iqreplay:...` is intentionally rejected; use `--iq-replay <path>`.
- Replay currently feeds the RTL radio path and reuses existing demod processing/state handling.

## Backend Notes

- RTL USB / RTL-TCP captures are `cu8`.
- `--iq-capture-format cf32` is only valid when the active backend stream is native `cf32` (for example Soapy CF32).
- If requested capture format does not match the active backend stream format, startup fails with a clear error.
