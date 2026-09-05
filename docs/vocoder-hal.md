# Vocoder HAL Design in dsd-neo

> **Scope:** This document summarises the Vocoder Hardware Abstraction Layer (HAL) in the `dsd-neo` project—covering abstract interfaces, supported vocoder types, encode/decode flow, state management, automatic codec selection, and inter-module collaboration.  Intended for PR discussion and code review.
>
> See also `docs/vocoder-libraries.md` for library details, protocol-to-codec mapping, and a full auto-matching walkthrough.

---

## 1. Overview

dsd-neo decodes multiple digital voice radio protocols (P25, DMR, D-STAR, NXDN, YSF, M17, ProVoice/EDACS, X2-TDMA, dPMR, TETRA, …).  Each protocol uses a different speech codec.  Rather than scattering codec calls across every protocol module, dsd-neo centralises vocoder logic in a thin HAL layer inside `src/core/vocoder/` with the public interface declared in `include/dsd-neo/core/vocoder.h`.

The three-tier structure is:

```
┌────────────────────────────────────────────────────────────────────┐
│  Protocol Modules  (P25, DMR, D-STAR, NXDN, M17, TETRA, …)        │
│  – extract raw codec frames from the radio bitstream               │
│  – call processMbeFrame() / soft_mbe() / protocol-specific decode  │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────────────┐
│  Vocoder HAL  (src/core/vocoder/)                                   │
│  – selects the correct codec based on state->synctype               │
│  – drives ECC, demodulation, decryption, and synthesis              │
│  – deposits float PCM into state->audio_out_temp_buf[160]           │
└──────────────────────────────┬─────────────────────────────────────┘
                               │
┌──────────────────────────────▼─────────────────────────────────────┐
│  Audio Output Layer  (src/core/audio/, platform/)                   │
│  – processAudio(), playSynthesizedVoice*(), writeSynthesizedVoice() │
│  – PulseAudio / PortAudio / UDP / WAV file routing                  │
└────────────────────────────────────────────────────────────────────┘
```

---

## 2. Abstract Interface

Public header: **`include/dsd-neo/core/vocoder.h`**

```c
/* Full decode with decryption + audio handoff */
void processMbeFrame(dsd_opts* opts, dsd_state* state,
                     char imbe_fr[8][23],
                     char ambe_fr[4][24],
                     char imbe7100_fr[7][24]);

/* ECC + soft-demod only (no decryption, no audio handoff) */
void soft_mbe(dsd_opts* opts, dsd_state* state,
              char imbe_fr[8][23],
              char ambe_fr[4][24],
              char imbe7100_fr[7][24]);

/* MBE file playback mode */
void playMbeFiles(dsd_opts* opts, dsd_state* state,
                  int argc, char** argv);
```

All three functions share the same `(dsd_opts*, dsd_state*)` context pair; callers never reference specific codec libraries directly.

---

## 3. Supported Vocoder Types

| Codec | Bit-rate (speech) | Frame Input | Protocols |
|-------|--------------------|-------------|-----------|
| IMBE 7200×4400 | 4.4 kbps | `imbe_fr[8][23]` | P25 Phase 1 |
| IMBE 7100×4400 | 4.4 kbps | `imbe7100_fr[7][24]` → converted to 7200 | ProVoice / EDACS |
| AMBE 3600×2400 | 2.4 kbps | `ambe_fr[4][24]` | D-STAR |
| AMBE+2 3600×2450 EHR | 2.45 kbps | `ambe_fr[4][24]` | DMR, P25 Phase 2, NXDN, YSF VD/VW, X2-TDMA, dPMR |
| Codec2 3200 bps | 3.2 kbps | 8-byte frame × 2 per stream frame | M17 full-rate |
| Codec2 1600 bps | 1.6 kbps | 8-byte frame × 1 per stream frame | M17 half-rate |
| TETRA ACELP TCH/FS | ~6 kbps | 137 bits/frame × 2 | TETRA |

### 3.1 mbelib-neo (IMBE / AMBE family)

