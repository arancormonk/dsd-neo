// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN grant tune matrix.
 *
 * Drives Type-C VCALL_ASSGN and Type-D SCCH busy grant paths with
 * result-returning trunk hooks. OK/PENDING must commit tuned state; rejected
 * results must leave voice-channel tune state untouched.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
void NXDN_decode_scch(dsd_opts* opts, dsd_state* state, const uint8_t* Message, uint8_t direction);

typedef enum {
    NXDN_MATRIX_TYPE_C = 0,
    NXDN_MATRIX_TYPE_D,
} nxdn_matrix_path;

typedef enum {
    NXDN_GUARD_GROUP_DISABLED = 0,
    NXDN_GUARD_PRIVATE_DISABLED,
    NXDN_GUARD_DATA_DISABLED,
    NXDN_GUARD_ALLOWLIST_BLOCK,
    NXDN_GUARD_MISSING_FREQ,
    NXDN_GUARD_MISSING_CC,
    NXDN_GUARD_SCCH_RECENT_ACTIVE,
    NXDN_GUARD_SCCH_REPEATER_ZERO,
} nxdn_guard_kind;

typedef struct {
    const char* name;
    nxdn_matrix_path path;
    uint8_t message_type;
    uint8_t call_type;
    uint16_t source;
    uint16_t target;
    uint16_t channel;
    int use_dfa;
    uint16_t ofn;
    uint8_t scch_gu;
    uint8_t scch_rep1;
    uint8_t scch_rep2;
    uint16_t scch_id;
    long expected_freq;
} nxdn_case;

static dsd_opts g_opts;
static dsd_state g_state;
static dsd_trunk_tune_result g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_tune_count = 0;
static long g_last_tune_freq = 0;

