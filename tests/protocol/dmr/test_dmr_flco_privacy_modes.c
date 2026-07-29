// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

int
getAfsStringFromBits(int a_bits, int f_bits, int s_bits, char* buffer, int a, int f, int s) {
    (void)a_bits;
    (void)f_bits;
    (void)s_bits;
    return DSD_SNPRINTF(buffer, 16, "%02d-%03d", a, (f * 8) + s);
}

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)state;
    if (BufferIn != NULL && BufferOut != NULL) {
        DSD_MEMCPY(BufferOut, BufferIn, 49);
    }
}

static void
write_bits_u64(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        const size_t shift = (nbits - 1U) - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
seed_voice_call(dsd_state* state, uint8_t slot, dsd_call_kind kind, uint64_t target, uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = slot,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
}

static void
assert_call(const dsd_state* state, uint8_t slot, dsd_call_phase phase, dsd_call_kind kind, uint64_t target,
            uint64_t source) {
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, slot, &call) > 0);
    assert(call.phase == phase);
    assert(call.kind == kind);
    assert(call.ota_target_id == target);
    assert(call.policy_target_id == target);
    assert(call.ota_source_id == source);
}

static void
assert_no_active_call(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
}

static void
bytes_to_bits(uint8_t* bits, const uint8_t* bytes, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++) {
        write_bits_u64(bits, i * 8U, bytes[i], 8U);
    }
}

static void
build_tact(uint8_t out[7], uint8_t lcss) {
    static const uint8_t codewords[4][7] = {
        {0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 1, 0, 1, 1},
        {0, 0, 1, 0, 1, 1, 0},
        {0, 0, 1, 1, 1, 0, 1},
    };
    assert(lcss < 4U);
    DSD_MEMCPY(out, codewords[lcss], 7U);
}

static void
build_cach_fragment(uint8_t cach_bits[25], uint8_t lcss, const uint8_t slc[17]) {
    DSD_MEMSET(cach_bits, 0, 25U);
    build_tact(cach_bits, lcss);
    for (int i = 0; i < 17; i++) {
        cach_bits[i + 7] = slc[i] & 1U;
    }
}

static void
build_slc17(uint8_t slc[17], uint8_t slco, uint8_t ts1_act, uint8_t ts2_act) {
    DSD_MEMSET(slc, 0, 17U);
    write_bits_u64(slc, 0U, slco, 4U);
    write_bits_u64(slc, 4U, ts1_act, 4U);
    write_bits_u64(slc, 8U, ts2_act, 4U);
    slc[12] = slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[6] ^ slc[7] ^ slc[9];
    slc[13] = slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[7] ^ slc[8] ^ slc[10];
    slc[14] = slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[5] ^ slc[8] ^ slc[9] ^ slc[11];
    slc[15] = slc[0] ^ slc[1] ^ slc[4] ^ slc[5] ^ slc[7] ^ slc[10];
    slc[16] = slc[0] ^ slc[1] ^ slc[2] ^ slc[5] ^ slc[6] ^ slc[8] ^ slc[11];
}

static void
build_hamming17123_payload(uint8_t out[17], const uint8_t payload[12]) {
    DSD_MEMSET(out, 0, 17U);
    for (int i = 0; i < 12; i++) {
        out[i] = payload[i] & 1U;
    }
    out[12] = out[0] ^ out[1] ^ out[2] ^ out[3] ^ out[6] ^ out[7] ^ out[9];
    out[13] = out[0] ^ out[1] ^ out[2] ^ out[3] ^ out[4] ^ out[7] ^ out[8] ^ out[10];
    out[14] = out[1] ^ out[2] ^ out[3] ^ out[4] ^ out[5] ^ out[8] ^ out[9] ^ out[11];
    out[15] = out[0] ^ out[1] ^ out[4] ^ out[5] ^ out[7] ^ out[10];
    out[16] = out[0] ^ out[1] ^ out[2] ^ out[5] ^ out[6] ^ out[8] ^ out[11];
}

static void
make_slco_crc_residue_zero(uint8_t slco_bits[36]) {
    for (uint32_t nonce = 0; nonce <= 0xFFU; nonce++) {
        write_bits_u64(slco_bits, 28U, nonce, 8U);
        if (crc8(slco_bits, 36U) == 0U) {
            return;
        }
    }
    assert(!"unable to derive zero-residue SLC payload");
}

static void
build_completed_slco_cach(uint8_t cach[4][25], uint8_t slco_bits[36]) {
    uint8_t expanded[68];
    uint8_t raw[68];

    make_slco_crc_residue_zero(slco_bits);
    DSD_MEMSET(expanded, 0, sizeof(expanded));
    DSD_MEMSET(raw, 0, sizeof(raw));
    for (int word = 0; word < 3; word++) {
        build_hamming17123_payload(&expanded[word * 17], &slco_bits[word * 12]);
    }

    for (int i = 0; i < 67; i++) {
        raw[(i * 4) % 67] = expanded[i];
    }
    raw[67] = expanded[67];

    build_cach_fragment(cach[0], 1U, &raw[0]);
    build_cach_fragment(cach[1], 3U, &raw[17]);
    build_cach_fragment(cach[2], 3U, &raw[34]);
    build_cach_fragment(cach[3], 2U, &raw[51]);
}

static void
build_completed_slco_cach_without_crc_fixup(uint8_t cach[4][25], const uint8_t slco_bits[36]) {
    uint8_t expanded[68];
    uint8_t raw[68];

    DSD_MEMSET(expanded, 0, sizeof(expanded));
    DSD_MEMSET(raw, 0, sizeof(raw));
    for (int word = 0; word < 3; word++) {
        build_hamming17123_payload(&expanded[word * 17], &slco_bits[word * 12]);
    }

    for (int i = 0; i < 67; i++) {
        raw[(i * 4) % 67] = expanded[i];
    }
    raw[67] = expanded[67];

    build_cach_fragment(cach[0], 1U, &raw[0]);
    build_cach_fragment(cach[1], 3U, &raw[17]);
    build_cach_fragment(cach[2], 3U, &raw[34]);
    build_cach_fragment(cach[3], 2U, &raw[51]);
}

static void
run_completed_slco(dsd_opts* opts, dsd_state* state, uint8_t slco_bits[36]) {
    uint8_t cach[4][25];

    build_completed_slco_cach(cach, slco_bits);
    assert(dmr_cach(opts, state, cach[0]) == 0U);
    assert(dmr_cach(opts, state, cach[1]) == 0U);
    assert(dmr_cach(opts, state, cach[2]) == 0U);
    assert(dmr_cach(opts, state, cach[3]) == 0U);
}

static int s_scan_activity_calls = 0;
static uint32_t s_scan_activity_target = 0;
static uint32_t s_scan_activity_source = 0;
static int s_scan_activity_is_private = 0;
static int s_scan_activity_encrypted = 0;
static int s_scan_activity_data_call = 0;
static int s_tune_to_cc_calls = 0;
static long int s_tune_to_cc_freq = 0;
static int s_return_to_cc_calls = 0;

static void
capture_scan_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                                       int is_private, int encrypted, int data_call) {
    (void)opts;
    (void)state;
    s_scan_activity_calls++;
    s_scan_activity_target = target;
    s_scan_activity_source = source;
    s_scan_activity_is_private = is_private;
    s_scan_activity_encrypted = encrypted;
    s_scan_activity_data_call = data_call;
}