All IMBE and AMBE variants are handled by the **mbelib-neo** library (`mbe-neo` vcpkg package, linked via CMake target `mbe_neo::mbe_shared`).  The HAL uses the following mbelib-neo entry points:

| mbelib-neo function | Purpose |
|---------------------|---------|
| `mbe_eccImbe7200x4400C0` | Class-0 ECC for P25p1 IMBE |
| `mbe_demodulateImbe7200x4400Data` | IMBE demodulation |
| `mbe_eccImbe7200x4400Data` | Class-1/2 ECC, fills `imbe_d[88]` |
| `mbe_processImbe4400Dataf` | Synthesis → `float[160]` PCM |
| `mbe_eccImbe7100x4400C0/Data` | Class ECC for ProVoice IMBE |
| `mbe_demodulateImbe7100x4400Data` | ProVoice demodulation |
| `mbe_convertImbe7100to7200` | Converts 7100 frame to 7200 for synthesis |
| `mbe_processAmbe3600x2400Framef` | D-STAR AMBE all-in-one |
| `mbe_eccAmbe3600x2450C0/Data` | Class ECC for AMBE+2 |
| `mbe_demodulateAmbe3600x2450Data` | AMBE+2 demodulation |
| `mbe_processAmbe2450Dataf` | AMBE+2 EHR synthesis → `float[160]` |
| `mbe_processAmbe2400Dataf` | D-STAR AMBE synthesis (file playback) |
| `mbe_initMbeParms` | Initialises model parameter state |

Frame parameters are tracked by `mbe_parms` objects (forward-declared as `struct mbe_parameters`) stored in `dsd_state`.

### 3.2 Codec2 (M17)

Codec2 is an **optional** dependency (`USE_CODEC2` compile definition, CMake feature target `dsd-neo_feature_codec2`).  When present:

- Contexts are created in `dsd_init.c`:
  ```c
  state->codec2_3200 = codec2_create(CODEC2_MODE_3200);
  state->codec2_1600 = codec2_create(CODEC2_MODE_1600);
  ```
- Decoding is performed in `src/protocol/m17/m17.c`:
  ```c
  codec2_decode(state->codec2_3200, samp, voice_bytes);  // full-rate
  codec2_decode(state->codec2_1600, samp, voice_bytes);  // half-rate
  ```
- M17 encoding (encoder mode) uses `codec2_encode(...)`.
- When `USE_CODEC2` is absent at build time, the decode/encode blocks compile out and Codec2 frames are silently skipped.

### 3.3 TETRA ACELP (External Subprocess)

The TETRA TCH/FS ACELP codec is proprietary (ETSI EN 300 395-2) and has no freely-available open-source implementation.  dsd-neo uses a **persistent external subprocess** model:

- Subprocess command: `TETRA_VOCODER_CMD` environment variable.
- If the variable is unset or empty, TETRA voice frames are decoded (FEC corrected and reordered) but audio output is suppressed with a one-time warning.

**Subprocess protocol:**

```
dsd-neo → stdin  : TETRA_TCH_FRAME_BITS (137) bytes, one byte per coded bit (0/1)
dsd-neo ← stdout : TETRA_TCH_FRAME_SAMPLES (160) × 2 bytes, PCM16LE @ 8 kHz mono
(repeat per frame until stdin is closed)
```

Two frames are exchanged per NDB block pair (one per TCH/FS ACELP sub-frame).

Cross-platform subprocess management is implemented in `src/protocol/tetra/tetra_acelp.c`:
- **POSIX:** `fork()` + `pipe()` + `dup2()`; pipe writes suppress per-thread
  `SIGPIPE`, and shutdown waits at most two seconds before terminating the child.
- **Windows:** `CreateProcess()` + anonymous pipes; shutdown uses the same
  two-second bound before terminating an unresponsive child.

Every response must contain exactly 160 PCM16LE samples. Short responses are
discarded in full and reset the subprocess so partial audio cannot reach a sink.

A silence-producing stub for integration testing is provided at `tools/tetra/vocoder_stub.py`.

---

## 4. Encode and Decode Flow

### 4.1 `processMbeFrame()` — Full Decode Path

