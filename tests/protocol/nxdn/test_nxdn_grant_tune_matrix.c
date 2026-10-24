// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN grant tune matrix.
 *
 * Drives Type-C VCALL_ASSGN and Type-D SCCH busy grant paths with
 * result-returning trunk hooks. OK/PENDING must commit tuned state; rejected
 * results must leave voice-channel tune state untouched. While a call is held,
 * candidates go through admission: a duplicate never untunes the receiver, the
 * held talkgroup and outranking preempting calls take over, and a Type-D SCCH
 * relabels or moves the receiver only by the repeater it names.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, const uint8_t* Message);
void NXDN_decode_scch(dsd_opts* opts, dsd_state* state, const uint8_t* Message, uint8_t direction);
void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, const uint8_t* ElementsContent,
                                  size_t ElementsContentBits);
// Defined here for the real-call-state build and by tests/test_support/call_state_stubs.c for the stubbed one.
// NOLINTNEXTLINE(misc-use-internal-linkage)
unsigned int dsd_call_state_stub_event_sync_count(uint8_t slot);

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
static int g_tune_cc_count = 0;
static long g_rigctl_freq = 0;
static int g_rigctl_queries = 0;

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
nxdn_alias_decode_arib(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits) {
    (void)opts;
    (void)state;
    (void)message_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_alias_decode_prop(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits) {
    (void)opts;
    (void)state;
    (void)message_bits;
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
dsd_event_note_current_call(dsd_opts* opts, dsd_state* state, uint8_t slot, const char* note) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)note;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
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

#ifdef NXDN_MATRIX_REAL_CALL_STATE
/* The real call state (src/core/util/call_state.c) stands in for test_support/call_state_stubs.c, whose event-sync
 * counter the cases read: counted here instead. */
static unsigned int g_event_sync_counts[DSD_CALL_STATE_SLOT_COUNT];

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    if (slot < DSD_CALL_STATE_SLOT_COUNT) {
        g_event_sync_counts[slot]++;
    }
}

unsigned int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_call_state_stub_event_sync_count(uint8_t slot) {
    return slot < DSD_CALL_STATE_SLOT_COUNT ? g_event_sync_counts[slot] : 0U;
}
#endif

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_rigctl_query_hook_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    g_rigctl_queries++;
    return g_rigctl_freq;
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

static dsd_trunk_tune_result
nxdn_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    (void)request_id;
    g_tune_cc_count++;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
nxdn_install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = nxdn_hook_tune_to_freq;
    hooks.tune_to_cc_request = nxdn_hook_tune_to_cc;
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
build_scch_identity(uint8_t* message_bits, uint8_t sf, uint8_t rep1, uint16_t id) {
    DSD_MEMSET(message_bits, 0, 32);
    write_bits_u32(message_bits, 0U, sf, 2U);
    write_bits_u32(message_bits, 3U, rep1, 5U);
    write_bits_u32(message_bits, 13U, id, 11U);
}

static void
nxdn_setup_fixture(const nxdn_case* test_case) {
    dsd_state_ext_free_all(&g_state);
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
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
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
    g_tune_cc_count = 0;
    g_rigctl_freq = 0;
    g_rigctl_queries = 0;
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
        case NXDN_GUARD_SCCH_RECENT_ACTIVE: g_state.last_vc_sync_time = dsd_decode_time(); break;
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
    g_state.last_vc_sync_time = dsd_decode_time();
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
    g_state.last_vc_sync_time = dsd_decode_time();
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

static int
nxdn_run_scch_termination_case(void) {
    const nxdn_case termination = {
        "type-d-scch-termination", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 0, 31U, 3U, 1400U, 935000000L,
    };
    nxdn_setup_fixture(&termination);
    g_state.last_vc_sync_time = dsd_decode_time();
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = termination.scch_id,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);

    nxdn_run_case_decode(&termination);

    dsd_call_snapshot call;
    int rc = 0;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1, termination.name, "termination",
                      "canonical call exists");
    rc |= nxdn_expect(call.phase == DSD_CALL_PHASE_ENDED, termination.name, "termination", "canonical call ended");
    rc |= nxdn_expect(dsd_call_state_stub_event_sync_count(0U) == 1U, termination.name, "termination",
                      "ended slot synchronized");
    return rc;
}

/* Where the receiver is when a busy repeater message arrives (nxdn_idas_assoc()). */
typedef enum {
    NXDN_RX_TUNED_THERE = 0, /* tuned to the message's repeater: MATCH */
    NXDN_RX_TUNED_ELSEWHERE, /* tuned to another repeater: OTHER */
    NXDN_RX_UNKNOWN,         /* nothing names the receiver's frequency: UNKNOWN */
    NXDN_RX_RIGCTL_THERE,    /* rigctl on an audio input, no followed route, the peer on the repeater: MATCH */
    NXDN_RX_RIGCTL_FAILS,    /* rigctl on an audio input whose frequency query fails: UNKNOWN */
} nxdn_rx_kind;

/* An anonymous live call takes a busy message's target only when the message names the receiver's repeater. */
static int
nxdn_run_scch_busy_enrich_case(const char* variant, nxdn_rx_kind rx, int expect_relabel) {
    const nxdn_case busy = {
        "type-d-scch-busy-enrich", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 0, 6U, 3U, 1400U, 938012500L,
    };
    const long other_freq = 939512500L;
    nxdn_setup_fixture(&busy);
    g_state.trunk_chan_map[9] = other_freq;
    switch (rx) {
        case NXDN_RX_TUNED_THERE:
        case NXDN_RX_TUNED_ELSEWHERE:
            g_opts.trunk_is_tuned = 1;
            g_state.trunk_vc_freq[0] = g_state.trunk_vc_freq[1] =
                rx == NXDN_RX_TUNED_THERE ? busy.expected_freq : other_freq;
            break;
        case NXDN_RX_UNKNOWN: break;
        case NXDN_RX_RIGCTL_THERE:
        case NXDN_RX_RIGCTL_FAILS:
            g_opts.use_rigctl = 1;
            g_opts.audio_in_type = AUDIO_IN_PULSE;
            g_rigctl_freq = rx == NXDN_RX_RIGCTL_THERE ? busy.expected_freq : 0;
            break;
    }
    g_state.last_vc_sync_time = dsd_decode_time();
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_update_media(&g_state, 0U, 1, 0.0);

    dsd_call_snapshot call;
    (void)dsd_call_state_get(&g_state, 0U, &call);
    const uint64_t previous_epoch = call.epoch;

    nxdn_install_hooks();
    nxdn_run_case_decode(&busy);
    nxdn_run_case_decode(&busy);

    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 0, busy.name, variant, "busy update did not retune");
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1, busy.name, variant, "canonical call exists");
    rc |= nxdn_expect(call.phase == DSD_CALL_PHASE_ACTIVE, busy.name, variant, "canonical call remains active");
    rc |= nxdn_expect(call.epoch == previous_epoch, busy.name, variant, "anonymous call kept its epoch");
    if (expect_relabel) {
        rc |= nxdn_expect(call.ota_target_id == busy.scch_id && call.policy_target_id == busy.scch_id, busy.name,
                          variant, "busy target applied to canonical call");
        rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == busy.scch_id, busy.name, variant,
                          "busy target became the held call");
    } else {
        rc |= nxdn_expect(call.ota_target_id == 0U && call.policy_target_id == 0U, busy.name, variant,
                          "another repeater's target left the call alone");
        rc |= nxdn_expect(g_state.nxdn_call.valid == 0U, busy.name, variant, "no held call was named");
    }
    rc |= nxdn_expect(call.media_active == 1U, busy.name, variant, "media state preserved");
    if (rx == NXDN_RX_RIGCTL_THERE || rx == NXDN_RX_RIGCTL_FAILS) {
        /* Each message asks where the receiver is, but a rigctl query is a round trip: once per tuning generation,
         * a failed one included. */
        rc |= nxdn_expect(g_rigctl_queries == 1, busy.name, variant, "one rigctl query per tuning generation");
    }
    return rc;
}