static void
clear_scan_hooks(void) {
    dsd_trunk_scan_hooks hooks = {0};
    dsd_trunk_scan_hooks_set(hooks);
}

static dsd_trunk_tune_result
capture_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    (void)ted_sps;
    s_tune_to_cc_calls++;
    s_tune_to_cc_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
capture_return_to_cc_ending_calls(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    s_return_to_cc_calls++;
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        (void)dsd_call_state_end(state, (uint8_t)slot, 0.0);
    }
    opts->trunk_is_tuned = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
build_regular_flco(uint8_t* bits, uint8_t flco, uint8_t fid, uint8_t so, uint32_t target, uint32_t source) {
    DSD_MEMSET(bits, 0, 80);
    write_bits_u64(bits, 2U, flco, 6U);
    write_bits_u64(bits, 8U, fid, 8U);
    write_bits_u64(bits, 16U, so, 8U);
    write_bits_u64(bits, 24U, target & 0x00FFFFFFU, 24U);
    write_bits_u64(bits, 48U, source & 0x00FFFFFFU, 24U);
}

static int
read_file_to_buffer(const char* path, char* out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';

    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    size_t nread = fread(out, 1, out_size - 1U, fp);
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    out[nread] = '\0';
    fclose(fp);
    return 0;
}

static int
capture_regular_flco(uint8_t type, char* out, size_t out_size) {
    if (out == NULL || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dmr_flco_output") != 0) {
        return -1;
    }

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, type);

    int rc = dsd_test_capture_stderr_end(&cap);
    if (rc != 0) {
        (void)remove(cap.path);
        dsd_state_ext_free_all(&state);
        return -1;
    }

    rc = read_file_to_buffer(cap.path, out, out_size);
    (void)remove(cap.path);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
capture_ms_direct_flco(char* out, size_t out_size) {
    if (out == NULL || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.dmr_ms_mode = 1;

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dmr_ms_direct_flco") != 0) {
        return -1;
    }

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    int rc = dsd_test_capture_stderr_end(&cap);
    if (rc != 0) {
        (void)remove(cap.path);
        dsd_state_ext_free_all(&state);
        return -1;
    }

    rc = read_file_to_buffer(cap.path, out, out_size);
    (void)remove(cap.path);
    if (irr != 0U) {
        dsd_state_ext_free_all(&state);
        return -1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
capture_hytera_basic_key_output(unsigned int slot, uint8_t segment_count, char* out, size_t out_size) {
    if (out == NULL || out_size == 0U || slot > 1U) {
        return -1;
    }
    out[0] = '\0';

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.show_keys = 1U;
    state.currentslot = slot;
    state.K1 = 0x0123456789ULL;
    state.K2 = 0xABCDEF0123456789ULL;
    state.K3 = 0x1111111111111111ULL;
    state.K4 = 0x2222222222222222ULL;
    state.hytera_key_segments = segment_count;
    state.payload_algid = 0U;
    state.payload_algidR = 0U;

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dmr_hytera_key_output") != 0) {
        return -1;
    }

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x68U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    int rc = dsd_test_capture_stderr_end(&cap);
    if (rc != 0) {
        (void)remove(cap.path);
        dsd_state_ext_free_all(&state);
        return -1;
    }

    rc = read_file_to_buffer(cap.path, out, out_size);
    (void)remove(cap.path);
    if (irr != 0U) {
        dsd_state_ext_free_all(&state);
        return -1;
    }
    dsd_state_ext_free_all(&state);
    return rc;
}

static void
test_flco_output_uses_real_newlines(void) {
    char out[2048];
    assert(capture_regular_flco(1U, out, sizeof(out)) == 0);
    assert(strchr(out, '\n') != NULL);
    assert(strstr(out, "\\n") == NULL);
}

static void
test_ms_direct_flco_reports_internal_slot_one(void) {
    char out[2048];
    assert(capture_ms_direct_flco(out, sizeof(out)) == 0);
    assert(strstr(out, " SLOT 1 ") != NULL);
    assert(strstr(out, " SLOT ?") == NULL);
}

static void
test_single_slot_flco_forces_slot_one_context(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_mono = 1;
    state.dmr_stereo = 0;
    state.currentslot = 1;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);

    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    assert(irr == 0U);
    assert(state.currentslot == 0);
    assert_call(&state, 0U, DSD_CALL_PHASE_ACTIVE, DSD_CALL_KIND_GROUP_VOICE, 1001U, 2002U);
    assert_no_active_call(&state, 1U);
    dsd_state_ext_free_all(&state);
}

static void
test_trunked_mono_bs_fallback_preserves_slot_context(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.dmr_mono = 1;
    state.dmr_stereo = 1;
    state.currentslot = 1;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);

    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    assert(irr == 0U);
    assert(state.currentslot == 1);
    assert_no_active_call(&state, 0U);
    assert_call(&state, 1U, DSD_CALL_PHASE_ACTIVE, DSD_CALL_KIND_GROUP_VOICE, 1001U, 2002U);
    dsd_state_ext_free_all(&state);
}

static void
test_hytera_basic_key_output_uses_segment_count(void) {
    char out[2048];

    assert(capture_hytera_basic_key_output(0U, 1U, out, sizeof(out)) == 0);
    assert(strstr(out, "0123456789") != NULL);
    assert(strstr(out, "0000000123456789") == NULL);
    assert(strstr(out, "ABCDEF0123456789") == NULL);

    assert(capture_hytera_basic_key_output(1U, 1U, out, sizeof(out)) == 0);
    assert(strstr(out, "0123456789") != NULL);
    assert(strstr(out, "0000000123456789") == NULL);
    assert(strstr(out, "ABCDEF0123456789") == NULL);

    assert(capture_hytera_basic_key_output(0U, 2U, out, sizeof(out)) == 0);
    assert(strstr(out, "0000000123456789 ABCDEF0123456789") != NULL);
    assert(strstr(out, "1111111111111111") == NULL);

    assert(capture_hytera_basic_key_output(1U, 2U, out, sizeof(out)) == 0);
    assert(strstr(out, "0000000123456789 ABCDEF0123456789") != NULL);
    assert(strstr(out, "1111111111111111") == NULL);

    assert(capture_hytera_basic_key_output(0U, 4U, out, sizeof(out)) == 0);
    assert(strstr(out, "0000000123456789 ABCDEF0123456789 1111111111111111 2222222222222222") != NULL);

    assert(capture_hytera_basic_key_output(1U, 4U, out, sizeof(out)) == 0);
    assert(strstr(out, "0000000123456789 ABCDEF0123456789 1111111111111111 2222222222222222") != NULL);
}

static void
test_kirisun_flco_sets_late_entry_mode(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x0AU, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(opts.dmr_le == 3);
    assert(irr == 0);

    opts.dmr_le = 3;
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x0AU, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(opts.dmr_le == 0);
    assert(irr == 0);
}

