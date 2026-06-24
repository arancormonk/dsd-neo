// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * EDACS/ProVoice grant tune matrix.
 *
 * Uses the DSD_NEO_TEST_HOOKS valid-frame shim to drive the real EDACS grant
 * dispatcher with already-decoded 28-bit message words. Grant cases are
 * intentionally digital so the shared tune path is exercised without entering
 * the analog audio loop.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void dsd_neo_edacs_test_process_valid_frame(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                            unsigned long long int msg_2);
const char* dsd_neo_edacs_test_lcn_status_string(int lcn);
short dsd_neo_edacs_test_apply_input_volume(int multiplier, short sample);
unsigned long long int dsd_neo_edacs_test_vote_frames(unsigned long long int fr_1_4, unsigned long long int fr_2_5,
                                                      unsigned long long int fr_3_6);
int dsd_neo_edacs_test_update_squelch_count(double pwr, double sql, int count);
int dsd_neo_edacs_test_should_release_voice(unsigned long long int sr, int sql_disabled, time_t start_time,
                                            double no_sql_watchdog_s);
void dsd_neo_edacs_test_update_lcn_count(dsd_state* state, int lcn);
void dsd_neo_edacs_test_build_raw_frames(const int edacs_bit[241], unsigned long long int* fr_1,
                                         unsigned long long int* fr_2, unsigned long long int* fr_3,
                                         unsigned long long int* fr_4, unsigned long long int* fr_5,
                                         unsigned long long int* fr_6);
unsigned long long int dsd_neo_edacs_test_build_symbol_register(const dsd_opts* opts, dsd_state* state,
                                                                const short analog1[960]);
void dsd_neo_edacs_test_reset_digitize_overflow(dsd_state* state);
int dsd_neo_edacs_test_collect_analog_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2,
                                              short* analog3, double* pwr);
void dsd_neo_edacs_test_emit_analog_audio(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                                          const short* analog3);
int dsd_neo_edacs_test_static_wav_downsample(const short* src, short* out, size_t out_count);
double dsd_neo_edacs_test_no_sql_watchdog_window(double trunk_hangtime);

typedef struct {
    const char* name;
    unsigned long long int msg_1;
    unsigned long long int msg_2;
    long freq_hz;
    int ea_mode;
    int lcn;
    int expected_flags;
    int expected_lasttg;
    int expected_lastsrc;
} edacs_grant_case;

typedef enum {
    EDACS_GUARD_GROUP_DISABLED = 0,
    EDACS_GUARD_PRIVATE_DISABLED,
    EDACS_GUARD_ALLOWLIST_BLOCK,
    EDACS_GUARD_MISSING_FREQUENCY,
    EDACS_GUARD_MISSING_CC_LCN,
    EDACS_GUARD_P25_TRUNK_DISABLED,
} edacs_no_tune_guard;

static dsd_opts g_opts;
static dsd_state g_state;
static dsd_trunk_tune_result g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
static dsd_trunk_tune_result g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_vc_tune_count = 0;
static int g_cc_tune_count = 0;
static int g_skip_dibit_count = 0;
static long g_last_vc_freq = 0;
static long g_last_cc_freq = 0;
static long g_rigctl_current_freq = 0;
static int g_tcp_read_count = 0;
static int g_tcp_close_count = 0;
static int g_tcp_fail_at = -1;
static int g_udp_read_count = 0;
static int g_udp_fail_every = 0;
static int g_udp_blast_count = 0;
static size_t g_udp_blast_bytes[3];
static short g_udp_blast_first[3];
#ifdef USE_RADIO
static int g_rtl_read_count = 0;
static int g_rtl_return_pwr_count = 0;
static int g_rtl_fail_at = -1;
#endif

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    g_skip_dibit_count += count;
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

static dsd_trunk_tune_result
edacs_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)ted_sps;
    g_vc_tune_count++;
    g_last_vc_freq = freq;
    if (dsd_trunk_tune_result_is_ok(g_vc_result)) {
        if (opts) {
            opts->p25_is_tuned = 1;
            opts->trunk_is_tuned = 1;
        }
        if (state) {
            state->p25_vc_freq[0] = freq;
            state->p25_vc_freq[1] = freq;
            state->trunk_vc_freq[0] = freq;
            state->trunk_vc_freq[1] = freq;
        }
    }
    return g_vc_result;
}

static dsd_trunk_tune_result
edacs_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)ted_sps;
    g_cc_tune_count++;
    g_last_cc_freq = freq;
    if (dsd_trunk_tune_result_is_ok(g_cc_result) && state) {
        state->trunk_cc_freq = freq;
    }
    return g_cc_result;
}

static long int
edacs_hook_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return g_rigctl_current_freq;
}

static void
edacs_install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_result = edacs_hook_tune_to_freq;
    hooks.tune_to_cc_result = edacs_hook_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){
        .get_current_freq_hz = edacs_hook_get_current_freq_hz,
    });
}

static int
edacs_fake_tcp_read_sample(tcp_input_ctx* ctx, int16_t* out) {
    (void)ctx;
    const int index = g_tcp_read_count++;
    if (out == NULL || (g_tcp_fail_at >= 0 && index == g_tcp_fail_at)) {
        return 0;
    }
    *out = (int16_t)(1000 + index);
    return 1;
}

static void
edacs_fake_tcp_close(tcp_input_ctx* ctx) {
    (void)ctx;
    g_tcp_close_count++;
}

static int
edacs_fake_udp_read_sample(dsd_opts* opts, int16_t* out) {
    (void)opts;
    const int index = g_udp_read_count++;
    if (out == NULL || (g_udp_fail_every > 0 && (index % g_udp_fail_every) == 0)) {
        return 0;
    }
    *out = (int16_t)(2000 + index);
    return 1;
}

static void
edacs_install_net_audio_hooks(void) {
    dsd_net_audio_input_hooks hooks = {0};
    hooks.tcp_read_sample = edacs_fake_tcp_read_sample;
    hooks.tcp_close = edacs_fake_tcp_close;
    hooks.udp_read_sample = edacs_fake_udp_read_sample;
    dsd_net_audio_input_hooks_set(hooks);
}

static void
edacs_fake_blast_analog(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    if (g_udp_blast_count < 3) {
        g_udp_blast_bytes[g_udp_blast_count] = nsam;
        g_udp_blast_first[g_udp_blast_count] = data != NULL ? ((const short*)data)[0] : 0;
    }
    g_udp_blast_count++;
}

static void
edacs_install_udp_output_hooks(void) {
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){
        .blast_analog = edacs_fake_blast_analog,
    });
}

#ifdef USE_RADIO
static int
edacs_fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    const int index = g_rtl_read_count++;
    if (out_got != NULL) {
        *out_got = 0;
    }
    if (out == NULL || out_got == NULL || count != 1U || (g_rtl_fail_at >= 0 && index == g_rtl_fail_at)) {
        return -1;
    }
    out[0] = 100.0f + (float)index;
    *out_got = 1;
    return 0;
}

static double
edacs_fake_rtl_return_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    g_rtl_return_pwr_count++;
    return 77.25;
}

static void
edacs_install_rtl_stream_hooks(void) {
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = edacs_fake_rtl_read,
        .return_pwr = edacs_fake_rtl_return_pwr,
    });
}
#endif

static void
edacs_reset_audio_hook_state(void) {
    g_tcp_read_count = 0;
    g_tcp_close_count = 0;
    g_tcp_fail_at = -1;
    g_udp_read_count = 0;
    g_udp_fail_every = 0;
    g_udp_blast_count = 0;
    DSD_MEMSET(g_udp_blast_bytes, 0, sizeof(g_udp_blast_bytes));
    DSD_MEMSET(g_udp_blast_first, 0, sizeof(g_udp_blast_first));
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
#ifdef USE_RADIO
    g_rtl_read_count = 0;
    g_rtl_return_pwr_count = 0;
    g_rtl_fail_at = -1;
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
#endif
    dsd_exitflag_store(0);
}

static int
edacs_expect(int cond, const char* test_case, const char* result_name, const char* check) {
    if (cond) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL case=%s result=%s check=%s\n", test_case, result_name, check);
    return 1;
}

