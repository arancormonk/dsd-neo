# RTL Demod Pipeline Audit

This note documents the RTL-family demod paths and the contracts that keep
clean samples and symbols flowing into the decoder, and clean audio into the
analog monitor. It covers RTL USB, RTL-TCP, SoapySDR, Airspy, and IQ replay; all
of them feed the same direct-output contracts. The analog monitor path is
described under [Analog Monitor Path](#analog-monitor-path).

## Source-To-Slicer Path

1. Capture ingestion converts backend I/Q into normalized interleaved float I/Q.
   - RTL USB and RTL-TCP consume CU8 I/Q.
   - IQ replay reuses the captured CU8/CF32 metadata and rate chain.
   - Optional fs/4 rotation is applied before samples enter the input ring when
     offset tuning is not active.
2. The demod thread reads complex samples from the input ring, waits for cold
   start configuration, and honors retune purge/mute gates before DSP state can
   train on new samples.
3. `full_demod()` applies baseband processing.
   - FSK direct output runs the channel LPF, then converts complex I/Q into
     centered PCM-like discriminator samples for `getSymbol()` sample-domain
     timing and slicing.
   - CQPSK symbol output runs the OP25-style chain:
     AGC -> band-edge FLL -> Gardner -> differential phasor -> Costas ->
     phase symbol scaling.
4. Direct outputs are written to the RTL output ring without monitor volume,
   audio resampling, deemphasis, audio LPF, DC-block audio filtering, or
   sample-domain matched filters.
5. `getSymbol()` detects CQPSK symbol output and uses the symbol-rate fast path.
   FSK discriminator output stays on the sample-domain timing/filter path.
6. `digitize()` maps floats to dibits.
   - CQPSK uses fixed OP25-compatible thresholds at `-2, 0, +2` when the CQPSK
     DSP path is active for P25.
   - FSK/GFSK modes use the existing sample-domain threshold path.

## Mode Matrix

| Mode | RTL output | Symbol rate | Levels | Channel LPF |
| --- | --- | ---: | ---: | --- |
| P25 Phase 1 C4FM | FSK discriminator | 4800 | 4 | P25 C4FM |
| P25 Phase 1 CQPSK | CQPSK symbols | 4800 | 4 | P25 CQPSK |
| P25 Phase 2 CQPSK | CQPSK symbols | 6000 | 4 | P25 CQPSK |
| DMR | FSK discriminator | 4800 | 4 | 12.5 kHz |
| NXDN48 | FSK discriminator | 2400 | 4 | 6.25 kHz |
| NXDN96 | FSK discriminator | 4800 | 4 | 12.5 kHz |
| D-STAR | FSK discriminator | 4800 | 2 | 6.25 kHz |
| X2-TDMA | FSK discriminator | 6000 | 4 | 12.5 kHz |
| YSF | FSK discriminator | 4800 | 4 | 12.5 kHz |
| dPMR | FSK discriminator | 2400 | 4 | 6.25 kHz |
| M17 | FSK discriminator | 4800 | 4 | 12.5 kHz |
| ProVoice / EDACS | FSK discriminator | 9600 | 2 | ProVoice |

## Filter And Rate Guardrails

- Channel LPF profiles are mode-specific and generated per demod sample rate.
  Protected edges are kept in the passband, not on the transition skirt.
- 12.5 kHz and CQPSK modes should use at least 16 kHz RTL DSP bandwidth, with
  24 or 48 kHz preferred for timing and data-decode margin.
- CQPSK timing recovery requires the correct samples-per-symbol:
  - P25 Phase 1 CQPSK: 10 SPS at 48 kHz, 5 SPS at 24 kHz.
  - P25 Phase 2 CQPSK: 8 SPS at 48 kHz, 4 SPS at 24 kHz.
- FSK discriminator output uses `getSymbol()` sample-domain timing. That path
  needs capture bandwidth wide enough to avoid shaving deviation energy.

## Analog Monitor Path

`-fA` runs the RTL front end in the analog receive family: `AUDIO_MONITOR`
output, the FM discriminator (`dsd_fm_demod`), de-emphasis, and the monitor
resampler to 48 kHz. The M17 encoder shares that output kind but is not the
analog family: the width-driven channel filter, its validation and live family
switching do not apply to it, and it keeps the fixed WIDE/FM path. The State
Hygiene rules key on the output kind, so they cover every monitor stream, the
M17 encoder's included.

Per block: half-band decimation, channel LPF, carrier squelch, FM
discrimination, de-emphasis, optional audio LPF, DC block, and the squelch
envelope.

### Channel Width

The analog channel filter is designed from a full RF channel width `W`: the
cutoff is `W/2 + 600 Hz` with a fixed 1200 Hz Blackman transition outside the
protected passband (about -0.3 dB at `W/2`, -30 dB at `W/2 + 1200 Hz`, -50 dB
from about `W/2 + 1450 Hz`). The width is neither the tuner bandwidth nor the
audio bandwidth. `W = 16000` runs the same design call as the historical WIDE
profile, so its taps are bit-identical wherever that design succeeded. That
identity fixes the skirt: 50 dB at `W/2 + 1200 Hz` would take a longer design
than WIDE's, so `DSP_CHANNEL_FILTERS` pins about -30 dB (bound -29 dB) from
`W/2 + 1200 Hz` and -50 dB from `W/2 + 1500 Hz`.

A width is realizable at DSP rate `R` only when `W/2 + 600 <= 0.45 x R` and the
Blackman tap count `74 x R / 26400` (odd) fits the 288-tap analog capacity:

| DSP rate | Largest width |
| ---: | ---: |
| 48 kHz | 42 kHz |
| 24 kHz | 20.4 kHz |
| 16 kHz | 13.2 kHz |
| 12 kHz | 9.6 kHz |
| 8 kHz | 6 kHz |
| 6 kHz | 4.2 kHz |
| above ~102.7 kHz | none (tap capacity) |

There is no Nyquist clamp and no fallback prototype on this path: an
unrealizable width is refused with a message naming the width, the DSP rate,
the largest width it fits and the RTL DSP bandwidths that would fit. Digital
profiles are unchanged (144-tap cap and 63-tap fallback).

Enable rule and validation:

- The unset default (16 kHz NFM) keeps the historical rule: the channel LPF is
  on from a 20 kHz `rate_in`, or as `DSD_NEO_CHANNEL_LPF` says. Where the rate
  cannot fit 16 kHz, the legacy WIDE plan stays in charge instead of failing,
  and frontends see the channel as DSP-limited at the width that plan passes
  (`dsd_channel_lpf_legacy_wide_width_hz()`): below ~19.1 kHz the 144-tap WIDE
  design with its cutoff held to 0.9 x Nyquist (13.2 kHz at 16 kHz), and above
  the ~102.7 kHz tap capacity the 63-tap fallback prototype, cut at a third of
  the rate (about 78.5 kHz at 128 kHz), rather than the whole DSP span.
- An explicit width turns the channel LPF on. With `DSD_NEO_CHANNEL_LPF=0` it
  is refused. The unset AM default is held to the same rules as an explicit
  width (AM itself is refused until the front end can demodulate it).
- The check that counts runs at stream start once the rate chain is final,
  including a rate the device forced; a refusal fails the start. A runtime
  request is checked against the running stream's rate; with no stream running
  only the kind, range and environment rules apply, and the next start decides.
  A refused runtime request (live, or attached to a retune target) is logged
  with the validator's text and what stays in place, once per kind, width and
  rate until an analog request is accepted. Both are checked again where they
  land: a retune can settle the device on another rate after a live request was
  checked against the published rate, so the demod thread holds the request to
  the rate it is on when it applies it, and refuses it there, as it would have
  had the retune come first. That includes a request that moves the stream onto
  the monitor (see Live Switching below).
- The design uses `rate_out`, which is the complex rate the channel filter runs
  at only while `post_downsample` is 1. Live sources always run that way; only
  IQ replay sidecars can set a larger post-demod decimation, and there a
  requested width (explicit, or the AM default) is refused at start and on
  every runtime request, since the filter would run at `rate_out` x
  `post_downsample`. The unset NFM default keeps the legacy design, and is
  published as DSP-limited at the width that design passes at the rate it
  really runs at (its width at `rate_out` times `post_downsample`).
- Device-forced rates above ~51.4 kHz (for example Airspy at 2.5 MS/s, demod
  rate 78,125 Hz) used to fall back to the 63-tap prototype designed for 24 kHz.
  They now get a real design (219 taps at 78,125 Hz).
- After the DC block, `low_pass_real()` takes the audio from `rate_in` to
  `rate_out2`, and the rational resampler then takes `rate_out` to 48 kHz. A
  live open sets `rate_in` and `rate_out2` to the DSP bandwidth, so that stage
  passes audio through unchanged even at a forced rate. IQ replay takes
  `rate_in` from the capture and sets `rate_out2` to match; it used to leave
  `rate_out2` at the sidecar's DSP bandwidth, which resampled a capture at any
  other rate twice (a 1 kHz tone recorded at 78,125 Hz played back at 1628 Hz,
  in 0.61 of its duration).

### State Hygiene

- De-emphasis and audio-LPF coefficients are recomputed from stored settings
  (`deemph_tau_us`, `audio_lpf_cutoff_hz`) every time the rate chain is
  finalized, so they follow the rate the device actually delivers. At unforced
  rates this is bit-identical.
- A retune on any `AUDIO_MONITOR` stream resets the de-emphasis, DC, audio-LPF
  and squelch-envelope state and the channel, half-band and resampler histories
  to their fresh-open values.
- When a retune leaves the stream on a different demod rate (a device that
  settles on another rate than it delivered before), the analog channel is
  resolved again for that rate: the unset default moves between the 16 kHz
  design and the legacy WIDE design, published as DSP-limited where the rate
  cannot fit 16 kHz. An explicit width the new rate cannot realize is never
  clamped, swapped for the legacy design, or run without its channel filter:
  the retune is refused before it finalizes
  (`controller_refuse_retune_for_analog_width()`), logged with the validator's
  text (once per kind, width and rate), the device goes back to the capture
  frequency and rate it had, where the width runs, and the retune fails as a
  retune that could not be applied does (`RTL_STREAM_TUNE_FAILED`,
  `NOTICE: Retune failed`), also when the centre it keeps is the one it asked
  for (a retune to the running centre, or one before any centre was applied).
  A retune profile for the target that switches to
  the digital family, or to an analog width the new rate fits, takes the
  monitor's width away and is not refused for it; one that carries only a
  symbol profile is held like a bare retune. Should the device report another
  rate the width cannot run at even after it was put back, no receive profile
  the stream can keep runs that width, so the stream stops as a start at that
  rate fails: logged with the validator's text, reported as a configuration
  input failure. This applies only while the monitor output runs on the analog
  channel: CQPSK toggled on under `-fA`, or a typed digital scan row's profile,
  keeps its own profile filter. A retune asks the device for the same capture
  rate every time, so this guards against a device that reports a different
  rate than it did before.

### Live Switching

`rtl_stream_request_analog_profile()` queues a receive-family request (family,
analog kind, width) that the demod thread applies between blocks, ahead of any
demod profile queued after it. A demod profile still queued from before it is
dropped: the analog family has no symbol clock, and a digital family lands on
the symbol profile queued after it, never on an older one (a CQPSK toggle
drained in the same pass of the command queue as the mode change):

- width-only change: new filter plan from empty channel and half-band
  histories, with the output ring, the generation and the monitor audio state
  left alone (a request for the width already running changes nothing);
- analog <-> digital: the new family's fresh-open defaults (output kind,
  demodulator, de-emphasis, channel filter, resampler), carrier and timing loops
  restarted as on an open (Costas, band-edge FLL, Gardner TED), the I/Q DC and
  balance estimates, the squelch dwell toward a multi-frequency hop and a
  replay's post-demod decimator zeroed as on an open, a cleared output ring and
  a bumped output generation. The stream records the
  family it switched to: its options are the orchestrator's copy from before
  the open, which a decoder-side mode change never reaches, so from then on that
  record decides whether a symbol profile without CQPSK runs the FSK
  discriminator or monitor audio (a `-fA` session switched to DMR stays on the
  discriminator through a CQPSK profile and back). The digital symbol profile follows as a
  demod profile request, and it decides the CQPSK family, unless `DSD_NEO_CQPSK`
  is set: that override decides it as it does at stream open, and the channel
  filter follows the family the switch lands on (the P25 CQPSK profile for CQPSK,
  P25 C4FM for a 4800 sym/s P25 CQPSK request turned onto the discriminator) and
  the open's enable rule: while the channel filter is off (`DSD_NEO_CHANNEL_LPF=0`,
  or a DSP rate below 20 kHz by default) an open on the discriminator keeps the
  WIDE profile, and so does the switch, whatever profile the mode names. The
  override does not reach the analog family: a `-fA` open runs the FM monitor
  under `DSD_NEO_CQPSK=1` (or a QPSK modulation), as the switch to analog does.
  The family request
  waits for that profile: when the demod thread reaches a block boundary between
  the two requests, it keeps the family request queued, so both apply at one
  boundary. The digital resampler (and so the output rate) is decided for that
  profile, as an open of it would decide it: at a forced rate such as 78,125 Hz
  CQPSK never resamples and a 2400 sym/s profile at 60 kHz needs no resampling,
  where the analog monitor's 4800 sym/s placeholder would have resampled both to
  48 kHz. The integer-SPS flag (`sps_is_integer`) is derived for that profile
  too, from the demod rate as an open derives it, and the TED keeps the open's
  floor of two samples per symbol (ProVoice's 9600 sym/s at a 12 kHz DSP rate
  gets two, where the symbol-profile setter alone would leave one);
