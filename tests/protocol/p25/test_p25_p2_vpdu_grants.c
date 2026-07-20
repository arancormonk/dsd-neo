// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25p2 MAC VPDU grant tests: MFID 0x90 regroup grants (A3/A4) and UU grants (0x44).
 * Asserts trunking tune side-effects via test shim capture.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#include "p25_test_shim.h"

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* tag, const char* text, const char* needle) {
    if (text == NULL || needle == NULL || strstr(text, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: '%s' did not contain '%s'\n", tag, text ? text : "(null)", needle ? needle : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_not_contains(const char* tag, const char* text, const char* needle) {
    if (text != NULL && needle != NULL && strstr(text, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s: '%s' unexpectedly contained '%s'\n", tag, text, needle);
        return 1;
    }
    return 0;
}

static int
seed_policy_group(dsd_state* st, uint32_t tg, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(tg, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_append_exact(st, &row);
}

static int
find_patch_idx(const dsd_state* st, uint16_t sgid) {
    for (int i = 0; i < st->p25_patch_count && i < 8; i++) {
        if (st->p25_patch_sgid[i] == sgid) {
            return i;
        }
    }
    return -1;
}

static int
patch_has_wgid(const dsd_state* st, int idx, uint16_t wgid) {
    if (!st || idx < 0 || idx >= 8) {
        return 0;
    }
    for (int i = 0; i < st->p25_patch_wgid_count[idx] && i < 8; i++) {
        if (st->p25_patch_wgid[idx][i] == wgid) {
            return 1;
        }
    }
    return 0;
}

static int
patch_has_wuid(const dsd_state* st, int idx, uint32_t wuid) {
    if (!st || idx < 0 || idx >= 8) {
        return 0;
    }
    for (int i = 0; i < st->p25_patch_wuid_count[idx] && i < 8; i++) {
        if (st->p25_patch_wuid[idx][i] == wuid) {
            return 1;
        }
    }
    return 0;
}

static int
read_capture_file(const char* path, char* out, size_t out_sz) {
    if (!path || !out || out_sz == 0) {
        return -1;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    size_t n = fread(out, 1, out_sz - 1, f);
    out[n] = '\0';
    fclose(f);
    return 0;
}

static void
seed_fdma_iden(dsd_state* state, int iden, int type, long base, int spac) {
    state->p25_iden_fdma[iden].base_freq = base;
    state->p25_iden_fdma[iden].chan_type = type;
    state->p25_iden_fdma[iden].chan_spac = spac;
    state->p25_iden_fdma[iden].trust = 2;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 1;
}

static void
seed_tdma_iden(dsd_state* state, int iden, int type, long base, int spac) {
    state->p25_iden_tdma[iden].base_freq = base;
    state->p25_iden_tdma[iden].chan_type = type;
    state->p25_iden_tdma[iden].chan_spac = spac;
    state->p25_iden_tdma[iden].trust = 2;
    state->p25_iden_tdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 2;
}

static int
run_standard_regroup_voice_user_case(int mfid, int slot, const char* tag) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    char label[128];
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = slot;
    state.gi[slot] = 1;
    state.p25_service_options_valid[slot] = 1;
    state.p25_call_emergency[slot] = 1;
    state.p25_call_is_packet[slot] = 1;
    state.p25_call_priority[slot] = 7;
    DSD_SNPRINTF(state.generic_talker_alias[slot], sizeof state.generic_talker_alias[slot], "%s", "STALE");
    state.generic_talker_alias_src[slot] = 0x0F0E0D;
    if (slot == 0) {
        state.dmr_so = 0x5A;
        state.lasttg = 0x1111;
        state.lastsrc = 0x010101;
    } else {
        state.dmr_soR = 0x6B;
        state.lasttgR = 0x2222;
        state.lastsrcR = 0x020202;
    }

    MAC[1] = 0x90;
    MAC[2] = (unsigned long long int)(mfid & 0xFF);
    MAC[3] = 0x34;
    MAC[4] = 0x56;
    MAC[5] = 0x01;
    MAC[6] = 0x02;
    MAC[7] = 0x03;

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    process_MAC_VPDU(&opts, &state, 0, MAC);

    DSD_SNPRINTF(label, sizeof label, "%s no grant dispatch", tag);
    rc |= expect_eq_long(label, p25_sm_get_ctx()->grant_count, 0);
    DSD_SNPRINTF(label, sizeof label, "%s no retune", tag);
    rc |= expect_eq_long(label, opts.trunk_is_tuned, 1);
    DSD_SNPRINTF(label, sizeof label, "%s group call", tag);
    rc |= expect_eq_long(label, state.gi[slot], 0);
    DSD_SNPRINTF(label, sizeof label, "%s mac wall timestamp", tag);
    rc |= expect_true(label, state.p25_p2_last_mac_active[slot] != 0);
    DSD_SNPRINTF(label, sizeof label, "%s mac mono timestamp", tag);
    rc |= expect_true(label, state.p25_p2_last_mac_active_m[slot] > 0.0);
    DSD_SNPRINTF(label, sizeof label, "%s patch count", tag);
    rc |= expect_eq_long(label, state.p25_patch_count, 1);
    DSD_SNPRINTF(label, sizeof label, "%s patch sg", tag);
    rc |= expect_eq_long(label, state.p25_patch_sgid[0], 0x3456);
    DSD_SNPRINTF(label, sizeof label, "%s patch active", tag);
    rc |= expect_eq_long(label, state.p25_patch_active[0], 1);
    DSD_SNPRINTF(label, sizeof label, "%s patch kind", tag);
    rc |= expect_eq_long(label, state.p25_patch_is_patch[0], 1);
    DSD_SNPRINTF(label, sizeof label, "%s alias cleared", tag);
    rc |= expect_true(label, state.generic_talker_alias[slot][0] == '\0');
    DSD_SNPRINTF(label, sizeof label, "%s alias src cleared", tag);
    rc |= expect_eq_long(label, state.generic_talker_alias_src[slot], 0);
    DSD_SNPRINTF(label, sizeof label, "%s call banner", tag);
    rc |= expect_contains(label, state.call_string[slot], "Group");
    DSD_SNPRINTF(label, sizeof label, "%s service options valid unchanged", tag);
    rc |= expect_eq_long(label, state.p25_service_options_valid[slot], 1);
    DSD_SNPRINTF(label, sizeof label, "%s emergency unchanged", tag);
    rc |= expect_eq_long(label, state.p25_call_emergency[slot], 1);
    DSD_SNPRINTF(label, sizeof label, "%s packet unchanged", tag);
    rc |= expect_eq_long(label, state.p25_call_is_packet[slot], 1);
    DSD_SNPRINTF(label, sizeof label, "%s priority unchanged", tag);
    rc |= expect_eq_long(label, state.p25_call_priority[slot], 7);
    DSD_SNPRINTF(label, sizeof label, "%s crypto state unchanged", tag);
    rc |= expect_eq_long(label, state.p25_crypto_state[slot], DSD_P25_CRYPTO_UNKNOWN);

    if (slot == 0) {
        DSD_SNPRINTF(label, sizeof label, "%s last tg", tag);
        rc |= expect_eq_long(label, state.lasttg, 0x3456);
        DSD_SNPRINTF(label, sizeof label, "%s last src", tag);
        rc |= expect_eq_long(label, state.lastsrc, 0x010203);
        DSD_SNPRINTF(label, sizeof label, "%s service options unchanged", tag);
        rc |= expect_eq_long(label, state.dmr_so, 0x5A);
    } else {
        DSD_SNPRINTF(label, sizeof label, "%s last tg", tag);
        rc |= expect_eq_long(label, state.lasttgR, 0x3456);
        DSD_SNPRINTF(label, sizeof label, "%s last src", tag);
        rc |= expect_eq_long(label, state.lastsrcR, 0x010203);
        DSD_SNPRINTF(label, sizeof label, "%s service options unchanged", tag);
        rc |= expect_eq_long(label, state.dmr_soR, 0x6B);
    }

    return rc;
}

static int
run_standard_regroup_voice_user_nonstandard_guard_case(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.currentslot = 0;
    state.gi[0] = 1;
    state.lasttg = 0x1111;
    state.lastsrc = 0x010101;
    state.dmr_so = 0x5A;
    state.p25_service_options_valid[0] = 1;
    state.p25_patch_count = 1;
    state.p25_patch_sgid[0] = 0x2222;
    state.p25_patch_active[0] = 1;
    DSD_SNPRINTF(state.generic_talker_alias[0], sizeof state.generic_talker_alias[0], "%s", "KEEP");
    state.generic_talker_alias_src[0] = 0x010101;

    MAC[1] = 0x90;
    MAC[2] = 0x90;
    MAC[3] = 0x34;
    MAC[4] = 0x56;
    MAC[5] = 0x01;
    MAC[6] = 0x02;
    MAC[7] = 0x03;

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    process_MAC_VPDU(&opts, &state, 0, MAC);

    rc |= expect_eq_long("0x90/mfid90 guard no grant dispatch", p25_sm_get_ctx()->grant_count, 0);
    rc |= expect_eq_long("0x90/mfid90 guard last tg", state.lasttg, 0x1111);
    rc |= expect_eq_long("0x90/mfid90 guard last src", state.lastsrc, 0x010101);
    rc |= expect_eq_long("0x90/mfid90 guard group flag", state.gi[0], 1);
    rc |= expect_eq_long("0x90/mfid90 guard service options", state.dmr_so, 0x5A);
    rc |= expect_eq_long("0x90/mfid90 guard service valid", state.p25_service_options_valid[0], 1);
    rc |= expect_eq_long("0x90/mfid90 guard patch count", state.p25_patch_count, 1);
    rc |= expect_eq_long("0x90/mfid90 guard patch sg", state.p25_patch_sgid[0], 0x2222);
    rc |= expect_eq_long("0x90/mfid90 guard mac timestamp", state.p25_p2_last_mac_active[0], 0);
    rc |= expect_contains("0x90/mfid90 guard alias", state.generic_talker_alias[0], "KEEP");

    return rc;
}

static int
test_harris_a4_grg_state_management(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.currentslot = 0;

    MAC[1] = 0xB0;
    MAC[2] = 0xA4;
    MAC[3] = 0x11;
    MAC[4] = (unsigned long long)((0x3 << 5) | 0x09); // active WGID patch, SSN 9
    MAC[5] = 0x12;
    MAC[6] = 0x34;
    MAC[7] = 0xBE;
    MAC[8] = 0xEF;
    MAC[9] = 0x84;
    MAC[10] = 0x11;
    MAC[11] = 0x11;
    MAC[12] = 0x00;
    MAC[13] = 0x00;
    MAC[14] = 0x22;
    MAC[15] = 0x22;
    MAC[16] = 0x33;
    MAC[17] = 0x33;
    process_MAC_VPDU(&opts, &state, 1, MAC);

    int idx = find_patch_idx(&state, 0x1234);
    rc |= expect_true("harris a4 wgid sg exists", idx >= 0);
    if (idx >= 0) {
        rc |= expect_eq_long("harris a4 wgid active", state.p25_patch_active[idx], 1);
        rc |= expect_eq_long("harris a4 wgid patch", state.p25_patch_is_patch[idx], 1);
        rc |= expect_eq_long("harris a4 wgid count", state.p25_patch_wgid_count[idx], 3);
        rc |= expect_true("harris a4 wgid 1111", patch_has_wgid(&state, idx, 0x1111));
        rc |= expect_true("harris a4 wgid 2222", patch_has_wgid(&state, idx, 0x2222));
        rc |= expect_true("harris a4 wgid 3333", patch_has_wgid(&state, idx, 0x3333));
        rc |= expect_eq_long("harris a4 key", state.p25_patch_key[idx], 0xBEEF);
        rc |= expect_eq_long("harris a4 alg", state.p25_patch_alg[idx], 0x84);
        rc |= expect_eq_long("harris a4 ssn", state.p25_patch_ssn[idx], 9);
    }

    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xB0;
    MAC[2] = 0xA4;
    MAC[3] = 0x0B;
    MAC[4] = (unsigned long long)((0x3 << 5) | 0x0A); // SSN replacement
    MAC[5] = 0x12;
    MAC[6] = 0x34;
    MAC[7] = 0xBE;
    MAC[8] = 0xEF;
    MAC[9] = 0x89;
    MAC[10] = 0x44;
    MAC[11] = 0x44;
    process_MAC_VPDU(&opts, &state, 1, MAC);
    idx = find_patch_idx(&state, 0x1234);
    rc |= expect_true("harris a4 replacement sg exists", idx >= 0);
    if (idx >= 0) {
        rc |= expect_eq_long("harris a4 replacement count", state.p25_patch_wgid_count[idx], 1);
        rc |= expect_true("harris a4 replacement new member", patch_has_wgid(&state, idx, 0x4444));
        rc |= expect_true("harris a4 replacement stale member cleared", !patch_has_wgid(&state, idx, 0x1111));
        rc |= expect_eq_long("harris a4 replacement alg", state.p25_patch_alg[idx], 0x89);
        rc |= expect_eq_long("harris a4 replacement ssn", state.p25_patch_ssn[idx], 10);
    }

    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xB0;
    MAC[2] = 0xA4;
    MAC[3] = 0x0B;
    MAC[4] = (unsigned long long)((0x2 << 5) | 0x0A); // inactive WGID command
    MAC[5] = 0x12;
    MAC[6] = 0x34;
    MAC[7] = 0xBE;
    MAC[8] = 0xEF;
    MAC[9] = 0x89;
    MAC[10] = 0x55;
    MAC[11] = 0x55;
    process_MAC_VPDU(&opts, &state, 1, MAC);
    idx = find_patch_idx(&state, 0x1234);
    rc |= expect_true("harris a4 inactive sg exists", idx >= 0);
    if (idx >= 0) {
        rc |= expect_eq_long("harris a4 inactive clears active", state.p25_patch_active[idx], 0);
        rc |= expect_eq_long("harris a4 inactive clears members", state.p25_patch_wgid_count[idx], 0);
        rc |= expect_eq_long("harris a4 inactive clears key", state.p25_patch_key_valid[idx], 0);
    }

    DSD_MEMSET(MAC, 0, sizeof MAC);
    MAC[1] = 0xB0;
    MAC[2] = 0xA4;
    MAC[3] = 0x0E;
    MAC[4] = (unsigned long long)((0x1 << 5) | 0x03); // active WUID patch, SSN 3
    MAC[5] = 0x22;
    MAC[6] = 0x22;
    MAC[7] = 0x12;
    MAC[8] = 0x34;
    MAC[9] = 0x01;
    MAC[10] = 0x02;
    MAC[11] = 0x03;
    MAC[12] = 0x00;
    MAC[13] = 0x00;
    MAC[14] = 0x00;
    process_MAC_VPDU(&opts, &state, 1, MAC);
    idx = find_patch_idx(&state, 0x2222);
    rc |= expect_true("harris a4 wuid sg exists", idx >= 0);
    if (idx >= 0) {
        rc |= expect_eq_long("harris a4 wuid count ignores zero", state.p25_patch_wuid_count[idx], 1);
        rc |= expect_true("harris a4 wuid member", patch_has_wuid(&state, idx, 0x010203));
        rc |= expect_eq_long("harris a4 wuid key", state.p25_patch_key[idx], 0x1234);
        rc |= expect_eq_long("harris a4 wuid ssn", state.p25_patch_ssn[idx], 3);
    }

    return rc;
}

static int
test_motorola_extended_function_supergroup_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    MAC[1] = 0x84;
    MAC[2] = 0x90;
    MAC[4] = 0x02;
    MAC[5] = 0x00;
    MAC[6] = 0x00;
    MAC[7] = 0x12;
    MAC[8] = 0x34;
    MAC[9] = 0x01;
    MAC[10] = 0x02;
    MAC[11] = 0x03;
    process_MAC_VPDU(&opts, &state, 0, MAC);

    int idx = find_patch_idx(&state, 0x1234);
    rc |= expect_true("moto ext create sg exists", idx >= 0);
    if (idx >= 0) {
        rc |= expect_eq_long("moto ext create active", state.p25_patch_active[idx], 1);
        rc |= expect_eq_long("moto ext create wuid count", state.p25_patch_wuid_count[idx], 1);
        rc |= expect_true("moto ext create wuid", patch_has_wuid(&state, idx, 0x010203));
    }

    MAC[5] = 0x01;
    process_MAC_VPDU(&opts, &state, 0, MAC);
    idx = find_patch_idx(&state, 0x1234);
    rc |= expect_true("moto ext cancel sg exists", idx >= 0);
    if (idx >= 0) {
        rc |= expect_eq_long("moto ext cancel inactive", state.p25_patch_active[idx], 0);
        rc |= expect_eq_long("moto ext cancel clears wuid", state.p25_patch_wuid_count[idx], 0);
    }

    DSD_MEMSET(&state, 0, sizeof state);
    MAC[4] = 0x00;
    MAC[5] = 0x7F;
    process_MAC_VPDU(&opts, &state, 0, MAC);
    rc |= expect_eq_long("moto ext class0 metadata only", state.p25_patch_count, 0);
    return rc;
}

static int
test_inband_encrypted_voice_starts_classification_deadline(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    ctx->state = P25_SM_TUNED;
    ctx->vc_is_tdma = 1;
    ctx->vc_freq_hz = 851000000;
    ctx->vc_tg = 0x3456;
    ctx->slots[0].grant_active = 1;
    ctx->slots[0].voice_active = 1;
    ctx->slots[0].last_active_m = 2.0;
    ctx->slots[0].crypto_attempt_m = 1.0;

    MAC[1] = 0x80;
    MAC[2] = 0x90;
    MAC[3] = 0x40;
    MAC[4] = 0x34;
    MAC[5] = 0x56;
    MAC[6] = 0x01;
    MAC[7] = 0x02;
    MAC[8] = 0x03;

    process_MAC_VPDU(&opts, &state, 0, MAC);
    rc |=
        expect_eq_long("in-band encrypted voice pending", state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq_long("in-band encrypted voice gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq_long("in-band encrypted voice activity cleared", ctx->slots[0].voice_active, 0);
    rc |= expect_true("in-band encrypted voice deadline refreshed", ctx->slots[0].crypto_attempt_m > 1.0);

    const double started_m = ctx->slots[0].crypto_attempt_m;
    ctx->slots[0].voice_active = 1;
    process_MAC_VPDU(&opts, &state, 0, MAC);
    rc |= expect_eq_long("repeated in-band encrypted voice activity cleared", ctx->slots[0].voice_active, 0);
    rc |= expect_true("repeated in-band encrypted voice keeps deadline",
                      fabs(ctx->slots[0].crypto_attempt_m - started_m) <= 1.0e-9);
    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    return rc;
}

static int
test_private_voice_ignores_regroup_clear_key_collision(void) {
    static dsd_opts opts;
    static dsd_state state;
    unsigned long long int MAC[24] = {0};
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = 0;
    state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state.p25_p2_audio_allowed[0] = 1;

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    ctx->state = P25_SM_TUNED;
    ctx->vc_is_tdma = 1;
    ctx->vc_freq_hz = 851000000;
    ctx->vc_tg = 0x123456;
    ctx->slots[0].grant_active = 1;
    ctx->slots[0].voice_active = 1;

    p25_patch_add_wgid(&state, 0x2222, 0x3456);
    p25_patch_set_kas(&state, 0x2222, /*key*/ 0, /*alg*/ 0x84, /*ssn*/ 1);

    MAC[1] = 0x02;
    MAC[2] = 0x40;
    MAC[3] = 0x12;
    MAC[4] = 0x34;
    MAC[5] = 0x56;
    MAC[6] = 0x01;
    MAC[7] = 0x02;
    MAC[8] = 0x03;

    process_MAC_VPDU(&opts, &state, 0, MAC);
    rc |= expect_eq_long("private patch collision call type", state.gi[0], 1);
    rc |= expect_eq_long("private patch collision target", state.lasttg, 0x123456);
    rc |=
        expect_eq_long("private patch collision pending", state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_eq_long("private patch collision gate closed", state.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq_long("private patch collision activity cleared", ctx->slots[0].voice_active, 0);
    rc |= expect_true("private patch collision deadline started", ctx->slots[0].crypto_attempt_m > 0.0);

    p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
    return rc;
}

int
main(void) {
    int rc = 0;
    install_trunk_tuning_hooks();

    // Common IDEN: iden=1, type=1 (FDMA), spac=12.5k, base=851.000 MHz
    // Note: base/spacing units match process_channel_to_freq expectations:
    // base is in 5 Hz units; spacing is in 125 Hz units.
    const int iden = 1, type = 1, tdma = 0, spac = 100; // 100*125 = 12.5 kHz
    const long base = 170200000;                        // 170200000*5 = 851,000,000 Hz
    const long cc = 851000000;                          // non-zero CC freq enables tuning

    rc |= test_harris_a4_grg_state_management();
    rc |= test_motorola_extended_function_supergroup_state();
    rc |= test_inband_encrypted_voice_starts_classification_deadline();
    rc |= test_private_voice_ignores_regroup_clear_key_collision();

    // Case A: MFID 0x90, opcode A3 (Group Regroup Channel Grant - Implicit)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0xA3;
        mac[2] = 0x90;
        mac[4] = 0xA5; // service options
        mac[5] = 0x10;
        mac[6] = 0x0A; // channel 0x100A -> 851.125 MHz
        mac[7] = 0x45;
        mac[8] = 0x67; // group id (arbitrary)
        mac[9] = 0x01;
        mac[10] = 0x02;
        mac[11] = 0x03; // source
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base,
            .spac = spac,
        };
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("A3 tuned", tuned == 1);
        rc |= expect_eq_long("A3 vc", vc, 851125000);
    }

    // Case B: UU Voice Service Channel Grant (opcode 0x44)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x44;
        mac[2] = 0x00; // std MFID
        mac[2] = 0x10;
        mac[3] = 0x0A; // channel 0x100A
        mac[4] = 0x00;
        mac[5] = 0x00;
        mac[6] = 0x01; // target
        mac[7] = 0x00;
        mac[8] = 0x00;
        mac[9] = 0x02; // source
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base,
            .spac = spac,
        };
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("UU tuned", tuned == 1);
        rc |= expect_eq_long("UU vc", vc, 851125000);
    }

    // Case B2: Service-option-unknown private grants must reach the centralized
    // silent probe policy when encrypted-call lockout is enabled.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_private_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x44;
        MAC[2] = 0x10;
        MAC[3] = 0x0A;
        MAC[6] = 0x01;
        MAC[9] = 0x02;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("UU unknown-service probe tunes", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("UU unknown-service probe vc", state.p25_vc_freq[0], 851125000);
        rc |= expect_eq_long("UU unknown-service probe pending", state.p25_crypto_state[0],
                             DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    }

    // Case C: Group Voice Channel Grant Update Multiple - Explicit (opcode 0x25)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x25;
        mac[2] = 0x00; // svc1
        mac[3] = 0x10;
        mac[4] = 0x0A; // channel T1 0x100A
        mac[5] = 0x00;
        mac[6] = 0x00; // channel R1 unused
        mac[7] = 0x12;
        mac[8] = 0x34; // group1
        mac[9] = 0x00; // svc2
        mac[10] = 0x10;
        mac[11] = 0x0B; // channel T2 0x100B
        mac[12] = 0x00;
        mac[13] = 0x00; // channel R2 unused
        mac[14] = 0x56;
        mac[15] = 0x78; // group2
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base,
            .spac = spac,
        };
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("0x25 tuned", tuned == 1);
        rc |= expect_eq_long("0x25 vc", vc, 851125000);
    }

    // Case D: SNDCP Data Channel Announcement resolves both T and R channels (opcode 0xD6)
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0xD6;
        mac[4] = 0x10;
        mac[5] = 0x0A; // CHAN-T 0x100A
        mac[6] = 0x10;
        mac[7] = 0x0B; // CHAN-R 0x100B
        long freq_t = 0;
        long freq_r = 0;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base,
            .spac = spac,
        };
        p25_test_invoke_mac_vpdu_channel_cache(mac, 24, &cfg, 0x100A, 0x100B, &freq_t, &freq_r);
        rc |= expect_eq_long("0xD6 CHAN-T cache", freq_t, 851125000);
        rc |= expect_eq_long("0xD6 CHAN-R cache", freq_r, 851137500);
    }

    // Case D2: Bridged P1 TSBK 0x03 uses synthetic opcode 0x43 and skips the reserved octet.
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0x43;
        mac[2] = 0x85; // service options
        mac[3] = 0xAA; // reserved octet; must not become CHAN-T high byte
        mac[4] = 0x10;
        mac[5] = 0x0A; // CHAN-T 0x100A
        mac[6] = 0x10;
        mac[7] = 0x0B; // CHAN-R 0x100B
        mac[8] = 0x34;
        mac[9] = 0x56; // group
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base,
            .spac = spac,
        };
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("0x43 tuned after reserved skip", tuned == 1);
        rc |= expect_eq_long("0x43 vc", vc, 851125000);
    }

    // Case D3: 0x43 propagates SVC, CHAN-T, TG, source=0, and resolves CHAN-R.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        opts.trunk_tune_data_calls = 1;
        state.p25_cc_freq = cc;
        seed_fdma_iden(&state, iden, type, base, spac);

        MAC[1] = 0x43;
        MAC[2] = 0x85;
        MAC[3] = 0xAA;
        MAC[4] = 0x10;
        MAC[5] = 0x0A;
        MAC[6] = 0x10;
        MAC[7] = 0x0B;
        MAC[8] = 0x34;
        MAC[9] = 0x56;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0x43 capture called", ctx->grant_count, 1);
        rc |= expect_eq_long("0x43 capture channel", ctx->vc_channel, 0x100A);
        rc |= expect_eq_long("0x43 capture svc", ctx->slots[0].svc_bits, 0x85);
        rc |= expect_eq_long("0x43 capture tg", ctx->slots[0].ota_tg, 0x3456);
        rc |= expect_eq_long("0x43 capture src", ctx->vc_src, 0);
        rc |= expect_eq_long("0x43 CHAN-R cache", state.trunk_chan_map[0x100B], 851137500);
        rc |= expect_contains("0x43 active channel", state.active_channel[0], "TG: 13398");
    }

    // Case D4: True MAC 0xC0 Group Voice Channel Grant Explicit propagates source and both channels.
    {
        unsigned char mac[24];
        DSD_MEMSET(mac, 0, sizeof mac);
        mac[1] = 0xC0;
        mac[2] = 0x23; // service options
        mac[3] = 0x10;
        mac[4] = 0x0C; // CHAN-T 0x100C
        mac[5] = 0x10;
        mac[6] = 0x0D; // CHAN-R 0x100D
        mac[7] = 0x22;
        mac[8] = 0x22; // group
        mac[9] = 0x01;
        mac[10] = 0x02;
        mac[11] = 0x03; // source
        long vc = 0;
        int tuned = 0;
        p25_test_iden_config cfg = {
            .iden = iden,
            .type = type,
            .tdma = tdma,
            .base = base,
            .spac = spac,
        };
        p25_test_invoke_mac_vpdu_capture(mac, 24, 1, cc, &cfg, &vc, &tuned);
        rc |= expect_true("0xC0 tuned", tuned == 1);
        rc |= expect_eq_long("0xC0 vc", vc, 851150000);
    }

    // Case D5: 0xC0 captures SVC, CHAN-T, TG, source address, service-option state, and CHAN-R cache.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        state.p25_cc_freq = cc;
        seed_fdma_iden(&state, iden, type, base, spac);

        MAC[1] = 0xC0;
        MAC[2] = 0x23;
        MAC[3] = 0x10;
        MAC[4] = 0x0C;
        MAC[5] = 0x10;
        MAC[6] = 0x0D;
        MAC[7] = 0x22;
        MAC[8] = 0x22;
        MAC[9] = 0x01;
        MAC[10] = 0x02;
        MAC[11] = 0x03;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0xC0 capture called", ctx->grant_count, 1);
        rc |= expect_eq_long("0xC0 capture channel", ctx->vc_channel, 0x100C);
        rc |= expect_eq_long("0xC0 capture svc", ctx->slots[0].svc_bits, 0x23);
        rc |= expect_eq_long("0xC0 capture tg", ctx->slots[0].ota_tg, 0x2222);
        rc |= expect_eq_long("0xC0 capture src", ctx->vc_src, 0x010203);
        rc |= expect_eq_long("0xC0 CHAN-R cache", state.trunk_chan_map[0x100D], 851162500);
        rc |= expect_eq_long("0xC0 stored service options", state.dmr_so, 0x23);
        rc |= expect_eq_long("0xC0 service options valid", state.p25_service_options_valid[0], 1);
    }

    // Case D5a: patched-supergroup grants dispatch to the SM even when TG hold matches only a member WGID.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        state.p25_cc_freq = cc;
        state.tg_hold = 0x3333;
        seed_fdma_iden(&state, iden, type, base, spac);
        p25_patch_add_wgid(&state, 0x2222, 0x3333);

        MAC[1] = 0xC0;
        MAC[2] = 0x23;
        MAC[3] = 0x10;
        MAC[4] = 0x0C;
        MAC[5] = 0x10;
        MAC[6] = 0x0D;
        MAC[7] = 0x22;
        MAC[8] = 0x22;
        MAC[9] = 0x01;
        MAC[10] = 0x02;
        MAC[11] = 0x03;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0xC0 patch member hold dispatch", ctx->grant_count, 1);
        rc |= expect_eq_long("0xC0 patch member hold tg", ctx->slots[0].ota_tg, 0x2222);
        rc |= expect_eq_long("0xC0 patch member hold src", ctx->vc_src, 0x010203);
    }

    // Case D5b: MFID90 0xA3 propagates service options and source.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        opts.trunk_tune_data_calls = 1;
        state.p25_cc_freq = cc;
        seed_fdma_iden(&state, iden, type, base, spac);

        MAC[1] = 0xA3;
        MAC[2] = 0x90;
        MAC[4] = 0xA5;
        MAC[5] = 0x10;
        MAC[6] = 0x0A;
        MAC[7] = 0x34;
        MAC[8] = 0x56;
        MAC[9] = 0x01;
        MAC[10] = 0x02;
        MAC[11] = 0x03;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0xA3 capture called", ctx->grant_count, 1);
        rc |= expect_eq_long("0xA3 capture channel", ctx->vc_channel, 0x100A);
        rc |= expect_eq_long("0xA3 capture svc", ctx->slots[0].svc_bits, 0xA5);
        rc |= expect_eq_long("0xA3 capture tg", ctx->slots[0].ota_tg, 0x3456);
        rc |= expect_eq_long("0xA3 capture src", ctx->vc_src, 0x010203);
        rc |= expect_eq_long("0xA3 stored service options", state.dmr_so, 0xA5);
        rc |= expect_eq_long("0xA3 emergency state", state.p25_call_emergency[0], 1);
    }

    // Case D5c: MFID90 0xA4 propagates service options, source, and CHAN-R cache.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        state.p25_cc_freq = cc;
        seed_fdma_iden(&state, iden, type, base, spac);

        MAC[1] = 0xA4;
        MAC[2] = 0x90;
        MAC[4] = 0x23;
        MAC[5] = 0x10;
        MAC[6] = 0x0A;
        MAC[7] = 0x10;
        MAC[8] = 0x0B;
        MAC[9] = 0x45;
        MAC[10] = 0x67;
        MAC[11] = 0x0A;
        MAC[12] = 0x0B;
        MAC[13] = 0x0C;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0xA4 capture called", ctx->grant_count, 1);
        rc |= expect_eq_long("0xA4 capture channel", ctx->vc_channel, 0x100A);
        rc |= expect_eq_long("0xA4 capture svc", ctx->slots[0].svc_bits, 0x23);
        rc |= expect_eq_long("0xA4 capture tg", ctx->slots[0].ota_tg, 0x4567);
        rc |= expect_eq_long("0xA4 capture src", ctx->vc_src, 0x0A0B0C);
        rc |= expect_eq_long("0xA4 CHAN-R cache", state.trunk_chan_map[0x100B], 851137500);
        rc |= expect_eq_long("0xA4 stored service options", state.dmr_so, 0x23);
    }

    // Case D5c: MFID90 0x83 propagates service options without a source.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        state.p25_cc_freq = cc;
        seed_fdma_iden(&state, iden, type, base, spac);

        MAC[1] = 0x83;
        MAC[2] = 0x90;
        MAC[3] = 0x81;
        MAC[4] = 0x55;
        MAC[5] = 0x66;
        MAC[6] = 0x10;
        MAC[7] = 0x0A;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0x83 capture called", ctx->grant_count, 1);
        rc |= expect_eq_long("0x83 capture channel", ctx->vc_channel, 0x100A);
        rc |= expect_eq_long("0x83 capture svc", ctx->slots[0].svc_bits, 0x81);
        rc |= expect_eq_long("0x83 capture tg", ctx->slots[0].ota_tg, 0x5566);
        rc |= expect_eq_long("0x83 capture src", ctx->vc_src, 0);
        rc |= expect_eq_long("0x83 stored service options", state.dmr_so, 0x81);
    }

    // Case D5c2: SACCH grants apply service-option state to the decoded slot, not currentslot.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        state.currentslot = 0;
        state.dmr_so = 0x11;
        state.dmr_soR = 0x22;
        state.p25_service_options_valid[0] = 1;

        MAC[1] = 0x40;
        MAC[2] = 0x93; // emergency, packet, priority 3
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x12;
        MAC[6] = 0x34;
        MAC[7] = 0x01;
        MAC[8] = 0x02;
        MAC[9] = 0x03;

        process_MAC_VPDU(&opts, &state, 1, MAC);
        rc |= expect_eq_long("0x40 SACCH slot0 svc unchanged", state.dmr_so, 0x11);
        rc |= expect_eq_long("0x40 SACCH slot1 svc", state.dmr_soR, 0x93);
        rc |= expect_eq_long("0x40 SACCH slot0 valid unchanged", state.p25_service_options_valid[0], 1);
        rc |= expect_eq_long("0x40 SACCH slot1 valid", state.p25_service_options_valid[1], 1);
        rc |= expect_eq_long("0x40 SACCH slot0 emergency unchanged", state.p25_call_emergency[0], 0);
        rc |= expect_eq_long("0x40 SACCH slot1 emergency", state.p25_call_emergency[1], 1);
        rc |= expect_eq_long("0x40 SACCH slot1 packet", state.p25_call_is_packet[1], 1);
    }

    // Case D5c3: shared explicit-grant helper also stores SACCH service bits on the decoded slot.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        state.currentslot = 1;
        state.dmr_so = 0x11;
        state.dmr_soR = 0x22;
        state.p25_service_options_valid[1] = 1;

        MAC[1] = 0xC0;
        MAC[2] = 0x50; // encrypted packet
        MAC[3] = 0x10;
        MAC[4] = 0x0C;
        MAC[5] = 0x10;
        MAC[6] = 0x0D;
        MAC[7] = 0x22;
        MAC[8] = 0x22;
        MAC[9] = 0x01;
        MAC[10] = 0x02;
        MAC[11] = 0x03;

        process_MAC_VPDU(&opts, &state, 1, MAC);
        rc |= expect_eq_long("0xC0 SACCH slot0 svc", state.dmr_so, 0x50);
        rc |= expect_eq_long("0xC0 SACCH slot1 svc unchanged", state.dmr_soR, 0x22);
        rc |= expect_eq_long("0xC0 SACCH slot0 valid", state.p25_service_options_valid[0], 1);
        rc |= expect_eq_long("0xC0 SACCH slot1 valid unchanged", state.p25_service_options_valid[1], 1);
        rc |= expect_eq_long("0xC0 SACCH slot0 packet", state.p25_call_is_packet[0], 1);
        rc |= expect_eq_long("0xC0 SACCH slot1 packet unchanged", state.p25_call_is_packet[1], 0);
    }

    // Case D5c4: MFID90 grant helpers use the decoded SACCH slot for call state.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        state.currentslot = 1;
        state.dmr_so = 0x11;
        state.dmr_soR = 0x22;

        MAC[1] = 0xA3;
        MAC[2] = 0x90;
        MAC[4] = 0x81; // emergency, priority 1
        MAC[5] = 0x10;
        MAC[6] = 0x0A;
        MAC[7] = 0x34;
        MAC[8] = 0x56;
        MAC[9] = 0x01;
        MAC[10] = 0x02;
        MAC[11] = 0x03;

        process_MAC_VPDU(&opts, &state, 1, MAC);
        rc |= expect_eq_long("0xA3 SACCH slot0 svc", state.dmr_so, 0x81);
        rc |= expect_eq_long("0xA3 SACCH slot1 svc unchanged", state.dmr_soR, 0x22);
        rc |= expect_eq_long("0xA3 SACCH slot0 emergency", state.p25_call_emergency[0], 1);
        rc |= expect_eq_long("0xA3 SACCH slot1 emergency unchanged", state.p25_call_emergency[1], 0);
    }

    // Case D5d: encrypted MFID90 0xA3 grants become silent classification probes
    // when encrypted following is disabled.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");
        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        seed_fdma_iden(&state, iden, type, base, spac);

        MAC[1] = 0xA3;
        MAC[2] = 0x90;
        MAC[4] = 0x40;
        MAC[5] = 0x10;
        MAC[6] = 0x0A;
        MAC[7] = 0x22;
        MAC[8] = 0x22;
        MAC[9] = 0x01;
        MAC[10] = 0x02;
        MAC[11] = 0x03;

        process_MAC_VPDU(&opts, &state, 0, MAC);

        rc |= expect_true("0xA3 encrypted probe tunes", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0xA3 encrypted probe gate closed", state.p25_p2_audio_allowed[0], 0);
        rc |=
            expect_eq_long("0xA3 encrypted probe pending", state.p25_crypto_state[0], DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_contains("0xA3 encrypted active", state.active_channel[0], "MFID90 Active Ch: 100A");
    }

    // Case D5e: MFID90 0x80 voice-user messages store service options.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_tune_enc_calls = 1;
        state.currentslot = 0;

        MAC[1] = 0x80;
        MAC[2] = 0x90;
        MAC[3] = 0xA5;
        MAC[4] = 0x34;
        MAC[5] = 0x56;
        MAC[6] = 0x01;
        MAC[7] = 0x02;
        MAC[8] = 0x03;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x80 last tg", state.lasttg, 0x3456);
        rc |= expect_eq_long("0x80 last src", state.lastsrc, 0x010203);
        rc |= expect_eq_long("0x80 stored service options", state.dmr_so, 0xA5);
        rc |= expect_eq_long("0x80 service options valid", state.p25_service_options_valid[0], 1);
        rc |= expect_eq_long("0x80 emergency state", state.p25_call_emergency[0], 1);
        rc |= expect_contains("0x80 call banner", state.call_string[0], "Emergency");
    }

    // Case D5e2: first regroup voice-user metadata preserves the patch-member policy target from the grant.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_use_allow_list = 1;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.currentslot = 0;
        state.lasttg = 0x2222;
        state.lastsrc = 0x010101;

        rc |= expect_eq_long("0x80 policy seed member", seed_policy_group(&state, 0x1234, "A", "PATCH-MEMBER"), 0);
        p25_patch_add_wgid(&state, 0x3456, 0x1234);
        state.p25_policy_tg[0] = 0x1234;

        MAC[1] = 0x80;
        MAC[2] = 0x90;
        MAC[3] = 0x00;
        MAC[4] = 0x34;
        MAC[5] = 0x56;
        MAC[6] = 0x01;
        MAC[7] = 0x02;
        MAC[8] = 0x03;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x80 policy member last tg", state.lasttg, 0x3456);
        rc |= expect_eq_long("0x80 policy member preserved", state.p25_policy_tg[0], 0x1234);
        state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
        rc |= expect_eq_long("0x80 policy member audio", dsd_p25p2_decode_audio_allowed(&opts, &state, 0, 0), 1);

        state.p25_policy_tg[0] = 0x7777;
        MAC[4] = 0x45;
        MAC[5] = 0x67;
        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x80 stale policy clears", state.p25_policy_tg[0], 0);

        dsd_state_ext_free_all(&state);
    }

    // Case D5f: MFID90 0xA0 extended voice-user messages store service options.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        opts.trunk_tune_enc_calls = 1;
        state.currentslot = 1;

        MAC[1] = 0xA0;
        MAC[2] = 0x90;
        MAC[4] = 0x40;
        MAC[5] = 0x45;
        MAC[6] = 0x67;
        MAC[7] = 0x0A;
        MAC[8] = 0x0B;
        MAC[9] = 0x0C;
        MAC[10] = 0x12;
        MAC[11] = 0x34;
        MAC[12] = 0x50;
        MAC[13] = 0x67;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0xA0 last tg", state.lasttgR, 0x4567);
        rc |= expect_eq_long("0xA0 last src", state.lastsrcR, 0x0A0B0C);
        rc |= expect_eq_long("0xA0 stored service options", state.dmr_soR, 0x40);
        rc |= expect_eq_long("0xA0 service options valid", state.p25_service_options_valid[1], 1);
        rc |= expect_contains("0xA0 call banner", state.call_string[1], "Encrypted");
    }

    rc |= run_standard_regroup_voice_user_case(0x00, 0, "0x90/mfid00");
    rc |= run_standard_regroup_voice_user_case(0x01, 1, "0x90/mfid01");
    rc |= run_standard_regroup_voice_user_nonstandard_guard_case();

    // Case D6: Group Affiliation Response 0x68 accepts on low GAV bits, not status bits 5-6.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        MAC[1] = 0x68;
        MAC[3] = 0x20; // old parser saw GAV=1 from bits 5-6; correct low bits are accepted (0)
        MAC[4] = 0x12;
        MAC[5] = 0x34; // AGA
        MAC[6] = 0x45;
        MAC[7] = 0x67; // GA
        MAC[8] = 0x01;
        MAC[9] = 0x02;
        MAC[10] = 0x03; // TA

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x68 accepted aff count", state.p25_aff_count, 1);
        rc |= expect_eq_long("0x68 accepted ga count", state.p25_ga_count, 1);
        rc |= expect_eq_long("0x68 accepted TA", state.p25_aff_rid[0], 0x010203);
        rc |= expect_eq_long("0x68 accepted GA rid", state.p25_ga_rid[0], 0x010203);
        rc |= expect_eq_long("0x68 accepted GA tg", state.p25_ga_tg[0], 0x4567);
    }

    // Case D7: Group Affiliation Response 0x68 rejects when low GAV bits are non-zero.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        MAC[1] = 0x68;
        MAC[3] = 0x82; // LG set, low GAV=2 rejected; old parser saw bits 5-6 as accepted (0)
        MAC[4] = 0x12;
        MAC[5] = 0x34;
        MAC[6] = 0x45;
        MAC[7] = 0x67;
        MAC[8] = 0x01;
        MAC[9] = 0x02;
        MAC[10] = 0x03;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x68 rejected aff count", state.p25_aff_count, 0);
        rc |= expect_eq_long("0x68 rejected ga count", state.p25_ga_count, 0);
    }

    // Case D8: Location Registration Response 0x6B accepts and tracks TA -> group.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        MAC[0] = 0x07; // bridged P1 TSBK layout
        MAC[1] = 0x6B;
        MAC[2] = 0x00; // RV=0 accepted
        MAC[3] = 0x45;
        MAC[4] = 0x67; // group
        MAC[5] = 0x02; // RFSS
        MAC[6] = 0x03; // site
        MAC[7] = 0x0A;
        MAC[8] = 0x0B;
        MAC[9] = 0x0C; // target address

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x6B accepted aff count", state.p25_aff_count, 1);
        rc |= expect_eq_long("0x6B accepted ga count", state.p25_ga_count, 1);
        rc |= expect_eq_long("0x6B accepted TA", state.p25_aff_rid[0], 0x0A0B0C);
        rc |= expect_eq_long("0x6B accepted GA rid", state.p25_ga_rid[0], 0x0A0B0C);
        rc |= expect_eq_long("0x6B accepted GA tg", state.p25_ga_tg[0], 0x4567);
    }

    // Case D9: Location Registration Response 0x6B rejects do not track affiliation.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        MAC[0] = 0x07;
        MAC[1] = 0x6B;
        MAC[2] = 0x02; // RV=2 rejected
        MAC[3] = 0x45;
        MAC[4] = 0x67;
        MAC[7] = 0x0A;
        MAC[8] = 0x0B;
        MAC[9] = 0x0C;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x6B rejected aff count", state.p25_aff_count, 0);
        rc |= expect_eq_long("0x6B rejected ga count", state.p25_ga_count, 0);
    }

    // Case E: a grant decoded through MAC VPDU remains eligible immediately
    // after an explicit release.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 accepted after release", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x40 reassigned vc", state.p25_vc_freq[0], 851125000);
    }

    // Case E2: same-carrier TDMA grants decoded on a VC MAC still reach the state machine.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        const int tdma_iden = 2;
        const int ch_slot1 = (tdma_iden << 12) | 0x0003;
        const long vc_freq = 851012500;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        opts.trunk_is_tuned = 1;
        state.p25_cc_freq = cc;
        state.p25_vc_freq[0] = vc_freq;
        state.p25_vc_freq[1] = vc_freq;
        state.trunk_vc_freq[0] = vc_freq;
        state.trunk_vc_freq[1] = vc_freq;
        seed_tdma_iden(&state, tdma_iden, /*type*/ 3, base, spac);

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = (unsigned long long int)((ch_slot1 >> 8) & 0xFF);
        MAC[4] = (unsigned long long int)(ch_slot1 & 0xFF);
        MAC[5] = 0x23;
        MAC[6] = 0x45; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x07; // source

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0x40 tuned same-carrier dispatch", ctx->grant_count, 1);
        rc |= expect_eq_long("0x40 tuned same-carrier channel", ctx->vc_channel, ch_slot1);
        rc |= expect_eq_long("0x40 tuned same-carrier tg", ctx->slots[1].ota_tg, 0x2345);
        rc |= expect_eq_long("0x40 tuned same-carrier src", ctx->vc_src, 7);
        rc |= expect_true("0x40 tuned same-carrier remains tuned", opts.trunk_is_tuned == 1);
    }

    // Case F: an LCCH VPDU grant seeds the current tuner as a recoverable CC before retuning.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtlsdr_center_freq = (uint32_t)cc;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p2_is_lcch = 1;
        state.p25_cc_is_tdma = 2;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 LCCH grant tuned", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x40 LCCH grant seeded CC", state.p25_cc_freq, cc);
        rc |= expect_eq_long("0x40 LCCH grant seeded trunk CC", state.trunk_cc_freq, cc);
        rc |= expect_eq_long("0x40 LCCH grant seeded LCN0", state.trunk_lcn_freq[0], cc);
        rc |= expect_true("0x40 LCCH grant marks TDMA CC", state.p25_cc_is_tdma == 1);
        rc |= expect_eq_long("0x40 LCCH grant vc", state.p25_vc_freq[0], 851125000);
    }

    // Case F2: an LCCH grant without a known or discoverable CC return target must not dispatch.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p2_is_lcch = 1;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);

        rc |= expect_eq_long("0x40 unseeded LCCH grant not dispatched", p25_sm_get_ctx()->grant_count, 0);
        rc |= expect_true("0x40 unseeded LCCH grant not tuned", opts.trunk_is_tuned == 0);
        rc |= expect_eq_long("0x40 unseeded LCCH no p25 CC", state.p25_cc_freq, 0);
        rc |= expect_eq_long("0x40 unseeded LCCH no trunk CC", state.trunk_cc_freq, 0);
        rc |= expect_eq_long("0x40 unseeded LCCH no vc", state.p25_vc_freq[0], 0);
    }

    // Case G: traffic-channel MACs must not learn the current VC tuner frequency as the CC.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.rtlsdr_center_freq = 852000000U;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p2_is_lcch = 0;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant carried on FACCH/SACCH traffic MAC
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 traffic MAC not tuned", opts.trunk_is_tuned == 0);
        rc |= expect_eq_long("0x40 traffic MAC no p25 CC seed", state.p25_cc_freq, 0);
        rc |= expect_eq_long("0x40 traffic MAC no trunk CC seed", state.trunk_cc_freq, 0);
        rc |= expect_eq_long("0x40 traffic MAC no vc", state.p25_vc_freq[0], 0);
    }

    // Case H: first live VPDU grant can use the generic trunk CC alias when the P25 alias is still unknown.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        state.trunk_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x40; // Group Voice Channel Grant
        MAC[2] = 0x00; // svc
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[5] = 0x12;
        MAC[6] = 0x34; // group
        MAC[7] = 0x00;
        MAC[8] = 0x00;
        MAC[9] = 0x02; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x40 trunk alias CC tuned", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x40 trunk alias p25 CC", state.p25_cc_freq, cc);
        rc |= expect_eq_long("0x40 trunk alias trunk CC", state.trunk_cc_freq, cc);
        rc |= expect_eq_long("0x40 trunk alias vc", state.p25_vc_freq[0], 851125000);
    }

    // Case I: compact grant-update paths remain eligible after an explicit release.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x42; // Group Voice Channel Grant Update - Implicit
        MAC[2] = 0x10;
        MAC[3] = 0x0A; // channel1 0x100A -> 851.125 MHz
        MAC[4] = 0x12;
        MAC[5] = 0x34; // group1
        MAC[6] = 0x10;
        MAC[7] = 0x0A; // channel2 same as channel1, so only one candidate is tried
        MAC[8] = 0x12;
        MAC[9] = 0x35; // group2

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x42 accepted after release", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x42 reassigned vc", state.p25_vc_freq[0], 851125000);
    }

    // Case J: no-SVC 0x42 grants are suppressed after a proven encrypted call.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;
        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        p25_emit_enc_lockout_once_typed(&opts, &state, 0, 0x1234, /*svc_bits*/ 0, 1);

        MAC[1] = 0x42; // Group Voice Channel Grant Update - Implicit
        MAC[2] = 0x10;
        MAC[3] = 0x0A; // channel1 0x100A -> 851.125 MHz
        MAC[4] = 0x12;
        MAC[5] = 0x34; // group1
        MAC[6] = 0x10;
        MAC[7] = 0x0A; // channel2 same as channel1, so only one candidate is tried
        MAC[8] = 0x12;
        MAC[9] = 0x35; // group2

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x42 suppressed by transient enc cache", opts.trunk_is_tuned == 0);
        rc |= expect_eq_long("0x42 transient enc cache no vc", state.p25_vc_freq[0], 0);
    }

    // Case K: rejected P2 NSBs still prove the system carries TDMA voice without changing return CC metadata.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_is_tuned = 1;
        state.p25_cc_freq = cc;
        state.trunk_cc_freq = cc;
        state.p25_cc_is_tdma = 0;
        state.p2_wacn = 0x11111;
        state.p2_sysid = 0x222;
        state.p2_cc = 0x333;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;

        MAC[1] = 0x7B; // Network Status Broadcast - Abbreviated
        MAC[2] = 0x01; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1; // WACN 0xABCDE, SYSID high nibble 0x1
        MAC[6] = 0x23; // SYSID low byte
        MAC[7] = 0x10;
        MAC[8] = 0x0A; // channel 0x100A -> 851.125 MHz, rejected while selected CC is 851 MHz
        MAC[9] = 0x00; // sysclass
        MAC[10] = 0x00;
        MAC[11] = 0x55; // NAC

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 rejected nsb sets system tdma hint", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 rejected nsb preserves cc modulation", state.p25_cc_is_tdma == 0);
        rc |= expect_eq_long("p2 rejected nsb preserves p25 cc", state.p25_cc_freq, cc);
        rc |= expect_eq_long("p2 rejected nsb preserves trunk cc", state.trunk_cc_freq, cc);
        rc |= expect_eq_long("p2 rejected nsb preserves wacn", (long)state.p2_wacn, 0x11111);
        rc |= expect_eq_long("p2 rejected nsb preserves sysid", (long)state.p2_sysid, 0x222);
        rc |= expect_eq_long("p2 rejected nsb preserves nac", (long)state.p2_cc, 0x333);
    }

    // Case K: P2 abbreviated NSB with unknown IDEN keeps identity metadata but
    // does not promote the unresolved channel to current CC or LCN0.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        state.p2_wacn = 0x11111;
        state.p2_sysid = 0x222;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;
        state.p25_pending_announcement_count = 1;
        state.p25_pending_announcements[0].populated = 1;
        state.p25_pending_announcements[0].channel = 0x1001;

        MAC[1] = 0x7B;
        MAC[2] = 0x05; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1;
        MAC[6] = 0x23;
        MAC[7] = 0x80;
        MAC[8] = 0x0A; // unknown IDEN 8
        MAC[9] = 0x00;
        MAC[10] = 0x00;
        MAC[11] = 0x55;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 unknown-iden nsb marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 unknown-iden nsb does not mark cc tdma", state.p25_cc_is_tdma == 0);
        rc |= expect_eq_long("p2 unknown-iden nsb p25 cc empty", state.p25_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb trunk cc empty", state.trunk_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb lcn0 empty", state.trunk_lcn_freq[0], 0);
        rc |= expect_eq_long("p2 unknown-iden nsb wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 unknown-iden nsb sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 unknown-iden nsb nac", (long)state.p2_cc, 0x055);
        rc |= expect_eq_long("p2 unknown-iden nsb lra", state.p25_site_lra, 0x05);
        rc |= expect_eq_long("p2 unknown-iden nsb lra valid", state.p25_site_lra_valid, 1);
        rc |= expect_eq_long("p2 unknown-iden nsb clears stale iden", state.p25_iden_fdma[iden].populated, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb clears explicit iden", state.p25_chan_tdma_explicit[iden], 0);
        rc |= expect_eq_long("p2 unknown-iden nsb clears pending", state.p25_pending_announcement_count, 0);
    }

    // Case L: P2 extended NSB with unknown IDEN keeps identity metadata but
    // does not promote the unresolved channel to current CC.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        state.p2_wacn = 0x11111;
        state.p2_sysid = 0x222;
        state.p25_iden_tdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 2;
        state.p25_pending_announcement_count = 1;
        state.p25_pending_announcements[0].populated = 1;
        state.p25_pending_announcements[0].channel = 0x1002;

        MAC[1] = 0xFB;
        MAC[2] = 0x06; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1;
        MAC[6] = 0x23;
        MAC[7] = 0x80;
        MAC[8] = 0x0A; // CHAN-T unknown IDEN 8
        MAC[9] = 0x80;
        MAC[10] = 0x0B; // CHAN-R unknown IDEN 8
        MAC[12] = 0x00;
        MAC[13] = 0x56;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 unknown-iden nsb-ext marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 unknown-iden nsb-ext does not mark cc tdma", state.p25_cc_is_tdma == 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext p25 cc empty", state.p25_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext trunk cc empty", state.trunk_cc_freq, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext lcn0 empty", state.trunk_lcn_freq[0], 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext nac", (long)state.p2_cc, 0x056);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext lra", state.p25_site_lra, 0x06);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext lra valid", state.p25_site_lra_valid, 1);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext clears stale iden", state.p25_iden_tdma[iden].populated, 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext clears explicit iden", state.p25_chan_tdma_explicit[iden], 0);
        rc |= expect_eq_long("p2 unknown-iden nsb-ext clears pending", state.p25_pending_announcement_count, 0);
    }

    // Case M: accepted P2 abbreviated NSB promotes the current CC to TDMA,
    // stores system identity, seeds LCN0, and confirms matching IDEN provenance.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 1;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_iden_fdma[iden].wacn = 0xABCDE;
        state.p25_iden_fdma[iden].sysid = 0x123;

        MAC[1] = 0x7B; // Network Status Broadcast - Abbreviated
        MAC[2] = 0x01; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1; // WACN 0xABCDE, SYSID high nibble 0x1
        MAC[6] = 0x23; // SYSID low byte
        MAC[7] = 0x10;
        MAC[8] = 0x0A; // channel 0x100A -> 851.125 MHz
        MAC[9] = 0x00; // sysclass
        MAC[10] = 0x00;
        MAC[11] = 0x55; // NAC

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 accepted nsb marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 accepted nsb marks cc tdma", state.p25_cc_is_tdma == 1);
        rc |= expect_eq_long("p2 accepted nsb p25 cc", state.p25_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb trunk cc", state.trunk_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb lcn0", state.trunk_lcn_freq[0], 851125000);
        rc |= expect_eq_long("p2 accepted nsb wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 accepted nsb sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 accepted nsb nac", (long)state.p2_cc, 0x055);
        rc |= expect_eq_long("p2 accepted nsb confirms iden", state.p25_iden_fdma[iden].trust, 2);
    }

    // Case M2: rejected voice-followed abbreviated NSB must not overwrite
    // current-site LRA metadata from a different control channel.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_is_tuned = 1;
        state.p25_cc_freq = 851000000;
        state.trunk_cc_freq = 851000000;
        state.p25_site_lra = 0x44;
        state.p25_site_lra_valid = 1;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].populated = 1;

        MAC[1] = 0x7B;
        MAC[2] = 0x01;
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1;
        MAC[6] = 0x23;
        MAC[7] = 0x10;
        MAC[8] = 0x0A;
        MAC[10] = 0x00;
        MAC[11] = 0x55;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("p2 rejected voice nsb preserves p25 cc", state.p25_cc_freq, 851000000);
        rc |= expect_eq_long("p2 rejected voice nsb preserves lra", state.p25_site_lra, 0x44);
        rc |= expect_eq_long("p2 rejected voice nsb preserves lra valid", state.p25_site_lra_valid, 1);
    }

    // Case N: accepted P2 extended NSB resolves both T/R channels and updates
    // TDMA CC identity through the same state-machine notification path.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].populated = 1;

        MAC[1] = 0xFB; // Network Status Broadcast - Extended
        MAC[2] = 0x02; // LRA
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1; // WACN 0xABCDE, SYSID high nibble 0x1
        MAC[6] = 0x23; // SYSID low byte
        MAC[7] = 0x10;
        MAC[8] = 0x0A; // CHAN-T 0x100A
        MAC[9] = 0x10;
        MAC[10] = 0x0B; // CHAN-R 0x100B
        MAC[12] = 0x00;
        MAC[13] = 0x56; // NAC

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("p2 accepted nsb-ext marks system tdma", state.p25_sys_is_tdma == 1);
        rc |= expect_true("p2 accepted nsb-ext marks cc tdma", state.p25_cc_is_tdma == 1);
        rc |= expect_eq_long("p2 accepted nsb-ext p25 cc", state.p25_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb-ext trunk cc", state.trunk_cc_freq, 851125000);
        rc |= expect_eq_long("p2 accepted nsb-ext chan-t cache", state.trunk_chan_map[0x100A], 851125000);
        rc |= expect_eq_long("p2 accepted nsb-ext chan-r cache", state.trunk_chan_map[0x100B], 851137500);
        rc |= expect_eq_long("p2 accepted nsb-ext wacn", (long)state.p2_wacn, 0xABCDE);
        rc |= expect_eq_long("p2 accepted nsb-ext sysid", (long)state.p2_sysid, 0x123);
        rc |= expect_eq_long("p2 accepted nsb-ext nac", (long)state.p2_cc, 0x056);
    }

    // Case N2: rejected voice-followed extended NSB must not overwrite
    // current-site LRA metadata from a different control channel.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_is_tuned = 1;
        state.p25_cc_freq = 851000000;
        state.trunk_cc_freq = 851000000;
        state.p25_site_lra = 0x45;
        state.p25_site_lra_valid = 1;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].populated = 1;

        MAC[1] = 0xFB;
        MAC[2] = 0x02;
        MAC[3] = 0xAB;
        MAC[4] = 0xCD;
        MAC[5] = 0xE1;
        MAC[6] = 0x23;
        MAC[7] = 0x10;
        MAC[8] = 0x0A;
        MAC[9] = 0x10;
        MAC[10] = 0x0B;
        MAC[12] = 0x00;
        MAC[13] = 0x56;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("p2 rejected voice nsb-ext preserves p25 cc", state.p25_cc_freq, 851000000);
        rc |= expect_eq_long("p2 rejected voice nsb-ext preserves lra", state.p25_site_lra, 0x45);
        rc |= expect_eq_long("p2 rejected voice nsb-ext preserves lra valid", state.p25_site_lra_valid, 1);
    }

    // Case O: encrypted explicit multi-grants publish channel state and tune the
    // selected candidate as a silent classification probe.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x25; // Group Voice Channel Grant Update Multiple - Explicit
        MAC[2] = 0x40; // encrypted svc1
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x00;
        MAC[6] = 0x00;
        MAC[7] = 0x12;
        MAC[8] = 0x34;
        MAC[9] = 0x40; // encrypted svc2
        MAC[10] = 0x10;
        MAC[11] = 0x0B;
        MAC[12] = 0x00;
        MAC[13] = 0x00;
        MAC[14] = 0x56;
        MAC[15] = 0x78;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x25 encrypted multi probe tunes", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x25 encrypted multi probe vc", state.p25_vc_freq[0], 851125000);
        rc |= expect_eq_long("0x25 encrypted multi probe pending", state.p25_crypto_state[0],
                             DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_contains("0x25 encrypted multi active ch1", state.active_channel[0], "Active Ch: 100A");
        rc |= expect_contains("0x25 encrypted multi active ch2", state.active_channel[0], "Ch: 100B");
        rc |= expect_contains("0x25 encrypted multi active ch group1", state.active_channel[0], "TG: 4660");
        rc |= expect_contains("0x25 encrypted multi active ch group2", state.active_channel[0], "TG: 22136");
    }

    // Case P: MAC words are specified as octets. If high bits leak into a
    // word, the VPDU decoder must mask them before rendering active channels.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x25; // Group Voice Channel Grant Update Multiple - Explicit
        MAC[2] = 0x00;
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x00;
        MAC[6] = 0x00;
        MAC[7] = 0xCE6387; // group high octet should become 0x87
        MAC[8] = 0x00;
        MAC[9] = 0x00;
        MAC[10] = 0xBF776F; // channel high octet should become 0x6F
        MAC[11] = 0x10;
        MAC[12] = 0x00;
        MAC[13] = 0x00;
        MAC[14] = 0x00;
        MAC[15] = 0x3E;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_contains("0x25 octet clamp active ch1", state.active_channel[0], "Active Ch: 100A");
        rc |= expect_contains("0x25 octet clamp active ch2", state.active_channel[0], "Ch: 6F10");
        rc |= expect_contains("0x25 octet clamp group1", state.active_channel[0], "TG: 34560");
        rc |= expect_contains("0x25 octet clamp group2", state.active_channel[0], "TG: 62");
    }

    // Case O2: a clear explicit grant must be selected before an earlier
    // encrypted grant can tune a different carrier as a silent probe.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x25;
        MAC[2] = 0x40; // encrypted probe listed first
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[7] = 0x12;
        MAC[8] = 0x34;
        MAC[9] = 0x00; // eligible clear call on another carrier
        MAC[10] = 0x10;
        MAC[11] = 0x0B;
        MAC[14] = 0x56;
        MAC[15] = 0x78;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x25 mixed update tunes", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x25 mixed update prefers clear vc", state.p25_vc_freq[0], 851137500);
        rc |= expect_eq_long("0x25 mixed update clear classification", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    }

    // Case O3: rejecting a cached encrypted candidate must not stop the
    // multi-grant decoder from selecting a later uncached probe.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;
        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        p25_emit_enc_lockout_once_typed(&opts, &state, 0, 0x1234, /*svc_bits*/ 0x40, 1);

        MAC[1] = 0x25;
        MAC[2] = 0x40; // cached encrypted group
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[7] = 0x12;
        MAC[8] = 0x34;
        MAC[9] = 0x40; // uncached encrypted group
        MAC[10] = 0x10;
        MAC[11] = 0x0B;
        MAC[14] = 0x56;
        MAC[15] = 0x78;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x25 cached first candidate falls through", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x25 later uncached probe selected", state.p25_vc_freq[0], 851137500);
        rc |= expect_eq_long("0x25 later uncached probe pending", state.p25_crypto_state[0],
                             DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    }

    // Case Q: encrypted implicit triple updates also tune one silent probe while
    // refreshing all active-channel state.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x05; // Group Voice Channel Grant Update Multiple - Implicit
        MAC[2] = 0x40;
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x12;
        MAC[6] = 0x34;
        MAC[7] = 0x40;
        MAC[8] = 0x10;
        MAC[9] = 0x0B;
        MAC[10] = 0x56;
        MAC[11] = 0x78;
        MAC[12] = 0x40;
        MAC[13] = 0x10;
        MAC[14] = 0x0C;
        MAC[15] = 0x9A;
        MAC[16] = 0xBC;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x05 encrypted triple probe tunes", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x05 encrypted triple probe vc", state.p25_vc_freq[0], 851125000);
        rc |= expect_eq_long("0x05 encrypted triple probe pending", state.p25_crypto_state[0],
                             DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_contains("0x05 encrypted triple active group1", state.active_channel[0], "TG: 4660");
        rc |= expect_contains("0x05 encrypted triple active group2", state.active_channel[0], "TG: 22136");
        rc |= expect_contains("0x05 encrypted triple active group3", state.active_channel[0], "TG: 39612");
    }

    // Case Q2: standard TDMA 0x05 with service option 0x90 is still a grant update, not MFID90 BSI.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x05;
        MAC[2] = 0x90; // service options, not a vendor MFID
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x12;
        MAC[6] = 0x34;
        MAC[7] = 0x00;
        MAC[8] = 0x10;
        MAC[9] = 0x0B;
        MAC[10] = 0x56;
        MAC[11] = 0x78;
        MAC[12] = 0x00;
        MAC[13] = 0x10;
        MAC[14] = 0x0C;
        MAC[15] = 0x9A;
        MAC[16] = 0xBC;

        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "p25_p2_0x05_not_bsi") != 0) {
            return 100;
        }
        process_MAC_VPDU(&opts, &state, 0, MAC);
        if (dsd_test_capture_stderr_end(&cap) != 0) {
            return 101;
        }

        char out[4096];
        if (read_capture_file(cap.path, out, sizeof out) != 0) {
            return 102;
        }
        (void)remove(cap.path);

        rc |=
            expect_contains("0x05 output is grant update", out, "Group Voice Channel Grant Update Multiple - Implicit");
        rc |= expect_not_contains("0x05 output is not BSI", out, "System Broadcast (BSI)");
        rc |= expect_contains("0x05 svc 0x90 active group1", state.active_channel[0], "TG: 4660");
    }

    // Case Q3: implicit multi-grants likewise select a later clear call before
    // considering an encrypted silent probe on another carrier.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_enc_calls = 0;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x05;
        MAC[2] = 0x40; // encrypted probe listed first
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x12;
        MAC[6] = 0x34;
        MAC[7] = 0x00; // eligible clear call on another carrier
        MAC[8] = 0x10;
        MAC[9] = 0x0B;
        MAC[10] = 0x56;
        MAC[11] = 0x78;
        MAC[12] = 0x40;
        MAC[13] = 0x10;
        MAC[14] = 0x0C;
        MAC[15] = 0x9A;
        MAC[16] = 0xBC;

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x05 mixed update tunes", opts.trunk_is_tuned == 1);
        rc |= expect_eq_long("0x05 mixed update prefers clear vc", state.p25_vc_freq[0], 851137500);
        rc |= expect_eq_long("0x05 mixed update clear classification", state.p25_crypto_state[0], DSD_P25_CRYPTO_CLEAR);
    }

    // Case R: telephone interconnect grants carry service state and tune like
    // private calls when private-call following is enabled.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_private_calls = 1;
        state.p25_cc_freq = cc;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[0] = 0x07; // implicit telephone grant uses the unshifted VPDU layout
        MAC[1] = 0x48; // Telephone Interconnect Voice Channel Grant - implicit
        MAC[2] = 0x93; // emergency, packet, priority 3
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x00;
        MAC[6] = 0x2A; // timer
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05; // target

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_true("0x48 telephone no unsupported tune", opts.trunk_is_tuned == 0);
        rc |= expect_eq_long("0x48 telephone no vc", state.p25_vc_freq[0], 0);
        rc |= expect_contains("0x48 telephone active", state.active_channel[0], "Active Tele Ch: 100A");
        rc |= expect_contains("0x48 telephone target", state.active_channel[0], "TGT: 197637");
        rc |= expect_eq_long("0x48 telephone svc", state.dmr_so, 0x93);
        rc |= expect_eq_long("0x48 telephone emergency", state.p25_call_emergency[0], 1);
        rc |= expect_eq_long("0x48 telephone packet", state.p25_call_is_packet[0], 1);
    }

    // Case S0: explicit SNDCP data grants use the data-grant SM surface and respect data tuning policy.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_private_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        opts.trunk_tune_data_calls = 0;
        state.p25_cc_freq = 851000000;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x54;
        MAC[2] = 0x22;
        MAC[3] = 0x10;
        MAC[4] = 0x0A;
        MAC[5] = 0x10;
        MAC[6] = 0x0B;
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_eq_long("0x54 data disabled no callback", p25_sm_get_ctx()->grant_count, 0);

        opts.trunk_tune_data_calls = 1;
        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);
        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("0x54 data callback", ctx->grant_count, 1);
        rc |= expect_eq_long("0x54 data callback channel", ctx->vc_channel, 0x100A);
        rc |= expect_eq_long("0x54 data callback svc", ctx->slots[0].svc_bits, P25_SM_SVC_UNKNOWN);
        rc |= expect_eq_long("0x54 data callback dst", ctx->slots[0].dst, 0x030405);
        rc |= expect_eq_long("0x54 data callback src", ctx->vc_src, 0);
    }

    // Case S1: L3Harris data grants also use the data-grant SM surface.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        opts.trunk_enable = 1;
        opts.trunk_tune_group_calls = 1;
        opts.trunk_tune_private_calls = 1;
        opts.trunk_tune_enc_calls = 1;
        opts.trunk_tune_data_calls = 1;
        state.p25_cc_freq = 851000000;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0xA0;
        MAC[2] = 0xA4;
        MAC[3] = 0x09;
        MAC[4] = 0x00;
        MAC[5] = 0x10;
        MAC[6] = 0x0A;
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);
        p25_sm_ctx_t* ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("Harris A0 data callback", ctx->grant_count, 1);
        rc |= expect_eq_long("Harris A0 data channel", ctx->vc_channel, 0x100A);
        rc |= expect_eq_long("Harris A0 data dst", ctx->slots[0].dst, 0x030405);
        rc |= expect_eq_long("Harris A0 data src", ctx->vc_src, 0);

        DSD_MEMSET(MAC, 0, sizeof MAC);
        MAC[1] = 0xAC;
        MAC[2] = 0xA4;
        MAC[3] = 0x0C;
        MAC[4] = 0x00;
        MAC[5] = 0x10;
        MAC[6] = 0x0A;
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05;
        MAC[10] = 0x98;
        MAC[11] = 0x04;
        MAC[12] = 0x18;

        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &state);
        process_MAC_VPDU(&opts, &state, 0, MAC);
        ctx = p25_sm_get_ctx();
        rc |= expect_eq_long("Harris AC data callback", ctx->grant_count, 1);
        rc |= expect_eq_long("Harris AC data channel", ctx->vc_channel, 0x100A);
        rc |= expect_eq_long("Harris AC data dst", ctx->slots[0].dst, 0x030405);
        rc |= expect_eq_long("Harris AC data src", ctx->vc_src, 0x980418);
    }

    // Case S: explicit SNDCP data grants update data-channel state in playback
    // mode, with P25p2 playback mirroring both TDMA slots.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 0;
        state.lasttg = 0x030405;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0x54; // SNDCP Data Channel Grant - explicit
        MAC[2] = 0x22; // DSO
        MAC[3] = 0x10;
        MAC[4] = 0x0A; // CHAN-T
        MAC[5] = 0x10;
        MAC[6] = 0x0B; // CHAN-R
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05; // target

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_contains("0x54 data active", state.active_channel[0], "Active Data Ch: 100A");
        rc |= expect_contains("0x54 data target", state.active_channel[0], "TGT: 197637");
        rc |= expect_eq_long("0x54 data vc0", state.p25_vc_freq[0], 851125000);
        rc |= expect_eq_long("0x54 data vc1", state.p25_vc_freq[1], 851125000);
    }

    // Case T: L3Harris private data grants use vendor MFID 0xA4 offsets.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 0;
        state.lasttg = 0x030405;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0xA0;
        MAC[2] = 0xA4;
        MAC[3] = 0x09;
        MAC[4] = 0x00;
        MAC[5] = 0x10;
        MAC[6] = 0x0A; // channel
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05; // target

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_contains("Harris A0 active", state.active_channel[0], "Harris Data Ch: 100A");
        rc |= expect_contains("Harris A0 target", state.active_channel[0], "TGT: 197637");
        rc |= expect_eq_long("Harris A0 vc0", state.p25_vc_freq[0], 851125000);
        rc |= expect_eq_long("Harris A0 vc1", state.p25_vc_freq[1], 851125000);
    }

    // Case U: L3Harris unit-to-unit data grants include both target and source radios.
    {
        static dsd_opts opts;
        static dsd_state state;
        unsigned long long int MAC[24] = {0};
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        p25_sm_release(p25_sm_get_ctx(), &opts, &state, "explicit-release");

        opts.trunk_enable = 0;
        state.lasttg = 0x030405;
        state.synctype = DSD_SYNC_P25P2_POS;
        state.p25_iden_fdma[iden].base_freq = base;
        state.p25_iden_fdma[iden].chan_type = type;
        state.p25_iden_fdma[iden].chan_spac = spac;
        state.p25_iden_fdma[iden].trust = 2;
        state.p25_iden_fdma[iden].populated = 1;
        state.p25_chan_tdma_explicit[iden] = 1;

        MAC[1] = 0xAC;
        MAC[2] = 0xA4;
        MAC[3] = 0x0C;
        MAC[4] = 0x00;
        MAC[5] = 0x10;
        MAC[6] = 0x0A; // channel
        MAC[7] = 0x03;
        MAC[8] = 0x04;
        MAC[9] = 0x05; // target
        MAC[10] = 0x98;
        MAC[11] = 0x04;
        MAC[12] = 0x18; // source

        process_MAC_VPDU(&opts, &state, 0, MAC);
        rc |= expect_contains("Harris AC active", state.active_channel[0], "Harris Data Ch: 100A");
        rc |= expect_contains("Harris AC target", state.active_channel[0], "TGT: 197637");
        rc |= expect_contains("Harris AC source", state.active_channel[0], "SRC: 9962520");
        rc |= expect_eq_long("Harris AC vc0", state.p25_vc_freq[0], 851125000);
        rc |= expect_eq_long("Harris AC vc1", state.p25_vc_freq[1], 851125000);
    }

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