static void
test_flco_canonical_crypto_uses_algorithm_aware_keys(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0U;
    dsd_call_snapshot call;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.payload_algid = 0x24;
    state.payload_keyid = 0x12;
    state.aes_key_loaded[0] = 1;
    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(call.audio_permitted == 1U);

    dsd_state_ext_free_all(&state);
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.payload_algid = 0x24;
    state.payload_keyid = 0x13;
    state.R = 0x123456789AULL;
    irr = 0U;
    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    assert(call.audio_permitted == 0U);

    dsd_state_ext_free_all(&state);
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.K = 42U;
    irr = 0U;
    build_regular_flco(bits, 0x00U, 0x10U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(call.audio_permitted == 1U);

    dsd_state_ext_free_all(&state);
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.K1 = 0x12345U;
    irr = 0U;
    build_regular_flco(bits, 0x00U, 0x68U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(call.audio_permitted == 1U);
    dsd_state_ext_free_all(&state);
}

static void
test_hytera_enhanced_flco_uses_secondary_checksum(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t bytes[9] = {0x02U, 0x68U, 0x34U, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0x00U};
    bytes[8] = 0x09U;

    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof(bits));
    bytes_to_bits(bits, bytes, sizeof(bytes));

    state.currentslot = 0;
    uint32_t irr = 0;
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    assert(irr == 0);
    assert(opts.dmr_le == 2);
    assert((state.dmr_so & 0x40U) != 0U);
    assert(state.payload_algid == 0x02);
    assert(state.payload_keyid == 0x34);
    assert(state.payload_mi == 0x0123456789ULL);
}

static void
test_flco_scan_hook_reports_encrypted_service_option(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = 0;

    s_scan_activity_calls = 0;
    s_scan_activity_target = 0;
    s_scan_activity_source = 0;
    s_scan_activity_is_private = 0;
    s_scan_activity_encrypted = 0;
    s_scan_activity_data_call = 0;

    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_conventional_activity = capture_scan_dmr_conventional_activity;
    dsd_trunk_scan_hooks_set(hooks);

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    clear_scan_hooks();

    assert(irr == 0);
    assert(s_scan_activity_calls == 1);
    assert(s_scan_activity_target == 1001U);
    assert(s_scan_activity_source == 2002U);
    assert(s_scan_activity_is_private == 0);
    assert(s_scan_activity_encrypted == 1);
    assert(s_scan_activity_data_call == 0);
}

static void
test_hytera_flco_scan_hook_uses_final_call_type(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;

    s_scan_activity_calls = 0;
    s_scan_activity_is_private = 0;

    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_conventional_activity = capture_scan_dmr_conventional_activity;
    dsd_trunk_scan_hooks_set(hooks);

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x03U, 0x68U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    clear_scan_hooks();

    assert(irr == 0);
    assert_call(&state, 0U, DSD_CALL_PHASE_ACTIVE, DSD_CALL_KIND_PRIVATE_VOICE, 1001U, 2002U);
    assert(s_scan_activity_calls == 1);
    assert(s_scan_activity_is_private == 1);
    dsd_state_ext_free_all(&state);
}

static void
seed_td_lc_slot(dsd_opts* opts, dsd_state* state, unsigned int slot) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    opts->floating_point = 1;
    opts->audio_gain = 6.25f;
    state->currentslot = slot;
    if (slot == 0U) {
        state->dmr_fid = 0x68;
        state->dmr_so = 0x40;
        state->payload_algid = 0x22;
        state->payload_keyid = 0x33;
        state->payload_mi = 0x123456789AULL;
        state->aout_gain = 1.0f;
    } else {
        state->dmr_fidR = 0x68;
        state->dmr_soR = 0x40;
        state->payload_algidR = 0x44;
        state->payload_keyidR = 0x55;
        state->payload_miR = 0xABCDEF0123ULL;
        state->aout_gainR = 2.0f;
    }
    seed_voice_call(state, (uint8_t)slot, DSD_CALL_KIND_GROUP_VOICE, slot == 0U ? 1001U : 3003U,
                    slot == 0U ? 2002U : 4004U);

    state->dmr_alias_block_len[slot] = 7;
    state->dmr_alias_char_size[slot] = 1;
    state->dmr_alias_format[slot] = 2;
    DSD_SNPRINTF(state->generic_talker_alias[slot], sizeof(state->generic_talker_alias[slot]), "alias");
    DSD_MEMSET(state->dmr_pdu_sf[slot], 0x5A, sizeof(state->dmr_pdu_sf[slot]));
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof(state->dmr_embedded_gps[slot]), "gps");
    DSD_SNPRINTF(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), "lrrp");
}

static void
assert_td_lc_slot_reset(const dsd_state* state, unsigned int slot) {
    if (slot == 0U) {
        assert(state->dmr_fid == 0);
        assert(state->dmr_so == 0);
        assert(state->payload_algid == 0);
        assert(state->payload_keyid == 0);
        assert(state->payload_mi == 0);
        assert(state->aout_gain == 6.25f);
    } else {
        assert(state->dmr_fidR == 0);
        assert(state->dmr_soR == 0);
        assert(state->payload_algidR == 0);
        assert(state->payload_keyidR == 0);
        assert(state->payload_miR == 0);
        assert(state->aout_gainR == 6.25f);
    }

    assert(state->dmr_alias_block_len[slot] == 0);
    assert(state->dmr_alias_char_size[slot] == 0);
    assert(state->dmr_alias_format[slot] == 0);
    assert(strcmp(state->generic_talker_alias[slot], "") == 0);
    assert(state->dmr_pdu_sf[slot][0] == 0);
    assert(strcmp(state->dmr_embedded_gps[slot], "") == 0);
    assert(strcmp(state->dmr_lrrp_gps[slot], "") == 0);
    assert_call(state, (uint8_t)slot, DSD_CALL_PHASE_ENDED, DSD_CALL_KIND_GROUP_VOICE, slot == 0U ? 1001U : 3003U,
                slot == 0U ? 2002U : 4004U);
}

// A terminator whose LC could not be read (FEC-failed or protected) may really be a mid-call
// voice burst mis-typed as a terminator. Its crypto reset is stashed for the heal, but the
// alias/GPS/superframe metadata has no stash -- so the handler must leave it in place, or a
// healed epoch would print "Invalid Header" for every remaining alias block.
static void
assert_td_lc_slot_crypto_reset_metadata_retained(const dsd_state* state, unsigned int slot) {
    if (slot == 0U) {
        assert(state->dmr_fid == 0);
        assert(state->dmr_so == 0);
        assert(state->payload_algid == 0);
        assert(state->payload_keyid == 0);
        assert(state->payload_mi == 0);
        assert(state->aout_gain == 6.25f);
    } else {
        assert(state->dmr_fidR == 0);
        assert(state->dmr_soR == 0);
        assert(state->payload_algidR == 0);
        assert(state->payload_keyidR == 0);
        assert(state->payload_miR == 0);
        assert(state->aout_gainR == 6.25f);
    }

    assert(state->dmr_alias_block_len[slot] == 7);
    assert(state->dmr_alias_char_size[slot] == 1);
    assert(state->dmr_alias_format[slot] == 2);
    assert(strcmp(state->generic_talker_alias[slot], "alias") == 0);
    assert(state->dmr_pdu_sf[slot][0] == 0x5A);
    assert(strcmp(state->dmr_embedded_gps[slot], "gps") == 0);
    assert(strcmp(state->dmr_lrrp_gps[slot], "lrrp") == 0);
    assert_call(state, (uint8_t)slot, DSD_CALL_PHASE_ENDED, DSD_CALL_KIND_GROUP_VOICE, slot == 0U ? 1001U : 3003U,
                slot == 0U ? 2002U : 4004U);
}