- FM <-> AM: demodulator and de-emphasis swap with a monitor-state reset (AM is
  refused until the front end can demodulate it).

The decoder keeps reading the output ring while the demod thread clears it for a
switch. A read holds the ring's `ready_m` from its tail snapshot to its tail
store, and the clear takes it too, so a read in flight finishes before the clear
(and is discarded by the generation bump) instead of storing its old tail over
the cleared indices, which would read as a ring full of the old family's samples.
A read can also load the generation after that bump and still reach the ring
before the clear, taking samples the clear was meant to drop under the bumped
generation, so the clear bumps the generation again under `ready_m` once the
ring is empty: the generation the stream runs on after a switch is one only a
read made after the clear can load.

A decode-mode change, from the mode control or a config apply, asks the
running front end first (`rtl_stream_check_analog_profile()`, which holds the
analog profile to the rate the stream publishes): a mode whose analog profile
the front end would refuse fails with a toast and leaves the decoder's mode as
it was, so the decoder and the front end agree about the family. The request
the command makes once it has committed is held to the same rules again: to
the published rate when it is made, and to the rate the demod thread finds
when it applies it, like any analog request. Only a retune that moves the rate
between the check and the switch gets it refused there; the refusal is logged
with the validator's text and the front end stays on the digital family,
rather than run the width without its channel filter (the decoder keeps the
Analog mode it committed to, and the log names the fix). The same holds for
the channel-scan leave, which restores an analog session's configured family,
and for a width change on the running monitor, which keeps its profile when
refused. The unset NFM default is never refused for its rate: a switch lands on
the legacy design or with the channel filter off, as an open at that rate does.