static int
nxdn_run_scch_identity_context_case(void) {
    const nxdn_case context = {
        "type-d-scch-identity-context", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 0, 0, 0, 0, 0,
    };
    nxdn_setup_fixture(&context);
    /* The receiver is on repeater 7, where the call is. */
    g_state.trunk_chan_map[7] = 938012500L;
    g_state.trunk_chan_map[8] = 938512500L;
    g_opts.trunk_is_tuned = 1;
    g_state.trunk_vc_freq[0] = g_state.trunk_vc_freq[1] = 938012500L;
    g_state.last_vc_sync_time = dsd_decode_time();
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1400U,
        .policy_target_id = 1400U,
        .ota_source_id = 1100U,
        .channel = 7U,
        .frequency_hz = 938012500L,
        .service_options = 0x85U,
        .emergency = 1U,
        .priority = 5U,
        .has_service_metadata = 1U,
    };
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "SOURCE 1100");
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "TARGET 1400");
    DSD_SNPRINTF(observation.route_text[0], sizeof(observation.route_text[0]), "%s", "RAN 7");
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    const dsd_call_crypto_update crypto = {
        .classification = DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
        .algid = 3U,
        .kid = 42U,
        .mi = 0x12345678U,
        .audio_permitted = 0U,
    };
    (void)dsd_call_state_update_crypto(&g_state, 0U, &crypto);
    (void)dsd_call_state_update_media(&g_state, 0U, 1, 0.0);
    dsd_call_snapshot call;
    (void)dsd_call_state_get(&g_state, 0U, &call);
    uint64_t previous_epoch = call.epoch;

    uint8_t message[32];
    int rc = 0;
    /* A target message naming another repeater is another call: the held one keeps its target and epoch. */
    build_scch_identity(message, 2U, 8U, 1600U);
    NXDN_decode_scch(&g_opts, &g_state, message, 1U);
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.ota_target_id == 1400U
                          && call.policy_target_id == 1400U && call.epoch == previous_epoch,
                      context.name, "other-repeater", "another repeater's target left the call alone");

    build_scch_identity(message, 2U, 7U, 1500U);
    NXDN_decode_scch(&g_opts, &g_state, message, 1U);

    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1, context.name, "target-fragment",
                      "canonical call exists");
    rc |= nxdn_expect(call.ota_target_id == 1500U && call.policy_target_id == 1500U, context.name, "target-fragment",
                      "target updated");
    rc |= nxdn_expect(call.epoch != previous_epoch, context.name, "target-fragment", "replacement epoch started");
    rc |= nxdn_expect(call.ota_source_id == 1100U, context.name, "target-fragment", "source preserved");
    rc |= nxdn_expect(call.channel == 7U && call.frequency_hz == 938012500L, context.name, "target-fragment",
                      "route preserved");
    rc |= nxdn_expect(call.service_options == 0x85U && call.has_service_metadata == 1U && call.emergency == 1U
                          && call.priority == 5U,
                      context.name, "target-fragment", "service metadata preserved");
    rc |= nxdn_expect(call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING && call.algid == 3U && call.kid == 42U
                          && call.mi == 0x12345678U && call.audio_permitted == 0U,
                      context.name, "target-fragment", "crypto preserved");
    rc |= nxdn_expect(call.media_active == 1U, context.name, "target-fragment", "media state preserved");
    rc |= nxdn_expect(strcmp(call.source_text, "SOURCE 1100") == 0, context.name, "target-fragment",
                      "counterpart text preserved");
    rc |=
        nxdn_expect(strcmp(call.route_text[0], "RAN 7") == 0, context.name, "target-fragment", "route text preserved");
    previous_epoch = call.epoch;

    build_scch_identity(message, 1U, 0U, 1200U);
    NXDN_decode_scch(&g_opts, &g_state, message, 1U);
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1, context.name, "source-fragment",
                      "canonical call exists");
    rc |= nxdn_expect(call.ota_target_id == 1500U && call.policy_target_id == 1500U, context.name, "source-fragment",
                      "target preserved");
    rc |= nxdn_expect(call.ota_source_id == 1200U, context.name, "source-fragment", "source updated");
    rc |= nxdn_expect(call.epoch != previous_epoch, context.name, "source-fragment", "replacement epoch started");
    rc |= nxdn_expect(call.channel == 7U && call.frequency_hz == 938012500L, context.name, "source-fragment",
                      "route preserved");
    rc |= nxdn_expect(call.service_options == 0x85U && call.has_service_metadata == 1U && call.emergency == 1U
                          && call.priority == 5U,
                      context.name, "source-fragment", "service metadata preserved");
    rc |= nxdn_expect(call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING && call.algid == 3U && call.kid == 42U
                          && call.mi == 0x12345678U && call.audio_permitted == 0U,
                      context.name, "source-fragment", "crypto preserved");
    rc |= nxdn_expect(call.media_active == 1U, context.name, "source-fragment", "media state preserved");
    return rc;
}

/* ---- Admission while a call is held ------------------------------------------------------------------------- */

static void
at_seconds(double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * 1e9));
}

static void
policy_row(uint32_t id, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, "A", "test", DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return;
    }
    row.priority = priority;
    row.preempt = (uint8_t)preempt;
    (void)dsd_tg_policy_upsert_exact(&g_state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

/* A Type-C fixture with channels 16-19 mapped and the control channel known, untuned on the control channel. */
static void
type_c_fixture(void) {
    const nxdn_case base = {"type-c", NXDN_MATRIX_TYPE_C, 0x04U, 1U, 2100U, 1100U, 16U, 0, 0, 0, 0, 0, 0, 936012500L};
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&base);
    for (uint16_t ch = 16U; ch <= 19U; ch++) {
        g_state.trunk_chan_map[ch] = 936012500L + ((long)(ch - 16U) * 500000L);
    }
    nxdn_install_hooks();
}

static long
type_c_freq(uint16_t ch) {
    return 936012500L + ((long)(ch - 16U) * 500000L);
}

static void
send_assignment(uint8_t message_type, uint16_t target, uint16_t channel) {
    const nxdn_case c = {"", NXDN_MATRIX_TYPE_C, message_type, 1U, 2100U, target, channel, 0, 0, 0, 0, 0, 0, 0};
    uint8_t message[128];
    build_vcall_assgn(message, &c);
    NXDN_decode_VCALL_ASSGN(&g_opts, &g_state, message);
}

