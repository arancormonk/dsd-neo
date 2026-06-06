# RTL Demod Pipeline Audit

This note documents the RTL-family digital demod path and the contracts that keep
clean symbols flowing into the legacy slicers. It covers RTL USB, RTL-TCP,
SoapySDR, and IQ replay; all four sources feed the same normalized symbol-output
contract into the legacy slicers.

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
   - FSK symbol output runs the channel LPF, then `dsd_fsk_modem_process()` on
     complex I/Q, producing one normalized float per symbol.
   - CQPSK symbol output runs the OP25-style chain:
     AGC -> band-edge FLL -> Gardner -> differential phasor -> Costas ->
     phase symbol scaling.
4. Symbol outputs are written to the RTL output ring without monitor volume,
   audio resampling, deemphasis, audio LPF, DC-block audio filtering, or legacy
   scalar matched filters.
5. `getSymbol()` detects RTL symbol output, forces `samplesPerSymbol = 1`, and
   applies fixed RTL symbol thresholds.
6. `digitize()` maps floats to dibits.
   - CQPSK uses fixed OP25-compatible thresholds at `-2, 0, +2` when the CQPSK
     DSP path is active for P25.
   - FSK/GFSK modes use the existing legacy threshold path, with RTL-provided
     symbol levels centered around `{-3,-1,+1,+3}` or `{-1,+1}`.

## Mode Matrix

| Mode | RTL output | Symbol rate | Levels | Channel LPF |
| --- | --- | ---: | ---: | --- |
| P25 Phase 1 C4FM | FSK symbols | 4800 | 4 | P25 C4FM |
| P25 Phase 1 CQPSK | CQPSK symbols | 4800 | 4 | P25 CQPSK |
| P25 Phase 2 CQPSK | CQPSK symbols | 6000 | 4 | P25 CQPSK |
| DMR | FSK symbols | 4800 | 4 | 12.5 kHz |
| NXDN48 | FSK symbols | 2400 | 4 | 6.25 kHz |
| NXDN96 | FSK symbols | 4800 | 4 | 12.5 kHz |
| D-STAR | FSK symbols | 4800 | 2 | 6.25 kHz |
| X2-TDMA | FSK symbols | 6000 | 4 | 12.5 kHz |
| YSF | FSK symbols | 4800 | 4 | 12.5 kHz |
| dPMR | FSK symbols | 2400 | 4 | 6.25 kHz |
| M17 | FSK symbols | 4800 | 4 | 12.5 kHz |
| ProVoice / EDACS | FSK symbols | 9600 | 2 | ProVoice |

## Filter And Rate Guardrails

- Channel LPF profiles are mode-specific and generated per demod sample rate.
  Protected edges are kept in the passband, not on the transition skirt.
- 12.5 kHz and CQPSK modes should use at least 16 kHz RTL DSP bandwidth, with
  24 or 48 kHz preferred for timing and data-decode margin.
- CQPSK timing recovery requires the correct samples-per-symbol:
  - P25 Phase 1 CQPSK: 10 SPS at 48 kHz, 5 SPS at 24 kHz.
  - P25 Phase 2 CQPSK: 8 SPS at 48 kHz, 4 SPS at 24 kHz.
- FSK symbol output uses the FSK modem clock directly, so it can handle
  non-integer clocks such as 9600 symbols/s at 24 kHz. That path still needs
  capture bandwidth wide enough to avoid shaving deviation energy.

## Regression Coverage

- `IO_RTL_DEMOD_CONFIG` validates the full mode matrix at 48 kHz and key 24 kHz
  cases, including output kind, symbol rate, level count, LPF profile, and CQPSK
  TED SPS.
- `RTL_SYMBOL_PIPELINE` synthesizes FSK and CQPSK symbols through `full_demod()`
  and checks the symbol-output contract:
  one float per symbol, expected 4-level decisions, and no audio-only
  post-processing on symbol streams.
- `DSP_FSK_MODEM` covers FSK modem normalization, 2-level and 4-level output,
  2400/4800 symbol clocks, CFO/noise tolerance, and reconfigure reset behavior.
- `DSP_DEMOD_MISC` checks channel LPF protected-edge gain for every LPF profile.

Run the focused audit checks with:

```bash
ctest --preset dev-debug --output-on-failure \
  -R '^(IO_RTL_DEMOD_CONFIG|RTL_SYMBOL_PIPELINE|DSP_FSK_MODEM|DSP_DEMOD_MISC)$'
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
