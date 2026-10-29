# IQ Capture And Replay

This guide covers `dsd-neo` RF/baseband I/Q capture and replay.

Capture writes a raw I/Q data file plus a metadata sidecar. Replay reads that metadata/data pair, or a two-channel WAV
I/Q recording made by other SDR software, and feeds the same RTL demodulation path used by live radio input.

## Quick Commands

```bash
dsd-neo -i rtl:0:851.375M:22:0:48:0:2 --iq-capture p25-control.iq --frontend terminal
dsd-neo --iq-info p25-control.iq.json
dsd-neo --iq-replay p25-control.iq.json -f1 --frontend terminal
dsd-neo --iq-info SDRSharp_20261015_123456Z_426700000Hz_IQ.wav
dsd-neo -fT --iq-replay SDRSharp_20261015_123456Z_426700000Hz_IQ.wav
```

## CLI Flags

- `--iq-capture <path>`: enable I/Q capture.
- `--iq-capture-format <cu8|cf32>`: requested capture format (default `cu8`).
- `--iq-capture-max-mb <n>`: size limit in MiB (`0` means unlimited). Decode continues after capture writer stops.
- `--iq-replay <path>`: replay a capture file pair, or a two-channel WAV I/Q recording (see
  [Replaying WAV I/Q Recordings From Other SDR Software](#replaying-wav-iq-recordings-from-other-sdr-software)).
- `--iq-replay-rate <fast|realtime>`: replay pacing mode (default `fast`).
- `--iq-loop`: loop replay at EOF.
  Without it, a replay ends once the decoder has read everything the capture delivered. A capture that cannot be read to
  its end (an I/O error partway, or a data file cut short while it replays, which ends before the bytes measured when
  the replay opened it) ends the same way after the samples already read, logs the error, and makes `dsd-neo` exit
  with status 1.
- `--iq-info <path>`: print metadata/size/alignment summary and exit. For a WAV recording it describes the header
  instead.

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
- These are operator-selected local paths, including absolute paths and paths outside the working directory.
  They are opened read-only with the current process's permissions; the CLI does not confine them to a capture
  directory. The reader requires regular files and rejects final symlinks/reparse points where supported.
- The `.json` suffix is matched case-insensitively, so `mycap.iq.JSON` is recognised as a sidecar on Windows.
- A path that does not end in `.json` is read as a WAV recording first: a file that starts with a RIFF, RF64 or BW64
  WAVE header replays as one, whatever its extension, and no sidecar is needed. A sidecar beside it (`<path>.json` or
  `<path>.iq.json`) is ignored, with a warning that names it: `IQ replay: 'tetra.wav' is a WAV file: its header gives
  the format and rate (2-channel PCM 24-bit, 48254 Hz), so the sidecar 'tetra.wav.json' is ignored`.
- A sidecar whose `data_file` names a WAV recording is read for that name only: the WAV replays from its own header,
  however wrong the sidecar's other fields are, with the same warning.
- A `.wav` path that holds no WAV header and has no sidecar is refused: `'x.wav' is not a WAV file (no RIFF, RF64 or
  BW64 WAVE header) and has no sidecar (tried 'x.wav.json' and 'x.wav.iq.json')`. A big-endian RIFX file is refused
  too.
- A path that cannot be opened, and has no sidecar to name a capture through, is refused with the reason: a missing
  file (`cannot open 'x.wav': No such file or directory, and it has no sidecar (tried ...)`), a symbolic link (`'x.wav'
  is a symbolic link, or a path the system would not follow; replay opens regular files only`), a directory or other
  non-regular file, or the system's error (`Permission denied`). A sidecar's `data_file` that is a symbolic link or
  cannot be opened is refused the same way; one that is missing leaves the sidecar readable for `--iq-info`, and the
  replay reports the missing file. A read of a file's first bytes that fails is reported as `failed to read`; a file
  shorter than a WAV header is read as not a WAV.

## Format Notes

Metadata is JSON with `format: "dsd-neo-iq"`.

- `version: 1` identifies historical single-segment captures with no replay event timeline.
- `version: 2` is used when the capture contains a replay event timeline.

The writer records:

- sample format (`cu8` or `cf32`: live capture writes only these; replay also reads `cs16` sidecars, see Backend
  Notes), sample rate, and tuned centers.
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
- `tuner_gain_tenth_db` and `tuner_gain_known`: the tuner gain in tenths of a dB as the device reported it (negative
  gains are real: an E4K's lowest is -1.0 dB, an FC0012's -9.9 dB), and `false` when the tuner reported none (an
  Airspy, a SoapySDR device without a gain range), with the gain written as 0. Replay reports no gain then. Metadata
  written by older builds has no `tuner_gain_known`; an Airspy capture among them recorded an error code as its gain and
  replays without one, and any other reads as known.
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

A gap in a live stream on the same channel, an RTL-SDR stall recovery or an rtl_tcp reconnect (see `docs/cli.md`, "Stall
recovery" and "rtl_tcp connections"), is recorded where the stream resumes as a `RETUNE` followed by a `RESET` at the
same offset, both with reason `usb_recovery` or `tcp_reconnect`, on the centre that was in force; the capture goes on in
the same file, and nothing from the outage is written. On replay those events restart the front end as any `RESET`
does, on an idle pipeline (see Operational Limits), and they also mark the replay batch after them as a stream gap:
every sample before them reaches the decoder first, in order, then the decoder resets what the gap cut, as it does
live (the frame being decoded is dropped and calls end as after a fade, while the control channel and the call being
followed stay), before it reads the first sample after them. That holds for the symbol-rate reader and for the analog
monitor's one-sample reader, in fast and realtime replay alike, so a capture with a recovery in it decodes the same in
every run.

`data_bytes` is reconciled against the finished file at close, so it reports what is actually on disk rather than what
the writer handed to `fwrite`. Anything the file system refused — a full disk, a size-limited file system — shows up in
`capture_drops` alongside queue overruns, and `notes` says the capture ended early. Because event `byte_offset` is
stamped from bytes accepted rather than bytes written, offsets past the end of a truncated capture are clamped to
`data_bytes` so the surviving part of the capture stays replayable.

`--iq-info` reports, for a sidecar capture:

- metadata bytes vs actual file bytes.
- aligned effective replay bytes and estimated duration.
- event timeline count.
- whether the size limit was reached.
- warnings for interrupted captures (`data_bytes == 0`), metadata/data mismatch, misalignment, and retune-containing
  captures that do not include a replay event timeline.

For a WAV recording it reports the container, the sample encoding and channel order, the sample rate, the centre
frequency and start time with where each came from (`auxi chunk`, `file name`, or `unknown`), the data offset and
bytes, the duration, the rate chain the replay would run at the default 48 kHz and at TETRA's 72 kHz, and whether it
replays, with warnings for a truncated or unfinalized header, a data chunk that runs past its container's end, a
partial last frame, and an ignored sidecar.

Replay uses `min(data_bytes, actual_file_size)` rounded down to sample alignment after metadata is finalized. If an
interrupted capture never finalized metadata (`data_bytes == 0`), replay falls back to the actual file size and still
rounds down to sample alignment. Zero effective bytes are rejected for `--iq-replay`.

## Replaying WAV I/Q Recordings From Other SDR Software

`--iq-replay` reads a two-channel WAV I/Q recording directly, with no sidecar. Its header gives the sample format and
rate, and the replay plans its rate chain from that rate (see [Rate Chain And Resampling](#rate-chain-and-resampling)):

```bash
dsd-neo --iq-info SDRSharp_20261015_123456Z_426700000Hz_IQ.wav
dsd-neo -fT --iq-replay SDRSharp_20261015_123456Z_426700000Hz_IQ.wav
```

What it reads:

- Exactly two channels: the first is I, the second Q. A one-channel WAV is refused with a pointer to audio input
  (`WAV 'x.wav' has 1 channel: I/Q replay needs 2 (I, then Q); decode an audio WAV with -i 'x.wav'`); a WAV of more
  channels is refused too.
- 8-bit unsigned PCM, 16-, 24- or 32-bit signed PCM, or 32- or 64-bit IEEE float, in a plain `fmt` chunk or a
  `WAVE_FORMAT_EXTENSIBLE` one whose subformat is PCM or IEEE float (its valid bits may be fewer than the container's).
  Any other format, sample size or subformat is refused, naming what replay reads. The block align must be two samples.
- RIFF, RF64 and BW64 containers (RF64 and BW64 carry 64-bit sizes in a `ds64` chunk, which must come first).
  Chunks other than `fmt`, `data` and `auxi` (`meta`, `LIST` and the like) are skipped, `fmt` may come after `data`,
  and an odd-sized chunk is read with or without the pad byte its writer should have added. Chunks after `data` are
  not samples.
- Samples are scaled to the floats a live receiver delivers: 8-bit unsigned as (x - 127.5) / 127.5, as a `cu8`
  capture is widened (so no DC offset is added); signed PCM by its full scale; 32-bit float as it is; 64-bit float
  with NaN read as 0, and infinities and magnitudes above 1e6 clamped to 1e6.
- The sample rate is the header's. Any rate from 8 kHz replays, within the limits under Rate Chain And Resampling.

The centre frequency comes from, in order:

1. An `auxi` chunk (the layout SDR#, HDSDR and SDRuno write): its centre frequency field.
2. The file name: the last `_`-separated token of the name, without its `.wav` extension, that reads as a number with
   a `Hz`, `kHz` or `MHz` unit (any case) and is a whole number of hertz: `426700000Hz`, `426.7MHz`, `426700kHz`, as
   SDR#, SDR++ and SDRuno name their recordings.
3. Otherwise the centre is unknown. The replay still runs and loops; nothing tunes, and only what needs the tuned
   frequency goes without it. TETRA, for one, publishes a call to the event history only once the tuned frequency
   shows the recording is on the carrier the call was allocated: with no centre it decodes the call's signalling and
   speech but records no call. Renaming the file to carry its centre (`..._426700000Hz.wav`) supplies one.

The start time, which the decode clock starts from (see
[Replay Pacing And The Decode Clock](#replay-pacing-and-the-decode-clock)), comes from the `auxi` chunk's start time,
taken as UTC (a year before 1601, such as the 0 a writer that left it unset stores, counts as none), or else from the
`YYYYMMDD_HHMMSSZ` tokens SDR# and SDRuno put in the name (`..._20261015_123456Z_...`). With neither, decode times start
at 2000-01-01T00:00:00Z.

The replay demodulates the channel at the recording's centre. A replay cannot be retuned, so a channel elsewhere in
a wide recording is not decoded: set the recording program's centre (tuner) frequency on the carrier before recording,
not only a VFO inside its band. A small offset, such as the +343 Hz the `tetra_call_real_air_72k` fixture's recording
carries, is recovered as it is on air (for TETRA, see the tuning tolerance under `-fT` in `docs/cli.md`).

### SDR++ Baseband Recordings

SDR++'s recorder, in baseband mode, writes the I/Q its tuner receives, centred on the tuner's centre frequency (the
middle of the waterfall), not on the VFO. In SDR++'s usual tuning mode a click on the waterfall moves the VFO while
the centre stays where it was, so a carrier under the VFO can sit tens of kHz from the recording's centre, and the
replay does not decode it. Before recording:

1. Turn on centre tuning with the toggle beside SDR++'s frequency display, so the frequency you tune is the centre
   and the VFO stays on it.
2. Tune to the carrier, then start the recording.

The `NNNHz` in SDR++'s default file name is that centre frequency, and the replay takes it as the recording's centre
(above). A carrier anywhere else in the recording is not decoded, whatever the VFO was on; record it again centred.

Headers a recording program did not finish:

- A RIFF header that was never finalized, as an interrupted recording leaves it (a `data` size of 0 or 0xFFFFFFFF in a
  container whose size is 0, 0xFFFFFFFF or past the end of the file, with `data` its last chunk), replays every whole
  frame to the end of the file. The replay logs a warning, and `--iq-info` reports `unfinalized header`.
- A header that declares more data than the file holds (a copy cut short) replays the whole frames the file has, with
  a warning. A partial frame at the end is not replayed.
- A finished header whose `data` chunk declares more than its container holds (a RIFF or `ds64` size that ends the
  container inside the data, a writer inconsistency) replays only the data inside the container, with a warning; bytes
  past the container's end are not samples, even when the file holds them.
- A `data` chunk that declares no data in a finished header is refused (`the data chunk is empty (no I/Q frames)`).

Large files: sizes and offsets are 64-bit on every platform, Windows included, so recordings past 2 GiB replay. Past
4 GiB a WAV needs RF64 or BW64, or a RIFF whose size fields its writer left at 0 or 0xFFFFFFFF, which replays to the end
of the file as above; a finished plain RIFF cannot describe more than 4 GiB.

At start the replay logs what it found, for example `IQ replay: WAV RIFF, 2-channel PCM 24-bit, 50000 Hz, ~1.5 s,
centre 467.913000 MHz (auxi chunk).`, or `centre unknown (no auxi chunk, none in the file name)`.

## Rate Chain And Resampling

Every replay start plans the rate chain the decode mode needs from the capture's rate, for a sidecar capture and a WAV
recording alike. The plan aims at a DSP rate D:

- the DSP bandwidth asked for, when one was: the DSP bandwidth row or the control API's `RTL_SET_BW`, or an RTL-SDR,
  rtl_tcp, SoapySDR or Airspy `[input]` with `rtl_bw_khz` in the config. `--iq-replay` replaces the `-i` input, so a
  bandwidth field in an `-i` spec does not reach a replay, and a decode-mode preset on the command line (`-fT`, `-fe`,
  `-fp` and the others that set a DSP bandwidth) replaces a config's request;
- otherwise the DSP bandwidth the sidecar recorded (`rtl_dsp_bw_khz`);
- otherwise (a WAV recording, or a sidecar that recorded none) the mode's preset bandwidth (72 kHz under `-fT`, 24 kHz
  under the EDACS/ProVoice presets) or the 48 kHz default.

Under TETRA a D that is not a multiple of 18 kHz from 36 to 144 kHz becomes 72 kHz. TETRA's pi/4-DQPSK runs at
18 ksym/s, its symbols are timed at a whole number of samples each (2 to 8), and its CQPSK path skips the decimation
after the demodulator (`post_downsample`), so the rate the half-band chain delivers must itself be one of those
rates.

The chain:

- **Kept as recorded.** A sidecar capture with no DSP bandwidth asked for runs the chain it recorded
  (`base_decimation`, `post_downsample`, `demod_rate_hz`), exactly as before, unless the mode is TETRA and the chain
  cannot run it: its rate after `base_decimation` is not a multiple of 18 kHz from 36 to 144 kHz, or its
  `post_downsample` is above 1. A TETRA chain that misses only through a power-of-two `post_downsample` folds it into
  the half-band chain where that reaches one of those rates. Every committed fixture keeps its chain, and so does
  every committed TETRA fixture under TETRA but the unmodified 50 kHz capture, which TETRA resamples.
- **Otherwise** (a WAV recording, a DSP bandwidth asked for, or a recorded chain TETRA cannot run): a capture at
  D x 2^k (k from 0 to 10) is decimated by 2^k in the half-band chain with no resampling. Any other capture is
  resampled first, to R = D x 2^k, the largest such rate at or below its own (R = D when the capture is slower than
  D), and then decimated by 2^k. `post_downsample` is 1 and the demodulator runs at D.

| Capture | DSP rate | Chain |
| --- | --- | --- |
| 48,254 Hz WAV | 72 kHz (`-fT`) | resampled to 72,000 Hz |
| 48,254 Hz WAV | 48 kHz | resampled to 48,000 Hz |
| 50 kHz `cs16` sidecar | 72 kHz (`-fT`) | resampled to 72,000 Hz |
| 48 kHz sidecar | 24 kHz asked for | decimated by 2, no resampling |
| 2.304 MS/s WAV | 72 kHz (`-fT`) | decimated by 32, no resampling |
| 2.048 MS/s | 72 kHz (`-fT`) | resampled to 1.152 MS/s, decimated by 16 |
| 2.4 MS/s | 48 kHz | resampled to 1.536 MS/s, decimated by 32 |

A plan is refused for a capture slower than 8 kHz, one that would need more than a 9x upsample, and one at
2 x 1024 x D or faster. The resampler also refuses a ratio it cannot hold within 250 ppm and a design that needs more
than 256 taps per phase. A refused plan stops the start and `dsd-neo` exits with status 1, for example `IQ replay:
'slow.wav' cannot run at the 48000 Hz DSP rate: the capture's rate (4000 Hz) is below the 8000 Hz a replay can
resample.` (with ` TETRA needs` after `DSP rate` under `-fT`). `--iq-info` shows a WAV's plan at 48 and 72 kHz, and
the reason where one is refused.

The DSP bandwidth asked for is kept as asked: the plan's own target never replaces it. A replay asked for 24 kHz runs
at 72 kHz while TETRA decodes and returns to 24 kHz when the mode leaves TETRA.

A resampled replay logs the resampler and the chain:

```text
IQ replay: resampling the 48254 Hz capture to 72000 Hz (L/M=4063/2723, +0.01 ppm, 40 taps per phase, group delay 0.41 ms).
Rate chain: capture=48254 Hz resampled to 72000 Hz (L/M=4063/2723, +0.01 ppm), base_decim=1, post=1 -> demod=72000 Hz; ...
```

- The resampler is a Kaiser-windowed sinc polyphase filter for I and Q together. It protects +-0.45 x D, the band the
  decoder reads after the half-band chain (+-32.4 kHz at TETRA's 72 kHz, which covers TETRA's +-12.15 kHz), with at
  most 0.1 dB of passband ripple and at least 60 dB against anything that would fold into that band.
- Its ratio L/M is exact where both reduced terms fit 4096, otherwise the nearest such ratio, within 250 ppm. The error
  shows as a sample-clock error of that size: +0.01 ppm for 48,254 to 72,000 Hz.
- Its group delay is not compensated. Pacing, media time, events and the decode clock count capture samples, so a
  resampled replay's decode timestamps run that much behind the capture time of the signal: 0.41 ms from 48,254 Hz to
  72 kHz, 0.45 ms from 48 kHz, up to about 2.5 ms from 8 kHz to 72 kHz.
- It starts over from silence at a `RETUNE`, a `RESET`, the samples a `MUTE` omitted, and an `--iq-loop` rewind, as the
  front end's filters do.
- A resampled chunk still spans no more than one live transfer (issue #626), and its input-level snapshot is taken
  from the capture's samples before resampling.
- The resampler runs only in a replay its plan resamples, in the replay reader thread. Live inputs and replays that
  run at their capture's rate do not run it, and a resampled replay costs the CPU its filter takes on top of the
  replay's usual work.

### Run-time Re-plan

During a replay, a change that plans another chain reopens the replay at the new chain where it stopped:

- a DSP bandwidth (the terminal DSP bandwidth row, the control API's `RTL_SET_BW`);
- a decode mode switch whose plan differs (a switch to TETRA on a 48 kHz replay, and back);
- a loaded config that leaves the replay running, for its `[mode]` and for an RTL-SDR, rtl_tcp, SoapySDR or Airspy
  `[input]` `rtl_bw_khz`.

A change that plans the chain already running reopens nothing: a DSP bandwidth chosen while TETRA runs at 72 kHz is
stored and takes effect when the mode leaves TETRA. A config's gain reopens nothing either, since a replay plays the
gain its capture recorded: a config with a gain and an `rtl_bw_khz` reopens the replay once, for the re-plan, where it
stopped.

An analog channel width beside the change is held to the rate the new chain runs at, not the one running: a config
that sets a 48 kHz `rtl_bw_khz` and a 25 kHz NFM width on a replay running at 24 kHz applies. Under a 25 kHz NFM width,
a config, DSP bandwidth row or `RTL_SET_BW` that narrows a 48 kHz replay to 24 kHz is refused before anything reopens,
and so is a switch from TETRA back to the Analog monitor whose chain returns to 24 kHz.

The resume is approximate. The stopped replay's reader leaves the position of the last capture chunk it handed the
demodulator, and the reopened replay starts there, on the centre the last `RETUNE` before it set. What was still in
the input ring or the decoder's symbol cache, at most one chunk plus that cache (a few milliseconds of capture), is not
replayed, and the front end's filters and the resampler start over, as at a `RESET`. The event schedule, the media
time, the `--iq-loop` pass count and the CU8 rotation phase go on from the checkpoint, so the decode clock stays on the
capture and never runs backwards: decode time steps forward by the samples skipped. The log says
`IQ replay: re-planning the rate chain for the TETRA decoder; resuming at capture byte <n>.`, and a mode switch's
toast names the new rate (`Decoding TETRA: replay resampled to 72 kHz`).

A plan no chain reaches is refused before anything stops: the replay keeps running at its chain, the mode, symbol
profile and DSP bandwidth go back to what they were, and the change fails with the planner's reason (logged as
`IQ replay: ... needs a rate chain this capture cannot run: ... The replay keeps its rate.`; a refused switch to TETRA
shows `this replay cannot be resampled to one`). A new chain that does not start puts the same settings back and
resumes the previous chain from the same checkpoint. When that start fails too, the replay is started again as any
failed change starts its input again: from the capture's start, with decode time back on the system clock.

## Raw I/Q Over The Network

The network input that carries raw I/Q is rtl_tcp: `-i rtltcp:host:port:freq:gain:ppm:bw` connects to a server that
speaks the rtl_tcp protocol, `rtl_tcp` itself or another that implements it. The server sends unsigned 8-bit I/Q
(`cu8`) after its `RTL0` header, and dsd-neo sets its frequency, sample rate, gain and correction over the
connection, so the server must honour those commands. For TETRA give it TETRA's 72 kHz DSP bandwidth:

```bash
dsd-neo -fT -i rtltcp:192.168.1.10:1234:426.7M:22:0:72
```

`-i tcp` and `-i udp` carry demodulated PCM audio, not I/Q (see `docs/network-audio.md`), so they cannot feed TETRA or
any other mode that needs I/Q. dsd-neo has no input for headerless I/Q streamed over TCP or UDP (`cf32` or otherwise)
from other SDR software. To decode TETRA from another SDR program, record a WAV I/Q recording there, centred on the
carrier (in SDR++, see [SDR++ Baseband Recordings](#sdr-baseband-recordings)), and replay it with `--iq-replay`.

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

The decoder's clock follows the capture as well. Under `--iq-replay`, decode time is the capture's start time (the
sidecar's `capture_started_utc`, or a WAV recording's start time, below) plus the capture time of the sample being
decoded, the time `MUTE` events omitted included:

- Timestamps in the decoder's output (the console, event and call history, logs and file names) show when the
  capture was made, in the local time zone as live, not when it was replayed. A `capture_started_utc` before
  2000-01-01T00:00:00Z, such as the `1970-01-01T00:00:00Z` the generated fixtures in `tests/fixtures/iq` carry, starts
  the clock at 2000-01-01T00:00:00Z instead; one that does not parse is logged and starts it there too. So does a WAV
  recording that records no start time, with `IQ replay: no start time recorded; decode times start at
  2000-01-01T00:00:00Z.`
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
  sample. When an interactive frontend restarts the stream (a gain or device change, or a stream restart) or switches
  the input away, the decode clock goes back to the system clock, and a replay that starts again within the same run
  decodes on the system clock. A run-time re-plan of the replay's rate chain (a DSP bandwidth or decode mode change,
  see [Run-time re-plan](#run-time-re-plan)) is the exception: it resumes where the replay stopped and stays on the
  capture's clock. A restart that replays the same capture logs this once, as "IQ
  replay restarted mid-run; decode timestamps now follow the system clock." Any call in progress ends first, on the
  capture's clock: its history row, per-call WAV and rdio-scanner sidecar keep the capture's times.
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

File input under `-r` or audio WAV input (`-i file.wav`) has no capture clock: it decodes on the system clock and is
unthrottled (only `.bin` symbol-capture replay is paced), so its call gaps, hangtime and staleness windows look shorter
than they were on air.

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

- The monitor runs at the demod rate the replay's rate chain plans (see
  [Rate Chain And Resampling](#rate-chain-and-resampling)). A sidecar capture keeps its recorded `demod_rate_hz`
  (48 kHz for every committed fixture) unless a DSP bandwidth is asked for, and then runs at that bandwidth, as an
  RTL-SDR does; a WAV recording runs at the DSP bandwidth in force (48 kHz by default). A chain kept at another rate,
  such as the 78,125 Hz an Airspy at 2.5 MS/s forces, is resampled to the 48 kHz output once, as live.
- A width the replay's demod rate cannot filter is refused with the DSP bandwidths that would fit (`...; set the DSP
  bandwidth to 48 or 72 kHz`), since a DSP bandwidth change re-plans the replay's rate.
- AM needs a replay that runs with `post_downsample` 1 (every committed fixture does): a sidecar that keeps a recorded
  chain decimating after the demodulator would run the AM channel filter at a multiple of the demod rate, so the
  stream start refuses it with `...; AM needs a capture with post_downsample 1`. A DSP bandwidth asked for re-plans the
  chain with `post_downsample` 1, which AM runs. FM replays such a capture as before.
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
- Two-channel WAV I/Q recordings replay directly (see
  [Replaying WAV I/Q Recordings From Other SDR Software](#replaying-wav-iq-recordings-from-other-sdr-software)), with
  their channel at the recording's centre. `tools/build_iq_fixtures.py` shows the conversion the committed analog
  fixtures were made with, which also centres an off-centre carrier: sample-exact WAV read, carrier centring,
  frequency-domain resampling to 48 kHz and a cu8 sidecar pair (`read_wav_iq`, `centre_carrier`,
  `resample_to_fixture_rate`, `write_fixture`).

## Operational Limits

- `--iq-capture` and `--iq-replay` are mutually exclusive in one invocation.
- Single-segment v1 captures continue to replay unchanged.
- A WAV recording replays as one segment with no events, as a virtual `cf32` capture: its byte counts and offsets (in
  `--iq-info`'s duration and a re-plan's resume) count 8 bytes per frame whatever its encoding.
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
  `--iq-loop`, but without the end-of-capture notice below.
- A pass that reads to the end of the capture with every chunk handed to the demodulator logs
  `IQ replay: end of capture after N samples`, where N is the capture's end position in complex samples at its own
  rate (before any resampling), counted from the capture's start: the whole capture, also for a replay that resumed
  partway through after a re-plan. It is logged once the decoder has read everything the pass decoded to: when the
  stream stops after a replay the decoder read to its end, just before `soft stopping...`, and under `--iq-loop` once
  per pass, just before the `--iq-loop rewound` line. A replay that stops early, fails to read, or cannot hand a chunk
  on does not log it, nor does an `--iq-loop` pass that handed the demodulator nothing, so a log without it did not
  replay the whole capture.
- Only the metadata-scheduled replay events retune a replay. The retunes a user asks for are refused during one, from
  the terminal, Qt/Android and the control API alike, with "An I/Q replay cannot retune.", as a failed command rather
  than a tune that never lands: a manual tune, a frequency entry, the tuner release, the return to the control
  channel (`return_cc`), and the channel cycle (`channel_cycle`: the next LCN, P25 control channel candidate or `-Y`
  row; `--trunk-scan` refuses a replay input altogether). The terminal hides its `Frequency...` row during a replay
  rather than open a prompt whose answer is refused. Commands whose main effect is not a retune still act: the scan
  hold and avoid, skip and lockout. The retunes trunking and the conventional scanner ask for are ignored, as
  described above.
- The tuned frequency the frontends show (the terminal `FRQ:` field, in MHz, the `FREQ` reading that opens the
  Qt/Android Monitor screen's signal strip, the Qt Spectrum label, the Android notification) follows the capture's
  recorded `RETUNE` events: it reads the centre the samples being decoded were captured on, the capture's opening
  centre until the first `RETUNE` and again after an `--iq-loop` rewind. The centre option the trunking paths read as
  the current channel keeps the opening centre, as before, so replay decoding is unchanged.
- Event timelines currently require a constant sample rate through the capture. Metadata with event sample-rate changes is
  rejected until segment-rate replay is supported.
- Replay converts `cu8`, `cf32` captured as the driver delivered it (`capture_stage: "post_driver_cf32_pre_ring"`),
  little-endian `cs16` stamped `post_mute_pre_widen`, and two-channel WAV I/Q recordings. `--iq-replay` refuses any
  other capture when it opens it, naming the sample format and stage, and `dsd-neo` exits with status 1: a `cf32` or
  `cs16` sidecar stamped with another stage. `--iq-info` still describes such a capture and reports `Replay compatible:
  no`.
- Direct `-i iqreplay:...` is intentionally rejected; use `--iq-replay <path>`.
- Replay currently feeds the RTL radio path and reuses existing demod processing/state handling.
- A replay with no known centre (a WAV recording with neither an `auxi` chunk nor a frequency in its name) opens,
  decodes and loops without one; the frequency check a live RTL start makes does not apply to a replay.
- `base_decimation` is capped at 1024 (10 half-band passes); metadata requesting more is rejected.
- A replay's rate follows its rate plan (see [Rate Chain And Resampling](#rate-chain-and-resampling)). A sidecar that
  keeps its recorded chain keeps its demod rate, so a capture whose demod rate yields a non-integer samples-per-symbol
  (for example 62,500 Hz at 4800 sym/s) is resampled after the demodulator to the resampler target (48,000 Hz by
  default) under the default `digital_resample = "auto"` policy. Decode output for such captures can therefore differ
  from releases that fed the raw demod rate through; set `digital_resample = "off"` to restore the previous behavior.
  See `docs/soapysdr.md` for the full policy.

## Backend Notes

- Capture is available on live radio inputs only: RTL USB, RTL-TCP, and SoapySDR when the active Soapy stream is `CF32`.
- RTL USB / RTL-TCP captures are `cu8`.
- `--iq-capture-format cf32` is only valid when the active backend stream is native `cf32` (for example Soapy CF32).
- Soapy drivers that only provide `CS16` can be used for live decode, but are not currently accepted by the IQ capture
  CLI.
- The metadata parser and public sample-format helpers recognize `cs16` metadata with 4-byte sample alignment. Replay
  converts little-endian interleaved `cs16` captured at `post_mute_pre_widen` to the same normalized float IQ stream
  the Soapy CS16 backend produces; `--iq-replay` refuses a `cs16` sidecar at any other capture stage at open
  (Operational Limits). Live IQ capture still writes only `cu8` and `cf32`, so a `cs16` sidecar is written by hand or
  by another tool, as `tests/fixtures/iq/tetra_scbs_real_air_50k.cs16.json` is for an unmodified upstream capture.
- If requested capture format does not match the active backend stream format, startup fails with a clear error.