/*
 * Link stubs:
 * Pulling focused grant handlers from nxdn_element.c requires auxiliary
 * symbols that are irrelevant to this matrix.
 */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_message_type(const dsd_opts* opts, dsd_state* state, uint8_t MessageType) {
    (void)opts;
    (void)state;
    (void)MessageType;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_arib(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_prop(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_reset(dsd_state* state) {
    (void)state;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    (void)opts;
    if (!state || channel >= (uint16_t)(sizeof(state->trunk_chan_map) / sizeof(state->trunk_chan_map[0]))) {
        return 0;
    }
    return state->trunk_chan_map[channel];
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    if (!state || channel >= (uint16_t)(sizeof(state->trunk_chan_map) / sizeof(state->trunk_chan_map[0]))) {
        return 0;
    }
    return state->trunk_chan_map[channel];
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    return 0U;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_trunk_diag_log_missing_channel_once(const dsd_opts* opts, dsd_state* state, uint16_t channel,
                                         const char* context) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)context;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)data_string;
    (void)slot;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
LFSR128n(dsd_state* state) {
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
des_ofb_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int nblocks) {
    (void)mi;
    (void)key_ulli;
    (void)output;
    (void)nblocks;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
aes_ofb_keystream_output(const uint8_t* iv, const uint8_t* key, uint8_t* output, dsd_aes_key_size key_size,
                         int nblocks) {
    (void)iv;
    (void)key;
    (void)output;
    (void)key_size;
    (void)nblocks;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_rigctl_query_hook_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return 0;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return 1000000000ULL;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ms(void) {
    return dsd_time_monotonic_ns() / 1000000U;
}

static dsd_trunk_tune_result
nxdn_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    g_tune_count++;
    g_last_tune_freq = freq;
    if (dsd_trunk_tune_result_is_ok(g_tune_result)) {
        if (opts) {
            opts->trunk_is_tuned = 1;
        }
        if (state) {
            state->p25_vc_freq[0] = freq;
            state->p25_vc_freq[1] = freq;
            state->trunk_vc_freq[0] = freq;
            state->trunk_vc_freq[1] = freq;
        }
    }
    return g_tune_result;
}

static void
nxdn_install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = nxdn_hook_tune_to_freq;
    dsd_trunk_tuning_hooks_set(hooks);
}

static int
nxdn_expect(int cond, const char* test_case, const char* result_name, const char* check) {
    if (cond) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL case=%s result=%s check=%s\n", test_case, result_name, check);
    return 1;
}

static void
write_bits_u32(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        size_t shift = (nbits - 1U) - i;
        bits[start + i] = (uint8_t)((value >> shift) & 1U);
    }
}

static void
set_message_type(uint8_t* bits, uint8_t type) {
    bits[2] = (uint8_t)((type >> 5U) & 1U);
    bits[3] = (uint8_t)((type >> 4U) & 1U);
    bits[4] = (uint8_t)((type >> 3U) & 1U);
    bits[5] = (uint8_t)((type >> 2U) & 1U);
    bits[6] = (uint8_t)((type >> 1U) & 1U);
    bits[7] = (uint8_t)(type & 1U);
}

static void
build_vcall_assgn(uint8_t* message_bits, const nxdn_case* test_case) {
    DSD_MEMSET(message_bits, 0, 128);
    set_message_type(message_bits, test_case->message_type);
    write_bits_u32(message_bits, 16U, test_case->call_type & 0x7U, 3U);
    write_bits_u32(message_bits, 24U, test_case->source, 16U);
    write_bits_u32(message_bits, 40U, test_case->target, 16U);
    write_bits_u32(message_bits, 62U, test_case->channel & 0x03FFU, 10U);
    if (test_case->use_dfa) {
        write_bits_u32(message_bits, 64U, test_case->ofn, 16U);
    }
}

static void
build_scch(uint8_t* message_bits, const nxdn_case* test_case) {
    DSD_MEMSET(message_bits, 0, 32);
    write_bits_u32(message_bits, 0U, 0U, 2U);
    write_bits_u32(message_bits, 3U, test_case->scch_rep1, 5U);
    write_bits_u32(message_bits, 8U, test_case->scch_rep2, 5U);
    write_bits_u32(message_bits, 13U, test_case->scch_id, 11U);
    message_bits[24] = test_case->scch_gu ? 1U : 0U;
}

static void
nxdn_setup_fixture(const nxdn_case* test_case) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));

    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_private_calls = 1;
    g_opts.trunk_tune_data_calls = 1;
    g_opts.trunk_hangtime = 1.0f;

    g_state.p25_cc_freq = 935000000L;
    g_state.trunk_cc_freq = 935000000L;
    g_state.lastsynctype = DSD_SYNC_NXDN_POS;
    g_state.last_vc_sync_time = time(NULL) - 4;
    g_state.nxdn_rcn = test_case->use_dfa ? 1 : 0;

    if (test_case->path == NXDN_MATRIX_TYPE_C) {
        uint16_t map_channel = test_case->use_dfa ? test_case->ofn : test_case->channel;
        g_state.trunk_chan_map[map_channel] = test_case->expected_freq;
    } else {
        g_state.trunk_chan_map[31] = g_state.p25_cc_freq;
        g_state.trunk_chan_map[test_case->scch_rep1] = test_case->expected_freq;
        g_state.trunk_chan_map[test_case->scch_rep2] = g_state.p25_cc_freq;
    }

    g_tune_count = 0;
    g_last_tune_freq = 0;
}

static void
nxdn_run_case_decode(const nxdn_case* test_case) {
    uint8_t message[128];
    if (test_case->path == NXDN_MATRIX_TYPE_C) {
        build_vcall_assgn(message, test_case);
        NXDN_decode_VCALL_ASSGN(&g_opts, &g_state, message);
    } else {
        build_scch(message, test_case);
        NXDN_decode_scch(&g_opts, &g_state, message, 1);
    }
}