/* A checked VCALL of a confirmed transmission on the channel the receiver is on. */
static void
send_vcall(uint16_t source, uint16_t target) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));
    set_message_type(bits, 0x01U);
    write_bits_u32(bits, 16U, 1U, 3U);
    write_bits_u32(bits, 24U, source, 16U);
    write_bits_u32(bits, 40U, target, 16U);
    g_state.nxdn_confirmed = 1;
    NXDN_Elements_Content_decode(&g_opts, &g_state, bits, sizeof(bits));
}

static void
send_disc(void) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));
    set_message_type(bits, 0x11U);
    NXDN_Elements_Content_decode(&g_opts, &g_state, bits, sizeof(bits));
}

/* Voice is being heard on the receiver's channel now. */
static void
voice_live(void) {
    g_state.last_vc_sync_time = dsd_decode_time();
}

/* A duplicate assignment heard on a voice channel past its call's hangtime is refused (here by the allow-list, or for
 * an unmapped channel) without untuning the receiver: DISC still takes it back to the control channel, and the voice
 * channel never becomes the control channel. */
static int
nxdn_run_refused_duplicate_keeps_tuned_case(void) {
    static const char* const test_case = "type-c-refused-duplicate-keeps-tuned";
    const long vc = type_c_freq(19U);
    int rc = 0;
    for (int variant = 0; variant < 3; variant++) {
        const char* const result_name = variant == 0 ? "allowlist" : (variant == 1 ? "unmapped" : "no-control-channel");
        type_c_fixture();
        g_opts.trunk_is_tuned = 1;
        g_state.trunk_vc_freq[0] = g_state.trunk_vc_freq[1] = vc;
        g_state.last_vc_sync_time = dsd_decode_time() - 4; /* past the 1 s hangtime */
        g_opts.use_rigctl = 1;
        g_rigctl_freq = vc;
        if (variant == 0) {
            g_opts.trunk_use_allow_list = 1;
        } else if (variant == 1) {
            g_state.trunk_chan_map[16] = 0;
        } else {
            g_state.p25_cc_freq = 0;
            g_state.trunk_cc_freq = 0;
        }
        send_assignment(0x05U, 1100U, 16U);
        rc |= nxdn_expect(g_tune_count == 0, test_case, result_name, "refused duplicate did not tune");
        rc |= nxdn_expect(g_opts.trunk_is_tuned == 1, test_case, result_name,
                          "refused duplicate kept the receiver tuned");
        rc |= nxdn_expect(variant == 2 ? g_state.p25_cc_freq == 0 : g_state.p25_cc_freq == 935000000L, test_case,
                          result_name, "voice channel not anchored as the control channel");
        if (variant != 2) {
            send_disc();
            rc |= nxdn_expect(g_tune_cc_count == 1, test_case, result_name, "DISC returned to the control channel");
            rc |= nxdn_expect(g_opts.trunk_is_tuned == 0, test_case, result_name, "DISC untuned the receiver");
        }
    }
    return rc;
}

/* A duplicate past the hangtime for an allowed, mapped call is followed, and the route is recorded. */
static int
nxdn_run_duplicate_after_hangtime_follows_case(void) {
    static const char* const name = "type-c-duplicate-after-hangtime";
    type_c_fixture();
    g_opts.trunk_is_tuned = 1;
    g_state.trunk_vc_freq[0] = g_state.trunk_vc_freq[1] = type_c_freq(19U);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    send_assignment(0x05U, 1100U, 16U);
    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 1 && g_last_tune_freq == type_c_freq(16U), name, "dup", "duplicate followed");
    rc |= nxdn_expect(g_state.nxdn_follow.chan == 16U && g_state.nxdn_follow.freq_hz == type_c_freq(16U), name, "dup",
                      "route recorded");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1100U, name, "dup",
                      "held call recorded");
    return rc;
}

/* While the followed call is live, a preempting candidate that outranks it takes the receiver once the call has been
 * held for the dwell; a candidate without the flag never does, and does not stand in for the held call. The takeover
 * starts a cooldown that holds back the next candidate. */
static int
nxdn_run_type_c_preemption_case(void) {
    static const char* const name = "type-c-preemption";
    type_c_fixture();
    policy_row(1100U, 10, 0);
    policy_row(1400U, 90, 0);
    policy_row(1200U, 60, 1);
    policy_row(1300U, 80, 1);
    int rc = 0;
    at_seconds(2000.0);
    send_assignment(0x04U, 1100U, 16U);
    rc |= nxdn_expect(g_tune_count == 1 && g_state.nxdn_follow.chan == 16U, name, "follow", "first call followed");

    at_seconds(2000.5);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 1, name, "dwell", "preemption waits for the dwell");

    at_seconds(2000.7);
    voice_live();
    send_assignment(0x05U, 1400U, 19U);
    rc |= nxdn_expect(g_tune_count == 1, name, "no-preempt", "a call without preempt never takes over");

    at_seconds(2000.8);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == type_c_freq(17U), name, "preempt",
                      "outranking preempting call taken against the held call, not the refused one");
    rc |= nxdn_expect(g_state.nxdn_follow.chan == 17U && g_state.nxdn_call.target == 1200U, name, "preempt",
                      "route and held call moved");

    at_seconds(2001.6);
    voice_live();
    send_assignment(0x05U, 1300U, 18U);
    rc |= nxdn_expect(g_tune_count == 2, name, "cooldown", "the takeover's cooldown holds back the next one");

    at_seconds(2001.9);
    voice_live();
    send_assignment(0x05U, 1300U, 18U);
    rc |= nxdn_expect(g_tune_count == 3 && g_last_tune_freq == type_c_freq(18U), name, "after-cooldown",
                      "taken once the cooldown ran out");
    return rc;
}

/* The engine took the receiver back to the control channel by itself (the hangtime return): the ledger still holds
 * the call left. Following a preempting call from the control channel is no takeover, so it starts no cooldown that
 * would hold back an outranking call once the new one was held for the dwell. */
static int
nxdn_run_follow_from_control_channel_is_no_takeover_case(void) {
    static const char* const name = "type-c-follow-is-no-takeover";
    type_c_fixture();
    policy_row(1100U, 10, 0);
    policy_row(1200U, 60, 1);
    policy_row(1300U, 80, 1);
    int rc = 0;
    at_seconds(5000.0);
    send_assignment(0x04U, 1100U, 16U);
    g_opts.trunk_is_tuned = 0; /* the engine's hangtime return */
    g_state.trunk_vc_freq[0] = g_state.trunk_vc_freq[1] = 0;
    at_seconds(5005.0);
    send_assignment(0x04U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == type_c_freq(17U), name, "follow",
                      "call followed from the control channel");
    at_seconds(5005.8);
    voice_live();
    send_assignment(0x05U, 1300U, 18U);
    rc |= nxdn_expect(g_tune_count == 3 && g_last_tune_freq == type_c_freq(18U), name, "preempt",
                      "outranking call taken with no cooldown from the follow");
    return rc;
}

/* A duplicate assignment past the hangtime that names another call on the carrier already tuned is that new call, not
 * a repeat: it becomes the held call, so preemption compares a candidate against it. */
static int
nxdn_run_same_carrier_new_call_case(void) {
    static const char* const test_case = "type-c-same-carrier-new-call";
    type_c_fixture();
    policy_row(1100U, 10, 0);
    policy_row(1500U, 100, 0);
    policy_row(1200U, 50, 1);
    int rc = 0;
    at_seconds(6000.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(6004.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4; /* past the 1 s hangtime */
    send_assignment(0x05U, 1500U, 16U);
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1500U, test_case, "new-call",
                      "the new call on the carrier is the held call");
    const int tunes = g_tune_count;
    at_seconds(6005.0);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == tunes, test_case, "outranked", "a candidate below the new call is refused");
    return rc;
}

