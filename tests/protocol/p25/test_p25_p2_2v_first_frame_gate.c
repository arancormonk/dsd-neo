// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate P25 Phase 2 2V first-subframe gating: when per-slot audio is not
 * allowed (e.g., due to encryption lockout), the first AMBE subframe should
 * not be decoded. When audio is allowed, both AMBE subframes in 2V should be
 * decoded.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

// Expose the P25p2 2V handler under test
void process_2V(dsd_opts* opts, dsd_state* state);

// NOLINTNEXTLINE(misc-use-internal-linkage)
bool SetFreq(int sockfd, long int freq);
// NOLINTNEXTLINE(misc-use-internal-linkage)
bool SetModulation(int sockfd, int bandwidth);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void return_to_cc(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void openMbeOutFile(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void openMbeOutFileR(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void getTimeC_buf(char out[9]);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void rotate_symbol_out_file(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void playSynthesizedVoiceFS4(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void playSynthesizedVoiceSS18(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSRP(dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSR128(dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage)
double dsd_time_now_monotonic_s(void);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int ez_rs28_facch_soft(int* payload, int* parity, const int* erasures, int n_erasures);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int ez_rs28_sacch_soft(int* payload, int* parity, const int* erasures, int n_erasures);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int ez_rs28_ess(int* payload, int* parity);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int ez_rs28_ess_soft(int* payload, int* parity, const int* erasures, int n_erasures);
// NOLINTNEXTLINE(misc-use-internal-linkage)
int isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits);

// Provide stubs to satisfy link dependencies (rigctl and return_to_cc)
bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// MBE file close stubs referenced by XCCH path
void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// Misc helpers referenced by P25p2 frame path
static void
rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta, int sacch_err_delta,
                            int aach_ok_delta) {
    (void)slot;
    (void)facch_ok_delta;
    (void)facch_err_delta;
    (void)sacch_ok_delta;
    (void)sacch_err_delta;
    (void)aach_ok_delta;
}

void
getTimeC_buf(char out[9]) {
    if (out) {
        DSD_MEMCPY(out, "00:00:00", 9);
    }
}

void
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
ncursesPrinter(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
playSynthesizedVoiceFS4(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
playSynthesizedVoiceSS18(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

// Audio/playback stubs referenced by P25p1 LDU2 object (pulled in by LFSR helpers)
static void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)tg;
    (void)svc;
}

void
LFSRP(dsd_state* state) {
    (void)state;
}

void
LFSR128(dsd_state* state) {
    (void)state;
}

double
dsd_time_now_monotonic_s(void) {
    return 0.0;
}

int
ez_rs28_facch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

int
ez_rs28_sacch_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

int
ez_rs28_ess(int* payload, int* parity) {
    (void)payload;
    (void)parity;
    return 0;
}

int
ez_rs28_ess_soft(int* payload, int* parity, const int* erasures, int n_erasures) {
    (void)payload;
    (void)parity;
    (void)erasures;
    (void)n_erasures;
    return 0;
}

int
isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]) {
    (void)isch;
    (void)reliab40;
    return -1;
}

void
process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    (void)opts;
    (void)state;
    (void)bits;
}

void
process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int* bits) {
    (void)opts;
    (void)state;
    (void)bits;
}

// Dibit helpers referenced from Phase 1 paths (not exercised here)
int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    if (out_soft) {
        out_soft->reliability = 128;
        out_soft->llr[0] = -128;
        out_soft->llr[1] = -128;
    }
    return 0;
}

int
getDibitWithReliability(dsd_opts* opts, dsd_state* state, uint8_t* out_reliability) {
    (void)opts;
    (void)state;
    if (out_reliability) {
        *out_reliability = 128;
    }
    return 0;
}

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    if (out_analog_signal) {
        *out_analog_signal = 0;
    }
    return 0;
}

void
skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    (void)count;
}

// Bit conversion helper
static uint64_t
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    (void)BufferIn;
    (void)BitLength;
    return 0ULL;
}

// MBE table pointers (from mbelib) referenced by Phase 1 code
static int iW[24] = {0};
static int iX[24] = {0};
static int iY[24] = {0};
static int iZ[24] = {0};

// Alias decode helpers referenced by VPDU path (not exercised here)
static void
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

static void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

static void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

static void
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

static void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

// Interpose the MBE frame decoder to count invocations without pulling in the
// full vocoder stack. The signature must match src/core/vocoder/dsd_mbe.c.
static int g_mbe_calls = 0;
static int g_mbe_hard_calls = 0;
static int g_mbe_soft_calls = 0;

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    g_mbe_calls++;
    g_mbe_hard_calls++;
}

void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    g_mbe_calls++;
    g_mbe_soft_calls++;
}

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
reset_state(dsd_opts* opts, dsd_state* st) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(st, 0, sizeof *st);
    // Ensure deterministic behavior
    opts->floating_point = 0;
}

static void
reset_mbe_calls(void) {
    g_mbe_calls = 0;
    g_mbe_hard_calls = 0;
    g_mbe_soft_calls = 0;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;

    // Slot 0: audio gated off -> expect 0 MBE calls (first-subframe gating active)
    reset_state(&opts, &st);
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 0;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 gated: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot0 gated: soft mbe calls", g_mbe_soft_calls, 0);
    rc |= expect_eq("slot0 gated: hard mbe calls", g_mbe_hard_calls, 0);

    // Slot 0: audio allowed -> expect 2 MBE calls (both 2V subframes decoded)
    reset_state(&opts, &st);
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 allowed: mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot0 allowed: soft mbe calls", g_mbe_soft_calls, 2);
    rc |= expect_eq("slot0 allowed: hard mbe calls", g_mbe_hard_calls, 0);

    // Slot 1: audio gated off -> expect 0 MBE calls
    reset_state(&opts, &st);
    st.currentslot = 1;
    st.p25_p2_audio_allowed[1] = 0;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 gated: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot1 gated: soft mbe calls", g_mbe_soft_calls, 0);
    rc |= expect_eq("slot1 gated: hard mbe calls", g_mbe_hard_calls, 0);

    // Slot 1: audio allowed -> expect 2 MBE calls
    reset_state(&opts, &st);
    st.currentslot = 1;
    st.p25_p2_audio_allowed[1] = 1;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 allowed: mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot1 allowed: soft mbe calls", g_mbe_soft_calls, 2);
    rc |= expect_eq("slot1 allowed: hard mbe calls", g_mbe_hard_calls, 0);

    return rc;
}