static unsigned long long int
edacs_standard_group_msg1(int mt_a, int lcn, int group, int source_lid) {
    unsigned long long int msg = ((unsigned long long int)(mt_a & 0x7) << 25U);
    msg |= ((unsigned long long int)(source_lid & 0x3F80) << 11U);
    msg |= ((unsigned long long int)(lcn & 0x1F) << 12U);
    msg |= (1ULL << 11U);
    msg |= (unsigned long long int)(group & 0x7FF);
    return msg;
}

static unsigned long long int
edacs_standard_group_msg2(int source_lid) {
    return (unsigned long long int)(source_lid & 0x7F) << 17U;
}

static unsigned long long int
edacs_standard_individual_msg1(int lcn, int target, int is_digital) {
    unsigned long long int msg = (7ULL << 25U) | (5ULL << 22U);
    msg |= 1ULL << 21U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_digital ? 1U : 0U) << 14U;
    msg |= (unsigned long long int)(target & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_extended_group_msg1(int mt1, int lcn, int group) {
    unsigned long long int msg = (unsigned long long int)(mt1 & 0x1F) << 23U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 17U;
    msg |= (unsigned long long int)(group & 0xFFFF);
    return msg;
}

static unsigned long long int
edacs_extended_group_update_msg1(int mt1, int lcn, int group, int is_update) {
    unsigned long long int msg = edacs_extended_group_msg1(mt1, lcn, group);
    msg |= (unsigned long long int)(is_update ? 1U : 0U) << 16U;
    return msg;
}

static unsigned long long int
edacs_extended_group_msg2(int source, int is_emergency, int is_tx_trunking) {
    unsigned long long int msg = (unsigned long long int)(source & 0xFFFFF);
    msg |= (unsigned long long int)(is_emergency ? 1U : 0U) << 20U;
    msg |= (unsigned long long int)(is_tx_trunking ? 1U : 0U) << 21U;
    return msg;
}

static unsigned long long int
edacs_extended_icall_msg1(int target) {
    unsigned long long int msg = 0x10ULL << 23U;
    msg |= 1ULL << 21U;
    msg |= (unsigned long long int)(target & 0xFFFFF);
    return msg;
}

static unsigned long long int
edacs_extended_icall_msg2(int lcn, int source) {
    unsigned long long int msg = (unsigned long long int)(lcn & 0x1F) << 20U;
    msg |= (unsigned long long int)(source & 0xFFFFF);
    return msg;
}

static unsigned long long int
edacs_standard_data_msg1(int is_individual_call, int lcn, int target, int is_individual_id) {
    unsigned long long int msg = 5ULL << 25U;
    msg |= (unsigned long long int)(is_individual_call ? 1U : 0U) << 24U;
    msg |= 1ULL << 23U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_individual_id ? 1U : 0U) << 14U;
    msg |= (unsigned long long int)(target & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_standard_data_msg2(int port) {
    return (unsigned long long int)(port & 0x7) << 20U;
}

static unsigned long long int
edacs_standard_interconnect_msg1(int mt_c, int lcn, int target, int is_individual_id) {
    unsigned long long int msg = (7ULL << 25U) | (1ULL << 22U);
    msg |= (unsigned long long int)(mt_c & 0x3) << 20U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_individual_id ? 1U : 0U) << 14U;
    msg |= (unsigned long long int)(target & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_standard_channel_update_msg1(int mt_c, int lcn, int group, int is_emergency) {
    unsigned long long int msg = (7ULL << 25U) | (3ULL << 22U);
    msg |= (unsigned long long int)(mt_c & 0x3) << 20U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_emergency ? 1U : 0U) << 13U;
    msg |= (unsigned long long int)(group & 0x7FF);
    return msg;
}

static unsigned long long int
edacs_standard_channel_update_individual_msg1(int mt_c, int lcn, int lid) {
    unsigned long long int msg = (7ULL << 25U) | (3ULL << 22U);
    msg |= (unsigned long long int)(mt_c & 0x3) << 20U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= 1ULL << 14U;
    msg |= (unsigned long long int)(lid & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_standard_mt_d_msg1(int mt_d) {
    return (7ULL << 25U) | (7ULL << 22U) | ((unsigned long long int)(mt_d & 0x1F) << 17U);
}

static unsigned long long int
edacs_standard_site_id_msg1(int cc_lcn, int priority, int site_id, int is_auxiliary) {
    unsigned long long int msg = edacs_standard_mt_d_msg1(0x08);
    msg |= (unsigned long long int)(cc_lcn & 0x1F) << 12U;
    msg |= (unsigned long long int)(priority & 0x7) << 9U;
    msg |= (unsigned long long int)(is_auxiliary ? 1U : 0U) << 5U;
    msg |= (unsigned long long int)(site_id & 0x1F);
    return msg;
}

static unsigned long long int
edacs_standard_all_call_msg1(int lcn, int is_digital, int lid) {
    unsigned long long int msg = edacs_standard_mt_d_msg1(0x0F);
    msg |= (unsigned long long int)(lcn & 0x1F) << 12U;
    msg |= (unsigned long long int)(is_digital ? 1U : 0U) << 11U;
    msg |= 1ULL << 9U;
    msg |= (unsigned long long int)(lid & 0x7F);
    return msg;
}

static unsigned long long int
edacs_standard_all_call_msg2(int lid) {
    return (unsigned long long int)((lid >> 7U) & 0x7F) << 1U;
}

static unsigned long long int
edacs_extended_mt2_msg1(int mt2) {
    return (0x1FULL << 23U) | ((unsigned long long int)(mt2 & 0xF) << 19U);
}

static unsigned long long int
edacs_extended_system_info_msg1(int system) {
    return edacs_extended_mt2_msg1(0x8) | (unsigned long long int)(system & 0xFFFF);
}

static unsigned long long int
edacs_extended_site_id_msg1(int site_id, int area) {
    unsigned long long int msg = edacs_extended_mt2_msg1(0xA);
    msg |= (unsigned long long int)(site_id & 0xE0) << 7U;
    msg |= (unsigned long long int)(area & 0x7F) << 5U;
    msg |= (unsigned long long int)(site_id & 0x1F);
    return msg;
}

static unsigned long long int
edacs_extended_test_call_msg1(int cc_lcn, int wc_lcn) {
    unsigned long long int msg = edacs_extended_mt2_msg1(0x0);
    msg |= (unsigned long long int)(cc_lcn & 0x1F) << 13U;
    msg |= (unsigned long long int)(wc_lcn & 0x1F) << 7U;
    return msg;
}

static unsigned long long int
edacs_extended_channel_assignment_msg2(int lcn, int source) {
    unsigned long long int msg = (unsigned long long int)(lcn & 0x1F) << 20U;
    msg |= (unsigned long long int)(source & 0xFFFFF);
    return msg;
}

static unsigned long long int
edacs_extended_all_call_msg1(int lcn, int is_digital, int is_update) {
    unsigned long long int msg = 0x16ULL << 23U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 17U;
    msg |= (unsigned long long int)(is_digital ? 1U : 0U) << 16U;
    msg |= (unsigned long long int)(is_update ? 1U : 0U) << 15U;
    return msg;
}

static void
edacs_setup_fixture(const edacs_grant_case* test_case) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));

    g_opts.p25_trunk = 1;
    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_private_calls = 1;
    g_opts.trunk_hangtime = 1.0f;
    g_state.ea_mode = test_case->ea_mode;
    g_state.edacs_cc_lcn = 1;
    g_state.edacs_tuned_lcn = -1;
    g_state.trunk_lcn_freq[0] = 851012500L;
    g_state.p25_cc_freq = g_state.trunk_lcn_freq[0];
    g_state.trunk_cc_freq = g_state.trunk_lcn_freq[0];
    g_state.trunk_lcn_freq[test_case->lcn - 1] = test_case->freq_hz;

    g_vc_tune_count = 0;
    g_cc_tune_count = 0;
    g_skip_dibit_count = 0;
    g_last_vc_freq = 0;
    g_last_cc_freq = 0;
    g_rigctl_current_freq = 0;
}

static void
edacs_setup_state_fixture(int ea_mode) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));

    g_opts.p25_trunk = 1;
    g_opts.trunk_enable = 1;
    g_state.ea_mode = ea_mode;
    g_state.edacs_cc_lcn = 1;
    g_state.edacs_tuned_lcn = -1;
    g_state.trunk_lcn_freq[0] = 851012500L;
    g_state.trunk_lcn_freq[3] = 852762500L;
    g_state.trunk_lcn_freq[8] = 854012500L;
    g_state.p25_cc_freq = g_state.trunk_lcn_freq[0];
    g_state.trunk_cc_freq = g_state.trunk_lcn_freq[0];

    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_vc_tune_count = 0;
    g_cc_tune_count = 0;
    g_skip_dibit_count = 0;
    g_last_vc_freq = 0;
    g_last_cc_freq = 0;
    g_rigctl_current_freq = 0;
    edacs_install_hooks();
}