static int
nxdn_run_tune_result_case(const nxdn_case* test_case, dsd_trunk_tune_result result, const char* result_name) {
    g_tune_result = result;
    nxdn_setup_fixture(test_case);
    nxdn_install_hooks();

    nxdn_run_case_decode(test_case);

    const int accepted = dsd_trunk_tune_result_is_ok(result);
    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 1, test_case->name, result_name, "tune attempted once");
    rc |= nxdn_expect(g_last_tune_freq == test_case->expected_freq, test_case->name, result_name,
                      "tune frequency matches map");

    if (accepted) {
        rc |= nxdn_expect(g_opts.trunk_is_tuned == 1, test_case->name, result_name, "accepted tune set tuned flags");
        rc |= nxdn_expect(g_state.p25_vc_freq[0] == test_case->expected_freq
                              && g_state.trunk_vc_freq[0] == test_case->expected_freq,
                          test_case->name, result_name, "accepted tune set VC frequency state");
        rc |= nxdn_expect(g_state.lastsynctype == DSD_SYNC_NONE, test_case->name, result_name,
                          "accepted tune reset last sync");
        rc |= nxdn_expect(g_state.nxdn_sacch_frame_segment[0][0] == 1, test_case->name, result_name,
                          "accepted tune reset SACCH segments");
    } else {
        rc |= nxdn_expect(g_opts.trunk_is_tuned == 0, test_case->name, result_name,
                          "rejected tune left tuned flags clear");
        rc |= nxdn_expect(g_state.p25_vc_freq[0] == 0 && g_state.trunk_vc_freq[0] == 0, test_case->name, result_name,
                          "rejected tune left VC frequency state clear");
        rc |= nxdn_expect(g_state.lastsynctype == DSD_SYNC_NXDN_POS, test_case->name, result_name,
                          "rejected tune preserved last sync");
        rc |= nxdn_expect(g_state.nxdn_sacch_frame_segment[0][0] == 0, test_case->name, result_name,
                          "rejected tune did not reset SACCH segments");
    }
    return rc;
}

static uint16_t
nxdn_case_map_channel(const nxdn_case* test_case) {
    if (test_case->path == NXDN_MATRIX_TYPE_C) {
        return test_case->use_dfa ? test_case->ofn : test_case->channel;
    }
    return test_case->scch_rep1;
}

static void
nxdn_apply_no_tune_guard(nxdn_case* test_case, nxdn_guard_kind guard) {
    switch (guard) {
        case NXDN_GUARD_GROUP_DISABLED: g_opts.trunk_tune_group_calls = 0; break;
        case NXDN_GUARD_PRIVATE_DISABLED: g_opts.trunk_tune_private_calls = 0; break;
        case NXDN_GUARD_DATA_DISABLED: g_opts.trunk_tune_data_calls = 0; break;
        case NXDN_GUARD_ALLOWLIST_BLOCK: g_opts.trunk_use_allow_list = 1; break;
        case NXDN_GUARD_MISSING_FREQ: g_state.trunk_chan_map[nxdn_case_map_channel(test_case)] = 0; break;
        case NXDN_GUARD_MISSING_CC:
            g_state.p25_cc_freq = 0;
            g_state.trunk_cc_freq = 0;
            if (test_case->path == NXDN_MATRIX_TYPE_D) {
                g_state.trunk_chan_map[31] = 0;
                g_state.trunk_chan_map[test_case->scch_rep2] = 0;
            }
            break;
        case NXDN_GUARD_SCCH_RECENT_ACTIVE: g_state.last_vc_sync_time = time(NULL); break;
        case NXDN_GUARD_SCCH_REPEATER_ZERO:
            test_case->scch_rep1 = 0U;
            g_state.trunk_chan_map[nxdn_case_map_channel(test_case)] = 0;
            break;
    }
}

static int
nxdn_run_no_tune_guard_case(const nxdn_case* base_case, nxdn_guard_kind guard, const char* guard_name) {
    nxdn_case test_case = *base_case;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&test_case);
    nxdn_apply_no_tune_guard(&test_case, guard);
    nxdn_install_hooks();

    nxdn_run_case_decode(&test_case);

    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 0, test_case.name, guard_name, "guard did not attempt tune");
    rc |= nxdn_expect(g_last_tune_freq == 0, test_case.name, guard_name, "guard left tune frequency clear");
    rc |= nxdn_expect(g_opts.trunk_is_tuned == 0, test_case.name, guard_name, "guard left tuned flags clear");
    rc |= nxdn_expect(g_state.p25_vc_freq[0] == 0 && g_state.trunk_vc_freq[0] == 0, test_case.name, guard_name,
                      "guard left VC frequency state clear");
    rc |=
        nxdn_expect(g_state.lastsynctype == DSD_SYNC_NXDN_POS, test_case.name, guard_name, "guard preserved last sync");
    rc |= nxdn_expect(g_state.nxdn_sacch_frame_segment[0][0] == 0, test_case.name, guard_name,
                      "guard did not reset SACCH segments");
    return rc;
}

