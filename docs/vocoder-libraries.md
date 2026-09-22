# Vocoder Libraries & Automatic Matching in dsd-neo

> **Scope:** This document covers the vocoder libraries in active use by dsd-neo, equivalent/alternative
> libraries in the broader digital-radio ecosystem, and a detailed walkthrough of the automatic
> vocoder-selection mechanism already built into dsd-neo.
>
> See also `docs/vocoder-hal.md` for the HAL interface design.

---

## 1. Libraries Currently Used in dsd-neo

### 1.1 mbelib-neo (required)

| Property | Value |
|----------|-------|
| Upstream | https://github.com/arancormonk/mbelib-neo |
| Fork of | https://github.com/szechyjs/mbelib |
| License | GPL-2.0-or-later |
| vcpkg port | `mbe-neo` (in `vcpkg-ports/mbe-neo/`) |
| CMake target | `mbe_neo::mbe_shared` |
| Header | `#include <mbelib.h>` |

**What it provides:** ECC, demodulation, and synthesis for all IMBE and AMBE frame variants used by P25,
ProVoice, D-STAR, DMR, NXDN, YSF, X2-TDMA, and dPMR.

Key API surface used by dsd-neo:

```
IMBE 7200x4400 (P25p1)
  mbe_eccImbe7200x4400C0()          – class-0 ECC
  mbe_demodulateImbe7200x4400Data() – demodulation
  mbe_eccImbe7200x4400Data()        – class-1/2 ECC, fills imbe_d[88]
  mbe_processImbe4400Dataf()        – synthesis → float[160] PCM

IMBE 7100x4400 (ProVoice)
  mbe_eccImbe7100x4400C0/Data()
  mbe_demodulateImbe7100x4400Data()
  mbe_convertImbe7100to7200()       – convert frame for synthesis

AMBE 3600x2400 (D-STAR)
  mbe_processAmbe3600x2400Framef()  – all-in-one

AMBE+2 3600x2450 EHR (DMR / P25p2 / NXDN / YSF / X2-TDMA / dPMR)
  mbe_eccAmbe3600x2450C0/Data()
  mbe_demodulateAmbe3600x2450Data()
  mbe_processAmbe2450Dataf()        – synthesis → float[160] PCM
  mbe_processAmbe2400Dataf()        – D-STAR file playback path

Lifecycle
  mbe_initMbeParms()                – reset model-parameter state
```

### 1.2 Codec2 (optional)

| Property | Value |
|----------|-------|
| Upstream | https://github.com/arancormonk/codec2 (pinned fork; upstream: https://github.com/drowe67/codec2) |
| License | LGPL-2.1-or-later |
| vcpkg port | `codec2` (in `vcpkg-ports/codec2/`) |
| Compile guard | `#ifdef USE_CODEC2` |
| CMake feature | `dsd-neo_feature_codec2` → `-DUSE_CODEC2` |
| Header | `#include <codec2/codec2.h>` |

**What it provides:** Open-source narrowband speech codec, used exclusively by the M17 protocol module.

Modes used in dsd-neo:

| Mode | Bit-rate | Frames decoded per M17 stream frame |
|------|----------|-------------------------------------|
| `CODEC2_MODE_3200` | 3.2 kbps | 2 × 8 bytes / 160 PCM samples each |
| `CODEC2_MODE_1600` | 1.6 kbps | 1 × 8 bytes / 320 PCM samples |

Lifecycle: created in `dsd_init.c` (`codec2_create()`), destroyed on shutdown (`codec2_destroy()`).
Both contexts live in `dsd_state.codec2_3200` / `dsd_state.codec2_1600` as opaque `struct CODEC2*` pointers
(always present for ABI stability; `NULL` when `USE_CODEC2` not compiled in).

### 1.3 TETRA ACELP (external subprocess, runtime)