/* The held call is the one heard: a new call on the followed carrier replaces the followed grant's, its follow time
 * starting then, so preemption compares a candidate against it. */
static int
nxdn_run_held_call_reconciled_case(void) {
    static const char* const name = "type-c-held-call-reconciled";
    int rc = 0;

    /* Followed 1100 (p10), then a call of 1500 (p100) on the same carrier: a p50 preempting call is refused. */
    type_c_fixture();
    policy_row(1100U, 10, 0);
    policy_row(1500U, 100, 0);
    policy_row(1200U, 50, 1);
    at_seconds(3000.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(3000.2);
    voice_live();
    send_vcall(2200U, 1500U);
    rc |= nxdn_expect(g_state.nxdn_call.target == 1500U, name, "outranked", "the heard call is the held call");
    at_seconds(3001.5);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 1, name, "outranked", "a candidate below the heard call is refused");

    /* Followed 1100 (p10), then a call of 1600 (p20): the dwell runs from the new call. */
    type_c_fixture();
    policy_row(1100U, 10, 0);
    policy_row(1600U, 20, 0);
    policy_row(1200U, 50, 1);
    at_seconds(3100.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(3100.2);
    voice_live();
    send_vcall(2200U, 1600U);
    at_seconds(3100.8);
    voice_live();
    send_vcall(2200U, 1600U); /* the same call goes on: its follow time stays */
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 1, name, "dwell", "dwell runs from the heard call, not the grant");
    at_seconds(3101.0);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == type_c_freq(17U), name, "dwell",
                      "taken once the heard call was held for the dwell");
    return rc;
}

/* A refused duplicate overwrites the last grant's channel, not the followed route: a new call heard on the followed
 * carrier names the followed channel. */
static int
nxdn_run_vcall_names_followed_route_case(void) {
    static const char* const name = "type-c-vcall-route";
    type_c_fixture();
    send_assignment(0x04U, 1100U, 16U);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    send_vcall(2300U, 1300U);
    dsd_call_snapshot call;
    int rc = 0;
    rc |= nxdn_expect(g_tune_count == 1, name, "refused", "duplicate refused");
    rc |= nxdn_expect(g_state.nxdn_grant_chan == 17U, name, "refused", "last grant names the refused channel");
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.ota_target_id == 1300U, name, "vcall",
                      "new call published");
    rc |= nxdn_expect(call.channel == 16U && call.frequency_hz == type_c_freq(16U), name, "vcall",
                      "new call names the followed channel");
    return rc;
}

/* A Type-D busy message for another repeater while voice is live takes the receiver for the held talkgroup or an
 * outranking preempting call; one for the receiver's own repeater, or for an unplaceable one, never does. */
static int
nxdn_run_type_d_takeover_case(void) {
    static const char* const name = "type-d-takeover";
    const nxdn_case first = {
        "type-d", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 0, 6U, 3U, 1400U, 938012500L,
    };
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&first);
    g_state.trunk_chan_map[7] = 938512500L;
    g_state.trunk_chan_map[8] = 939012500L;
    nxdn_install_hooks();
    policy_row(1400U, 10, 0);
    policy_row(1500U, 60, 1);
    policy_row(1700U, 90, 1);
    int rc = 0;

    at_seconds(4000.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    nxdn_run_case_decode(&first);
    rc |= nxdn_expect(g_tune_count == 1 && g_state.nxdn_follow.repeater == 6U, name, "follow", "first call followed");

    nxdn_case other = first;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(4000.5);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 1, name, "dwell", "preemption waits for the dwell");
    at_seconds(4000.8);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == 938512500L && g_state.nxdn_follow.repeater == 7U, name,
                      "preempt", "outranking call on another repeater taken");

    /* The receiver's own repeater: whatever it names, the receiver stays. */
    nxdn_case own = first;
    own.scch_rep1 = 7U;
    own.scch_id = 1700U;
    at_seconds(4002.0);
    voice_live();
    nxdn_run_case_decode(&own);
    rc |= nxdn_expect(g_tune_count == 2, name, "own-repeater", "own repeater's message never moves the receiver");

    /* An unmapped repeater cannot be placed. */
    nxdn_case unknown = first;
    unknown.scch_rep1 = 9U;
    unknown.scch_id = 1700U;
    at_seconds(4003.5);
    voice_live();
    nxdn_run_case_decode(&unknown);
    rc |= nxdn_expect(g_tune_count == 2, name, "unknown-repeater", "unplaceable repeater never moves the receiver");

    /* The held talkgroup on another repeater takes over a call that is not of it. */
    g_state.tg_hold = 1800U;
    nxdn_case hold = first;
    hold.scch_rep1 = 8U;
    hold.scch_id = 1800U;
    at_seconds(4003.6);
    voice_live();
    nxdn_run_case_decode(&hold);
    rc |= nxdn_expect(g_tune_count == 3 && g_last_tune_freq == 939012500L, name, "hold", "held talkgroup taken");
    g_state.tg_hold = 0U;
    return rc;
}

/* A Type-D busy message repeated past the voice window for the call the receiver still holds re-follows that call: its
 * dwell keeps running from the first follow, so an outranking preempting call is taken once voice resumes. */
static int
nxdn_run_type_d_refollow_keeps_dwell_case(void) {
    static const char* const name = "type-d-refollow-keeps-dwell";
    const nxdn_case held = {
        "type-d", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 0, 6U, 3U, 1400U, 938012500L,
    };
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&held);
    g_state.trunk_chan_map[7] = 938512500L;
    nxdn_install_hooks();
    policy_row(1400U, 10, 0);
    policy_row(1500U, 100, 1);
    int rc = 0;

    at_seconds(7000.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    nxdn_run_case_decode(&held);
    rc |= nxdn_expect(g_tune_count == 1 && g_state.nxdn_follow.repeater == 6U, name, "follow", "first call followed");

    at_seconds(7002.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 2; /* past the 1 s voice window */
    nxdn_run_case_decode(&held);
    rc |= nxdn_expect(g_state.nxdn_follow.repeater == 6U && g_state.nxdn_call.target == 1400U, name, "refollow",
                      "the held call re-followed");

    nxdn_case other = held;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(7002.1);
    voice_live();
    const int tunes = g_tune_count;
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == tunes + 1 && g_last_tune_freq == 938512500L && g_state.nxdn_follow.repeater == 7U,
                      name, "preempt", "dwell runs from the first follow, not the repeat");
    return rc;
}

/* A Type-D private call followed from a busy message, whose late-entry audio opened an anonymous generic voice call:
 * a repeat of the busy message names that call's target, and the held call keeps the message's private classification
 * (its G/U bit), never group policy (here a priority-100 group range over the private ID), nor a restarted dwell. */