static int
edacs_run_grant_result_case(const edacs_grant_case* test_case, dsd_trunk_tune_result result, const char* result_name) {
    g_vc_result = result;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_install_hooks();

    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    const int accepted = dsd_trunk_tune_result_is_ok(result);
    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 1, test_case->name, result_name, "voice tune attempted once");
    rc |= edacs_expect(g_last_vc_freq == test_case->freq_hz, test_case->name, result_name,
                       "voice tune frequency matches LCN map");
    rc |= edacs_expect(g_state.edacs_vc_lcn == test_case->lcn, test_case->name, result_name, "grant tracked VC LCN");
    rc |= edacs_expect(g_state.lasttg == test_case->expected_lasttg && g_state.lastsrc == test_case->expected_lastsrc,
                       test_case->name, result_name, "grant tracked target/source");
    rc |= edacs_expect((g_state.edacs_vc_call_type & test_case->expected_flags) == test_case->expected_flags,
                       test_case->name, result_name, "grant call flags");

    if (accepted) {
        rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, result_name,
                           "accepted tune set tuned LCN");
        rc |= edacs_expect(g_opts.p25_is_tuned == 1 && g_opts.trunk_is_tuned == 1, test_case->name, result_name,
                           "accepted tune set tuned flags");
        rc |=
            edacs_expect(g_state.trunk_vc_freq[0] == test_case->freq_hz && g_state.p25_vc_freq[0] == test_case->freq_hz,
                         test_case->name, result_name, "accepted tune set VC frequencies");
    } else {
        rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, result_name,
                           "rejected tune left tuned LCN clear");
        rc |= edacs_expect(g_opts.p25_is_tuned == 0 && g_opts.trunk_is_tuned == 0, test_case->name, result_name,
                           "rejected tune left tuned flags clear");
        rc |= edacs_expect(g_state.trunk_vc_freq[0] == 0 && g_state.p25_vc_freq[0] == 0, test_case->name, result_name,
                           "rejected tune left VC frequencies clear");
    }
    return rc;
}

static void
edacs_apply_no_tune_guard(const edacs_grant_case* test_case, edacs_no_tune_guard guard) {
    switch (guard) {
        case EDACS_GUARD_GROUP_DISABLED: g_opts.trunk_tune_group_calls = 0; break;
        case EDACS_GUARD_PRIVATE_DISABLED: g_opts.trunk_tune_private_calls = 0; break;
        case EDACS_GUARD_ALLOWLIST_BLOCK: g_opts.trunk_use_allow_list = 1; break;
        case EDACS_GUARD_MISSING_FREQUENCY: g_state.trunk_lcn_freq[test_case->lcn - 1] = 0; break;
        case EDACS_GUARD_MISSING_CC_LCN: g_state.edacs_cc_lcn = 0; break;
        case EDACS_GUARD_P25_TRUNK_DISABLED: g_opts.p25_trunk = 0; break;
    }
}

static int
edacs_run_no_tune_guard_case(const edacs_grant_case* test_case, edacs_no_tune_guard guard, const char* guard_name) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_apply_no_tune_guard(test_case, guard);
    edacs_install_hooks();

    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 0, test_case->name, guard_name, "guard did not attempt tune");
    rc |= edacs_expect(g_last_vc_freq == 0, test_case->name, guard_name, "guard left tune frequency clear");
    rc |= edacs_expect(g_state.edacs_vc_lcn == test_case->lcn, test_case->name, guard_name,
                       "guard still tracked parsed VC LCN");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, guard_name, "guard left tuned LCN clear");
    rc |= edacs_expect(g_opts.p25_is_tuned == 0 && g_opts.trunk_is_tuned == 0, test_case->name, guard_name,
                       "guard left tuned flags clear");
    rc |= edacs_expect(g_state.trunk_vc_freq[0] == 0 && g_state.p25_vc_freq[0] == 0, test_case->name, guard_name,
                       "guard left VC frequencies clear");
    rc |= edacs_expect(g_state.lasttg == test_case->expected_lasttg && g_state.lastsrc == test_case->expected_lastsrc,
                       test_case->name, guard_name, "guard preserved parsed target/source");
    rc |= edacs_expect((g_state.edacs_vc_call_type & test_case->expected_flags) == test_case->expected_flags,
                       test_case->name, guard_name, "guard preserved parsed call flags");
    return rc;
}

static int
edacs_run_retry_after_reject_case(const edacs_grant_case* test_case, dsd_trunk_tune_result first_result,
                                  const char* result_name) {
    g_vc_result = first_result;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_install_hooks();

    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 1, test_case->name, result_name, "rejected tune attempted once");
    rc |=
        edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, result_name, "rejected tune left tuned LCN clear");
    rc |= edacs_expect(g_opts.p25_is_tuned == 0 && g_opts.trunk_is_tuned == 0, test_case->name, result_name,
                       "rejected tune left tuned flags clear");

    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    rc |= edacs_expect(g_vc_tune_count == 2, test_case->name, result_name, "later grant retried tune");
    rc |= edacs_expect(g_last_vc_freq == test_case->freq_hz, test_case->name, result_name,
                       "retried tune frequency matches LCN map");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, result_name,
                       "retried tune set tuned LCN");
    rc |= edacs_expect(g_opts.p25_is_tuned == 1 && g_opts.trunk_is_tuned == 1, test_case->name, result_name,
                       "retried tune set tuned flags");
    rc |= edacs_expect(g_state.trunk_vc_freq[0] == test_case->freq_hz && g_state.p25_vc_freq[0] == test_case->freq_hz,
                       test_case->name, result_name, "retried tune set VC frequencies");
    return rc;
}

static void
edacs_setup_eot_fixture(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));

    g_opts.p25_trunk = 1;
    g_opts.trunk_enable = 1;
    g_opts.p25_is_tuned = 1;
    g_opts.trunk_is_tuned = 1;
    g_state.p25_cc_freq = 851012500L;
    g_state.trunk_cc_freq = 851012500L;
    g_state.p25_vc_freq[0] = 852012500L;
    g_state.p25_vc_freq[1] = 852012500L;
    g_state.trunk_vc_freq[0] = 852012500L;
    g_state.trunk_vc_freq[1] = 852012500L;
    g_state.edacs_tuned_lcn = 5;
    g_state.lasttg = 1201;
    g_state.lastsrc = 42001;
    g_state.payload_algid = 0x84;
    g_state.payload_keyid = 0x1234;
    g_state.payload_miP = 0x5678;
    DSD_SNPRINTF(g_state.call_string[0], sizeof(g_state.call_string[0]), "%s", "edacs active");
    DSD_SNPRINTF(g_state.active_channel[0], sizeof(g_state.active_channel[0]), "%s", "active");

    g_vc_tune_count = 0;
    g_cc_tune_count = 0;
    g_skip_dibit_count = 0;
    g_last_vc_freq = 0;
    g_last_cc_freq = 0;
}