Toggling CQPSK on under `-fA` leaves the analog family flag set but takes the
output off the monitor, so that stream keeps its P25 CQPSK profile filter and
publishes no analog profile. A typed digital scan row's symbol profile on an
analog session keeps the monitor output but puts the row's channel profile in
place of the analog (WIDE) one, so the row filters with its own profile, as it
did before the analog filter became width-driven, and publishes no analog
profile either. Neither leaves the analog family: the stream still reports it
(`rtl_stream_analog_family_active()`), and a digital family request leaves it
from either state, onto the same fresh open of the digital mode a switch from
the monitor output lands on (without it, the digital mode's symbol profile
would land on the analog session, where a profile without CQPSK runs monitor
audio). The decoder asks for the digital family only when its configured mode
is digital (`svc_publish_symbol_profile()` reads the scan baseline under a row
constraint), so a scoped command republishing a typed digital row's profile on
an analog session queues that profile alone and the row keeps its monitor
output. An analog request brings the monitor back from either state. The
modulation auto-switch never does this: frame sync stands it down in the analog
family (`dsd_opts_is_analog_family()`), because a carrier within a few hertz of
0 Hz votes for CQPSK there and the switch used to follow that vote onto the P25
CQPSK path (`FRAME_SYNC_INTERNAL_HELPERS`,
`DECODE_IQ_ANALOG_NO_MOD_AUTO_SWITCH`).