static void
test_td_lc_resets_slot_call_privacy_and_alias_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;

    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(irr == 0);
    assert_td_lc_slot_reset(&state, 0U);

    irr = 0;
    seed_td_lc_slot(&opts, &state, 1U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 3003U, 4004U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(irr == 0);
    assert_td_lc_slot_reset(&state, 1U);
    dsd_state_ext_free_all(&state);
}

// A terminator whose link control failed RS(12,9) still ends the call -- on
// RAS systems under the aggressive default every LC fails the masked CRC, so
// gating the end on the CRC would leave those calls active forever -- but with
// the recoverable unverified-terminator reason, so a voice burst mis-typed as
// a terminator mid-call is healed by the next identity-less media mark
// reacquiring the epoch instead of splitting the transmission in two. The
// slot's payload crypto resets either way (stashed first, so the heal can
// restore it); the alias/GPS metadata is cleared only when the LC itself was
// readable, since an unreadable "terminator" may be a mis-typed voice burst
// whose half-built alias the healed epoch still needs.
static void
test_unverified_terminator_ends_call_recoverably(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_call_snapshot call;

    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(irr == 0);
    assert_td_lc_slot_reset(&state, 0U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR);

    // A repeated unverified terminator corroborates the unverified end -- two
    // independently mis-typed bursts in a row is not a plausible fade -- so the
    // reason tightens to the final terminator reason, which retracts the
    // reacquisition permission and releases the held VOICE_END alert where the
    // hangtime repeat runs.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(irr == 0);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ENDED);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);

    // The verified terminator ends with the final terminator reason in one
    // step -- not EXPLICIT, which is the engine's retune/teardown reason and
    // says nothing about the air.
    irr = 0;
    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(irr == 0);
    assert_td_lc_slot_reset(&state, 0U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
    dsd_state_ext_free_all(&state);
}

// After an unverified terminator ends the call, what may reopen its epoch
// depends on what the next observation carries. The identity-less media mark
// the vocoder emits when a voice burst was mis-typed as a terminator heals the
// epoch -- same transmission, one row. A same-identity voice LC header inside
// the reacquisition gap is the next PTT's preamble and must open its own
// epoch: the terminator was positive evidence the previous transmission
// ended, and folding the header in would merge two calls into one row.
static void
test_unverified_terminator_reopen_depends_on_identity(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_call_snapshot call;

    // Identity-less media mark: heals the epoch in place.
    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR);
    const uint64_t ended_epoch = call.epoch;

    const dsd_call_observation media_mark = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(&state, &media_mark, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.epoch != ended_epoch);
    // Reacquired, not new: the healed epoch keeps the terminated call's identity.
    assert(call.ota_source_id == 2002U);
    assert(call.ota_target_id == 1001U);

    // Same-identity header after the unverified end: the next transmission.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR);
    const uint64_t second_ended_epoch = call.epoch;

    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.epoch != second_ended_epoch);
    assert(call.ota_source_id == 2002U);
    // A fresh epoch, not a reacquisition: the new PTT does not merge into the
    // terminated call's row.
    dsd_call_context_snapshot context;
    assert(dsd_call_context_copy_snapshot(&state, &context) > 0);
    assert(context.events[0].reacquired_epoch != call.epoch);
    dsd_state_ext_free_all(&state);
}

// An unverified terminator arriving after a sync loss already ended the epoch
// is the same fallible evidence the recoverable end exists to distrust: it
// must not tighten the reason to explicit, or a single corrupt burst mis-typed
// as a terminator during a fade would retract the reacquisition permission and
// split the resuming transmission in two.
static void
test_unverified_terminator_does_not_tighten_sync_loss_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_call_snapshot call;

    seed_td_lc_slot(&opts, &state, 0U);
    assert(dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);

    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ENDED);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_SYNC_LOSS);

    // A verified terminator is trustworthy evidence and does retract it.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
    dsd_state_ext_free_all(&state);
}

// The terminator's burst type is FEC-protected separately from its LC payload,
// so a terminator whose LC cannot be read -- irrecoverable FEC errors, a
// protected LC, or an unknown vendor FID whose dispatch returns early -- still
// ends the slot's call instead of leaving it to fade into a sync-loss end the
// next same-identity transmission could merge into.
static void
test_terminator_ends_call_despite_unreadable_lc(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_call_snapshot call;

    // Irrecoverable LC FEC errors: the burst type alone ends the call, with
    // the recoverable unverified reason since the CRC could not vouch for it.
    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    irr = 1;
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(irr == 1);
    assert_td_lc_slot_crypto_reset_metadata_retained(&state, 0U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR);

    // Protected LC, even with a verified CRC: the FLCO is hidden, so the burst
    // cannot rule out being a TD_LC that terminates a data session rather than
    // the slot's voice call. It ends the call only recoverably, so a data
    // terminator arriving mid-voice-call heals on the next media mark instead
    // of hard-ending the voice call.
    irr = 0;
    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    bits[0] = 1U;
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(irr == 0);
    assert_td_lc_slot_crypto_reset_metadata_retained(&state, 0U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR);

    // Unknown vendor FID: the no-error dispatch returns early for the LC, but
    // the terminator transition has already run, and the clean unprotected CRC
    // makes it a verified end.
    irr = 0;
    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x2AU, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(irr == 0);
    assert_td_lc_slot_reset(&state, 0U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
    dsd_state_ext_free_all(&state);
}

// A cleanly read TD_LC terminates a data session, not the slot's voice call:
// the voice call state and its live crypto stay untouched, with or without a
// verified CRC (RAS masks the CRC on every LC).
static void
test_data_terminator_does_not_end_voice_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;

    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x30U, 0x00U, 0x00U, 0U, 0U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(irr == 0);
    assert_call(&state, 0U, DSD_CALL_PHASE_ACTIVE, DSD_CALL_KIND_GROUP_VOICE, 1001U, 2002U);
    assert(state.payload_algid == 0x22);
    assert(state.dmr_so == 0x40U);
    assert(state.data_header_format[0] == 7);

    irr = 0;
    build_regular_flco(bits, 0x30U, 0x00U, 0x00U, 0U, 0U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(irr == 0);
    assert_call(&state, 0U, DSD_CALL_PHASE_ACTIVE, DSD_CALL_KIND_GROUP_VOICE, 1001U, 2002U);
    assert(state.payload_algid == 0x22);
    dsd_state_ext_free_all(&state);
}

// A protected LC hides its own FLCO -- the payload bits are ciphertext -- so a protected
// terminator burst whose scrambled FLCO happens to decode as 0x30 must not be trusted as a
// TD_LC: the slot's voice call still ends (recoverably, like every unreadable terminator
// shape), but a live data session's reassembly state stays intact instead of being wiped on
// ciphertext.
static void
test_protected_td_lc_shape_does_not_wipe_data_session(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_call_snapshot call;

    seed_td_lc_slot(&opts, &state, 0U);
    state.data_header_format[0] = 2;
    state.data_header_sap[0] = 4;
    state.data_header_valid[0] = 1;
    state.data_conf_data[0] = 1;
    state.data_block_poc[0] = 3;
    state.data_byte_ctr[0] = 42;
    state.data_ks_start[0] = 5;

    build_regular_flco(bits, 0x30U, 0x00U, 0x00U, 0U, 0U);
    bits[0] = 1; /* pf: the LC payload, including its FLCO, is ciphertext */
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);
    assert(irr == 0);

    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ENDED);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR);
    assert(state.data_header_format[0] == 2);
    assert(state.data_header_sap[0] == 4);
    assert(state.data_header_valid[0] == 1);
    assert(state.data_conf_data[0] == 1);
    assert(state.data_block_poc[0] == 3);
    assert(state.data_byte_ctr[0] == 42);
    assert(state.data_ks_start[0] == 5);
    dsd_state_ext_free_all(&state);
}