static int
nxdn_run_type_d_private_reconcile_case(void) {
    static const char* const name = "type-d-private-reconcile";
    const nxdn_case held = {
        "type-d", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, 1, 6U, 3U, 1400U, 938012500L,
    };
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&held);
    g_state.trunk_chan_map[7] = 938512500L;
    nxdn_install_hooks();
    dsd_tg_policy_entry range;
    if (dsd_tg_policy_make_exact_entry(1400U, "A", "range", DSD_TG_POLICY_SOURCE_IMPORTED, &range) == 0) {
        range.id_end = 1499U;
        range.is_range = 1U;
        range.priority = 100;
        (void)dsd_tg_policy_add_range_entry(&g_state, &range);
    }
    policy_row(1500U, 50, 1);
    int rc = 0;

    at_seconds(8000.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    nxdn_run_case_decode(&held);
    rc |= nxdn_expect(g_tune_count == 1 && g_state.nxdn_call.valid == 1U && g_state.nxdn_call.private_call == 1U, name,
                      "follow", "private call followed");

    at_seconds(8000.3);
    voice_live();
    const dsd_call_observation late_entry = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(&g_state, &late_entry, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_update_media(&g_state, 0U, 1, 0.0);

    at_seconds(8000.4);
    voice_live();
    nxdn_run_case_decode(&held);
    dsd_call_snapshot call;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.policy_target_id == 1400U, name, "repeat",
                      "the repeat named the anonymous call");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1400U
                          && g_state.nxdn_call.private_call == 1U,
                      name, "repeat", "the held call stays private");

    nxdn_case other = held;
    other.scch_gu = 0U;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(8001.2);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == 938512500L, name, "preempt",
                      "a p50 preempt outranks the private call (no group priority, dwell from the follow)");
    return rc;
}

/* A Type-D fixture on repeater 6 with repeater 7 mapped too, untuned on the control channel. */
static nxdn_case
type_d_fixture(uint8_t gu, uint16_t id) {
    const nxdn_case base = {
        "type-d", NXDN_MATRIX_TYPE_D, 0U, 0U, 0U, 0U, 0U, 0, 0, gu, 6U, 3U, id, 938012500L,
    };
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    nxdn_setup_fixture(&base);
    g_state.trunk_chan_map[7] = 938512500L;
    nxdn_install_hooks();
    return base;
}

/* An anonymous call (late entry, before any message names it) opens on the receiver's channel. */
static void
late_entry_call(void) {
    const dsd_call_observation late_entry = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(&g_state, &late_entry, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_update_media(&g_state, 0U, 1, 0.0);
}

/* Voice decoded on the receiver's channel, as the vocoder marks it (mark_vocoder_call_media()): it continues the active
 * canonical call, and opens an anonymous one only when none is active. */
static void
vocoder_media(void) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(&g_state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        late_entry_call();
        return;
    }
    (void)dsd_call_state_update_media(&g_state, 0U, 1, 0.0);
}

/* What a takeover did to the displaced call A (epoch @p a_epoch, target @p a_target): it ended there, explicitly and
 * synchronized (@p syncs_before: slot 0's event syncs before the takeover), so the preempting call's voice on the new
 * carrier opens an epoch of its own, which the held call binds to, instead of continuing A under A's identity. */
static int
expect_left_call_ended(const char* name, uint64_t a_epoch, uint32_t a_target, unsigned int syncs_before) {
    dsd_call_snapshot call;
    int rc = 0;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.epoch == a_epoch
                          && call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT,
                      name, "left", "the displaced call ended explicitly at the carrier boundary");
    rc |= nxdn_expect(dsd_call_state_stub_event_sync_count(0U) == syncs_before + 1U, name, "left",
                      "the displaced call's end was synchronized");
    vocoder_media();
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.epoch != a_epoch && call.ota_target_id != a_target,
                      name, "new-carrier", "voice on the new carrier opened its own call, not the displaced one");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.call_floor == a_epoch
                          && g_state.nxdn_call.call_epoch == 0U,
                      name, "new-carrier", "the held call awaits the first call after the displaced one");
    return rc;
}

/* A Type-C preemption leaves the carrier: the displaced call ends there, so the preempting call's voice (before its
 * VCALL) never continues it, and the held call binds to the preempting call's own epoch. */
static int
nxdn_run_type_c_takeover_ends_left_call_case(void) {
    static const char* const name = "type-c-takeover-ends-left-call";
    type_c_fixture();
    policy_row(1100U, 10, 0);
    policy_row(1200U, 60, 1);
    int rc = 0;
    at_seconds(9500.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(9500.2);
    voice_live();
    send_vcall(2100U, 1100U);
    dsd_call_snapshot call;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.ota_target_id == 1100U,
                      name, "held", "the followed call is on the air");
    const uint64_t a_epoch = call.epoch;
    const unsigned int syncs = dsd_call_state_stub_event_sync_count(0U);
    at_seconds(9500.9);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == type_c_freq(17U), name, "preempt", "preempted");
    rc |= expect_left_call_ended(name, a_epoch, 1100U, syncs);
    at_seconds(9501.0);
    voice_live();
    send_vcall(2200U, 1200U);
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && g_state.nxdn_call.target == 1200U
                          && g_state.nxdn_call.call_epoch == dsd_call_snapshot_transmission(&call),
                      name, "vcall", "the held call is bound to the preempting call");
    return rc;
}

/* A Type-D busy message repeated for the call the receiver still holds is an in-place repeat: the call on the air
 * keeps its epoch. A preemption to another repeater leaves the carrier: the displaced call ends there. */
static int
nxdn_run_type_d_takeover_ends_left_call_case(void) {
    static const char* const name = "type-d-takeover-ends-left-call";
    const nxdn_case held = type_d_fixture(0U, 1400U);
    policy_row(1400U, 10, 0);
    policy_row(1500U, 60, 1);
    int rc = 0;
    at_seconds(9600.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    nxdn_run_case_decode(&held);
    at_seconds(9600.2);
    voice_live();
    send_vcall(2200U, 1400U);
    dsd_call_snapshot call;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE, name, "held",
                      "the followed call is on the air");
    const uint64_t a_epoch = call.epoch;
    const unsigned int syncs = dsd_call_state_stub_event_sync_count(0U);

    at_seconds(9602.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 2; /* past the 1 s voice window */
    nxdn_run_case_decode(&held);
    rc |= nxdn_expect(g_tune_count == 2 && dsd_call_state_get(&g_state, 0U, &call) == 1
                          && call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == a_epoch
                          && dsd_call_state_stub_event_sync_count(0U) == syncs,
                      name, "repeat", "an in-place repeat keeps the call on the air");

    nxdn_case other = held;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(9602.1);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 3 && g_last_tune_freq == 938512500L, name, "preempt", "preempted");
    rc |= expect_left_call_ended(name, a_epoch, 1400U, syncs);
    return rc;
}

/* The engine's hangtime return ended the followed call and untuned the receiver, leaving the held call's record behind.
 * Following the same talkgroup on the same channel again is a new follow: its dwell runs from now, never from the
 * follow of the call that ended, so a preempting call heard right after it is refused. */
