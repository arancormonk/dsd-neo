// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Protocol-generic noCarrier() trunk return matrix.
 *
 * Covers non-P25 trunk sync families that share the generic return path rather
 * than the P25 trunk state machine helper.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

typedef struct {
    const char* name;
    int synctype;
} generic_return_case;

enum {
    GENERIC_TRUNK_CC_HZ = 851012500,
    GENERIC_TRUNK_VC_HZ = 852012500,
    GENERIC_P25_CC_HZ = 769868750,
    GENERIC_P25_VC_HZ = 771056250,
    GENERIC_REST_CC_HZ = 853012500,
};

static int g_return_to_cc_calls = 0;

static dsd_trunk_tune_result
generic_return_to_cc_guard(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)state;
    g_return_to_cc_calls++;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static void
install_return_to_cc_guard(void) {
    g_return_to_cc_calls = 0;
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = generic_return_to_cc_guard;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
clear_return_to_cc_guard(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    g_return_to_cc_calls = 0;
}

static int
expect_true(const char* protocol, const char* scenario, const char* check, int cond) {
    if (cond) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL protocol=%s scenario=%s check=%s\n", protocol, scenario, check);
    return 1;
}

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);
    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_test_runtime(dsd_opts* opts, dsd_state* state) {
    if (state) {
        freeState(state);
    }
    free(state);
    free(opts);
}

static int
setup_generic_fixture(dsd_opts** opts_out, dsd_state** state_out, const generic_return_case* test_case,
                      const char* scenario, int recent_vc, int with_cc) {
    if (init_test_runtime(opts_out, state_out) != 0) {
        return 1;
    }

    dsd_opts* opts = *opts_out;
    dsd_state* state = *state_out;
    const time_t now = time(NULL);

    opts->trunk_enable = 1;
    opts->trunk_hangtime = 1.0f;
    opts->trunk_is_tuned = 1;
    opts->audio_in_type = AUDIO_IN_PULSE;

    state->synctype = test_case->synctype;
    state->lastsynctype = test_case->synctype;
    state->last_cc_sync_time = now - 11;
    state->last_vc_sync_time = recent_vc ? now : (now - 11);
    state->trunk_cc_freq = with_cc ? GENERIC_TRUNK_CC_HZ : 0L;
    state->p25_cc_freq = with_cc ? GENERIC_P25_CC_HZ : 0L;
    state->trunk_vc_freq[0] = GENERIC_TRUNK_VC_HZ;
    state->trunk_vc_freq[1] = GENERIC_TRUNK_VC_HZ;
    state->p25_vc_freq[0] = GENERIC_P25_VC_HZ;
    state->p25_vc_freq[1] = GENERIC_P25_VC_HZ;
    state->p2_cc = 0x293;
    state->p2_wacn = 0xABCDE;
    state->p2_sysid = 0x123;
    state->p2_rfssid = 2;
    state->p2_siteid = 3;
    state->p25_sys_is_tdma = 1;
    state->p25_p2_active_slot = 1;
    state->p25_p2_audio_allowed[0] = 1;
    state->p25_p2_audio_allowed[1] = 1;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_BLOCKED;
    state->p25_call_is_packet[0] = 1;
    state->p25_call_is_packet[1] = 1;
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "%s", scenario);
    return 0;
}

static int
p25_p2_voice_aliases_cleared(const dsd_state* state) {
    return state->p25_p2_active_slot == -1 && state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_allowed[1] == 0
           && state->p25_crypto_state[0] == DSD_P25_CRYPTO_UNKNOWN
           && state->p25_crypto_state[1] == DSD_P25_CRYPTO_UNKNOWN && state->p25_call_is_packet[0] == 0
           && state->p25_call_is_packet[1] == 0;
}

static int
run_recent_vc_case(const generic_return_case* test_case) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = setup_generic_fixture(&opts, &state, test_case, "recent-vc", 1, 1);
    if (rc != 0) {
        return rc;
    }

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, "recent-vc", "generic tuned flag preserved", opts->trunk_is_tuned == 1);
    rc |= expect_true(test_case->name, "recent-vc", "generic VC frequency preserved",
                      state->trunk_vc_freq[0] == GENERIC_TRUNK_VC_HZ && state->trunk_vc_freq[1] == GENERIC_TRUNK_VC_HZ);
    rc |= expect_true(test_case->name, "recent-vc", "p25 VC alias cleared",
                      state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);
    rc |= expect_true(test_case->name, "recent-vc", "p25 identity aliases cleared",
                      state->p2_cc == 0 && state->p2_wacn == 0 && state->p2_sysid == 0 && state->p2_rfssid == 0
                          && state->p2_siteid == 0 && state->p25_sys_is_tdma == 0);
    rc |= expect_true(test_case->name, "recent-vc", "p25 slot and audio aliases cleared",
                      p25_p2_voice_aliases_cleared(state));

    free_test_runtime(opts, state);
    return rc;
}