// A voice burst mis-typed as a terminator clears the slot's live crypto before
// the identity-less media mark reopens the epoch, but the terminator handler
// stashed the live fields first. The heal restores them so the encrypted
// continuation keeps decrypting; an epoch that ends for real starts the next
// call from cleared crypto as before.
static void
test_reacquired_epoch_restores_cleared_slot_crypto(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_call_snapshot call;

    // An encrypted call whose crypto reached the canonical snapshot.
    seed_td_lc_slot(&opts, &state, 0U);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.algid == 0x22U);
    assert(call.mi == 0x123456789AULL);

    // A CRC-failed terminator -- possibly a mis-typed voice burst -- ends the
    // call recoverably and runs the slot reset.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(state.dmr_so == 0U);
    assert(state.payload_algid == 0);
    assert(state.payload_mi == 0ULL);

    // The healing media mark reacquires the epoch and restores the decoder's
    // live slot crypto from the retained snapshot.
    const dsd_call_observation media_mark = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(&state, &media_mark, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(state.dmr_so == 0x40U);
    assert(state.payload_algid == 0x22);
    assert(state.payload_keyid == 0x33);
    assert(state.payload_mi == 0x123456789AULL);

    // A corroborated end is not recoverable: the media mark opens a fresh
    // epoch and nothing is restored.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
    assert(dsd_call_state_observe(&state, &media_mark, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(state.dmr_so == 0U);
    assert(state.payload_algid == 0);
    assert(state.payload_mi == 0ULL);

    // Slot 1 restores through the R-side fields.
    irr = 0;
    seed_td_lc_slot(&opts, &state, 1U);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 3003U, 4004U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 3003U, 4004U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(state.dmr_soR == 0U);
    assert(state.payload_algidR == 0);
    const dsd_call_observation media_mark_r = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 1U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(&state, &media_mark_r, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(state.dmr_soR == 0x40U);
    assert(state.payload_algidR == 0x44);
    assert(state.payload_keyidR == 0x55);
    assert(state.payload_miR == 0xABCDEF0123ULL);
    dsd_state_ext_free_all(&state);
}

// The heal must put back what the vocoder was actually decrypting with, not what the canonical
// snapshot last recorded. The superframe machinery (the PI/LE LFSR advances) rolls the live MI
// forward without publishing a canonical crypto update, and the Basic Privacy decrypt gates in
// dsd_mbe.c key on the slot FID -- which the canonical snapshot never carried at all. A restore
// sourced from the snapshot resumed with a stale MI (wrong keystream: still noise) and a zero
// FID (Basic Privacy never re-applied); the stash of the live fields carries both.
static void
test_heal_restores_live_mi_and_basic_privacy_fid(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;

    // Motorola Basic Privacy shape: enc service option set, algid/keyid zero, FID 0x10 gating
    // the keystream application, and a live MI the superframe machinery has advanced past
    // whatever crypto signaling last published.
    seed_td_lc_slot(&opts, &state, 0U);
    state.dmr_fid = 0x10;
    state.dmr_so = 0x40;
    state.payload_algid = 0;
    state.payload_keyid = 0;
    state.payload_mi = 0xDEADBEEFULL;

    // A CRC-failed terminator -- possibly a mis-typed voice burst -- ends the call recoverably
    // and the reset clears every live field, FID included.
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(state.dmr_fid == 0U);
    assert(state.dmr_so == 0U);
    assert(state.payload_mi == 0ULL);

    // The healing media mark restores the live capture: advanced MI, gate-bearing FID.
    const dsd_call_observation media_mark = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(&state, &media_mark, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(state.dmr_fid == 0x10U);
    assert(state.dmr_so == 0x40U);
    assert(state.payload_algid == 0);
    assert(state.payload_keyid == 0);
    assert(state.payload_mi == 0xDEADBEEFULL);
    dsd_state_ext_free_all(&state);
}

// A lingering stash from one call must never be applied to another epoch's heal: the stash is
// tied to the epoch whose terminator captured it. A later call on the slot that fades into a
// sync-loss end and heals -- with its live crypto simply never signaled -- gets nothing
// restored, rather than the previous call's crypto.
static void
test_stale_heal_stash_does_not_leak_into_later_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_call_snapshot call;

    // Call A: encrypted, ends on an unverified terminator, stashing its crypto.
    seed_td_lc_slot(&opts, &state, 0U);
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);
    assert(state.dmr_heal_valid[0] == 1U);

    // Call B: a new clear call on the slot (identity-bearing, so it can never heal A's
    // unverified end), which then fades into a sync-loss end and heals identity-lessly.
    const dsd_call_observation call_b = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 7007U,
        .policy_target_id = 7007U,
        .ota_source_id = 8008U,
    };
    assert(dsd_call_state_observe(&state, &call_b, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);
    const dsd_call_observation media_mark = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(&state, &media_mark, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.ota_target_id == 7007U);
    // A's stash did not leak into B's healed epoch.
    assert(state.dmr_so == 0U);
    assert(state.payload_algid == 0);
    assert(state.payload_mi == 0ULL);
    assert(state.dmr_fid == 0U);
    dsd_state_ext_free_all(&state);
}

// A Hytera-Enhanced-shaped LC (fid 0x68, flco 0x02, valid vendor checksum) arriving as an
// FEC-failed terminator burst: the terminator transition has already ended the call, stashed
// the live crypto, and reset the slot, so the Hytera Enhanced handler must not run afterward
// and re-populate payload_algid/keyid/mi -- that would leave dmr_fid=0 beside a non-zero ALGID,
// make dmr_flco_slot_crypto_is_clear() refuse the restore, and strand the stash forever.
static void
test_irrecoverable_hytera_terminator_does_not_repopulate_crypto(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 1;

    seed_td_lc_slot(&opts, &state, 0U);
    uint8_t bytes[9] = {0x02U, 0x68U, 0x77U, 0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0x00U};
    uint8_t sum = 0U;
    for (size_t i = 0U; i < 8U; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    bytes[8] = (uint8_t)(0U - sum);
    DSD_MEMSET(bits, 0, sizeof(bits));
    for (size_t i = 0U; i < 9U; i++) {
        write_bits_u64(bits, i * 8U, bytes[i], 8U);
    }

    dmr_flco(&opts, &state, bits, 0U, &irr, 2U);

    assert_td_lc_slot_crypto_reset_metadata_retained(&state, 0U);
    assert(state.dmr_heal_valid[0] == 1U);

    // And the heal still restores the stashed crypto when the epoch reopens.
    const dsd_call_observation media_mark = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(&state, &media_mark, DSD_CALL_BOUNDARY_BEGIN) > 0);
    assert(state.dmr_fid == 0x68U);
    assert(state.dmr_so == 0x40U);
    assert(state.payload_mi == 0x123456789AULL);
    dsd_state_ext_free_all(&state);
}

static void
test_capacity_plus_rest_channel_and_call_class_are_packed_fields(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    build_regular_flco(bits, 0x04U, 0x10U, 0x00U, 0x123456U, 0xA5BEEFU);
    write_bits_u64(bits, 52U, 9U, 4U);
    write_bits_u64(bits, 56U, 0xBEEFU, 16U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(irr == 0);
    assert(state.dmr_rest_channel == 9);
    assert_call(&state, 0U, DSD_CALL_PHASE_ACTIVE, DSD_CALL_KIND_GROUP_VOICE, 0x123456U, 0xBEEFU);

    irr = 0;
    dsd_state_ext_free_all(&state);
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    build_regular_flco(bits, 0x07U, 0x10U, 0x00U, 0x654321U, 0xC0DEU);
    write_bits_u64(bits, 52U, 3U, 4U);
    write_bits_u64(bits, 56U, 0xC0DEU, 16U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(irr == 0);
    assert(state.dmr_rest_channel == 3);
    assert_call(&state, 1U, DSD_CALL_PHASE_ACTIVE, DSD_CALL_KIND_PRIVATE_VOICE, 0x654321U, 0xC0DEU);
    dsd_state_ext_free_all(&state);
}

static void
test_hytera_xpt_alert_records_free_lcn_and_targets(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t bits[80];
    uint32_t irr = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    build_regular_flco(bits, 0x09U, 0x68U, 0x00U, 0U, 0U);
    bits[1] = 1U;
    write_bits_u64(bits, 16U, 4U, 4U);
    write_bits_u64(bits, 24U, 6U, 4U);
    write_bits_u64(bits, 28U, 2U, 4U);
    write_bits_u64(bits, 32U, 0x00FEU, 16U);
    write_bits_u64(bits, 56U, 0xCAFEU, 16U);

    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    assert(irr == 0);
    assert(strcmp(state.dmr_branding, "  Hytera") == 0);
    assert(strcmp(state.dmr_branding_sub, "XPT ") == 0);
    assert(strcmp(state.dmr_site_parms, "Free LCN - 6 ") == 0);
    assert_no_active_call(&state, 0U);
}

static void
test_cach_single_fragment_throttle_and_reject_paths(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slc[17];
    uint8_t cach[25];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    build_slc17(slc, 0x1U, 0x8U, 0x0U);
    build_cach_fragment(cach, 0U, slc);

    assert(dmr_cach(&opts, &state, cach) == 0U);
    assert(state.slco_sfrag_last[0] != 0);
    time_t first_seen = state.slco_sfrag_last[0];

    assert(dmr_cach(&opts, &state, cach) == 0U);
    assert(state.slco_sfrag_last[0] == first_seen);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    build_slc17(slc, 0x1U, 0x8U, 0x0U);
    slc[0] ^= 1U;
    slc[1] ^= 1U;
    slc[2] ^= 1U;
    build_cach_fragment(cach, 0U, slc);

    assert(dmr_cach(&opts, &state, cach) == 1U);
    assert(state.slco_sfrag_last[1] == 0);
}

static void
test_cach_single_fragment_labels_cover_known_and_reserved_codes(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slc[17];
    uint8_t cach[25];
    char out[2048];

    const uint8_t cases[] = {0x0U, 0x2U, 0x3U, 0x8U, 0x9U, 0xAU, 0xEU};
    const char* labels[] = {"SLCO NULL",       "SLC C_SYS_PARMS",           "SLC P_SYS_PARMS",
                            "SLCO Hytera XPT", "SLCO Connect Plus Traffic", "SLCO Connect Plus Control",
                            "OPC=0xE"};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        state.currentslot = 0;
        build_slc17(slc, cases[i], 0x8U, 0xDU);
        build_cach_fragment(cach, 0U, slc);

        dsd_test_capture_stderr cap;
        assert(dsd_test_capture_stderr_begin(&cap, "dmr_cach_single_labels") == 0);
        assert(dmr_cach(&opts, &state, cach) == 0U);
        assert(dsd_test_capture_stderr_end(&cap) == 0);
        assert(read_file_to_buffer(cap.path, out, sizeof(out)) == 0);
        (void)remove(cap.path);
        assert(strstr(out, labels[i]) != NULL);
    }
}

static void
test_cach_completed_fragment_crc_error_reports_voice_payload_context(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slco[36];
    uint8_t cach[4][25];
    char out[2048];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(slco, 0, sizeof(slco));
    opts.payload = 1;
    state.currentslot = 0;
    state.dmrburstL = 16;
    write_bits_u64(slco, 0U, 0x1U, 4U);
    write_bits_u64(slco, 4U, 0xCU, 4U);
    write_bits_u64(slco, 8U, 0xFU, 4U);
    build_completed_slco_cach_without_crc_fixup(cach, slco);

    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_cach_crc_error") == 0);
    assert(dmr_cach(&opts, &state, cach[0]) == 0U);
    assert(dmr_cach(&opts, &state, cach[1]) == 0U);
    assert(dmr_cach(&opts, &state, cach[2]) == 0U);
    assert(dmr_cach(&opts, &state, cach[3]) == 0U);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(read_file_to_buffer(cap.path, out, sizeof(out)) == 0);
    (void)remove(cap.path);
    assert(strstr(out, "SLCO CRC ERR") != NULL);
}

static void
test_cach_fragment_counter_overflow_resets_fragments(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slc[17];
    uint8_t cach[25];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;
    build_slc17(slc, 0x0U, 0x0U, 0x0U);

    build_cach_fragment(cach, 1U, slc);
    assert(dmr_cach(&opts, &state, cach) == 0U);
    assert(state.dmr_cach_counter == 0);

    build_cach_fragment(cach, 3U, slc);
    assert(dmr_cach(&opts, &state, cach) == 0U);
    assert(state.dmr_cach_counter == 1);
    assert(dmr_cach(&opts, &state, cach) == 0U);
    assert(state.dmr_cach_counter == 2);
    assert(dmr_cach(&opts, &state, cach) == 0U);
    assert(state.dmr_cach_counter == 3);
    assert(dmr_cach(&opts, &state, cach) == 1U);
    assert(state.dmr_cach_counter == 0);
    assert(state.dmr_cach_fragment[0][0] == 1U);
}

static void
test_encrypted_flco_lockout_inserts_policy_and_event_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_tg_policy_lookup lookup;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));
    init_event_history(&history[0], 0, 1);
    init_event_history(&history[1], 0, 1);
    state.event_history_s = history;
    opts.trunk_enable = 1;
    opts.trunk_tune_enc_calls = 0;
    state.currentslot = 0;

    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1234U, 5678U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    assert(irr == 0);
    assert(dsd_tg_policy_lookup_id(&state, 1234U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    assert(strcmp(lookup.entry.mode, "B") == 0);
    assert(strcmp(lookup.entry.name, "ENC LO") == 0);
    assert(lookup.entry.source == DSD_TG_POLICY_SOURCE_ENC_LOCKOUT);
    assert(strstr(history[0].Event_History_Items[0].internal_str, "Target: 1234; has been locked out;") != NULL);
    dsd_state_ext_free_all(&state);
}

static void
test_encrypted_flco_lockout_return_keeps_canonical_calls_ended(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[80];
    uint32_t irr = 0U;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));
    init_event_history(&history[0], 0, 1);
    init_event_history(&history[1], 0, 1);
    state.event_history_s = history;
    opts.trunk_enable = 1;
    opts.trunk_tune_enc_calls = 0;
    opts.trunk_is_tuned = 1;
    state.currentslot = 0;
    state.trunk_cc_freq = 851250000L;
    state.trunk_vc_freq[0] = 852000000L;
    state.trunk_vc_freq[1] = 852000000L;
    state.dmrburstL = 16;
    state.dmrburstR = 9;
    seed_voice_call(&state, 0U, DSD_CALL_KIND_GROUP_VOICE, 1234U, 5678U);
    seed_voice_call(&state, 1U, DSD_CALL_KIND_GROUP_VOICE, 4321U, 8765U);

    dmr_sm_init(&opts, &state);
    dmr_sm_get_ctx()->state = DMR_SM_TUNED;
    s_return_to_cc_calls = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .return_to_cc_request = capture_return_to_cc_ending_calls,
    });

    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1234U, 5678U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    assert(irr == 0U);
    assert(s_return_to_cc_calls == 1);
    assert(opts.trunk_is_tuned == 0);
    assert_call(&state, 0U, DSD_CALL_PHASE_ENDED, DSD_CALL_KIND_GROUP_VOICE, 1234U, 5678U);
    assert_call(&state, 1U, DSD_CALL_PHASE_ENDED, DSD_CALL_KIND_GROUP_VOICE, 4321U, 8765U);
    dsd_state_ext_free_all(&state);
}