static int
edacs_run_eot_result_case(dsd_trunk_tune_result result, const char* result_name) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = result;
    edacs_setup_eot_fixture();
    edacs_install_hooks();

    eot_cc(&g_opts, &g_state);

    const int accepted = dsd_trunk_tune_result_is_ok(result);
    int rc = 0;
    rc |= edacs_expect(g_cc_tune_count == 1, "eot-cc", result_name, "CC tune attempted once");
    rc |= edacs_expect(g_last_cc_freq == 851012500L, "eot-cc", result_name, "CC tune frequency");
    rc |= edacs_expect(g_skip_dibit_count == (240 * 8), "eot-cc", result_name, "EOT dibit skip was bounded");

    if (accepted) {
        rc |= edacs_expect(g_opts.p25_is_tuned == 0 && g_opts.trunk_is_tuned == 0, "eot-cc", result_name,
                           "accepted CC return cleared tuned flags");
        rc |=
            edacs_expect(g_state.edacs_tuned_lcn == -1, "eot-cc", result_name, "accepted CC return cleared tuned LCN");
        rc |= edacs_expect(g_state.p25_vc_freq[0] == 0 && g_state.trunk_vc_freq[0] == 0, "eot-cc", result_name,
                           "accepted CC return cleared VC frequencies");
        rc |= edacs_expect(g_state.lasttg == 0 && g_state.lastsrc == 0, "eot-cc", result_name,
                           "accepted CC return cleared call ids");
        rc |= edacs_expect(g_state.payload_algid == 0 && g_state.payload_keyid == 0 && g_state.payload_miP == 0,
                           "eot-cc", result_name, "accepted CC return cleared payload metadata");
        rc |= edacs_expect(g_state.active_channel[0][0] == '\0', "eot-cc", result_name,
                           "accepted CC return cleared active display");
    } else {
        rc |= edacs_expect(g_opts.p25_is_tuned == 1 && g_opts.trunk_is_tuned == 1, "eot-cc", result_name,
                           "rejected CC return preserved tuned flags");
        rc |=
            edacs_expect(g_state.edacs_tuned_lcn == 5, "eot-cc", result_name, "rejected CC return preserved tuned LCN");
        rc |= edacs_expect(g_state.p25_vc_freq[0] == 852012500L && g_state.trunk_vc_freq[0] == 852012500L, "eot-cc",
                           result_name, "rejected CC return preserved VC frequencies");
        rc |= edacs_expect(g_state.lasttg == 1201 && g_state.lastsrc == 42001, "eot-cc", result_name,
                           "rejected CC return preserved call ids");
        rc |= edacs_expect(g_state.payload_algid == 0x84 && g_state.payload_keyid == 0x1234
                               && g_state.payload_miP == 0x5678,
                           "eot-cc", result_name, "rejected CC return preserved payload metadata");
    }
    return rc;
}

static int
edacs_run_eot_retry_after_reject_case(dsd_trunk_tune_result first_result, const char* result_name) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = first_result;
    edacs_setup_eot_fixture();
    edacs_install_hooks();

    eot_cc(&g_opts, &g_state);

    int rc = 0;
    rc |= edacs_expect(g_cc_tune_count == 1, "eot-retry", result_name, "rejected CC tune attempted once");
    rc |= edacs_expect(g_opts.p25_is_tuned == 1 && g_opts.trunk_is_tuned == 1, "eot-retry", result_name,
                       "rejected CC tune preserved tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == 5, "eot-retry", result_name, "rejected CC tune preserved tuned LCN");

    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    eot_cc(&g_opts, &g_state);

    rc |= edacs_expect(g_cc_tune_count == 2, "eot-retry", result_name, "later EOT retried CC tune");
    rc |= edacs_expect(g_opts.p25_is_tuned == 0 && g_opts.trunk_is_tuned == 0, "eot-retry", result_name,
                       "retried CC tune cleared tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, "eot-retry", result_name, "retried CC tune cleared tuned LCN");
    rc |= edacs_expect(g_state.p25_vc_freq[0] == 0 && g_state.trunk_vc_freq[0] == 0, "eot-retry", result_name,
                       "retried CC tune cleared VC frequencies");
    return rc;
}

static int
edacs_run_retune_after_eot_case(const edacs_grant_case* test_case) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_install_hooks();

    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 1, test_case->name, "retune-after-eot", "initial grant tuned once");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, "retune-after-eot",
                       "initial grant set tuned LCN");

    eot_cc(&g_opts, &g_state);
    rc |= edacs_expect(g_cc_tune_count == 1, test_case->name, "retune-after-eot", "EOT returned to CC once");
    rc |= edacs_expect(g_opts.p25_is_tuned == 0 && g_opts.trunk_is_tuned == 0, test_case->name, "retune-after-eot",
                       "EOT cleared tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, "retune-after-eot", "EOT cleared tuned LCN");

    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);
    rc |= edacs_expect(g_vc_tune_count == 2, test_case->name, "retune-after-eot", "post-EOT grant retuned");
    rc |= edacs_expect(g_last_vc_freq == test_case->freq_hz, test_case->name, "retune-after-eot",
                       "post-EOT tune frequency matches LCN map");
    rc |= edacs_expect(g_opts.p25_is_tuned == 1 && g_opts.trunk_is_tuned == 1, test_case->name, "retune-after-eot",
                       "post-EOT grant set tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, "retune-after-eot",
                       "post-EOT grant set tuned LCN");
    return rc;
}

