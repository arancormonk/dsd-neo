// SPDX-License-Identifier: GPL-3.0-or-later
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
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void dsd_neo_edacs_test_process_valid_frame(dsd_opts* opts, dsd_state* state, unsigned long long int msg_1,
                                            unsigned long long int msg_2);

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

static void
edacs_install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_result = edacs_hook_tune_to_freq;
    hooks.tune_to_cc_result = edacs_hook_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
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

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    if (rc == 0) {
        printf("EDACS_GRANT_TUNE_MATRIX: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