Retune profiles carry the same fields bound to their target frequency; an analog
one applies no symbol profile, CQPSK toggle or timing queued for the same
target. Its width is checked again when the retune lands, against the demod rate
then in force (a profile queued with no stream running was checked only against
the kind and range rules). A width that rate cannot realize is refused with the
validator's text, logged once per kind, width and rate, and the front end keeps
its receive profile. Because the switch lands after the requesting command
returns, `rtl_stream_output_rate_for_family()` predicts the output rate a
pending switch will produce; the decoder uses it to set symbol timing when
leaving analog.

### Output Scale

Live radio sets `output_scale = 1/pi` in `optimal_settings()`; IQ replay never
calls it. `output_scale` belongs to the process-wide demod state and nothing
resets it, so a replay in a process that has not run a live stream (the CLI's
`--iq-replay`, the replay tests) leaves the discriminator output unscaled, and
replayed FM monitor audio is about pi times louder than live; a replay opened
after a live session in the same process keeps the 1/pi that session set. The
scale applies to monitor audio only: `demod_write_output_block()` skips it for
FSK discriminator and CQPSK symbol output, so digital decoding and the digital
`DECODE_IQ_*` baselines do not depend on it. The analog replay checks
(`DECODE_IQ_ANALOG_*`) do: their level bounds (RMS, peak and audible thresholds
in dBFS) were measured at the replay scale. Ratio-based audio metrics are
invariant to it, so it is left as is.