static int
edacs_run_standard_state_cases(void) {
    int rc = 0;

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_data_msg1(0, 4, 777, 0),
                                           edacs_standard_data_msg2(5));
    rc |= edacs_expect(g_state.edacs_vc_lcn == 4, "standard-data-group", "state", "tracked data LCN");
    rc |= edacs_expect(g_state.lasttg == 777 && g_state.lastsrc == 0x800, "standard-data-group", "state",
                       "tracked data target/source");
    rc |= edacs_expect(g_state.edacs_vc_call_type == EDACS_IS_GROUP, "standard-data-group", "state",
                       "tracked group data call type");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_data_msg1(1, 9, 12345, 1),
                                           edacs_standard_data_msg2(3));
    rc |= edacs_expect(g_state.edacs_vc_lcn == 9, "standard-data-individual", "state", "tracked data LCN");
    rc |= edacs_expect(g_state.lasttg == 12345 && g_state.lastsrc == 0x800, "standard-data-individual", "state",
                       "tracked individual data target/source");
    rc |= edacs_expect(g_state.edacs_vc_call_type == EDACS_IS_INDIVIDUAL, "standard-data-individual", "state",
                       "tracked individual data call type");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_interconnect_msg1(3, 4, 3210, 1), 0);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 4, "standard-interconnect", "state", "tracked interconnect LCN");
    rc |= edacs_expect(g_state.lasttg == 0 && g_state.lastsrc == 3210, "standard-interconnect", "state",
                       "tracked interconnect target");
    rc |= edacs_expect((g_state.edacs_vc_call_type & (EDACS_IS_VOICE | EDACS_IS_INTERCONNECT | EDACS_IS_DIGITAL))
                           == (EDACS_IS_VOICE | EDACS_IS_INTERCONNECT | EDACS_IS_DIGITAL),
                       "standard-interconnect", "state", "tracked interconnect flags");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_msg1(1, 4, 654, 1), 0);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 4, "standard-channel-update", "state", "tracked update LCN");
    rc |= edacs_expect(g_state.lasttg == 654 && g_state.lastsrc == 0x800, "standard-channel-update", "state",
                       "tracked update target/source");
    rc |= edacs_expect(
        (g_state.edacs_vc_call_type & (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_DIGITAL | EDACS_IS_EMERGENCY))
            == (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_DIGITAL | EDACS_IS_EMERGENCY),
        "standard-channel-update", "state", "tracked update flags");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_group_msg1(1, 6, 0x080, 3000),
                                           edacs_standard_group_msg2(3000));
    rc |=
        edacs_expect(g_state.edacs_vc_lcn == 6, "standard-analog-agency-emergency", "state", "tracked voice group LCN");
    rc |= edacs_expect(g_state.lasttg == 0x080 && g_state.lastsrc == 3000, "standard-analog-agency-emergency", "state",
                       "tracked voice group ids");
    rc |= edacs_expect(
        (g_state.edacs_vc_call_type
         & (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_EMERGENCY | EDACS_IS_AGENCY_CALL | EDACS_IS_DIGITAL))
            == (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_EMERGENCY | EDACS_IS_AGENCY_CALL),
        "standard-analog-agency-emergency", "state", "tracked analog agency emergency flags");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_msg1(0, 7, 0x088, 0), 0);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 7, "standard-channel-update-fleet", "state", "tracked fleet update LCN");
    rc |= edacs_expect(g_state.lasttg == 0x088 && g_state.lastsrc == 0x800, "standard-channel-update-fleet", "state",
                       "tracked fleet update target/source");
    rc |= edacs_expect(
        (g_state.edacs_vc_call_type & (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_FLEET_CALL | EDACS_IS_DIGITAL))
            == (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_FLEET_CALL),
        "standard-channel-update-fleet", "state", "tracked analog fleet update flags");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_individual_msg1(3, 8, 4321),
                                           1234);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 8, "standard-channel-update-individual", "state",
                       "tracked individual update LCN");
    rc |= edacs_expect(g_state.lasttg == 4321 && g_state.lastsrc == 0x800, "standard-channel-update-individual",
                       "state", "tracked individual update target/source");
    rc |= edacs_expect((g_state.edacs_vc_call_type & (EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL | EDACS_IS_DIGITAL))
                           == (EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL | EDACS_IS_DIGITAL),
                       "standard-channel-update-individual", "state", "tracked digital individual update flags");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_individual_msg1(0, 9, 0),
                                           0);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 9, "standard-channel-update-test-call", "state",
                       "tracked test-call update LCN");
    rc |= edacs_expect(g_state.lasttg == 0 && g_state.lastsrc == 0x800, "standard-channel-update-test-call", "state",
                       "tracked test-call update target/source");
    rc |= edacs_expect(g_state.edacs_vc_call_type == (EDACS_IS_VOICE | EDACS_IS_TEST_CALL),
                       "standard-channel-update-test-call", "state", "tracked test-call update flags");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_individual_msg1(10, 0, 1), 0);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 10, "standard-individual-test-call", "state",
                       "tracked individual test-call LCN");
    rc |= edacs_expect(g_state.lasttg == 0 && g_state.lastsrc == 0, "standard-individual-test-call", "state",
                       "tracked individual test-call ids");
    rc |= edacs_expect(g_state.edacs_vc_call_type == (EDACS_IS_VOICE | EDACS_IS_TEST_CALL | EDACS_IS_DIGITAL),
                       "standard-individual-test-call", "state", "tracked individual test-call flags");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(4, 2, 0x1B, 0), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0x1B, "standard-site-id", "state", "tracked site id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 4, "standard-site-id", "state", "tracked CC LCN");
    rc |= edacs_expect(g_state.p25_cc_freq == 852762500L && g_state.trunk_cc_freq == 852762500L, "standard-site-id",
                       "state", "updated CC frequency from LCN map");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(9, 3, 0x0C, 1), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0x0C, "standard-aux-site-id", "state", "tracked auxiliary site id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 1, "standard-aux-site-id", "state",
                       "auxiliary site did not replace CC LCN");
    rc |= edacs_expect(g_state.trunk_cc_freq == 851012500L, "standard-aux-site-id", "state",
                       "auxiliary site preserved CC frequency");

    edacs_setup_state_fixture(0);
    g_opts.use_rigctl = 1;
    g_rigctl_current_freq = 855262500L;
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(12, 1, 0x12, 0), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0x12, "standard-site-id-rigctl-capture", "state", "tracked site id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 12, "standard-site-id-rigctl-capture", "state", "tracked rigctl CC LCN");
    rc |= edacs_expect(g_state.trunk_lcn_freq[11] == 855262500L, "standard-site-id-rigctl-capture", "state",
                       "captured missing LCN frequency from rigctl");
    rc |= edacs_expect(g_state.p25_cc_freq == 855262500L && g_state.trunk_cc_freq == 855262500L,
                       "standard-site-id-rigctl-capture", "state", "updated CC frequency from captured rigctl LCN");

    edacs_setup_state_fixture(0);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = 856012500U;
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(13, 1, 0x13, 0), 0);
    rc |= edacs_expect(g_state.edacs_cc_lcn == 13, "standard-site-id-rtl-capture", "state", "tracked RTL CC LCN");
    rc |= edacs_expect(g_state.trunk_lcn_freq[12] == 856012500L, "standard-site-id-rtl-capture", "state",
                       "captured missing LCN frequency from RTL center frequency");
    rc |= edacs_expect(g_state.p25_cc_freq == 856012500L && g_state.trunk_cc_freq == 856012500L,
                       "standard-site-id-rtl-capture", "state", "updated CC frequency from captured RTL LCN");

    edacs_setup_state_fixture(0);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_standard_all_call_msg1(9, 1, 1010),
                                           edacs_standard_all_call_msg2(1010));
    rc |= edacs_expect(g_state.edacs_vc_lcn == 9, "standard-all-call", "state", "tracked all-call LCN");
    rc |= edacs_expect(g_state.lasttg == 0 && g_state.lastsrc == 1010, "standard-all-call", "state",
                       "tracked all-call ids");
    rc |= edacs_expect((g_state.edacs_vc_call_type & (EDACS_IS_VOICE | EDACS_IS_ALL_CALL | EDACS_IS_DIGITAL))
                           == (EDACS_IS_VOICE | EDACS_IS_ALL_CALL | EDACS_IS_DIGITAL),
                       "standard-all-call", "state", "tracked all-call flags");

    return rc;
}

static int
edacs_run_extended_state_cases(void) {
    int rc = 0;

    edacs_setup_state_fixture(1);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_extended_system_info_msg1(0x4567), 9);
    rc |= edacs_expect(g_state.edacs_sys_id == 0x4567, "extended-system-info", "state", "tracked system id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 9, "extended-system-info", "state", "tracked CC LCN");
    rc |= edacs_expect(g_state.p25_cc_freq == 854012500L && g_state.trunk_cc_freq == 854012500L, "extended-system-info",
                       "state", "updated CC frequency from LCN map");

    edacs_setup_state_fixture(1);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_extended_site_id_msg1(0xA5, 0x34), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0xA5, "extended-site-id", "state", "tracked site id");
    rc |= edacs_expect(g_state.edacs_area_code == 0x34, "extended-site-id", "state", "tracked area code");

    edacs_setup_state_fixture(1);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_extended_test_call_msg1(4, 9), 0);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 9, "extended-test-call", "state", "tracked test-call working LCN");
    rc |= edacs_expect(g_state.lasttg == 999999999 && g_state.lastsrc == 999999999, "extended-test-call", "state",
                       "tracked test-call ids");
    rc |= edacs_expect(g_state.edacs_vc_call_type == (EDACS_IS_VOICE | EDACS_IS_TEST_CALL), "extended-test-call",
                       "state", "tracked test-call flags");

    edacs_setup_state_fixture(1);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, 0x12ULL << 23U,
                                           edacs_extended_channel_assignment_msg2(4, 0xABCDE));
    rc |= edacs_expect(g_state.edacs_vc_lcn == 4, "extended-channel-assignment", "state",
                       "tracked channel assignment LCN");
    rc |= edacs_expect(g_state.lastsrc == 0xABCDE, "extended-channel-assignment", "state",
                       "tracked channel assignment source");
    rc |= edacs_expect(g_state.edacs_vc_call_type == EDACS_IS_INDIVIDUAL, "extended-channel-assignment", "state",
                       "tracked channel assignment call type");

    edacs_setup_state_fixture(1);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_extended_all_call_msg1(4, 1, 1), 0xBCDEF);
    rc |= edacs_expect(g_state.edacs_vc_lcn == 4, "extended-all-call", "state", "tracked all-call LCN");
    rc |= edacs_expect(g_state.lasttg == 0 && g_state.lastsrc == 0xBCDEF, "extended-all-call", "state",
                       "tracked all-call ids");
    rc |= edacs_expect((g_state.edacs_vc_call_type & (EDACS_IS_VOICE | EDACS_IS_ALL_CALL | EDACS_IS_DIGITAL))
                           == (EDACS_IS_VOICE | EDACS_IS_ALL_CALL | EDACS_IS_DIGITAL),
                       "extended-all-call", "state", "tracked all-call flags");

    edacs_setup_state_fixture(1);
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_extended_group_update_msg1(0x6, 5, 0x3456, 1),
                                           edacs_extended_group_msg2(0x45678, 1, 1));
    rc |= edacs_expect(g_state.edacs_vc_lcn == 5, "extended-analog-group-emergency-update", "state",
                       "tracked analog group update LCN");
    rc |= edacs_expect(g_state.lasttg == 0x3456 && g_state.lastsrc == 0x45678, "extended-analog-group-emergency-update",
                       "state", "tracked analog group update ids");
    rc |= edacs_expect(
        (g_state.edacs_vc_call_type & (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_EMERGENCY | EDACS_IS_DIGITAL))
            == (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_EMERGENCY),
        "extended-analog-group-emergency-update", "state", "tracked analog emergency update flags");

    edacs_setup_state_fixture(1);
    g_state.edacs_vc_lcn = 17;
    g_state.lastsrc = 0x12345;
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_extended_group_msg1(0x3, 0, 0x2222),
                                           edacs_extended_group_msg2(0, 0, 1));
    rc |= edacs_expect(g_state.edacs_vc_lcn == 17, "extended-zero-lcn-source-group", "state",
                       "zero LCN preserved previous VC LCN");
    rc |= edacs_expect(g_state.lasttg == 0x2222 && g_state.lastsrc == 0x12345, "extended-zero-lcn-source-group",
                       "state", "zero source preserved previous source");
    rc |= edacs_expect(g_state.edacs_vc_call_type == (EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_DIGITAL),
                       "extended-zero-lcn-source-group", "state", "tracked digital group flags with zero fields");

    edacs_setup_state_fixture(1);
    g_state.edacs_sys_id = 0x1111;
    g_state.edacs_cc_lcn = 6;
    g_state.p25_cc_freq = 852512500L;
    g_state.trunk_cc_freq = 852512500L;
    dsd_neo_edacs_test_process_valid_frame(&g_opts, &g_state, edacs_extended_system_info_msg1(0x7777), 0);
    rc |= edacs_expect(g_state.edacs_sys_id == 0x1111, "extended-system-info-zero-lcn", "state",
                       "zero LCN preserved previous system id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 6, "extended-system-info-zero-lcn", "state",
                       "zero LCN preserved previous CC LCN");
    rc |= edacs_expect(g_state.p25_cc_freq == 852512500L && g_state.trunk_cc_freq == 852512500L,
                       "extended-system-info-zero-lcn", "state", "zero LCN preserved CC frequencies");

    return rc;
}