| Property | Value |
|----------|-------|
| Standard | ETSI EN 300 395-2 (proprietary, no open-source implementation) |
| Integration | Persistent child process (`TETRA_VOCODER_CMD` env var) |
| Protocol | 137 bytes/frame → 240 × int16 PCM16LE @ 8 kHz |
| Source | `src/protocol/tetra/tetra_acelp.c` |
| Test stub | `tools/tetra/vocoder_stub.py` (produces silence) |

If `TETRA_VOCODER_CMD` is unset, TETRA voice frames are fully decoded (FEC, Viterbi, class reorder)
but audio output is suppressed—no codec2/mbelib fallback is attempted.

The top-level build does not compile this codec and must not gain a fetch step for it. A developer who already has the ETSI `C-CODE` sources builds `tools/tetra/acelp_adapter` as a second CMake project and points `TETRA_VOCODER_CMD` at that executable. Those sources are not committed. Linux and macOS start the command through `sh -c`; Windows starts it through `cmd.exe /C`.

---

## 2. Vocoder Library Ecosystem

The table below covers the libraries relevant to dsd-neo and the broader radio ecosystem.

### 2.1 Open-source libraries

| Library | Bit-rates | License | Protocols covered | Notes |
|---------|-----------|---------|-------------------|-------|
| **mbelib-neo** | IMBE/AMBE (decode only) | GPL-2.0+ | P25p1, P25p2, DMR, D-STAR, NXDN, YSF, X2-TDMA, dPMR, ProVoice | Required by dsd-neo; fork of szechyjs/mbelib with API extensions |
| **Codec2** | 0.7–3.2 kbps | LGPL-2.1+ | M17, FreeDV | Optional in dsd-neo; official STM32 port available |
| **LPCNet** | 1.6 kbps | BSD | (none yet in dsd-neo) | Neural-net vocoder; near-natural quality; requires Cortex-A class CPU |
| **Opus** | 6–510 kbps | BSD | PoC/VoIP | Too wide-band for narrowband radio; used in push-to-talk apps |
| **Speex** | 2.15–44.2 kbps | BSD | legacy VoIP | Superseded by Opus; still present in some embedded systems |
| **mbelib (original)** | IMBE/AMBE (decode) | ISC | P25, DMR, D-STAR, NXDN | szechyjs/mbelib; encode not supported; dsd-neo migrated to mbelib-neo |

### 2.2 Commercial / proprietary codecs

| Codec | Bit-rates | Protocols | Availability |
|-------|-----------|-----------|--------------|
| **AMBE+2** (DVSI) | 2.0–9.6 kbps | DMR, P25p2, dPMR, NXDN, D-STAR, YSF, X2-TDMA, PDT | DVSI chip (AMBE-3000/3003/4020) or SDK (IP license) |
| **IMBE** (DVSI) | 4.4 kbps | P25 Phase 1, ProVoice | DVSI chip or SDK |
| **ACELP** (ETSI) | ~6 kbps | TETRA TCH/FS | Proprietary; dsd-neo delegates to external subprocess |

### 2.3 Protocol-to-library mapping

| Protocol | Vocoder | Library used in dsd-neo | TDMA slots |
|----------|---------|-------------------------|------------|
| P25 Phase 1 | IMBE 7200x4400 | mbelib-neo | 1 |
| P25 Phase 2 | AMBE+2 EHR | mbelib-neo | 2 |
| DMR BS/MS | AMBE+2 EHR | mbelib-neo | 2 |
| D-STAR | AMBE 3600x2400 | mbelib-neo | 1 |
| NXDN 48/96 | AMBE+2 EHR | mbelib-neo | 1 |
| YSF VD/VW/FR | AMBE+2 EHR | mbelib-neo | 1 |
| X2-TDMA | AMBE+2 EHR | mbelib-neo | 2 |
| dPMR | AMBE+2 EHR | mbelib-neo | 2 |
| ProVoice / EDACS | IMBE 7100x4400 | mbelib-neo | 1 |
| M17 (full-rate) | Codec2 3200 bps | Codec2 (optional) | 1 |
| M17 (half-rate) | Codec2 1600 bps | Codec2 (optional) | 1 |
| TETRA TCH/FS | ACELP | External subprocess | 2 (NDB Block 1 + Block 2 = one TCH/FS pair) |

