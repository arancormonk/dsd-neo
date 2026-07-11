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
static int g_open_mbe_calls[2];

// Expose the P25p2 2V handler under test
void process_2V(dsd_opts* opts, dsd_state* state);
void p25p2_test_decode_voice_frame_for_lockout(dsd_opts* opts, dsd_state* state);

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
void p25_lfsr128_slot(dsd_state* state, int slot);
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

// MBE file stubs referenced by XCCH path
void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_open_mbe_calls[0]++;
    opts->mbe_out_f = stdout;
}

void
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_open_mbe_calls[1]++;
    opts->mbe_out_fR = stdout;
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

void
p25_lfsr128_slot(dsd_state* state, int slot) {
    (void)state;
    (void)slot;
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
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;
}

static void
reset_mbe_calls(void) {
    g_mbe_calls = 0;
    g_mbe_hard_calls = 0;
    g_mbe_soft_calls = 0;
    g_open_mbe_calls[0] = 0;
    g_open_mbe_calls[1] = 0;
}

static void
set_ess_algid(dsd_state* st, int slot, uint8_t algid) {
    for (int i = 0; i < 8; i++) {
        st->ess_b[slot][i] = (algid >> (7 - i)) & 1;
    }
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
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
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
    st.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 allowed: mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot1 allowed: soft mbe calls", g_mbe_soft_calls, 2);
    rc |= expect_eq("slot1 allowed: hard mbe calls", g_mbe_hard_calls, 0);

    // Slot 0: stale allowed gate, encrypted/no key, lockout enabled -> no decode.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.dmr_so = 0x40;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 encrypted lockout: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot0 encrypted lockout: gate closed", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("slot0 encrypted lockout: ring flushed", st.p25_p2_audio_ring_count[0], 0);

    // Slot 0: service-option ENC before ESS completion mutes voice but must
    // not reset ESS_B collection; ALGID may still resolve to a loaded key.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.dmr_so = 0x40;
    st.fourv_counter[0] = 2;
    reset_mbe_calls();
    p25p2_test_decode_voice_frame_for_lockout(&opts, &st);
    rc |= expect_eq("slot0 pre-ess lockout: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot0 pre-ess lockout: gate closed", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("slot0 pre-ess lockout: fourv preserved", st.fourv_counter[0], 2);
    rc |= expect_eq("slot0 pre-ess lockout: pending marker set", st.p25_p2_enc_lockout_muted[0], 1);
    rc |= expect_eq("slot0 pre-ess lockout: pending state", st.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    // A mid-call encrypted service transition must revoke a stale clear gate
    // and purge only the affected slot before another voice frame is decoded.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 0;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    st.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.p25_p2_audio_ring_count[0] = 2;
    st.p25_p2_audio_ring_count[1] = 3;
    st.s_l4[0][0] = 11;
    st.s_r4[0][0] = 22;
    st.dmr_so = 0x40;
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "captures");
    reset_mbe_calls();
    p25p2_test_decode_voice_frame_for_lockout(&opts, &st);
    rc |= expect_eq("slot0 clear-to-encrypted: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot0 clear-to-encrypted: recording stays closed", g_open_mbe_calls[0], 0);
    rc |=
        expect_eq("slot0 clear-to-encrypted: pending state", st.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq("slot0 clear-to-encrypted: gate closed", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("slot0 clear-to-encrypted: ring purged", st.p25_p2_audio_ring_count[0], 0);
    rc |= expect_eq("slot0 clear-to-encrypted: int16 purged", st.s_l4[0][0], 0);
    rc |= expect_eq("slot0 clear-to-encrypted: companion gate preserved", st.p25_p2_audio_allowed[1], 1);
    rc |= expect_eq("slot0 clear-to-encrypted: companion ring preserved", st.p25_p2_audio_ring_count[1], 3);
    rc |= expect_eq("slot0 clear-to-encrypted: companion int16 preserved", st.s_r4[0][0], 22);

    // Definitive clear metadata remains authoritative when the cached service
    // options still carry the encrypted bit on a later voice burst.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 0;
    st.payload_algid = 0x80;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    st.p25_p2_audio_allowed[0] = 1;
    st.dmr_so = 0x40;
    reset_mbe_calls();
    p25p2_test_decode_voice_frame_for_lockout(&opts, &st);
    rc |= expect_eq("slot0 definitive clear: mbe calls", g_mbe_calls, 1);
    rc |= expect_eq("slot0 definitive clear: crypto state", st.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    rc |= expect_eq("slot0 definitive clear: gate remains open", st.p25_p2_audio_allowed[0], 1);

    reset_state(&opts, &st);
    st.currentslot = 0;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    st.p25_p2_audio_allowed[0] = 1;
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "captures");
    reset_mbe_calls();
    p25p2_test_decode_voice_frame_for_lockout(&opts, &st);
    rc |= expect_eq("slot0 classified voice: mbe calls", g_mbe_calls, 1);
    rc |= expect_eq("slot0 classified voice: recording opens", g_open_mbe_calls[0], 1);

    // Slot 0: definitive encrypted ESS remains authoritative until a later
    // definitive clear ESS indication arrives.
    reset_state(&opts, &st);
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_so = 0x40;
    st.payload_algid = 0x84;
    st.payload_keyid = 0x1234;
    st.payload_miP = 0x1122334455667788ULL;
    set_ess_algid(&st, 0, 0x84);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 stale algid cleanup: encrypted mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot0 encrypted metadata: algid retained", st.payload_algid, 0x84);
    rc |= expect_eq("slot0 stale algid cleanup: keyid cleared", st.payload_keyid, 0);
    rc |= expect_eq("slot0 stale algid cleanup: mi cleared", st.payload_miP == 0ULL, 1);
    rc |= expect_eq("slot0 stale algid cleanup: marker set", st.p25_p2_enc_lockout_muted[0], 1);
    st.p25_p2_audio_allowed[0] = 1;
    st.dmr_so = 0;
    set_ess_algid(&st, 0, 0x80);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 stale algid cleanup: clear mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot0 stale algid cleanup: clear gate open", st.p25_p2_audio_allowed[0], 1);
    rc |= expect_eq("slot0 stale algid cleanup: marker cleared", st.p25_p2_enc_lockout_muted[0], 0);

    // Slot 0: even when encrypted calls are followed, unresolved frames do not
    // reach the vocoder before definitive crypto metadata arrives.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 1;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.dmr_so = 0x40;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 unresolved encrypted follow: mbe calls", g_mbe_calls, 0);

    // Explicit encrypted-audio unmute restores the legacy undeciphered-audio
    // path while encrypted calls are being followed.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 1;
    opts.unmute_encrypted_p25 = 1;
    opts.dmr_mute_encL = 0;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    st.dmr_so = 0x40;
    st.dmrburstL = 21;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 explicit encrypted unmute: mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot0 explicit encrypted unmute: gate open", st.p25_p2_audio_allowed[0], 1);

    // Reverse mute also requests encrypted audio, but lockout probes remain a
    // hard-suppressed classification path even when both controls are active.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 1;
    opts.reverse_mute = 1;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    st.dmr_so = 0x40;
    st.dmrburstL = 21;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 reverse mute encrypted follow: mbe calls", g_mbe_calls, 2);

    opts.trunk_tune_enc_calls = 0;
    opts.unmute_encrypted_p25 = 1;
    opts.dmr_mute_encL = 0;
    st.p25_p2_audio_allowed[0] = 1;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 explicit unmute lockout probe: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot0 explicit unmute lockout probe: gate closed", st.p25_p2_audio_allowed[0], 0);

    // Slot 0: encrypted lockout enabled but decryptable audio remains allowed.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.dmr_so = 0x40;
    st.dmrburstL = 21;
    st.aes_key_loaded[0] = 1;
    st.aes_key_segments[0] = 4U;
    set_ess_algid(&st, 0, 0x84);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 encrypted decryptable: mbe calls", g_mbe_calls, 2);

    // Slot 0: keyed encrypted call muted by media policy is not an encrypted lockout.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    opts.trunk_use_allow_list = 1;
    st.currentslot = 0;
    st.lasttg = 1234;
    st.tg_hold = 4321;
    st.p25_p2_audio_allowed[0] = 0;
    st.dmr_so = 0x40;
    st.dmrburstL = 21;
    st.aes_key_loaded[0] = 1;
    st.aes_key_segments[0] = 4U;
    set_ess_algid(&st, 0, 0x84);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 policy-muted decryptable: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot0 policy-muted decryptable: gate stays closed", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("slot0 policy-muted decryptable: burst preserved", st.dmrburstL, 21);

    // Slot 0: decoded clear ALGID overrides a stale encrypted service bit.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 0;
    st.p25_p2_audio_allowed[0] = 1;
    st.dmr_so = 0x40;
    st.dmrburstL = 21;
    set_ess_algid(&st, 0, 0x80);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot0 clear algid overrides svc: mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot0 clear algid overrides svc: gate open", st.p25_p2_audio_allowed[0], 1);

    // Slot 1: stale allowed gate, encrypted/no key, lockout enabled -> no decode.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_soR = 0x40;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 encrypted lockout: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot1 encrypted lockout: gate closed", st.p25_p2_audio_allowed[1], 0);
    rc |= expect_eq("slot1 encrypted lockout: ring flushed", st.p25_p2_audio_ring_count[1], 0);

    // Slot 1: same pre-ESS service-option mute must preserve fragment index.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_soR = 0x40;
    st.fourv_counter[1] = 3;
    reset_mbe_calls();
    p25p2_test_decode_voice_frame_for_lockout(&opts, &st);
    rc |= expect_eq("slot1 pre-ess lockout: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot1 pre-ess lockout: gate closed", st.p25_p2_audio_allowed[1], 0);
    rc |= expect_eq("slot1 pre-ess lockout: fourv preserved", st.fourv_counter[1], 3);
    rc |= expect_eq("slot1 pre-ess lockout: pending marker set", st.p25_p2_enc_lockout_muted[1], 1);
    rc |= expect_eq("slot1 pre-ess lockout: pending state", st.p25_crypto_state[1], DSD_P25_CRYPTO_ENCRYPTED_PENDING);

    // Slot 1: same sticky encrypted metadata behavior as slot 0.
    reset_state(&opts, &st);
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 1;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_soR = 0x40;
    st.payload_algidR = 0x84;
    st.payload_keyidR = 0x5678;
    st.payload_miN = 0x8877665544332211ULL;
    set_ess_algid(&st, 1, 0x84);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 stale algid cleanup: encrypted mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot1 encrypted metadata: algid retained", st.payload_algidR, 0x84);
    rc |= expect_eq("slot1 stale algid cleanup: keyid cleared", st.payload_keyidR, 0);
    rc |= expect_eq("slot1 stale algid cleanup: mi cleared", st.payload_miN == 0ULL, 1);
    rc |= expect_eq("slot1 stale algid cleanup: marker set", st.p25_p2_enc_lockout_muted[1], 1);
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_soR = 0;
    set_ess_algid(&st, 1, 0x80);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 stale algid cleanup: clear mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot1 stale algid cleanup: clear gate open", st.p25_p2_audio_allowed[1], 1);
    rc |= expect_eq("slot1 stale algid cleanup: marker cleared", st.p25_p2_enc_lockout_muted[1], 0);

    // Slot 1: unresolved followed encryption is also gated before the vocoder.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 1;
    st.currentslot = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_soR = 0x40;
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 unresolved encrypted follow: mbe calls", g_mbe_calls, 0);

    // Slot 1: encrypted lockout enabled but decryptable audio remains allowed.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_soR = 0x40;
    st.dmrburstR = 21;
    st.aes_key_loaded[1] = 1;
    st.aes_key_segments[1] = 4U;
    set_ess_algid(&st, 1, 0x84);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 encrypted decryptable: mbe calls", g_mbe_calls, 2);

    // Slot 1: keyed encrypted call muted by media policy is not an encrypted lockout.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    opts.trunk_use_allow_list = 1;
    st.currentslot = 1;
    st.lasttgR = 5678;
    st.tg_hold = 8765;
    st.p25_p2_audio_allowed[1] = 0;
    st.dmr_soR = 0x40;
    st.dmrburstR = 21;
    st.aes_key_loaded[1] = 1;
    st.aes_key_segments[1] = 4U;
    set_ess_algid(&st, 1, 0x84);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 policy-muted decryptable: mbe calls", g_mbe_calls, 0);
    rc |= expect_eq("slot1 policy-muted decryptable: gate stays closed", st.p25_p2_audio_allowed[1], 0);
    rc |= expect_eq("slot1 policy-muted decryptable: burst preserved", st.dmrburstR, 21);

    // Slot 1: decoded clear ALGID overrides a stale encrypted service bit.
    reset_state(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    st.currentslot = 1;
    st.p25_p2_audio_allowed[1] = 1;
    st.dmr_soR = 0x40;
    st.dmrburstR = 21;
    set_ess_algid(&st, 1, 0x80);
    reset_mbe_calls();
    process_2V(&opts, &st);
    rc |= expect_eq("slot1 clear algid overrides svc: mbe calls", g_mbe_calls, 2);
    rc |= expect_eq("slot1 clear algid overrides svc: gate open", st.p25_p2_audio_allowed[1], 1);

    return rc;
}