static int
edacs_run_helper_contract_cases(void) {
    int rc = 0;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    rc |= edacs_expect(strcmp(dsd_neo_edacs_test_lcn_status_string(26), "[Reserved LCN Status]") == 0, "helpers",
                       "lcn-status", "reserved status label");
    rc |= edacs_expect(strcmp(dsd_neo_edacs_test_lcn_status_string(28), "[Convert To Callee]") == 0, "helpers",
                       "lcn-status", "convert-to-callee label");
    rc |= edacs_expect(strcmp(dsd_neo_edacs_test_lcn_status_string(31), "[Call Denied]") == 0, "helpers", "lcn-status",
                       "call-denied label");
    rc |= edacs_expect(dsd_neo_edacs_test_lcn_status_string(12)[0] == '\0', "helpers", "lcn-status",
                       "ordinary LCN has no status label");

    rc |= edacs_expect(dsd_neo_edacs_test_apply_input_volume(0, 1234) == 1234, "helpers", "input-volume",
                       "disabled multiplier preserves sample");
    rc |= edacs_expect(dsd_neo_edacs_test_apply_input_volume(3, 12000) == 32767, "helpers", "input-volume",
                       "positive multiplier clamps high");
    rc |= edacs_expect(dsd_neo_edacs_test_apply_input_volume(4, -12000) == -32768, "helpers", "input-volume",
                       "positive multiplier clamps low");
    rc |= edacs_expect(dsd_neo_edacs_test_apply_input_volume(2, 1000) == 2000, "helpers", "input-volume",
                       "positive multiplier scales in range");

    const unsigned long long int frame_a = 0x00F0F0F0F0ULL;
    const unsigned long long int frame_b_inverted = (~0x00F0F0F0F1ULL) & 0xFFFFFFFFFFULL;
    const unsigned long long int frame_c = 0x00F0F0F0F0ULL;
    rc |= edacs_expect(dsd_neo_edacs_test_vote_frames(frame_a, frame_b_inverted, frame_c) == frame_a, "helpers", "vote",
                       "majority vote repairs inverted copy bit");

    rc |= edacs_expect(dsd_neo_edacs_test_update_squelch_count(1.0, 2.0, 5) == 4, "helpers", "squelch",
                       "below-squelch decrements countdown");
    rc |= edacs_expect(dsd_neo_edacs_test_update_squelch_count(3.0, 2.0, 1) == 5, "helpers", "squelch",
                       "above-squelch resets countdown");

    rc |= edacs_expect(dsd_neo_edacs_test_should_release_voice(0xAAAAAAAAAAAAAAAAULL, 0, time(NULL), 20.0) == 1,
                       "helpers", "release", "dotting sequence releases voice");
    rc |= edacs_expect(dsd_neo_edacs_test_should_release_voice(0x0000000000000000ULL, 0, time(NULL), 20.0) == 0,
                       "helpers", "release", "non-dotting with squelch enabled stays active");
    rc |= edacs_expect(dsd_neo_edacs_test_should_release_voice(0x0000000000000000ULL, 1, time(NULL) - 30, 20.0) == 1,
                       "helpers", "release", "disabled-squelch watchdog releases voice");

    dsd_neo_edacs_test_update_lcn_count(&state, 5);
    rc |= edacs_expect(state.edacs_lcn_count == 5, "helpers", "lcn-count", "valid LCN raises count");
    dsd_neo_edacs_test_update_lcn_count(&state, 31);
    rc |= edacs_expect(state.edacs_lcn_count == 5, "helpers", "lcn-count", "reserved status LCN does not raise count");
    dsd_neo_edacs_test_update_lcn_count(&state, 4);
    rc |= edacs_expect(state.edacs_lcn_count == 5, "helpers", "lcn-count", "lower LCN preserves count");

    static const unsigned long long int words[6] = {
        0x0123456789ULL, 0x0FEDCBA987ULL, 0x055AA55AA5ULL, 0x0AA55AA55AULL, 0x0000000000ULL, 0x0FFFFFFFFFULL,
    };
    int edacs_bit[241];
    DSD_MEMSET(edacs_bit, 0, sizeof(edacs_bit));
    for (size_t word = 0U; word < 6U; word++) {
        for (int bit = 0; bit < 40; bit++) {
            edacs_bit[(word * 40U) + (size_t)bit] = (int)((words[word] >> (39 - bit)) & 1ULL);
        }
    }

    unsigned long long int fr_1 = 0ULL;
    unsigned long long int fr_2 = 0ULL;
    unsigned long long int fr_3 = 0ULL;
    unsigned long long int fr_4 = 0ULL;
    unsigned long long int fr_5 = 0ULL;
    unsigned long long int fr_6 = 0ULL;
    dsd_neo_edacs_test_build_raw_frames(edacs_bit, &fr_1, &fr_2, &fr_3, &fr_4, &fr_5, &fr_6);
    rc |= edacs_expect(fr_1 == words[0], "helpers", "raw-frame-build", "first raw frame packed from bits");
    rc |= edacs_expect(fr_2 == words[1], "helpers", "raw-frame-build", "second raw frame packed from bits");
    rc |= edacs_expect(fr_3 == words[2], "helpers", "raw-frame-build", "third raw frame packed from bits");
    rc |= edacs_expect(fr_4 == words[3], "helpers", "raw-frame-build", "fourth raw frame packed from bits");
    rc |= edacs_expect(fr_5 == words[4], "helpers", "raw-frame-build", "fifth raw frame packed from bits");
    rc |= edacs_expect(fr_6 == words[5], "helpers", "raw-frame-build", "sixth raw frame packed from bits");

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    static int dibit_buf[900256];
    static int payload_buf[900256];
    DSD_MEMSET(dibit_buf, 0, sizeof(dibit_buf));
    DSD_MEMSET(payload_buf, 0, sizeof(payload_buf));
    state.dibit_buf = dibit_buf;
    state.dmr_payload_buf = payload_buf;
    state.synctype = DSD_SYNC_EDACS_POS;
    state.center = 0;
    state.dibit_buf_p = state.dibit_buf;
    state.dmr_payload_p = state.dmr_payload_buf;
    static short analog[960];
    unsigned long long int expected_sr = 0ULL;
    for (int sample = 0; sample < 960; sample++) {
        analog[sample] = 0;
    }
    for (int bit = 0; bit < 192; bit++) {
        const int value = (bit % 3) == 0 ? 700 : -700;
        analog[bit * 5] = (short)value;
        expected_sr = (expected_sr << 1U) | (unsigned long long int)(value > state.center ? 0U : 1U);
    }

    const unsigned long long int sr = dsd_neo_edacs_test_build_symbol_register(&opts, &state, analog);
    rc |= edacs_expect(sr == expected_sr, "helpers", "symbol-register", "register packed digitized analog symbols");
    rc |= edacs_expect(state.dibit_buf_p == state.dibit_buf + 192, "helpers", "symbol-register",
                       "digitizer advanced dibit buffer once per symbol");
    for (int bit = 0; bit < 192; bit++) {
        const int want_stored_dibit = analog[bit * 5] > state.center ? 1 : 3;
        rc |= edacs_expect(state.dibit_buf[bit] == want_stored_dibit, "helpers", "symbol-register",
                           "stored two-level EDACS dibit");
    }
    rc |= edacs_expect(dsd_neo_edacs_test_build_symbol_register(NULL, &state, analog) == 0ULL, "helpers",
                       "symbol-register", "null opts guard returns zero");

    state.dibit_buf_p = state.dibit_buf + 900001;
    state.dmr_payload_p = state.dmr_payload_buf + 900005;
    dsd_neo_edacs_test_reset_digitize_overflow(&state);
    rc |= edacs_expect(state.dibit_buf_p == state.dibit_buf + 200, "helpers", "overflow-reset",
                       "large dibit pointer reset to guard offset");
    rc |= edacs_expect(state.dmr_payload_p == state.dmr_payload_buf + 200, "helpers", "overflow-reset",
                       "large payload pointer reset to guard offset");
    state.dibit_buf_p = state.dibit_buf + 199;
    state.dmr_payload_p = state.dmr_payload_buf + 199;
    dsd_neo_edacs_test_reset_digitize_overflow(&state);
    rc |= edacs_expect(state.dibit_buf_p == state.dibit_buf + 199, "helpers", "overflow-reset",
                       "in-range dibit pointer preserved");
    rc |= edacs_expect(state.dmr_payload_p == state.dmr_payload_buf + 199, "helpers", "overflow-reset",
                       "in-range payload pointer preserved");

    return rc;
}