```
Protocol module
    │
    ▼  call processMbeFrame(opts, state, imbe_fr, ambe_fr, imbe7100_fr)
    │
    ├─ Load encryption keys (keyring() if keyloader == 1 and algid set)
    │
    ├─ P25p1 / ProVoice path (IMBE)
    │     1. ECC / demodulate IMBE frame bits
    │     2. Apply decryption keystream (DES-OFB / DES-XL / 3DES / AES-OFB / RC4)
    │     3. mbe_processImbe4400Dataf() → audio_out_temp_buf[160]
    │     4. Update P25p1 rolling error-rate history (p25_p1_voice_err_hist)
    │     5. Save IMBE frame to .mbe file if mbe_out_f set
    │
    ├─ D-STAR path (AMBE 2400)
    │     mbe_processAmbe3600x2400Framef() → audio_out_temp_buf[160]
    │
    ├─ NXDN path (AMBE+2 EHR)
    │     1. ECC + demodulate AMBE+2 bits
    │     2. Apply NXDN LFSR / DES-OFB / AES-OFB decryption
    │     3. mbe_processAmbe2450Dataf() → audio_out_temp_buf[160]
    │
    └─ Default (DMR, P25p2, YSF, dPMR, X2-TDMA — AMBE+2 EHR)
          1. ECC + demodulate AMBE+2 bits
          2. Apply BP / Hytera / DES-OFB / AES-OFB / DES-XL / 3DES / AES-OFB decryption
          3. mbe_processAmbe2450Dataf() → audio_out_temp_buf[left] or audio_out_temp_bufR[right]
             (slot selection via state->currentslot)
          4. Update P25p2 rolling error-rate history (p25_p2_voice_err_hist)
          5. Save AMBE frame to .mbe file if mbe_out_f set
```

After `processMbeFrame()` returns, the calling protocol module drives audio playback through `processAudio()` / `playSynthesizedVoice*()` / `writeSynthesizedVoice()`.

### 4.2 `soft_mbe()` — ECC-only / Soft-Demod Path

`soft_mbe()` performs the same codec selection logic but **skips decryption**.  It is used for error-rate diagnostics and light-weight decode passes where encryption handling is not needed.

### 4.3 `playMbeFiles()` — File Playback

Reads raw IMBE or AMBE codec frames from `.mbe` binary files (or SDRTrunk JSON format) and synthesises audio using the same mbelib-neo functions.  File type is identified by `state->mbe_file_type`:

| `mbe_file_type` | Content |
|-----------------|---------|
| `0` | IMBE 4400 raw bits |
| `1` | AMBE+2 2450 raw bits |
| `2` | D-STAR AMBE 2400 raw bits |
| `3` | SDRTrunk JSON format |

### 4.4 M17 Codec2 Decode

M17 frames bypass `processMbeFrame()`; the M17 protocol module (`src/protocol/m17/m17.c`) calls `M17processCodec2_3200()` or `M17processCodec2_1600()` directly after detecting the data type from the Link Setup Frame (LSF).

### 4.5 TETRA ACELP Decode

TETRA frames bypass `processMbeFrame()`; `tetra_acelp_process_tch()` is called from the TETRA protocol layer after Viterbi decoding:

```
type2_bits (292 bits from Viterbi)
    │
    ▼  tetra_acelp_reorder()
    │  scatter 272 bits into 2 × 137-bit ACELP codec frames
    │
    ▼  (for each of the 2 frames)
    │  voc_send_recv()  →  TETRA_VOCODER_CMD subprocess
    │  receive PCM16LE [160 samples]
    │
    ▼  audio routing (PA / UDP / raw FD / WAV)
```

---

## 5. Vocoder Registration and State Management

There is no runtime vocoder registry or plugin table; selection is compile-time and dispatch-time.

### 5.1 `dsd_state` Fields