---

## 3. Automatic Vocoder Matching

dsd-neo selects the correct vocoder automatically at runtime.  There are no manual codec configuration
switches—codec selection is derived deterministically from the protocol identified by the DSP layer.

### 3.1 The matching chain

```
RF signal (IQ samples / audio)
         │
         ▼
┌─────────────────────────────────────────────────────────────────────┐
│  DSP / Frame-Sync  (src/dsp/dsd_frame_sync.c)                       │
│                                                                      │
│  getFrameSync() scans the bitstream for known sync words:           │
│    P25p1 NID, X2-TDMA sync, YSF sync, TETRA SB/NDB, M17 LSF/STR,  │
│    DMR BS/MS burst, ProVoice, NXDN, dPMR FS1–4, D-STAR sync, …    │
│                                                                      │
│  On match → state->lastsynctype = DSD_SYNC_<PROTOCOL>_{POS|NEG}    │
│           → state->synctype set identically for the voice frame     │
└──────────────────────────────────┬──────────────────────────────────┘
                                   │ state->synctype
                                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Engine / Dispatch  (src/engine/dispatch/)                          │
│                                                                      │
│  Reads state->synctype → routes bitstream to correct protocol module│
└──────────────────────────────────┬──────────────────────────────────┘
                                   │
         ┌─────────────────────────┼───────────────────────────┐
         │                         │                           │
         ▼                         ▼                           ▼
  P25/DMR/NXDN/…             M17 module              TETRA module
  protocol module       (src/protocol/m17/)   (src/protocol/tetra/)
         │                         │                           │
         │ processMbeFrame()       │ M17processCodec2_*()     │ tetra_acelp_process_tch()
         ▼                         ▼                           ▼
  Vocoder HAL              Codec2 library          External subprocess
  (core/vocoder/)          (optional)              (TETRA_VOCODER_CMD)
```

### 3.2 Sync-word detection table

The DSP layer matches these sync patterns to identify the protocol (and therefore the vocoder):

| `DSD_SYNC_*` constant | Decimal | Protocol | Vocoder selected |
|-----------------------|---------|----------|-----------------|
| `DSD_SYNC_P25P1_POS/NEG` | 0, 1 | P25 Phase 1 | IMBE 7200x4400 |
| `DSD_SYNC_X2TDMA_*` | 2–5 | X2-TDMA | AMBE+2 EHR |
| `DSD_SYNC_DSTAR_VOICE_POS/NEG` | 6, 7 | D-STAR voice | AMBE 3600x2400 |
| `DSD_SYNC_M17_STR_POS/NEG` | 8, 9 | M17 stream | Codec2 3200/1600 |
| `DSD_SYNC_DMR_BS_*` | 10–13 | DMR BS | AMBE+2 EHR |
| `DSD_SYNC_PROVOICE_POS/NEG` | 14, 15 | ProVoice/EDACS | IMBE 7100x4400 |
| `DSD_SYNC_M17_LSF_POS/NEG` | 16, 17 | M17 LSF | Codec2 (type from LSF) |
| `DSD_SYNC_DSTAR_HD_POS/NEG` | 18, 19 | D-STAR header | (no voice) |
| `DSD_SYNC_DPMR_FS1–4_POS/NEG` | 20–27 | dPMR | AMBE+2 EHR |
| `DSD_SYNC_NXDN_POS/NEG` | 28, 29 | NXDN 48/96 | AMBE+2 EHR |
| `DSD_SYNC_YSF_POS/NEG` | 30, 31 | YSF | AMBE+2 EHR |
| `DSD_SYNC_DMR_MS_VOICE` | 32 | DMR MS voice | AMBE+2 EHR |
| `DSD_SYNC_DMR_MS_DATA` | 33 | DMR MS data | (no voice) |
| `DSD_SYNC_DMR_RC_DATA` | 34 | DMR RC data | (no voice) |
| `DSD_SYNC_P25P2_POS/NEG` | 35, 36 | P25 Phase 2 | AMBE+2 EHR |
| `DSD_SYNC_EDACS_POS/NEG` | 37, 38 | EDACS | (trunking control) |
| `DSD_SYNC_TETRA_NDB_POS/NEG` | 41, 42 | TETRA Normal Downlink | ACELP (subprocess) |
| `DSD_SYNC_TETRA_SB_POS/NEG` | 43, 44 | TETRA Sync Burst | (no voice) |
| `DSD_SYNC_M17_PKT_POS/NEG` | 86, 87 | M17 packet | Codec2 / data |
| `DSD_SYNC_M17_PRE_POS/NEG` | 98, 99 | M17 preamble | (sync only) |