static int
edacs_run_analog_loop_helper_cases(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static short analog1[960];
    static short analog2[960];
    static short analog3[960];
    double pwr = -1.0;
    static int tcp_ctx_token;
    tcp_ctx_token = 0xEAA5;

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(analog1, 0, sizeof(analog1));
    DSD_MEMSET(analog2, 0, sizeof(analog2));
    DSD_MEMSET(analog3, 0, sizeof(analog3));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.input_volume_multiplier = 2;
    opts.tcp_in_ctx = (tcp_input_ctx*)&tcp_ctx_token;
    edacs_install_net_audio_hooks();

    rc |= edacs_expect(dsd_neo_edacs_test_collect_analog_triplet(&opts, &state, analog1, analog2, analog3, &pwr) == 1,
                       "analog-helpers", "tcp-collect", "TCP triplet collection succeeded");
    rc |= edacs_expect(g_tcp_read_count == 2880, "analog-helpers", "tcp-collect", "TCP read exactly three blocks");
    rc |= edacs_expect(g_tcp_close_count == 0, "analog-helpers", "tcp-collect", "TCP success did not close input");
    rc |= edacs_expect(analog1[0] == 2000 && analog1[959] == 3918 && analog2[0] == 3920 && analog3[959] == 7758,
                       "analog-helpers", "tcp-collect", "TCP samples preserved block ordering and volume scaling");
    rc |= edacs_expect(pwr > 0.0, "analog-helpers", "tcp-collect", "TCP collection updated power");

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_UDP;
    g_udp_fail_every = 4;
    edacs_install_net_audio_hooks();
    pwr = -1.0;
    rc |= edacs_expect(dsd_neo_edacs_test_collect_analog_triplet(&opts, &state, analog1, analog2, analog3, &pwr) == 1,
                       "analog-helpers", "udp-collect", "UDP triplet collection tolerates dropped samples");
    rc |= edacs_expect(g_udp_read_count == 2880, "analog-helpers", "udp-collect", "UDP read exactly three blocks");
    rc |= edacs_expect(analog1[0] == 0 && analog1[1] == 2001 && analog1[4] == 0 && analog2[0] == 0, "analog-helpers",
                       "udp-collect", "UDP failed reads become zero samples");
    rc |= edacs_expect(pwr > 0.0, "analog-helpers", "udp-collect", "UDP collection updated power");

#ifdef USE_RADIO
    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_volume_multiplier = 2;
    state.rtl_ctx = (struct RtlSdrContext*)&tcp_ctx_token;
    edacs_install_rtl_stream_hooks();
    pwr = -1.0;
    rc |= edacs_expect(dsd_neo_edacs_test_collect_analog_triplet(&opts, &state, analog1, analog2, analog3, &pwr) == 1,
                       "analog-helpers", "rtl-collect", "RTL triplet collection succeeded");
    rc |= edacs_expect(g_rtl_read_count == 2880, "analog-helpers", "rtl-collect", "RTL read exactly three blocks");
    rc |= edacs_expect(g_rtl_return_pwr_count == 1 && pwr == 77.25, "analog-helpers", "rtl-collect",
                       "RTL collection used squelch power hook");
    rc |= edacs_expect(analog1[0] == 200 && analog2[0] == 2120 && analog3[959] == 5958, "analog-helpers", "rtl-collect",
                       "RTL samples preserved block ordering and volume scaling");
#endif

    static short wav_src[960];
    static short wav_out[320];
    for (int i = 0; i < 960; i++) {
        wav_src[i] = (short)i;
    }
    DSD_MEMSET(wav_out, 0, sizeof(wav_out));
    rc |= edacs_expect(dsd_neo_edacs_test_static_wav_downsample(wav_src, wav_out, 320U) == 0, "analog-helpers",
                       "static-wav", "static WAV downsample helper accepted full output");
    rc |= edacs_expect(wav_out[0] == 0 && wav_out[1] == 0 && wav_out[2] == 6 && wav_out[3] == 6 && wav_out[318] == 954
                           && wav_out[319] == 954,
                       "analog-helpers", "static-wav", "static WAV helper picked every sixth sample as stereo");
    wav_out[0] = -123;
    rc |= edacs_expect(dsd_neo_edacs_test_static_wav_downsample(wav_src, wav_out, 319U) == -1 && wav_out[0] == -123,
                       "analog-helpers", "static-wav", "short output buffer is rejected without mutation");

    rc |= edacs_expect(dsd_neo_edacs_test_no_sql_watchdog_window(0.5) == 20.0, "analog-helpers", "watchdog",
                       "No-SQL watchdog clamps low hangtime");
    rc |= edacs_expect(dsd_neo_edacs_test_no_sql_watchdog_window(4.5) == 45.0, "analog-helpers", "watchdog",
                       "No-SQL watchdog preserves midrange hangtime");
    rc |= edacs_expect(dsd_neo_edacs_test_no_sql_watchdog_window(8.0) == 60.0, "analog-helpers", "watchdog",
                       "No-SQL watchdog clamps high hangtime");
    rc |= edacs_expect(dsd_neo_edacs_test_should_release_voice(0x0000000000000000ULL, 1, time(NULL) - 5, 20.0) == 0,
                       "analog-helpers", "watchdog", "No-SQL watchdog does not release before window");

    static short out1[960];
    static short out2[960];
    static short out3[960];
    for (int i = 0; i < 960; i++) {
        out1[i] = (short)(11 + i);
        out2[i] = (short)(22 + i);
        out3[i] = (short)(33 + i);
    }

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    edacs_install_udp_output_hooks();
    dsd_neo_edacs_test_emit_analog_audio(&opts, &state, out1, out2, out3);
    rc |= edacs_expect(g_udp_blast_count == 3, "analog-helpers", "udp-output", "UDP output emitted three blocks");
    rc |= edacs_expect(g_udp_blast_bytes[0] == 1920U && g_udp_blast_bytes[1] == 1920U && g_udp_blast_bytes[2] == 1920U,
                       "analog-helpers", "udp-output", "UDP output emitted 960 short samples per block");
    rc |= edacs_expect(g_udp_blast_first[0] == 11 && g_udp_blast_first[1] == 22 && g_udp_blast_first[2] == 33,
                       "analog-helpers", "udp-output", "UDP output preserved block order");

    char raw_path[] = "dsdneo_edacs_raw_XXXXXX";
    int raw_fd = dsd_mkstemp(raw_path);
    if (raw_fd < 0) {
        rc |= edacs_expect(0, "analog-helpers", "raw-output", "created temporary raw output");
    } else {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.audio_out_type = 1;
        opts.floating_point = 0;
        opts.slot1_on = 1;
        opts.audio_out_fd = raw_fd;
        dsd_neo_edacs_test_emit_analog_audio(&opts, &state, out1, out2, out3);
        dsd_stat_t st;
        DSD_MEMSET(&st, 0, sizeof(st));
        rc |= edacs_expect(dsd_fstat(raw_fd, &st) == 0 && (long long)st.st_size == 5760LL, "analog-helpers",
                           "raw-output", "raw fd output wrote three 960-sample blocks");
        (void)dsd_close(raw_fd);
        FILE* fp = fopen(raw_path, "rb");
        if (fp == NULL) {
            rc |= edacs_expect(0, "analog-helpers", "raw-output", "reopened raw output for verification");
        } else {
            short first1 = 0;
            short first2 = 0;
            short first3 = 0;
            rc |= edacs_expect(fread(&first1, sizeof(first1), 1U, fp) == 1U, "analog-helpers", "raw-output",
                               "read first raw block sample");
            rc |= edacs_expect(fseek(fp, (long)(960U * sizeof(short)), SEEK_SET) == 0
                                   && fread(&first2, sizeof(first2), 1U, fp) == 1U,
                               "analog-helpers", "raw-output", "read second raw block sample");
            rc |= edacs_expect(fseek(fp, (long)(1920U * sizeof(short)), SEEK_SET) == 0
                                   && fread(&first3, sizeof(first3), 1U, fp) == 1U,
                               "analog-helpers", "raw-output", "read third raw block sample");
            rc |= edacs_expect(first1 == 11 && first2 == 22 && first3 == 33, "analog-helpers", "raw-output",
                               "raw fd output preserved block order");
            (void)fclose(fp);
        }
        (void)remove(raw_path);
    }

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.tcp_in_ctx = (tcp_input_ctx*)&tcp_ctx_token;
    g_tcp_fail_at = 5;
    edacs_install_net_audio_hooks();
    pwr = 123.0;
    rc |= edacs_expect(dsd_neo_edacs_test_collect_analog_triplet(&opts, &state, analog1, analog2, analog3, &pwr) == 0,
                       "analog-helpers", "tcp-cleanup", "TCP read failure aborts collection");
    rc |= edacs_expect(g_tcp_read_count == 6 && g_tcp_close_count == 1 && opts.tcp_in_ctx == NULL, "analog-helpers",
                       "tcp-cleanup", "TCP read failure closes and clears input context");
    rc |=
        edacs_expect(dsd_exitflag_load() == 1, "analog-helpers", "tcp-cleanup", "TCP read failure requested shutdown");

#ifdef USE_RADIO
    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_volume_multiplier = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&tcp_ctx_token;
    g_rtl_fail_at = 3;
    edacs_install_rtl_stream_hooks();
    rc |= edacs_expect(dsd_neo_edacs_test_collect_analog_triplet(&opts, &state, analog1, analog2, analog3, &pwr) == 0,
                       "analog-helpers", "rtl-cleanup", "RTL read failure aborts collection");
    rc |= edacs_expect(g_rtl_read_count == 4 && dsd_exitflag_load() == 1, "analog-helpers", "rtl-cleanup",
                       "RTL read failure requested shutdown after bounded reads");
#endif

    edacs_reset_audio_hook_state();
    return rc;
}