static int
run_stale_vc_with_cc_case(const generic_return_case* test_case) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = setup_generic_fixture(&opts, &state, test_case, "stale-vc-with-cc", 0, 1);
    if (rc != 0) {
        return rc;
    }

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, "stale-vc-with-cc", "tuned flags cleared", opts->trunk_is_tuned == 0);
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "generic VC frequencies cleared",
                      state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "p25 VC aliases cleared",
                      state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "generic CC retained",
                      state->trunk_cc_freq == GENERIC_TRUNK_CC_HZ);
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "stale p25 CC alias cleared", state->p25_cc_freq == 0);
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "p25 identity aliases cleared",
                      state->p2_cc == 0 && state->p2_wacn == 0 && state->p2_sysid == 0 && state->p25_sys_is_tdma == 0);
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "p25 slot and audio aliases cleared",
                      p25_p2_voice_aliases_cleared(state));
    rc |=
        expect_true(test_case->name, "stale-vc-with-cc", "active display cleared", state->active_channel[0][0] == '\0');

    free_test_runtime(opts, state);
    return rc;
}

static int
run_stale_vc_without_cc_case(const generic_return_case* test_case) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = setup_generic_fixture(&opts, &state, test_case, "stale-vc-without-cc", 0, 0);
    if (rc != 0) {
        return rc;
    }

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, "stale-vc-without-cc", "unreturnable tuned flags cleared",
                      opts->trunk_is_tuned == 0);
    rc |= expect_true(test_case->name, "stale-vc-without-cc", "unreturnable generic VC cleared",
                      state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);
    rc |= expect_true(test_case->name, "stale-vc-without-cc", "control channels remain empty",
                      state->trunk_cc_freq == 0 && state->p25_cc_freq == 0);
    rc |= expect_true(test_case->name, "stale-vc-without-cc", "p25 aliases cleared",
                      state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0 && state->p2_cc == 0
                          && state->p2_wacn == 0 && state->p2_sysid == 0);
    rc |= expect_true(test_case->name, "stale-vc-without-cc", "p25 slot and audio aliases cleared",
                      p25_p2_voice_aliases_cleared(state));

    free_test_runtime(opts, state);
    return rc;
}

static int
run_stale_vc_with_matching_p25_cc_alias_case(const generic_return_case* test_case) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = setup_generic_fixture(&opts, &state, test_case, "stale-vc-matching-p25-cc", 0, 1);
    if (rc != 0) {
        return rc;
    }
    state->p25_cc_freq = state->trunk_cc_freq;

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, "stale-vc-matching-p25-cc", "matching p25 CC alias retained",
                      state->trunk_cc_freq == GENERIC_TRUNK_CC_HZ && state->p25_cc_freq == GENERIC_TRUNK_CC_HZ);
    rc |= expect_true(test_case->name, "stale-vc-matching-p25-cc", "voice state cleared",
                      opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0 && state->p25_vc_freq[0] == 0
                          && p25_p2_voice_aliases_cleared(state));

    free_test_runtime(opts, state);
    return rc;
}

static int
run_mapped_rest_channel_case(const generic_return_case* test_case) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = setup_generic_fixture(&opts, &state, test_case, "mapped-rest-channel", 0, 1);
    if (rc != 0) {
        return rc;
    }
    state->dmr_rest_channel = 7;
    state->trunk_chan_map[7] = GENERIC_REST_CC_HZ;

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, "mapped-rest-channel", "mapped rest channel selected",
                      state->trunk_cc_freq == GENERIC_REST_CC_HZ);
    rc |= expect_true(test_case->name, "mapped-rest-channel", "mapped rest channel clears stale p25 cc alias",
                      state->p25_cc_freq == 0);
    rc |= expect_true(test_case->name, "mapped-rest-channel", "mapped rest channel consumed",
                      state->dmr_rest_channel == -1);
    rc |= expect_true(test_case->name, "mapped-rest-channel", "voice state cleared",
                      opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0 && state->p25_vc_freq[0] == 0
                          && p25_p2_voice_aliases_cleared(state));

    free_test_runtime(opts, state);
    return rc;
}