### 3.3 Codec dispatch in `processMbeFrame()` / `soft_mbe()`

For protocols that use mbelib-neo, the dispatch is a simple if/else chain on `state->synctype`:

```c
// src/core/vocoder/dsd_mbe.c  (condensed)

if (DSD_SYNC_IS_P25P1(state->synctype)) {
    // ── IMBE 7200x4400 ─────────────────────────────────
    state->errs  = mbe_eccImbe7200x4400C0(imbe_fr);
    mbe_demodulateImbe7200x4400Data(imbe_fr);
    state->errs2 += mbe_eccImbe7200x4400Data(imbe_fr, imbe_d);
    /* decrypt if needed (DES/AES/RC4) */
    mbe_processImbe4400Dataf(audio_out_temp_buf, …, imbe_d, cur_mp, …);

} else if (DSD_SYNC_IS_PROVOICE(state->synctype)) {
    // ── IMBE 7100x4400 → converted to 7200 ────────────
    mbe_eccImbe7100x4400C0/Data(imbe7100_fr, imbe_d);
    mbe_convertImbe7100to7200(imbe_d);
    mbe_processImbe4400Dataf(audio_out_temp_buf, …);

} else if (DSD_SYNC_IS_DSTAR_VOICE(state->synctype)) {
    // ── AMBE 3600x2400 ─────────────────────────────────
    mbe_processAmbe3600x2400Framef(audio_out_temp_buf, …, ambe_fr, ambe_d, …);

} else if (DSD_SYNC_IS_X2TDMA(state->synctype)) {
    // ── AMBE+2 EHR (X2-TDMA variant) ──────────────────
    soft_demod_ambe2_ehr(state, ambe_fr, ambe_d);     // ECC + demod
    mbe_processAmbe2450Dataf(audio_out_temp_buf, …);

} else {
    // ── AMBE+2 EHR (DMR / P25p2 / NXDN / YSF / dPMR) ─
    // Slot selection: currentslot 0 → left buf, 1 → right buf
    soft_demod_ambe2_ehr(state, ambe_fr, ambe_d);
    /* decrypt if needed */
    mbe_processAmbe2450Dataf(audio_out_temp_buf[slot], …);
}
```

M17 and TETRA bypass `processMbeFrame()` entirely and call Codec2 / `tetra_acelp_process_tch()` from
their own protocol modules.

### 3.4 TDMA dual-slot routing

When `state->currentslot` is set by the protocol layer, the HAL routes PCM into separate buffers:

```
currentslot == 0  →  audio_out_temp_buf[160]   (left / slot 1)
                       cur_mp / prev_mp / errs / errs2
currentslot == 1  →  audio_out_temp_bufR[160]  (right / slot 2)
                       cur_mp2 / prev_mp2 / errsR / errs2R
```

This is transparent to callers—the protocol module only needs to set `state->currentslot` before
calling the HAL.

### 3.5 Matching sequence — end-to-end example (DMR voice frame)