static int
nxdn_run_retry_after_reject_case(const nxdn_case* test_case) {
    g_tune_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    nxdn_setup_fixture(test_case);
    nxdn_install_hooks();

    nxdn_run_case_decode(test_case);

    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 1, test_case->name, "retry-after-deferred", "deferred tune attempted");
    rc |= nxdn_expect(g_opts.trunk_is_tuned == 0 && g_state.trunk_vc_freq[0] == 0, test_case->name,
                      "retry-after-deferred", "deferred tune left state clear");

    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_run_case_decode(test_case);

    rc |= nxdn_expect(g_tune_count == 2, test_case->name, "retry-after-deferred", "later grant retried tune");
    rc |= nxdn_expect(g_last_tune_freq == test_case->expected_freq, test_case->name, "retry-after-deferred",
                      "retried tune frequency matches map");
    rc |= nxdn_expect(g_opts.trunk_is_tuned == 1, test_case->name, "retry-after-deferred",
                      "retried tune set tuned flags");
    rc |= nxdn_expect(g_state.p25_vc_freq[0] == test_case->expected_freq
                          && g_state.trunk_vc_freq[0] == test_case->expected_freq,
                      test_case->name, "retry-after-deferred", "retried tune set VC frequency state");
    return rc;
}

static int
nxdn_run_duplicate_no_tune_case(void) {
    nxdn_case duplicate = {
        "type-c-duplicate-no-tune", NXDN_MATRIX_TYPE_C, 0x05U, 1U, 2100U, 1100U, 16U, 0, 0, 0, 0, 0, 0, 936012500L,
    };
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&duplicate);
    g_opts.trunk_is_tuned = 1;
    g_state.last_vc_sync_time = time(NULL);
    g_state.p25_vc_freq[0] = duplicate.expected_freq;
    g_state.trunk_vc_freq[0] = duplicate.expected_freq;

    nxdn_install_hooks();

    nxdn_run_case_decode(&duplicate);

    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 0, duplicate.name, "duplicate", "duplicate grant did not tune");
    rc |= nxdn_expect(g_opts.trunk_is_tuned == 1, duplicate.name, "duplicate", "duplicate grant preserved tuned flags");
    rc |= nxdn_expect(g_state.p25_vc_freq[0] == duplicate.expected_freq, duplicate.name, "duplicate",
                      "duplicate grant preserved existing VC frequency");
    return rc;
}

static int
nxdn_run_active_other_tg_no_tune_case(void) {
    nxdn_case active = {
        "type-c-active-other-tg-no-tune",
        NXDN_MATRIX_TYPE_C,
        0x04U,
        1U,
        2100U,
        1100U,
        16U,
        0,
        0,
        0,
        0,
        0,
        0,
        936012500L,
    };
    const long existing_freq = 936912500L;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&active);
    g_opts.trunk_is_tuned = 1;
    g_state.p25_vc_freq[0] = existing_freq;
    g_state.trunk_vc_freq[0] = existing_freq;
    nxdn_install_hooks();

    nxdn_run_case_decode(&active);

    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 0, active.name, "active-other-tg", "active non-held call did not retune");
    rc |= nxdn_expect(g_opts.trunk_is_tuned == 1, active.name, "active-other-tg",
                      "active non-held call preserved tuned flags");
    rc |= nxdn_expect(g_state.p25_vc_freq[0] == existing_freq && g_state.trunk_vc_freq[0] == existing_freq, active.name,
                      "active-other-tg", "active non-held call preserved VC frequency");
    return rc;
}

static int
nxdn_run_hold_match_retune_case(void) {
    nxdn_case hold = {
        "type-c-hold-match-retune", NXDN_MATRIX_TYPE_C, 0x05U, 1U, 2100U, 1100U, 16U, 0, 0, 0, 0, 0, 0, 936012500L,
    };
    const long existing_freq = 936912500L;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&hold);
    g_opts.trunk_is_tuned = 1;
    g_state.tg_hold = hold.target;
    g_state.last_vc_sync_time = time(NULL);
    g_state.p25_vc_freq[0] = existing_freq;
    g_state.trunk_vc_freq[0] = existing_freq;
    nxdn_install_hooks();

    nxdn_run_case_decode(&hold);

    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 1, hold.name, "hold-match", "held duplicate grant retuned");
    rc |= nxdn_expect(g_last_tune_freq == hold.expected_freq, hold.name, "hold-match",
                      "held duplicate tune frequency matches map");
    rc |= nxdn_expect(g_state.p25_vc_freq[0] == hold.expected_freq && g_state.trunk_vc_freq[0] == hold.expected_freq,
                      hold.name, "hold-match", "held duplicate updated VC frequency");
    rc |= nxdn_expect(g_state.lastsynctype == DSD_SYNC_NONE, hold.name, "hold-match", "held duplicate reset last sync");
    return rc;
}