```c
/* mbelib-neo model parameters — two sets for TDMA dual-slot */
mbe_parms* cur_mp;             // current frame model, slot 0
mbe_parms* prev_mp;            // previous frame model, slot 0
mbe_parms* prev_mp_enhanced;   // enhanced previous, slot 0
mbe_parms* cur_mp2;            // current frame model, slot 1
mbe_parms* prev_mp2;           // previous frame model, slot 1
mbe_parms* prev_mp_enhanced2;  // enhanced previous, slot 1

/* Codec2 contexts (NULL when USE_CODEC2 not built) */
struct CODEC2* codec2_3200;    // M17 full-rate
struct CODEC2* codec2_1600;    // M17 half-rate

/* PCM output scratch buffers (float, 20 ms @ 8 kHz) */
float audio_out_temp_buf[160];   // slot 0 / mono
float audio_out_temp_bufR[160];  // slot 1 (TDMA right channel)

/* Error counters (updated by each ECC pass) */
int errs;   int errs2;    // slot 0
int errsR;  int errs2R;   // slot 1

/* Rolling voice error history for P25 hangtime control */
uint8_t p25_p1_voice_err_hist[64];
uint8_t p25_p2_voice_err_hist[2][64];

/* Raw decrypted/decoded AMBE bits (single-frame scratch) */
char ambe_ciphered[49];
char ambe_deciphered[49];

/* Current TDMA timeslot (0 or 1) */
int currentslot;

/* MBE file type for playMbeFiles() */
int mbe_file_type;
```

### 5.2 `dsd_opts` Fields

```c
int uvquality;          // unvoiced quality (passed to all mbelib synthesis calls)
int floating_point;     // 0 = int16 path, 1 = float path
FILE* mbe_out_f;        // raw codec frame capture file (slot 0)
FILE* mbe_out_fR;       // raw codec frame capture file (slot 1)
FILE* mbe_in_f;         // playback source file
char mbe_in_file[1024]; // path to playback file
char mbe_out_dir[1024]; // directory for per-call .mbe files
int payload;            // 1 = print raw codec bits to stderr
```

### 5.3 Lifecycle

| Phase | Action |
|-------|--------|
| Init (`dsd_init.c`) | `mbe_initMbeParms()` for all six `mbe_parms` pointers; `codec2_create()` when `USE_CODEC2` |
| Decode | `processMbeFrame()` / `soft_mbe()` per voice frame |
| File playback | `playMbeFiles()` opens `.mbe`, calls `mbe_initMbeParms()`, reads and synthesises frames, closes file |
| Protocol reset | `tetra_vocoder_close()` terminates the TETRA subprocess |
| Shutdown (`dsd_init.c`) | `codec2_destroy()` when `USE_CODEC2`; `tetra_vocoder_close()` |

---

## 6. Automatic Codec Matching / Switching

`processMbeFrame()` and `soft_mbe()` select the codec by inspecting `state->synctype` using the protocol-family macros defined in `include/dsd-neo/core/synctype_ids.h`:

```c
if (DSD_SYNC_IS_P25P1(state->synctype))       → IMBE 7200×4400
else if (DSD_SYNC_IS_PROVOICE(state->synctype))→ IMBE 7100×4400 → convert → 4400
else if (synctype == DSD_SYNC_DSTAR_VOICE_POS/NEG) → AMBE 3600×2400
else if (DSD_SYNC_IS_X2TDMA(state->synctype)) → AMBE+2 EHR (via soft_demod_ambe_x2)
else                                           → AMBE+2 EHR (DMR/P25p2/YSF/NXDN/dPMR)
```

`state->synctype` is maintained by the DSP frame-sync module (`src/dsp/dsd_frame_sync.c`) and is updated atomically before each voice frame is dispatched.  No additional vocoder configuration is required at runtime; the correct codec is selected transparently.

M17 and TETRA are **out-of-band**—their voice frames never pass through `processMbeFrame()`; they are handled in their respective protocol modules which call Codec2 or `tetra_acelp_process_tch()` directly.

### TDMA Slot Switching

For dual-slot protocols (DMR, P25 Phase 2, X2-TDMA), the decode path is split at `state->currentslot`:

- Slot 0 → `cur_mp` / `audio_out_temp_buf` / `errs` / `errs2`
- Slot 1 → `cur_mp2` / `audio_out_temp_bufR` / `errsR` / `errs2R`

This allows true stereo output (left = slot 0, right = slot 1).