static void
test_encrypted_flco_allowed_tuning_skips_lockout_policy(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    uint8_t bits[80];
    uint32_t irr = 0;
    dsd_tg_policy_lookup lookup;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));
    init_event_history(&history[0], 0, 1);
    init_event_history(&history[1], 0, 1);
    state.event_history_s = history;
    opts.trunk_enable = 1;
    opts.trunk_tune_enc_calls = 1;
    state.currentslot = 0;

    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 4321U, 8765U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    assert(irr == 0);
    assert(dsd_tg_policy_lookup_id(&state, 4321U, &lookup) == 0);
    assert(lookup.match == DSD_TG_POLICY_MATCH_NONE);
    assert(history[0].Event_History_Items[0].internal_str[0] == '\0');
    dsd_state_ext_free_all(&state);
}

static void
test_completed_slco_tier3_site_parameters_update_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slco[36];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(slco, 0, sizeof(slco));
    write_bits_u64(slco, 0U, 0x2U, 4U);
    write_bits_u64(slco, 4U, 1U, 2U);
    write_bits_u64(slco, 6U, 17U, 7U);
    write_bits_u64(slco, 13U, 3U, 5U);
    slco[18] = 1U;
    write_bits_u64(slco, 19U, 0x12U, 9U);

    run_completed_slco(&opts, &state, slco);

    assert(strncmp(state.dmr_site_parms, "TIII Small:", 11U) == 0);
    assert(strstr(state.dmr_site_parms, ";") != NULL);
}