```
1. IQ/audio samples arrive
2. dsd_frame_sync.c: DMR BS burst sync word detected
   → state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS (12)
   → state->synctype     = DSD_SYNC_DMR_BS_VOICE_POS (12)
3. Engine dispatches to DMR BS protocol module
4. DMR decodes burst, extracts AMBE+2 frame bits → ambe_fr[4][24]
   Sets state->currentslot (0 or 1)
5. Calls processMbeFrame(opts, state, NULL, ambe_fr, NULL)
6. HAL: DSD_SYNC_IS_P25P1(12)?  No
         DSD_SYNC_IS_PROVOICE(12)? No
         DSD_SYNC_IS_DSTAR(12)?  No
         DSD_SYNC_IS_X2TDMA(12)? No
         → falls through to default AMBE+2 EHR branch
7. ECC + demodulate AMBE+2 frame  →  ambe_d[49]
8. Apply DMR AMBE decryption keystream (if encrypted)
9. mbe_processAmbe2450Dataf()  →  audio_out_temp_buf[160] (float PCM)
10. Protocol module calls playSynthesizedVoice*() / processAudio()
11. PCM routed to PulseAudio / WAV / UDP output
```

### 3.6 Polarity inversion handling

Some receivers invert signal polarity.  dsd-neo supports both positive (`_POS`) and negative (`_NEG`)
sync variants for every protocol.  The HAL codec selection is polarity-agnostic:
`DSD_SYNC_IS_DMR_BS(s)` matches both `DSD_SYNC_DMR_BS_VOICE_POS` and `DSD_SYNC_DMR_BS_VOICE_NEG`.
No additional work is required in the vocoder layer.

---

## 4. Extending dsd-neo with a New Vocoder

The steps to add a new vocoder (e.g., Opus for a future PoC trunking protocol):

1. **Add a CMake feature target** in `CMakeLists.txt`:
   ```cmake
   find_package(Opus)
   if(OPUS_FOUND)
       add_library(dsd-neo_feature_opus INTERFACE)
       target_compile_definitions(dsd-neo_feature_opus INTERFACE USE_OPUS)
       target_include_directories(dsd-neo_feature_opus SYSTEM INTERFACE ${OPUS_INCLUDE_DIRS})
   else()
       add_library(dsd-neo_feature_opus INTERFACE)
   endif()
   ```

2. **Define a new `DSD_SYNC_*` constant** in `include/dsd-neo/core/synctype_ids.h` for the new protocol,
   along with a `DSD_SYNC_IS_<PROTOCOL>()` macro.

3. **Implement the protocol module** in `src/protocol/<name>/`.

4. **Route audio** in the protocol module by calling the new codec directly (following the M17/Codec2
   pattern) or by adding a new branch in `processMbeFrame()` (for mbelib-neo compatible frame shapes).

5. **Initialize/destroy** the codec context in `src/core/util/dsd_init.c`, storing the context pointer
   in `dsd_state` (use `#ifdef USE_<CODEC>` guards, always keep the field present for ABI stability).

---

## 5. Build-time Summary

| Vocoder | Required / Optional | Guard | How to enable |
|---------|---------------------|-------|---------------|
| mbelib-neo (IMBE / AMBE) | **Required** | *(always)* | Automatically resolved by CMake / vcpkg |
| Codec2 | Optional | `USE_CODEC2` | Install `codec2` vcpkg port; detected by `find_package(CODEC2)` |
| TETRA ACELP | Runtime optional | *(none)* | Set `TETRA_VOCODER_CMD` env var at runtime |

---

## 6. Related Documents

- `docs/vocoder-hal.md` — HAL interface design, state management, and inter-module collaboration
- `tools/tetra/README.md` — TETRA vocoder subprocess integration guide
- `include/dsd-neo/core/synctype_ids.h` — complete sync-type constant and macro reference
- `include/dsd-neo/core/vocoder.h` — public HAL entry points