## Regression Coverage

- `IO_RTL_DEMOD_CONFIG` validates the full mode matrix at 48 kHz and key 24 kHz
  cases, including output kind, symbol rate, level count, LPF profile, and CQPSK
  TED SPS. It also covers the retune finalize path, which preserves the active
  symbol profile; deriving the profile from the enabled decoders is stream-open
  behavior only.
- `RTL_SYMBOL_PIPELINE` synthesizes FSK discriminator samples and CQPSK symbols
  through `full_demod()` and checks the direct-output contracts.
- `DSP_FSK_MODEM` covers discriminator count, sign, centering, scale, and reset
  behavior.
- `DSP_DEMOD_MISC` checks channel LPF protected-edge gain for every LPF profile,
  the analog width's protected edge, and plan-cache invalidation on a width
  change.
- `DSP_CHANNEL_FILTERS` pins the 16 kHz analog taps to the WIDE taps at 24000,
  46875 and 48000 Hz, checks the passband/stopband of each width, the 288-tap
  design at forced rates, that unrealizable widths fail rather than clamp, and
  that the width reported for the legacy WIDE plan is the passband that plan
  has, the 63-tap fallback included.
- `RUNTIME_ANALOG_CHANNEL` covers the width ranges, parser and validator text
  across DSP rates.
