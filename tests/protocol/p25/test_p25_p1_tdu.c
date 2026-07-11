// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Focused checks for P25 Phase 1 TDU state cleanup and status-symbol handoff.
 */

#include <assert.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/timing.h"

static int g_read_zeros_calls;
static unsigned int g_read_zeros_length;
static int g_read_zeros_initial_status_count;
static int g_status_count_after_read = 35;
static int g_get_dibit_soft_calls;
static int g_status_symbol = 3;
static int g_status_ensure_calls;
static int g_status_add_calls;
static int g_last_status_add_value;
static int g_status_classify_calls;
static int g_sm_tdu_calls;

static int
float_close(float got, float want, float epsilon) {
    float delta = got - want;
    if (delta < 0.0F) {
        delta = -delta;
    }
    return delta <= epsilon;
}

void read_zeros(dsd_opts* opts, dsd_state* state, unsigned int length, int* status_count);

uint64_t
dsd_time_monotonic_ns(void) {
    return 1234500000000ULL;
}

void
read_zeros(dsd_opts* opts, dsd_state* state, unsigned int length, int* status_count) {
    (void)opts;
    (void)state;
    assert(status_count != NULL);
    ++g_read_zeros_calls;
    g_read_zeros_length = length;
    g_read_zeros_initial_status_count = *status_count;
    *status_count = g_status_count_after_read;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    assert(out_soft != NULL);
    ++g_get_dibit_soft_calls;
    out_soft->reliability = 255U;
    out_soft->llr[0] = 100;
    out_soft->llr[1] = -100;
    return g_status_symbol;
}

void
p25_status_accum_ensure_started(dsd_state* state) {
    (void)state;
    ++g_status_ensure_calls;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    ++g_status_add_calls;
    g_last_status_add_value = dibit_value;
}

void
p25_status_accum_classify(dsd_state* state, const dsd_opts* opts) {
    (void)state;
    (void)opts;
    ++g_status_classify_calls;
}

void
p25_sm_emit_tdu(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    ++g_sm_tdu_calls;
}

void
p25_crypto_reset_slot(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    state->p25_crypto_state[slot] = DSD_P25_CRYPTO_UNKNOWN;
    state->p25_p2_audio_allowed[slot] = 0;
    state->p25_p2_enc_lockout_muted[slot] = 0U;
}

static void
reset_harness(void) {
    g_read_zeros_calls = 0;
    g_read_zeros_length = 0;
    g_read_zeros_initial_status_count = 0;
    g_status_count_after_read = 35;
    g_get_dibit_soft_calls = 0;
    g_status_symbol = 3;
    g_status_ensure_calls = 0;
    g_status_add_calls = 0;
    g_last_status_add_value = -1;
    g_status_classify_calls = 0;
    g_sm_tdu_calls = 0;
}

static void
seed_active_call_state(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", "active slot 0");
    DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], "%s", "active slot 1");
    state->currentslot = 1;
    state->payload_miP = 0x112233445566ULL;
    state->payload_algid = 0x80;
    state->payload_keyid = 0x1234;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    state->p25_call_emergency[0] = 1;
    state->p25_call_priority[0] = 1;
    state->p25_call_is_packet[0] = 1;
}

static void
assert_common_tdu_state(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    assert(state->p25_p1_duid_tdu == 1U);
    assert(state->currentslot == 0);
    assert(g_status_ensure_calls == 1);
    assert(g_read_zeros_calls == 1);
    assert(g_read_zeros_length == 28U);
    assert(g_read_zeros_initial_status_count == 21);
    assert(g_get_dibit_soft_calls == 1);
    assert(g_status_add_calls == 1);
    assert(g_last_status_add_value == g_status_symbol);
    assert(g_status_classify_calls == 1);
    assert(g_sm_tdu_calls == 1);
    assert(strcmp(state->call_string[0], "                     ") == 0);
    assert(strcmp(state->call_string[1], "                     ") == 0);
    assert(state->p25_p1_last_tdu != (time_t)0);
    assert(state->p25_p1_last_tdu_m > 0.0);
    assert(state->payload_miP == 0);
    assert(state->payload_algid == 0);
    assert(state->payload_keyid == 0);
    assert(state->p25_crypto_state[0] == DSD_P25_CRYPTO_UNKNOWN);
    assert(state->p25_call_emergency[0] == 0);
    assert(state->p25_call_priority[0] == 0);
    assert(state->p25_call_is_packet[0] == 0);
}

static void
test_tdu_resets_call_crypto_state_and_restores_float_gain(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset_harness();
    seed_active_call_state(&opts, &state);
    opts.floating_point = 1;
    opts.audio_gain = 2.25F;
    state.aout_gain = -1.0F;

    processTDU(&opts, &state);

    assert_common_tdu_state(&opts, &state);
    assert(float_close(state.aout_gain, opts.audio_gain, 1e-6F));
}

static void
test_tdu_preserves_gain_when_integer_output_and_reports_sync_mismatch(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset_harness();
    seed_active_call_state(&opts, &state);
    g_status_count_after_read = 12;
    g_status_symbol = 1;
    opts.floating_point = 0;
    opts.audio_gain = 2.25F;
    state.aout_gain = 7.0F;

    processTDU(&opts, &state);

    assert_common_tdu_state(&opts, &state);
    assert(float_close(state.aout_gain, 7.0F, 1e-6F));
}

int
main(void) {
    test_tdu_resets_call_crypto_state_and_restores_float_gain();
    test_tdu_preserves_gain_when_integer_output_and_reports_sync_mismatch();
    printf("P25_P1_TDU: OK\n");
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