static int
nxdn_run_refollow_after_hangtime_return_case(void) {
    static const char* const name = "type-c-refollow-after-hangtime-return";
    type_c_fixture();
    policy_row(1100U, 10, 0);
    policy_row(1200U, 60, 1);
    int rc = 0;
    at_seconds(9700.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(9700.2);
    voice_live();
    send_vcall(2100U, 1100U);
    at_seconds(9705.0); /* the engine's hangtime return */
    (void)dsd_call_state_end_ex(&g_state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_EXPLICIT);
    g_opts.trunk_is_tuned = 0;
    g_state.trunk_vc_freq[0] = g_state.trunk_vc_freq[1] = 0;
    at_seconds(9710.0);
    send_assignment(0x04U, 1100U, 16U);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == type_c_freq(16U), name, "follow", "followed again");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.follow_m > 9709.0, name, "follow",
                      "the follow time is the new follow's");
    at_seconds(9710.1);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 2, name, "dwell", "a preempting call waits for the new follow's dwell");
    at_seconds(9710.8);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 3 && g_last_tune_freq == type_c_freq(17U), name, "preempt",
                      "taken once the new follow was held for the dwell");
    return rc;
}

#ifdef NXDN_MATRIX_REAL_CALL_STATE
/* A checked transmission release (TX_REL) of the followed call. */
static void
send_tx_release(uint16_t source, uint16_t target) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));
    set_message_type(bits, 0x08U);
    write_bits_u32(bits, 16U, 1U, 3U);
    write_bits_u32(bits, 24U, source, 16U);
    write_bits_u32(bits, 40U, target, 16U);
    NXDN_Elements_Content_decode(&g_opts, &g_state, bits, sizeof(bits));
}

/* The followed call's transmission ends with TX_REL long after its identity was last read, and the reply follows within
 * the hangtime of the release: the reply is the conversation's gap, measured from the release (the ended call's own
 * end), not from the last read. Its VCALL names the same call, which keeps its follow time, so a preempting call right
 * after the reply is judged against the dwell from the first follow. Needs the real call state: the release's end time
 * is the call state's own stamp. */
static int
nxdn_run_release_reply_keeps_dwell_case(void) {
    static const char* const name = "type-c-release-reply-keeps-dwell";
    type_c_fixture();
    g_opts.trunk_hangtime = 2.0f;
    policy_row(1100U, 10, 0);
    policy_row(1200U, 100, 1);
    int rc = 0;
    at_seconds(1000.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(1000.2);
    voice_live();
    send_vcall(2100U, 1100U);
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1100U, name, "held",
                      "the followed call is held");
    at_seconds(1004.0);
    voice_live();
    send_tx_release(2100U, 1100U);
    dsd_call_snapshot call;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ENDED, name,
                      "release", "the transmission ended");
    rc |= nxdn_expect(fabs(g_state.nxdn_call.call_seen_m - call.ended_m) < 1e-6, name, "release",
                      "the held call is bound to the ended transmission's end");
    at_seconds(1004.2);
    voice_live();
    send_vcall(2200U, 1100U);
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1100U
                          && g_state.nxdn_call.follow_m < 1001.0,
                      name, "reply", "the reply within the hangtime keeps the conversation's follow time");
    at_seconds(1004.3);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == type_c_freq(17U), name, "preempt",
                      "a priority-100 preempting call is taken (dwell from the first follow)");
    return rc;
}

/* As the release case, but the followed call's transmission ends by a sync loss (no TX_REL), as the engine's no-carrier
 * pass ends it and tells NXDN (nxdn_held_call_note_call_end()): the reply, past the reacquire gap (a new transmission)
 * and within the hangtime of the end, is the conversation's gap measured from the end, not from the last read. */
static int
nxdn_run_sync_loss_reply_keeps_dwell_case(void) {
    static const char* const name = "type-c-sync-loss-reply-keeps-dwell";
    type_c_fixture();
    g_opts.trunk_hangtime = 2.0f;
    policy_row(1100U, 10, 0);
    policy_row(1200U, 100, 1);
    int rc = 0;
    at_seconds(1100.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(1100.2);
    voice_live();
    send_vcall(2100U, 1100U);
    at_seconds(1104.0);
    voice_live();
    (void)dsd_call_state_end_ex(&g_state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS); /* the engine's no-carrier pass */
    nxdn_held_call_note_call_end(&g_state);
    dsd_call_snapshot call;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ENDED
                          && fabs(g_state.nxdn_call.call_seen_m - call.ended_m) < 1e-6,
                      name, "sync-loss", "the held call is bound to the faded transmission's end");
    at_seconds(1104.6);
    voice_live();
    send_vcall(2200U, 1100U);
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && g_state.nxdn_call.valid == 1U && g_state.nxdn_call.follow_m < 1101.0,
                      name, "reply", "the reply within the hangtime keeps the conversation's follow time");
    at_seconds(1104.7);
    voice_live();
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == type_c_freq(17U), name, "preempt",
                      "a priority-100 preempting call is taken (dwell from the first follow)");
    return rc;
}

/* The followed call ends with TX_REL and its held call lapses past the hangtime while the receiver is still on the
 * carrier: a duplicate assignment there for the next talkgroup is a new call, not a repeat of one no longer known. It
 * is followed in place and becomes the held call, so once voice resumes (before any VCALL names it) an outranking
 * preempting call is judged against it and taken after the dwell. */
static int
nxdn_run_lapsed_call_same_carrier_duplicate_case(void) {
    static const char* const name = "type-c-lapsed-call-same-carrier-duplicate";
    type_c_fixture();
    g_opts.trunk_hangtime = 2.0f;
    policy_row(1100U, 10, 0);
    policy_row(1500U, 10, 0);
    policy_row(1200U, 100, 1);
    int rc = 0;
    at_seconds(1200.0);
    send_assignment(0x04U, 1100U, 16U);
    at_seconds(1200.2);
    voice_live();
    send_vcall(2100U, 1100U);
    at_seconds(1201.0);
    voice_live();
    send_tx_release(2100U, 1100U);
    at_seconds(1204.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4; /* past the 2 s hangtime */
    send_assignment(0x05U, 1500U, 16U);
    rc |= nxdn_expect(g_opts.trunk_is_tuned == 1 && g_state.trunk_vc_freq[0] == type_c_freq(16U), name, "new-call",
                      "the receiver stays on the carrier");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1500U, name, "new-call",
                      "the new call is the held call");
    at_seconds(1204.2);
    voice_live();
    vocoder_media();
    at_seconds(1205.0);
    voice_live();
    const int tunes = g_tune_count;
    send_assignment(0x05U, 1200U, 17U);
    rc |= nxdn_expect(g_tune_count == tunes + 1 && g_last_tune_freq == type_c_freq(17U), name, "preempt",
                      "a priority-100 preempting call is taken from the new held call");
    return rc;
}

/* The receiver sits on a carrier (no grant tune) decoding a call a VCALL named, and follows an announcement of that
 * very call there (@p variant 0: a Type-D busy message past the voice window; 1: a Type-C assignment while untuned).
 * The follow is in place, so the held call binds to the call on the air instead of awaiting a later one: the call's
 * TX_REL ends it, and the next, anonymous transmission past the hangtime inherits neither its identity nor its
 * priority, so an outranking preempting call on another channel never takes the receiver from it. */
