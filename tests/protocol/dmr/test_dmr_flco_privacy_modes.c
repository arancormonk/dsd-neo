// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
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
getAfsString(const dsd_state* state, char* buffer, int a, int f, int s) {
    (void)state;
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
bytes_to_bits(uint8_t* bits, const uint8_t* bytes, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++) {
        write_bits_u64(bits, i * 8U, bytes[i], 8U);
    }
}

static void
build_tact(uint8_t out[7], uint8_t lcss) {
    unsigned char orig[4] = {0, 0, (uint8_t)((lcss >> 1U) & 1U), (uint8_t)(lcss & 1U)};
    unsigned char enc[7];
    Hamming_7_4_encode(orig, enc);
    for (int i = 0; i < 7; i++) {
        out[i] = enc[i] & 1U;
    }
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

static uint8_t
hytera_checksum(const uint8_t bytes[8]) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < 8; i++) {
        checksum = (uint8_t)((checksum + bytes[i]) & 0xFFU);
    }
    checksum = (uint8_t)(~checksum & 0xFFU);
    checksum++;
    return checksum;
}

static int s_scan_activity_calls = 0;
static uint32_t s_scan_activity_target = 0;
static uint32_t s_scan_activity_source = 0;
static int s_scan_activity_is_private = 0;
static int s_scan_activity_encrypted = 0;
static int s_scan_activity_data_call = 0;
static int s_tune_to_cc_calls = 0;
static long int s_tune_to_cc_freq = 0;

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
capture_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    s_tune_to_cc_calls++;
    s_tune_to_cc_freq = freq;
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
        return -1;
    }

    rc = read_file_to_buffer(cap.path, out, out_size);
    (void)remove(cap.path);
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
        return -1;
    }

    rc = read_file_to_buffer(cap.path, out, out_size);
    (void)remove(cap.path);
    if (irr != 0U) {
        return -1;
    }
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
test_hytera_enhanced_flco_uses_secondary_checksum(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    uint8_t bytes[9] = {0x02U, 0x68U, 0x34U, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0x00U};
    bytes[8] = hytera_checksum(bytes);

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
    state.gi[0] = 0;

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
    assert(state.gi[0] == 1);
    assert(s_scan_activity_calls == 1);
    assert(s_scan_activity_is_private == 1);
}

static void
seed_td_lc_slot(dsd_opts* opts, dsd_state* state, unsigned int slot) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    opts->floating_point = 1;
    opts->audio_gain = 6.25f;
    state->currentslot = slot;
    if (slot == 0U) {
        state->dmr_fid = 0x68;
        state->dmr_so = 0x40;
        state->lasttg = 1001;
        state->lastsrc = 2002;
        state->payload_algid = 0x22;
        state->payload_keyid = 0x33;
        state->payload_mi = 0x123456789AULL;
        state->aout_gain = 1.0f;
    } else {
        state->dmr_fidR = 0x68;
        state->dmr_soR = 0x40;
        state->lasttgR = 3003;
        state->lastsrcR = 4004;
        state->payload_algidR = 0x44;
        state->payload_keyidR = 0x55;
        state->payload_miR = 0xABCDEF0123ULL;
        state->aout_gainR = 2.0f;
    }

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
        assert(state->lasttg == 0);
        assert(state->lastsrc == 0);
        assert(state->payload_algid == 0);
        assert(state->payload_keyid == 0);
        assert(state->payload_mi == 0);
        assert(state->aout_gain == 6.25f);
    } else {
        assert(state->dmr_fidR == 0);
        assert(state->dmr_soR == 0);
        assert(state->lasttgR == 0);
        assert(state->lastsrcR == 0);
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
    assert(state.lasttg == 0x123456U);
    assert(state.lastsrc == 0xBEEFU);
    assert(state.gi[0] == 0);
    assert(strcmp(state.call_string[0], "   Group             ") == 0);

    irr = 0;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.currentslot = 1;
    build_regular_flco(bits, 0x07U, 0x10U, 0x00U, 0x654321U, 0xC0DEU);
    write_bits_u64(bits, 52U, 3U, 4U);
    write_bits_u64(bits, 56U, 0xC0DEU, 16U);
    dmr_flco(&opts, &state, bits, 1U, &irr, 1U);
    assert(irr == 0);
    assert(state.dmr_rest_channel == 3);
    assert(state.lasttgR == 0x654321U);
    assert(state.lastsrcR == 0xC0DEU);
    assert(state.gi[1] == 1);
    assert(strcmp(state.call_string[1], " Private             ") == 0);
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
    assert(state.lasttg == 0);
    assert(state.lastsrc == 0);
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
    opts.p25_is_tuned = 1;
    state.tg_hold = 99U;
    state.lasttg = 11;
    state.lasttgR = 22;
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
        .tune_to_cc_result = capture_tune_to_cc,
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
    assert(opts.p25_is_tuned == 0);
    assert(state.trunk_vc_freq[0] == 0);
    assert(state.p25_vc_freq[0] == 0);
}

int
main(void) {
    InitAllFecFunction();

    test_flco_output_uses_real_newlines();
    test_hytera_basic_key_output_uses_segment_count();
    test_kirisun_flco_sets_late_entry_mode();
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
    test_encrypted_flco_allowed_tuning_skips_lockout_policy();
    test_completed_slco_tier3_site_parameters_update_state();
    test_completed_slco_connect_plus_and_xpt_update_site_state();
    test_completed_slco_capacity_plus_hold_returns_to_rest_channel();
    printf("DMR FLCO privacy modes: OK\n");
    return 0;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