---

## 7. Inter-Module Collaboration

```
┌─────────────┐    imbe_fr / ambe_fr     ┌──────────────────┐
│  Protocol   │ ────────────────────────► │  Vocoder HAL     │
│  Modules    │                           │  (core/vocoder/) │
│             │ ◄──── errs / errs2 ──────  │                  │
└─────────────┘                           └────────┬─────────┘
                                                   │ audio_out_temp_buf[160]
                                          ┌────────▼─────────┐
┌─────────────┐    synctype              │  Audio Output    │
│  DSP /      │ ──────────────────────── ► │  (core/audio/)   │
│  Frame Sync │   (sets state->synctype)   │  processAudio()  │
└─────────────┘                           │  playSynth*()    │
                                           └────────┬─────────┘
┌─────────────┐    opts->uvquality                  │ PCM int16 / float
│  dsd_opts   │ ──────────────────────── ►  mbelib  │
│  (config)   │    opts->floating_point   └────────▼─────────┐
└─────────────┘                           │  Platform Audio  │
                                           │  (PA / WAV / UDP)│
                                           └──────────────────┘
```

Key dependency edges:

| Consumer | Provides | Used by |
|----------|----------|---------|
| `core/vocoder/` | ECC + synthesis, `audio_out_temp_buf` | Protocol modules |
| `mbe-neo` (lib) | IMBE/AMBE ECC + synthesis API | `core/vocoder/` |
| `codec2` (lib, optional) | Codec2 encode/decode | `protocol/m17/` |
| `TETRA_VOCODER_CMD` (subprocess) | ACELP synthesis | `protocol/tetra/tetra_acelp.c` |
| `core/audio/` | PCM routing helpers | Protocol modules + vocoder HAL |
| `core/crypto/` | Keystream generators (DES/AES/RC4) | `core/vocoder/dsd_mbe.c` |
| `dsd_state.synctype` | Codec identity | `core/vocoder/` (dispatch) |

---

## 8. Build Configuration

| CMake Feature Target | Compile Definition | Effect |
|----------------------|--------------------|--------|
| `dsd-neo_feature_codec2` | `USE_CODEC2` | Enables Codec2 decode/encode in M17 |
| *(none — required)* | *(always)* | mbelib-neo always linked (`mbe_neo::mbe_shared`) |
| *(env var, runtime)* | `TETRA_VOCODER_CMD` | Enables TETRA ACELP audio output |

vcpkg dependencies (`vcpkg.json`):
- **Required:** `mbe-neo`
- **Optional:** `codec2` (Windows platform feature; auto-detected via `find_package(CODEC2)` on Linux/macOS)

---

## 9. File Reference

| File | Role |
|------|------|
| `include/dsd-neo/core/vocoder.h` | Public HAL API |
| `src/core/vocoder/dsd_mbe.c` | `processMbeFrame()`, `playMbeFiles()` — full decode + crypto |
| `src/core/vocoder/dsd_mbe2.c` | `soft_mbe()`, per-codec soft-demod helpers |
| `include/dsd-neo/core/state.h` | `dsd_state` — mbe_parms pointers, codec2 contexts, audio scratch buffers, error counters |
| `include/dsd-neo/core/opts.h` | `dsd_opts` — uvquality, floating_point, mbe file paths |
| `include/dsd-neo/core/audio.h` | Post-decode PCM routing API |
| `include/dsd-neo/core/file_io.h` | MBE/IMBE/AMBE file save/load API |
| `include/dsd-neo/core/synctype_ids.h` | Synctype constants + codec-family macros |
| `src/core/util/dsd_init.c` | Codec2 create/destroy; mbe_parms init |
| `src/protocol/m17/m17.c` | Codec2 decode/encode for M17 |
| `src/protocol/tetra/tetra_acelp.c` | TETRA ACELP reorder + subprocess IPC |
| `include/dsd-neo/protocol/tetra/tetra_acelp.h` | TETRA ACELP public API |
| `tools/tetra/vocoder_stub.py` | Silence-producing TETRA vocoder stub |
| `tools/tetra/README.md` | TETRA vocoder integration guide |