static int
nxdn_run_in_place_follow_binds_call_variant(int variant) {
    const char* const name = variant == 0 ? "type-d-in-place-follow-binds-call" : "type-c-in-place-follow-binds-call";
    const nxdn_case type_d = type_d_fixture(0U, 1400U);
    if (variant != 0) {
        type_c_fixture();
    }
    const long here = variant == 0 ? type_d.expected_freq : type_c_freq(16U);
    g_opts.trunk_hangtime = 2.0f;
    g_opts.use_rigctl = 1;
    g_opts.audio_in_type = AUDIO_IN_PULSE;
    g_rigctl_freq = here; /* the receiver sits on the carrier */
    policy_row(1400U, 10, 0);
    policy_row(1500U, 100, 1);
    int rc = 0;

    at_seconds(9800.0);
    voice_live();
    send_vcall(2200U, 1400U);
    dsd_call_snapshot call;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE, name,
                      "named", "the named call is on the air");
    const uint64_t a_epoch = call.epoch;

    at_seconds(9801.5);
    g_state.last_vc_sync_time = dsd_decode_time() - 2; /* past the 1 s voice window */
    if (variant == 0) {
        nxdn_run_case_decode(&type_d);
    } else {
        send_assignment(0x04U, 1400U, 16U);
    }
    rc |= nxdn_expect(g_tune_count == 1 && g_last_tune_freq == here, name, "follow", "followed in place");
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.epoch == a_epoch,
                      name, "follow", "the call on the air stays");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1400U
                          && g_state.nxdn_call.call_epoch == dsd_call_snapshot_transmission(&call),
                      name, "follow", "the held call is bound to the call on the air");

    at_seconds(9802.0);
    voice_live();
    send_tx_release(2200U, 1400U);
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.phase == DSD_CALL_PHASE_ENDED
                          && fabs(g_state.nxdn_call.call_seen_m - call.ended_m) < 1e-6,
                      name, "release", "the release ends the held call's own call");

    at_seconds(9805.0); /* past the 2 s hangtime */
    voice_live();
    late_entry_call();
    at_seconds(9805.2);
    voice_live();
    if (variant == 0) {
        nxdn_case other = type_d;
        other.scch_rep1 = 7U;
        other.scch_id = 1500U;
        nxdn_run_case_decode(&other);
    } else {
        send_assignment(0x05U, 1500U, 17U);
    }
    rc |= nxdn_expect(g_tune_count == 1 && g_state.nxdn_call.valid == 0U, name, "anonymous",
                      "the next anonymous call is not preempted as the released one");
    return rc;
}

static int
nxdn_run_in_place_follow_binds_call_case(void) {
    return nxdn_run_in_place_follow_binds_call_variant(0) | nxdn_run_in_place_follow_binds_call_variant(1);
}
#endif

/* A resident Type-D call heard without a grant tune (the receiver parked on its repeater) ends with its DISC: the next,
 * anonymous call there is not the ended one, so it yields only to Hold, never to a preempting call that merely
 * outranks the ended call. */
static int
nxdn_run_type_d_resident_disc_case(void) {
    static const char* const name = "type-d-resident-disc";
    const nxdn_case base = type_d_fixture(0U, 1400U);
    g_opts.use_rigctl = 1;
    g_opts.audio_in_type = AUDIO_IN_PULSE;
    g_rigctl_freq = base.expected_freq; /* the receiver sits on repeater 6 */
    policy_row(1400U, 10, 0);
    policy_row(1500U, 50, 1);
    int rc = 0;

    at_seconds(9000.0);
    voice_live();
    send_vcall(2200U, 1400U);
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1400U, name, "resident",
                      "the resident call is the held call");
    at_seconds(9001.0);
    voice_live();
    send_disc();
    rc |= nxdn_expect(g_opts.trunk_is_tuned == 0 && g_tune_cc_count == 0, name, "disc", "no grant tune to release");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 0U, name, "disc", "the ended call is no longer held");

    at_seconds(9001.5);
    voice_live();
    late_entry_call();
    nxdn_case other = base;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(9002.0);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 0, name, "anonymous", "an anonymous call is not preempted as the ended one");
    g_state.tg_hold = 1500U;
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 1 && g_last_tune_freq == 938512500L, name, "hold", "it yields to Hold");
    g_state.tg_hold = 0U;
    return rc;
}

/* A resident Type-D call that ends by sync loss (noCarrier() ends its canonical epoch, no DISC) ends the held call too:
 * the next, anonymous late-entry call on that repeater inherits neither its identity, its priority nor its dwell, so it
 * yields only to Hold, never to a preempting call that merely outranks the ended one. */
static int
nxdn_run_type_d_resident_sync_loss_case(void) {
    static const char* const name = "type-d-resident-sync-loss";
    const nxdn_case base = type_d_fixture(0U, 1400U);
    g_opts.use_rigctl = 1;
    g_opts.audio_in_type = AUDIO_IN_PULSE;
    g_rigctl_freq = base.expected_freq; /* the receiver sits on repeater 6 */
    policy_row(1400U, 10, 0);
    policy_row(1500U, 50, 1);
    int rc = 0;

    at_seconds(9300.0);
    voice_live();
    send_vcall(2200U, 1400U);
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1400U, name, "resident",
                      "the resident call is the held call");
    at_seconds(9301.0);
    (void)dsd_call_state_end_ex(&g_state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS);

    at_seconds(9302.0);
    voice_live();
    late_entry_call();
    nxdn_case other = base;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(9302.5);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 0 && g_state.nxdn_call.valid == 0U, name, "anonymous",
                      "an anonymous call after the sync loss is not preempted as the ended one");
    g_state.tg_hold = 1500U;
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 1 && g_last_tune_freq == 938512500L, name, "hold", "it yields to Hold");
    g_state.tg_hold = 0U;
    return rc;
}

/* Follow group 1400 on repeater 6 at @p t0, name it by a VCALL, and lose its sync 0.9 s later. */
static nxdn_case
followed_call_faded(double t0) {
    const nxdn_case held = type_d_fixture(0U, 1400U);
    policy_row(1400U, 10, 0);
    policy_row(1500U, 50, 1);
    at_seconds(t0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    nxdn_run_case_decode(&held);
    at_seconds(t0 + 0.2);
    voice_live();
    send_vcall(2200U, 1400U);
    at_seconds(t0 + 0.9);
    voice_live();
    (void)dsd_call_state_end_ex(&g_state, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_SYNC_LOSS);
    nxdn_case other = held;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    return other;
}

/* On the followed route a held call is held through its gaps: its call's sync-loss end keeps the identity within the
 * hangtime (an outranking preempting call takes the receiver from it), but a new anonymous call there is not the held
 * one: it yields only to Hold. */
static int
nxdn_run_type_d_followed_new_call_case(void) {
    static const char* const name = "type-d-followed-new-call";
    nxdn_case other = followed_call_faded(9400.0);
    int rc = nxdn_expect(g_tune_count == 1 && g_state.nxdn_follow.repeater == 6U, name, "follow", "call followed");
    at_seconds(9401.0);
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == 938512500L, name, "gap",
                      "the faded call is held within the hangtime: an outranking preempt takes it");

    other = followed_call_faded(9500.0);
    at_seconds(9501.5); /* past the canonical layer's reacquire gap (a resumed transmission is the faded call's) */
    late_entry_call();
    at_seconds(9501.6);
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 1 && g_state.nxdn_call.valid == 0U, name, "anonymous",
                      "a new anonymous call on the route is not the held one");
    g_state.tg_hold = 1500U;
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == 938512500L, name, "hold", "it yields to Hold");
    g_state.tg_hold = 0U;
    return rc;
}

/* A Type-D held call is described as Type-D admission judges it, with no source fallback: an INFO3 source whose own
 * row ranks high never lends that priority to the held, unlisted group, so a preempting call at the source's priority
 * takes the receiver. */