static void
test_completed_slco_activity_uses_each_slot_value(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slco[36];
    char out[2048];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(slco, 0, sizeof(slco));
    write_bits_u64(slco, 0U, 0x1U, 4U);
    write_bits_u64(slco, 4U, 0x4U, 4U);
    write_bits_u64(slco, 8U, 0x5U, 4U);

    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_slco_slot_activity") == 0);
    run_completed_slco(&opts, &state, slco);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(read_file_to_buffer(cap.path, out, sizeof(out)) == 0);
    (void)remove(cap.path);
    assert(strstr(out, "TS1: Res 4") != NULL);
    assert(strstr(out, "TS2: Res 5") != NULL);
}

static void
test_completed_slco_connect_plus_and_xpt_update_site_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slco[36];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(slco, 0, sizeof(slco));
    write_bits_u64(slco, 0U, 0x9U, 4U);
    write_bits_u64(slco, 8U, 0x12U, 8U);
    write_bits_u64(slco, 16U, 0x34U, 8U);
    run_completed_slco(&opts, &state, slco);
    assert(strcmp(state.dmr_site_parms, "18-52 ") == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(slco, 0, sizeof(slco));
    write_bits_u64(slco, 0U, 0xAU, 4U);
    write_bits_u64(slco, 8U, 0x56U, 8U);
    write_bits_u64(slco, 16U, 0x78U, 8U);
    state.last_vc_sync_time = time(NULL);
    run_completed_slco(&opts, &state, slco);
    assert(strcmp(state.dmr_site_parms, "86-120 ") == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(slco, 0, sizeof(slco));
    write_bits_u64(slco, 0U, 0x8U, 4U);
    write_bits_u64(slco, 12U, 6U, 4U);
    write_bits_u64(slco, 16U, 2U, 4U);
    write_bits_u64(slco, 20U, 0x5AU, 8U);
    run_completed_slco(&opts, &state, slco);
    assert(strcmp(state.dmr_branding_sub, "XPT ") == 0);
    assert(strcmp(state.dmr_site_parms, "Free LCN - 6 ") == 0);
}

static void
test_completed_slco_capacity_plus_hold_returns_to_rest_channel(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t slco[36];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(slco, 0, sizeof(slco));
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    state.tg_hold = 99U;
    seed_voice_call(&state, 0U, DSD_CALL_KIND_GROUP_VOICE, 11U, 101U);
    seed_voice_call(&state, 1U, DSD_CALL_KIND_GROUP_VOICE, 22U, 202U);
    state.dmrburstL = 16;
    state.dmrburstR = 16;
    state.trunk_vc_freq[0] = 852000000L;
    state.trunk_vc_freq[1] = 852000000L;
    state.p25_vc_freq[0] = 853000000L;
    state.p25_vc_freq[1] = 853000000L;
    state.trunk_chan_map[4] = 851250000L;
    s_tune_to_cc_calls = 0;
    s_tune_to_cc_freq = 0;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_cc_request = capture_tune_to_cc,
    });

    write_bits_u64(slco, 0U, 0xFU, 4U);
    write_bits_u64(slco, 16U, 4U, 4U);
    write_bits_u64(slco, 20U, 2U, 2U);
    write_bits_u64(slco, 22U, 5U, 3U);
    run_completed_slco(&opts, &state, slco);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    assert(s_tune_to_cc_calls == 1);
    assert(s_tune_to_cc_freq == 851250000L);
    assert(opts.trunk_is_tuned == 0);
    assert(state.trunk_vc_freq[0] == 0);
    assert(state.p25_vc_freq[0] == 0);
    dsd_state_ext_free_all(&state);
}