int
main(void) {
    int rc = 0;
    static const nxdn_case cases[] = {
        {"type-c-group-normal-map", NXDN_MATRIX_TYPE_C, 0x04U, 1U, 2100U, 1100U, 16U, 0, 0, 0, 0, 0, 0, 936012500L},
        {"type-c-private-normal-map", NXDN_MATRIX_TYPE_C, 0x04U, 4U, 9002U, 9001U, 17U, 0, 0, 0, 0, 0, 0, 936512500L},
        {"type-c-data-normal-map", NXDN_MATRIX_TYPE_C, 0x0DU, 1U, 2200U, 1200U, 18U, 0, 0, 0, 0, 0, 0, 937012500L},
        {"type-c-group-dfa-ofn-map", NXDN_MATRIX_TYPE_C, 0x04U, 1U, 2300U, 1300U, 0U, 1, 0x120U, 0, 0, 0, 0,
         937512500L},
        {"type-d-scch-group", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 0, 6, 3, 1400U, 938012500L},
        {"type-d-scch-private", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 1, 7, 3, 1500U, 938512500L},
    };

    static const struct {
        const char* name;
        dsd_trunk_tune_result result;
    } results[] = {
        {"ok", DSD_TRUNK_TUNE_RESULT_OK},
        {"pending", DSD_TRUNK_TUNE_RESULT_PENDING},
        {"deferred", DSD_TRUNK_TUNE_RESULT_DEFERRED},
        {"failed", DSD_TRUNK_TUNE_RESULT_FAILED},
        {"timeout", DSD_TRUNK_TUNE_RESULT_TIMEOUT},
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (size_t r = 0; r < sizeof(results) / sizeof(results[0]); r++) {
            rc |= nxdn_run_tune_result_case(&cases[c], results[r].result, results[r].name);
        }
    }

    static const struct {
        size_t case_index;
        nxdn_guard_kind guard;
        const char* name;
    } guard_cases[] = {
        {0U, NXDN_GUARD_GROUP_DISABLED, "group-disabled"},
        {1U, NXDN_GUARD_PRIVATE_DISABLED, "private-disabled"},
        {2U, NXDN_GUARD_DATA_DISABLED, "data-disabled"},
        {0U, NXDN_GUARD_ALLOWLIST_BLOCK, "allowlist-block"},
        {0U, NXDN_GUARD_MISSING_FREQ, "missing-frequency"},
        {0U, NXDN_GUARD_MISSING_CC, "missing-control-channel"},
        {4U, NXDN_GUARD_GROUP_DISABLED, "scch-group-disabled"},
        {5U, NXDN_GUARD_PRIVATE_DISABLED, "scch-private-disabled"},
        {4U, NXDN_GUARD_ALLOWLIST_BLOCK, "scch-allowlist-block"},
        {4U, NXDN_GUARD_MISSING_FREQ, "scch-missing-frequency"},
        {4U, NXDN_GUARD_MISSING_CC, "scch-missing-control-channel"},
        {4U, NXDN_GUARD_SCCH_RECENT_ACTIVE, "scch-recent-active"},
        {4U, NXDN_GUARD_SCCH_REPEATER_ZERO, "scch-repeater-zero"},
    };

    for (size_t g = 0; g < sizeof(guard_cases) / sizeof(guard_cases[0]); g++) {
        rc |= nxdn_run_no_tune_guard_case(&cases[guard_cases[g].case_index], guard_cases[g].guard, guard_cases[g].name);
    }
    rc |= nxdn_run_retry_after_reject_case(&cases[0]);
    rc |= nxdn_run_retry_after_reject_case(&cases[4]);
    rc |= nxdn_run_duplicate_no_tune_case();
    rc |= nxdn_run_active_other_tg_no_tune_case();
    rc |= nxdn_run_hold_match_retune_case();

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    if (rc == 0) {
        printf("NXDN_GRANT_TUNE_MATRIX: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
