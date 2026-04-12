// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify policy-backed NXDN VCALL assignment grant filtering.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void NXDN_decode_VCALL_ASSGN(dsd_opts* opts, dsd_state* state, uint8_t* Message);

static int g_tune_count = 0;
static long g_last_tune_freq = 0;

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

/*
 * Link stubs:
 * Pulling NXDN_decode_VCALL_ASSGN from nxdn_element.c requires auxiliary
 * symbols that are irrelevant to this focused grant-policy test.
 */
uint64_t
ConvertBitIntoBytes(uint8_t* bits, uint32_t n) {
    uint64_t v = 0ULL;
    for (uint32_t i = 0U; i < n; i++) {
        v = (v << 1U) | (uint64_t)(bits[i] & 1U);
    }
    return v;
}

uint64_t
convert_bits_into_output(uint8_t* input, int len) {
    if (input == NULL || len <= 0) {
        return 0ULL;
    }
    return ConvertBitIntoBytes(input, (uint32_t)len);
}

void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    if (input == NULL || output == NULL || len <= 0) {
        return;
    }
    memset(output, 0, (size_t)len * sizeof(uint8_t));
    for (int i = 0; i < len; i++) {
        output[i] = (uint8_t)((input[i / 8] >> (7 - (i % 8))) & 1U);
    }
}

void
nxdn_message_type(dsd_opts* opts, dsd_state* state, uint8_t MessageType) {
    (void)opts;
    (void)state;
    (void)MessageType;
}

uint32_t
nxdn_message_crc32(uint8_t* input, int len) {
    (void)input;
    (void)len;
    return 0U;
}

void
nxdn_alias_decode_arib(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
nxdn_alias_decode_prop(dsd_opts* opts, dsd_state* state, const uint8_t* message_bits, uint8_t crc_ok) {
    (void)opts;
    (void)state;
    (void)message_bits;
    (void)crc_ok;
}

void
nxdn_alias_reset(dsd_state* state) {
    (void)state;
}

long int
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    (void)opts;
    if (!state || channel >= (uint16_t)(sizeof(state->trunk_chan_map) / sizeof(state->trunk_chan_map[0]))) {
        return 0;
    }
    return state->trunk_chan_map[channel];
}

long int
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    if (!state || channel >= (uint16_t)(sizeof(state->trunk_chan_map) / sizeof(state->trunk_chan_map[0]))) {
        return 0;
    }
    return state->trunk_chan_map[channel];
}

void
nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
}

uint8_t
nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    return 0U;
}

void
nxdn_trunk_diag_log_missing_channel_once(const dsd_opts* opts, dsd_state* state, uint16_t channel,
                                         const char* context) {
    (void)opts;
    (void)state;
    (void)channel;
    (void)context;
}

void
watchdog_event_current(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)data_string;
    (void)slot;
}

void
LFSR128n(dsd_state* state) {
    (void)state;
}

void
des_multi_keystream_output(unsigned long long int mi, unsigned long long int key_ulli, uint8_t* output, int type,
                           int len) {
    (void)mi;
    (void)key_ulli;
    (void)output;
    (void)type;
    (void)len;
}

void
aes_ofb_keystream_output(uint8_t* iv, uint8_t* key, uint8_t* output, int type, int nblocks) {
    (void)iv;
    (void)key;
    (void)output;
    (void)type;
    (void)nblocks;
}

long int
dsd_rigctl_query_hook_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return 0;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 0ULL;
}

static void
hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)ted_sps;
    g_tune_count++;
    g_last_tune_freq = freq;
    if (opts) {
        opts->p25_is_tuned = 1;
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    }
}

static void
hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
}

static void
hook_return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
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
build_vcall_assgn(uint8_t* message_bits, uint8_t message_type, uint8_t call_type, uint16_t source, uint16_t target,
                  uint16_t channel) {
    memset(message_bits, 0, 96);
    set_message_type(message_bits, message_type);
    write_bits_u32(message_bits, 16U, call_type & 0x7U, 3U);
    write_bits_u32(message_bits, 24U, source, 16U);
    write_bits_u32(message_bits, 40U, target, 16U);
    write_bits_u32(message_bits, 62U, channel & 0x03FFU, 10U);
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_upsert_legacy_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

static void
reset_tune_state(dsd_opts* opts, dsd_state* st) {
    g_tune_count = 0;
    g_last_tune_freq = 0;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    st->trunk_vc_freq[0] = st->trunk_vc_freq[1] = 0;
}

int
main(void) {
    int rc = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    uint8_t msg[96];
    const uint16_t chan = 16U;
    const long freq = 936012500L;

    if (!opts || !st) {
        fprintf(stderr, "FAIL: alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !st ? " dsd_state" : "");
        free(st);
        free(opts);
        return 1;
    }

    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->trunk_use_allow_list = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    st->p25_cc_freq = 935000000L;
    st->trunk_cc_freq = st->p25_cc_freq;
    st->trunk_chan_map[chan] = freq;

    dsd_trunk_tuning_hooks hooks = {
        .tune_to_freq = hook_tune_to_freq,
        .tune_to_cc = hook_tune_to_cc,
        .return_to_cc = hook_return_to_cc,
    };
    dsd_trunk_tuning_hooks_set(hooks);

    build_vcall_assgn(msg, 0x04U, 0U, 2100U, 1100U, chan);
    reset_tune_state(opts, st);
    NXDN_decode_VCALL_ASSGN(opts, st, msg);
    rc |= expect_true("group unknown blocked in allow-list", g_tune_count == 0);

    rc |= expect_true("seed group allow", seed_exact(st, 1100U, "A", "ALLOW-GRP") == 0);
    reset_tune_state(opts, st);
    NXDN_decode_VCALL_ASSGN(opts, st, msg);
    rc |= expect_true("group known allowed", g_tune_count == 1 && g_last_tune_freq == freq);

    build_vcall_assgn(msg, 0x04U, 4U, 9002U, 9001U, chan);
    reset_tune_state(opts, st);
    NXDN_decode_VCALL_ASSGN(opts, st, msg);
    rc |= expect_true("private unknown blocked in allow-list", g_tune_count == 0);

    rc |= expect_true("seed private allow source", seed_exact(st, 9002U, "A", "ALLOW-SRC") == 0);
    reset_tune_state(opts, st);
    NXDN_decode_VCALL_ASSGN(opts, st, msg);
    rc |= expect_true("private known source allowed", g_tune_count == 1 && g_last_tune_freq == freq);

    build_vcall_assgn(msg, 0x04U, 0U, 7200U, 7100U, chan);
    rc |= expect_true("seed source fallback allow", seed_exact(st, 7200U, "A", "ALLOW-SRC-GRP") == 0);
    reset_tune_state(opts, st);
    NXDN_decode_VCALL_ASSGN(opts, st, msg);
    rc |= expect_true("group source fallback allowed", g_tune_count == 1 && g_last_tune_freq == freq);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    if (rc == 0) {
        printf("NXDN_GRANT_POLICY: OK\n");
    }
    free(st);
    free(opts);
    return rc;
}