// The voice LC header repeats through the start of a transmission for late
// entry, always before the first voice superframe. Repeats that re-describe the
// running call stay in its epoch; a second epoch would commit the first one's
// freshly built row as a duplicate event. Once voice media has run, an
// identical header is a same-identity re-key after a missed terminator and
// marks a transmission boundary. Embedded LCs re-describe the active call and
// must remain in its epoch.
static void
test_repeated_voice_lc_header_keeps_one_epoch(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;

    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_flco_repeated_header") == 0);

    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);

    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    const uint64_t first_epoch = call.epoch;

    // An embedded LC continues the transmission described by the header.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 3U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.epoch == first_epoch);

    // A header repeat before any voice media is the same transmission.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.epoch == first_epoch);
    assert(call.ota_source_id == 2002U);

    // A header naming a different talker is the next transmission, not a repeat.
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 5005U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.epoch != first_epoch);
    assert(call.ota_source_id == 5005U);
    const uint64_t talker_epoch = call.epoch;

    // Once the epoch has carried voice media, an identical header is a
    // same-identity re-key after a missed terminator: a new transmission.
    assert(dsd_call_state_update_media(&state, 0U, 1, 0.0) > 0);
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 5005U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.epoch != talker_epoch);
    assert(call.ota_source_id == 5005U);
    const uint64_t second_epoch = call.epoch;

    // A header after the terminator opens the following transmission.
    assert(dsd_call_state_end(&state, 0U, 0.0) > 0);
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.epoch != second_epoch);
    const uint64_t third_epoch = call.epoch;

    // The same header replayed after a sync-loss end is that transmission being reacquired,
    // not a new one, so the reopened epoch is tagged for the event layer to merge. The header
    // passes BEGIN, which is exactly why the tagging cannot depend on the boundary token.
    assert(dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) > 0);
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.epoch != third_epoch);
    assert(call.ota_target_id == 1001U);
    assert(call.ota_source_id == 2002U);
    dsd_call_context_snapshot context;
    assert(dsd_call_context_copy_snapshot(&state, &context) > 0);
    assert(context.events[0].reacquired_epoch == call.epoch);

    assert(dsd_test_capture_stderr_end(&cap) == 0);
    (void)remove(cap.path);
    dsd_state_ext_free_all(&state);
}

// Header repeats only precede the transmission's first voice superframe, so an
// active epoch that has gone longer than the repeat window without a single
// decodable voice frame stops absorbing same-identity headers: the next
// identical header is a new transmission, not a preamble repeat. Without the
// bound, a header-only epoch whose voice bursts all failed to decode would
// swallow a PTT on the same talkgroup minutes later as a continuation.
static void
test_stale_headerless_epoch_does_not_absorb_next_header(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 0;

    // A header-shaped epoch whose start predates the repeat window, with no
    // voice media ever marked on it.
    const dsd_call_observation stale = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1001U,
        .policy_target_id = 1001U,
        .ota_source_id = 2002U,
        .observed_m = dsd_time_now_monotonic_s() - 10.0,
    };
    assert(dsd_call_state_observe(&state, &stale, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    const uint64_t stale_epoch = call.epoch;

    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_flco_stale_header") == 0);
    uint8_t bits[80];
    uint32_t irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x00U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    (void)remove(cap.path);

    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.epoch != stale_epoch);
    assert(call.ota_source_id == 2002U);
    dsd_state_ext_free_all(&state);
}

// One transmission whose voice LC header repeats must leave exactly one row in
// event history: the repeats stay in the first epoch instead of each minting an
// epoch whose change commits the previous epoch's freshly built row again.
// Reported in discussion #152.
static void
test_repeated_voice_lc_headers_commit_one_history_row(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(event_history, 0, sizeof(event_history));
    state.event_history_s = event_history;
    init_event_history(&event_history[0], 0, 255);
    init_event_history(&event_history[1], 0, 255);
    state.currentslot = 0;
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;

    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_flco_one_row") == 0);

    uint8_t bits[80];
    uint32_t irr = 0;
    for (int header = 0; header < 3; header++) {
        irr = 0;
        build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
        dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    }
    for (int emb = 0; emb < 3; emb++) {
        irr = 0;
        build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
        dmr_flco(&opts, &state, bits, 1U, &irr, 3U);
    }
    irr = 0;
    build_regular_flco(bits, 0x00U, 0x00U, 0x40U, 1001U, 2002U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 2U);

    assert(dsd_test_capture_stderr_end(&cap) == 0);
    (void)remove(cap.path);

    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ENDED);

    int rows = 0;
    for (int i = 1; i < 255; i++) {
        if (event_history[0].Event_History_Items[i].event_string[0] != '\0') {
            rows++;
        }
    }
    assert(rows == 1);
    assert(event_history[0].Event_History_Items[1].target_id == 1001U);
    assert(event_history[0].Event_History_Items[1].source_id == 2002U);
    dsd_state_ext_free_all(&state);
}

int
main(void) {
    InitAllFecFunction();

    test_repeated_voice_lc_header_keeps_one_epoch();
    test_stale_headerless_epoch_does_not_absorb_next_header();
    test_repeated_voice_lc_headers_commit_one_history_row();
    test_unverified_terminator_ends_call_recoverably();
    test_unverified_terminator_reopen_depends_on_identity();
    test_unverified_terminator_does_not_tighten_sync_loss_end();
    test_terminator_ends_call_despite_unreadable_lc();
    test_data_terminator_does_not_end_voice_call();
    test_protected_td_lc_shape_does_not_wipe_data_session();
    test_reacquired_epoch_restores_cleared_slot_crypto();
    test_heal_restores_live_mi_and_basic_privacy_fid();
    test_stale_heal_stash_does_not_leak_into_later_call();
    test_irrecoverable_hytera_terminator_does_not_repopulate_crypto();
    test_flco_output_uses_real_newlines();
    test_ms_direct_flco_reports_internal_slot_one();
    test_single_slot_flco_forces_slot_one_context();
    test_trunked_mono_bs_fallback_preserves_slot_context();
    test_hytera_basic_key_output_uses_segment_count();
    test_kirisun_flco_sets_late_entry_mode();
    test_flco_canonical_crypto_uses_algorithm_aware_keys();
    test_hytera_enhanced_flco_uses_secondary_checksum();
    test_flco_scan_hook_reports_encrypted_service_option();
    test_hytera_flco_scan_hook_uses_final_call_type();
    test_td_lc_resets_slot_call_privacy_and_alias_state();
    test_capacity_plus_rest_channel_and_call_class_are_packed_fields();
    test_hytera_xpt_alert_records_free_lcn_and_targets();
    test_cach_single_fragment_throttle_and_reject_paths();
    test_cach_single_fragment_labels_cover_known_and_reserved_codes();
    test_cach_completed_fragment_crc_error_reports_voice_payload_context();
    test_cach_fragment_counter_overflow_resets_fragments();
    test_encrypted_flco_lockout_inserts_policy_and_event_history();
    test_encrypted_flco_lockout_return_keeps_canonical_calls_ended();
    test_encrypted_flco_allowed_tuning_skips_lockout_policy();
    test_completed_slco_tier3_site_parameters_update_state();
    test_completed_slco_activity_uses_each_slot_value();
    test_completed_slco_connect_plus_and_xpt_update_site_state();
    test_completed_slco_capacity_plus_hold_returns_to_rest_channel();
    printf("DMR FLCO privacy modes: OK\n");
    return 0;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