int
main(void) {
    int rc = 0;
    const int std_group = 321;
    const int std_group_src = 1234;
    const int std_indiv_target = 4321;
    const int std_indiv_src = 2345;
    const int ea_group = 54321;
    const int ea_group_src = 34567;
    const int ea_icall_target = 654321;
    const int ea_icall_src = 45678;

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

    const edacs_grant_case cases[] = {
        {"standard-digital-group", edacs_standard_group_msg1(2, 5, std_group, std_group_src),
         edacs_standard_group_msg2(std_group_src), 852012500L, 0, 5, EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_GROUP,
         std_group, std_group_src},
        {"standard-digital-individual", edacs_standard_individual_msg1(6, std_indiv_target, 1),
         (unsigned long long int)std_indiv_src, 852512500L, 0, 6,
         EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_INDIVIDUAL, std_indiv_target, std_indiv_src},
        {"ea-digital-group", edacs_extended_group_msg1(3, 7, ea_group), (unsigned long long int)ea_group_src,
         853012500L, 1, 7, EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_GROUP, ea_group, ea_group_src},
        {"ea-digital-icall", edacs_extended_icall_msg1(ea_icall_target), edacs_extended_icall_msg2(8, ea_icall_src),
         853512500L, 1, 8, EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_INDIVIDUAL, ea_icall_target, ea_icall_src},
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (size_t r = 0; r < sizeof(results) / sizeof(results[0]); r++) {
            rc |= edacs_run_grant_result_case(&cases[c], results[r].result, results[r].name);
        }
    }

    static const struct {
        size_t case_index;
        edacs_no_tune_guard guard;
        const char* name;
    } guard_cases[] = {
        {0U, EDACS_GUARD_GROUP_DISABLED, "group-disabled"},
        {1U, EDACS_GUARD_PRIVATE_DISABLED, "private-disabled"},
        {2U, EDACS_GUARD_GROUP_DISABLED, "ea-group-disabled"},
        {3U, EDACS_GUARD_PRIVATE_DISABLED, "ea-private-disabled"},
        {0U, EDACS_GUARD_ALLOWLIST_BLOCK, "group-allowlist-block"},
        {1U, EDACS_GUARD_ALLOWLIST_BLOCK, "private-allowlist-block"},
        {2U, EDACS_GUARD_ALLOWLIST_BLOCK, "ea-group-allowlist-block"},
        {3U, EDACS_GUARD_ALLOWLIST_BLOCK, "ea-private-allowlist-block"},
        {2U, EDACS_GUARD_MISSING_FREQUENCY, "missing-frequency"},
        {2U, EDACS_GUARD_MISSING_CC_LCN, "missing-cc-lcn"},
        {3U, EDACS_GUARD_P25_TRUNK_DISABLED, "p25-trunk-disabled"},
    };

    for (size_t g = 0; g < sizeof(guard_cases) / sizeof(guard_cases[0]); g++) {
        rc |=
            edacs_run_no_tune_guard_case(&cases[guard_cases[g].case_index], guard_cases[g].guard, guard_cases[g].name);
    }

    rc |= edacs_run_retry_after_reject_case(&cases[0], DSD_TRUNK_TUNE_RESULT_DEFERRED, "retry-after-deferred");
    rc |= edacs_run_retry_after_reject_case(&cases[1], DSD_TRUNK_TUNE_RESULT_FAILED, "retry-after-failed");
    rc |= edacs_run_retry_after_reject_case(&cases[2], DSD_TRUNK_TUNE_RESULT_TIMEOUT, "retry-after-timeout");

    for (size_t r = 0; r < sizeof(results) / sizeof(results[0]); r++) {
        rc |= edacs_run_eot_result_case(results[r].result, results[r].name);
    }
    rc |= edacs_run_eot_retry_after_reject_case(DSD_TRUNK_TUNE_RESULT_DEFERRED, "deferred-then-ok");
    rc |= edacs_run_eot_retry_after_reject_case(DSD_TRUNK_TUNE_RESULT_FAILED, "failed-then-ok");
    rc |= edacs_run_eot_retry_after_reject_case(DSD_TRUNK_TUNE_RESULT_TIMEOUT, "timeout-then-ok");
    rc |= edacs_run_retune_after_eot_case(&cases[3]);
    rc |= edacs_run_standard_state_cases();
    rc |= edacs_run_extended_state_cases();
    rc |= edacs_run_helper_contract_cases();
    rc |= edacs_run_analog_loop_helper_cases();

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
#ifdef USE_RADIO
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
#endif
    if (rc == 0) {
        printf("EDACS_GRANT_TUNE_MATRIX: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