static int
run_generic_overrides_p25_helper_case(const generic_return_case* test_case) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = setup_generic_fixture(&opts, &state, test_case, "generic-overrides-p25-helper", 0, 1);
    if (rc != 0) {
        return rc;
    }
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    install_return_to_cc_guard();

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, "generic-overrides-p25-helper", "p25 helper not called",
                      g_return_to_cc_calls == 0);
    rc |= expect_true(test_case->name, "generic-overrides-p25-helper", "generic return accepted",
                      opts->trunk_is_tuned == 0 && state->trunk_cc_freq == GENERIC_TRUNK_CC_HZ
                          && state->p25_cc_freq == 0);
    rc |= expect_true(test_case->name, "generic-overrides-p25-helper", "p25 hints cleared",
                      state->p25_vc_freq[0] == 0 && state->p2_cc == 0 && state->p25_sys_is_tdma == 0
                          && p25_p2_voice_aliases_cleared(state));

    clear_return_to_cc_guard();
    free_test_runtime(opts, state);
    return rc;
}

static int
run_sync_source_case(const generic_return_case* test_case, int current_only) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    const char* scenario = current_only ? "current-sync-only" : "last-sync-only";
    int rc = setup_generic_fixture(&opts, &state, test_case, scenario, 0, 1);
    if (rc != 0) {
        return rc;
    }
    if (current_only) {
        state->lastsynctype = DSD_SYNC_NONE;
    } else {
        state->synctype = DSD_SYNC_NONE;
    }

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, scenario, "generic sync source selected generic path",
                      opts->trunk_is_tuned == 0 && state->trunk_cc_freq == GENERIC_TRUNK_CC_HZ
                          && state->p25_cc_freq == 0);
    rc |= expect_true(test_case->name, scenario, "p25 aliases cleared",
                      state->p25_vc_freq[0] == 0 && state->p2_cc == 0 && p25_p2_voice_aliases_cleared(state));

    free_test_runtime(opts, state);
    return rc;
}

int
main(void) {
    int rc = 0;
    static const generic_return_case cases[] = {
        {"dmr-bs-data-pos", DSD_SYNC_DMR_BS_DATA_POS},
        {"dmr-bs-voice-neg", DSD_SYNC_DMR_BS_VOICE_NEG},
        {"dmr-bs-voice-pos", DSD_SYNC_DMR_BS_VOICE_POS},
        {"dmr-bs-data-neg", DSD_SYNC_DMR_BS_DATA_NEG},
        {"dmr-ms-voice", DSD_SYNC_DMR_MS_VOICE},
        {"dmr-ms-data", DSD_SYNC_DMR_MS_DATA},
        {"dmr-rc-data", DSD_SYNC_DMR_RC_DATA},
        {"nxdn-pos", DSD_SYNC_NXDN_POS},
        {"nxdn-neg", DSD_SYNC_NXDN_NEG},
        {"edacs-pos", DSD_SYNC_EDACS_POS},
        {"edacs-neg", DSD_SYNC_EDACS_NEG},
        {"provoice-pos", DSD_SYNC_PROVOICE_POS},
        {"provoice-neg", DSD_SYNC_PROVOICE_NEG},
        {"x2tdma-data-pos", DSD_SYNC_X2TDMA_DATA_POS},
        {"x2tdma-voice-neg", DSD_SYNC_X2TDMA_VOICE_NEG},
        {"x2tdma-voice-pos", DSD_SYNC_X2TDMA_VOICE_POS},
        {"x2tdma-data-neg", DSD_SYNC_X2TDMA_DATA_NEG},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rc |= run_recent_vc_case(&cases[i]);
        rc |= run_stale_vc_with_cc_case(&cases[i]);
        rc |= run_stale_vc_without_cc_case(&cases[i]);
        rc |= run_stale_vc_with_matching_p25_cc_alias_case(&cases[i]);
        rc |= run_sync_source_case(&cases[i], 1);
        rc |= run_sync_source_case(&cases[i], 0);
    }

    rc |= run_mapped_rest_channel_case(&(generic_return_case){"dmr-rest", DSD_SYNC_DMR_BS_VOICE_POS});
    rc |= run_generic_overrides_p25_helper_case(&(generic_return_case){"dmr-p25-helper", DSD_SYNC_DMR_BS_VOICE_POS});
    rc |= run_generic_overrides_p25_helper_case(&(generic_return_case){"nxdn-p25-helper", DSD_SYNC_NXDN_POS});
    clear_return_to_cc_guard();

    if (rc == 0) {
        printf("ENGINE_GENERIC_TRUNK_RETURN_MATRIX: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