- `IO_RTL_DEMOD_CONFIG` covers the analog enable rule at 12/16/24/48 kHz, the
  explicit-width and `DSD_NEO_CHANNEL_LPF=0` cases, the unchanged M17 encoder, a
  `-fA` open under `DSD_NEO_CQPSK=1` (FM monitor) and the AM refusal; `IO_RTL_RETUNE_PREPARE` covers the analog retune resets, the
  coefficient refresh after a forced rate change, the channel a rate change
  resolves (the fallback prototype's width published past the tap capacity),
  the retune refused when its rate cannot realize an explicit width (the
  capture, centre, rate and width put back, and the retune completing as
  failed, a retune to the running centre included), the stop when the device does not
  return to a rate that fits it, and analog retune profiles;
  `IO_RTL_ANALOG_FAMILY_SWITCH` checks that digital -> analog -> digital ends on
  a fresh open for P25 C4FM/CQPSK, DMR, NXDN48 and dPMR, at unforced rates and at
  forced 78,125 and 60,000 Hz rates, under `DSD_NEO_CQPSK=0`/`=1` and with the
  channel filter off (`DSD_NEO_CHANNEL_LPF=0`, a 12 kHz DSP rate), with loop state, monitor audio state, the
  I/Q corrections, the post-demod decimator and the channel, half-band and
  resampler histories and the squelch dwell included, a typed digital row on a
  `-fA` session and on a
  DMR session switched to analog, a `-fA` session a CQPSK toggle or a typed
  digital row had moved off the monitor output switched to digital, and one
  whose CQPSK toggle was still queued when the digital mode was picked (each
  equal to a fresh open too), and covers width-only
  changes, requests made with no stream running, and live requests and retune
  profiles a running stream's rate or post-demod decimation cannot realize
  (refused before anything is queued, or at the rate a retune moved the stream
  to before the demod thread consumed it), a DMR session's switch onto the
  monitor refused the same ways (at its rate, at the rate a retune landed
  before the switch was consumed, and when requested after a retune moved the
  rate its check accepted), with the unset default switching at a rate that
  cannot fit 16 kHz instead, all with the stream keeping the
  options snapshot it opened with, a switch whose ring clear meets a decoder
  read between its copy and its tail store (`RUNTIME_RINGS` holds the replay
  reader to the same contract), and one whose clear a read loading the bumped
  generation reaches the ring before; `IO_RTL_ANALOG_OPEN` opens IQ
  replays whose demod rate differs from their DSP bandwidth and checks the
  start-time channel decision and refusal, the width a post-demod decimating
  replay publishes, that a tone captured at 78,125 Hz reaches the output once,
  at 48 kHz and at its own frequency, and that a `-fA` replay switched to DMR
  through the stream API runs the FSK discriminator and returns to the monitor.

Run the focused audit checks with:

```bash
ctest --preset dev-debug --output-on-failure \
  -R '^(IO_RTL_DEMOD_CONFIG|RTL_SYMBOL_PIPELINE|DSP_FSK_MODEM|DSP_DEMOD_MISC|DSP_CHANNEL_FILTERS|IO_RTL_ANALOG_FAMILY_SWITCH|IO_RTL_ANALOG_OPEN|RUNTIME_ANALOG_CHANNEL|IO_RTL_RETUNE_PREPARE)$'
```

For release readiness, run the full suite:

```bash
ctest --preset dev-debug --output-on-failure
```

## Open Audit Items

- Capture-backed IQ replay vectors should be added for real-world marginal
  signals once representative captures are available.
- Any future change to channel LPF cutoffs, default RTL DSP bandwidth, CQPSK
  loop gains, or FSK normalization must update the mode matrix tests first.
- The live (1/pi) versus cold-start replay (unscaled) analog output scale
  difference is documented above and left in place; aligning it moves replayed
  monitor audio levels, so the analog replay checks' dBFS bounds would need new
  baselines (digital output never takes the scale).
- The forced-rate analog channel filter (Airspy 2.5 MS/s -> 78,125 Hz) needs a
  hardware listen check.