static int
nxdn_run_type_d_held_no_source_fallback_case(void) {
    static const char* const name = "type-d-held-no-source-fallback";
    const nxdn_case held = type_d_fixture(0U, 1400U);
    policy_row(1500U, 100, 1);
    int rc = 0;

    at_seconds(9100.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    nxdn_run_case_decode(&held);
    rc |=
        nxdn_expect(g_tune_count == 1 && g_state.nxdn_follow.repeater == 6U, name, "follow", "unlisted group followed");
    at_seconds(9100.2);
    voice_live();
    late_entry_call();
    at_seconds(9100.3);
    voice_live();
    nxdn_run_case_decode(&held); /* names the anonymous call */
    uint8_t message[32];
    at_seconds(9100.4);
    voice_live();
    build_scch_identity(message, 1U, 0U, 1500U); /* INFO3: source 1500 */
    NXDN_decode_scch(&g_opts, &g_state, message, 1U);
    at_seconds(9100.5);
    voice_live();
    nxdn_run_case_decode(&held);
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1400U
                          && g_state.nxdn_call.source == 1500U,
                      name, "source", "the held call carries the INFO3 source");

    nxdn_case other = held;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(9101.0);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == 938512500L, name, "preempt",
                      "a p100 preempt outranks the unlisted held group (no priority from its source)");
    return rc;
}

/* A busy message for the receiver's own repeater names a private call while the previous group call's canonical epoch
 * is still active: the canonical call takes the message's private kind, so a later INFO2 target message for that
 * destination keeps the held call private, never the stale group class (here a priority-100 group range over the
 * private destination), nor a restarted dwell. */
static int
nxdn_run_type_d_private_after_group_case(void) {
    static const char* const name = "type-d-private-after-group";
    const nxdn_case held = type_d_fixture(0U, 1400U);
    dsd_tg_policy_entry range;
    if (dsd_tg_policy_make_exact_entry(1400U, "A", "range", DSD_TG_POLICY_SOURCE_IMPORTED, &range) == 0) {
        range.id_end = 1499U;
        range.is_range = 1U;
        range.priority = 100;
        (void)dsd_tg_policy_add_range_entry(&g_state, &range);
    }
    policy_row(1500U, 50, 1);
    int rc = 0;

    at_seconds(9200.0);
    g_state.last_vc_sync_time = dsd_decode_time() - 4;
    nxdn_run_case_decode(&held);
    rc |= nxdn_expect(g_tune_count == 1, name, "follow", "group followed");
    at_seconds(9200.2);
    voice_live();
    const dsd_call_observation group = {
        .protocol = DSD_SYNC_NXDN_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1400U,
        .policy_target_id = 1400U,
    };
    (void)dsd_call_state_observe(&g_state, &group, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_update_media(&g_state, 0U, 1, 0.0);

    nxdn_case priv = held;
    priv.scch_gu = 1U;
    priv.scch_id = 1450U;
    at_seconds(9200.3);
    voice_live();
    nxdn_run_case_decode(&priv);
    dsd_call_snapshot call;
    rc |= nxdn_expect(dsd_call_state_get(&g_state, 0U, &call) == 1 && call.policy_target_id == 1450U
                          && call.kind == DSD_CALL_KIND_PRIVATE_VOICE,
                      name, "busy", "the canonical call takes the busy message's private kind");
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1450U
                          && g_state.nxdn_call.private_call == 1U,
                      name, "busy", "the held call is the private call");

    uint8_t message[32];
    at_seconds(9200.4);
    voice_live();
    build_scch_identity(message, 2U, 6U, 1450U); /* INFO2 target 1450 on the receiver's repeater */
    NXDN_decode_scch(&g_opts, &g_state, message, 1U);
    rc |= nxdn_expect(g_state.nxdn_call.valid == 1U && g_state.nxdn_call.target == 1450U
                          && g_state.nxdn_call.private_call == 1U,
                      name, "info2", "an INFO2 repeat keeps the held call private");

    nxdn_case other = held;
    other.scch_rep1 = 7U;
    other.scch_id = 1500U;
    at_seconds(9201.1);
    voice_live();
    nxdn_run_case_decode(&other);
    rc |= nxdn_expect(g_tune_count == 2 && g_last_tune_freq == 938512500L, name, "preempt",
                      "a p50 preempt outranks the private call (no group range priority, dwell kept)");
    return rc;
}

int
main(void) {
    int rc = 0;
    dsd_decode_clock_use_test(1000000000000ULL);
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
    rc |= nxdn_run_scch_termination_case();
    rc |= nxdn_run_scch_busy_enrich_case("tuned-there", NXDN_RX_TUNED_THERE, 1);
    rc |= nxdn_run_scch_busy_enrich_case("tuned-elsewhere", NXDN_RX_TUNED_ELSEWHERE, 0);
    rc |= nxdn_run_scch_busy_enrich_case("receiver-unknown", NXDN_RX_UNKNOWN, 0);
    rc |= nxdn_run_scch_busy_enrich_case("rigctl-there", NXDN_RX_RIGCTL_THERE, 1);
    rc |= nxdn_run_scch_busy_enrich_case("rigctl-query-fails", NXDN_RX_RIGCTL_FAILS, 0);
    rc |= nxdn_run_scch_identity_context_case();
    rc |= nxdn_run_refused_duplicate_keeps_tuned_case();
    rc |= nxdn_run_duplicate_after_hangtime_follows_case();
    rc |= nxdn_run_type_c_preemption_case();
    rc |= nxdn_run_follow_from_control_channel_is_no_takeover_case();
    rc |= nxdn_run_held_call_reconciled_case();
    rc |= nxdn_run_same_carrier_new_call_case();
    rc |= nxdn_run_vcall_names_followed_route_case();
    rc |= nxdn_run_type_d_takeover_case();
    rc |= nxdn_run_type_d_refollow_keeps_dwell_case();
    rc |= nxdn_run_type_d_private_reconcile_case();
    rc |= nxdn_run_type_d_resident_disc_case();
    rc |= nxdn_run_type_d_resident_sync_loss_case();
    rc |= nxdn_run_type_d_followed_new_call_case();
    rc |= nxdn_run_type_d_held_no_source_fallback_case();
    rc |= nxdn_run_type_d_private_after_group_case();
    rc |= nxdn_run_type_c_takeover_ends_left_call_case();
    rc |= nxdn_run_type_d_takeover_ends_left_call_case();
    rc |= nxdn_run_refollow_after_hangtime_return_case();
#ifdef NXDN_MATRIX_REAL_CALL_STATE
    rc |= nxdn_run_release_reply_keeps_dwell_case();
    rc |= nxdn_run_sync_loss_reply_keeps_dwell_case();
    rc |= nxdn_run_lapsed_call_same_carrier_duplicate_case();
    rc |= nxdn_run_in_place_follow_binds_call_case();
#endif

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_decode_clock_use_system();
    if (rc == 0) {
#ifdef NXDN_MATRIX_REAL_CALL_STATE
        printf("NXDN_GRANT_TUNE_MATRIX_REAL_CALL_STATE: OK\n");
#else
        printf("NXDN_GRANT_TUNE_MATRIX: OK\n");
#endif
    }
    dsd_state_ext_free_all(&g_state);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
