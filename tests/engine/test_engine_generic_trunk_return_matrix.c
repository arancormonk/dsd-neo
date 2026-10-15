// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Protocol-generic noCarrier() trunk return matrix.
 *
 * Covers non-P25 trunk sync families that share the generic return path rather
 * than the P25 trunk state machine helper, and TETRA, whose state machine owns
 * the return from the call it follows, so the generic path leaves it alone.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"

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

static int g_tetra_ted_sps = -1;

/* The engine's tune hook as far as the TETRA state machine sees it: an accepted voice-channel tune. */
static dsd_trunk_tune_result
tetra_tune_accepted(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    g_tetra_ted_sps = ted_sps;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* Put the real TETRA state machine on the traffic channel the fixture describes: a control channel some CRC-passed
 * block proved, and a followed grant. Without a control channel to return to, the fixture loses it afterwards. */
static int
tetra_follow_traffic(dsd_opts* opts, dsd_state* state) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tetra_tune_accepted;
    dsd_trunk_tuning_hooks_set(hooks);
    const long cc = state->trunk_cc_freq;
    state->trunk_cc_freq = GENERIC_TRUNK_CC_HZ;
    tetra_sm_on_cc_sync(opts, state);
    tetra_confirm_note_signalling(state);
    const tetra_sm_grant grant = {GENERIC_TRUNK_VC_HZ, 40U, 8U, 0x321U};
    g_tetra_ted_sps = -1;
    const int followed = tetra_sm_on_grant(opts, state, &grant);
    state->trunk_cc_freq = cc;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return followed == 1 && g_tetra_ted_sps == 0 && tetra_sm_get_state(state) == TETRA_SM_TUNED;
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
    state->trunk_cc_freq = with_cc ? GENERIC_TRUNK_CC_HZ : 0L;
    if (DSD_SYNC_IS_TETRA(test_case->synctype)) {
        opts->frame_tetra = 1;
        if (!tetra_follow_traffic(opts, state)) {
            DSD_FPRINTF(stderr, "FAIL protocol=%s scenario=%s check=TETRA follows the traffic channel\n",
                        test_case->name, scenario);
            free_test_runtime(opts, state);
            *opts_out = NULL;
            *state_out = NULL;
            return 1;
        }
    }

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
    if (DSD_SYNC_IS_TETRA(test_case->synctype)) {
        /* The followed call's record: a group call on timeslot 1 of carrier 40, transmitting. */
        tetra_call_event setup;
        DSD_MEMSET(&setup, 0, sizeof setup);
        setup.kind = TETRA_CALL_EV_SETUP;
        setup.call_id = 0x321U;
        setup.basic_service = 0x04U;
        tetra_calls_on_event(NULL, state, &setup, NULL);
        tetra_call_t* const call = tetra_calls_find(state, 0x321U);
        call->phase = TETRA_CALL_TX_GRANTED;
        call->alloc.timeslots = 8;
        call->alloc.carrier = 40;
        call->alloc.freq_hz = GENERIC_TRUNK_VC_HZ;
    }
    const dsd_call_observation observation = {
        .protocol = test_case->synctype,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .policy_target_id = 1201U,
        .ota_source_id = 42001U,
        .channel = 1U,
        .frequency_hz = GENERIC_TRUNK_VC_HZ,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) <= 0
        || dsd_recent_activity_publish(state, 1U, &observation, scenario, 0U) <= 0) {
        free_test_runtime(opts, state);
        *opts_out = NULL;
        *state_out = NULL;
        return 1;
    }
    return 0;
}

static int
p25_p2_voice_aliases_cleared(const dsd_state* state) {
    return state->p25_p2_active_slot == -1 && state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_allowed[1] == 0
           && state->p25_crypto_state[0] == DSD_P25_CRYPTO_UNKNOWN
           && state->p25_crypto_state[1] == DSD_P25_CRYPTO_UNKNOWN;
}

static int
canonical_call_ended(const dsd_state* state) {
    dsd_call_snapshot call = {0};
    return dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED && call.ota_target_id == 1201U
           && call.ota_source_id == 42001U;
}

static int
recent_activity_cleared(const dsd_state* state) {
    dsd_recent_activity_snapshot recent = {0};
    return dsd_recent_activity_copy_snapshot(state, &recent) <= 0 || recent.entries[1].notice[0] == '\0';
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
    rc |= expect_true(test_case->name, "recent-vc", "carrier loss ended canonical call", canonical_call_ended(state));
    if (DSD_SYNC_IS_TETRA(test_case->synctype)) {
        rc |= expect_true(test_case->name, "recent-vc", "TETRA SM keeps the followed call",
                          tetra_sm_get_state(state) == TETRA_SM_TUNED);
    }

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
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "canonical call ended", canonical_call_ended(state));
    rc |= expect_true(test_case->name, "stale-vc-with-cc", "recent activity cleared", recent_activity_cleared(state));

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

/* The TETRA state machine owns the return from the call it follows (tetra_sm_owns_return()): its grant timeout and
 * the hangtime after the last speech decide, so a no-carrier pass two seconds after the follow's last reception, past
 * the generic one-second hangtime but within the four-second grant timeout, takes no return of its own and does not
 * drop the follow, with a control channel known or without one. The pass still ends the canonical call on the lost
 * carrier and forgets its decoding; ENGINE_TETRA_TRUNK runs the machine's own return at its deadline. */
static int
run_tetra_owner_case(const generic_return_case* test_case, int with_cc) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    const char* scenario = with_cc ? "tetra-owner-with-cc" : "tetra-owner-without-cc";
    int rc = setup_generic_fixture(&opts, &state, test_case, scenario, 0, with_cc);
    if (rc != 0) {
        return rc;
    }
    state->last_vc_sync_time = time(NULL) - 2;
    install_return_to_cc_guard();

    noCarrier(opts, state);

    rc |= expect_true(test_case->name, scenario, "no return taken", g_return_to_cc_calls == 0);
    rc |= expect_true(test_case->name, scenario, "tuned flag kept", opts->trunk_is_tuned == 1);
    rc |= expect_true(test_case->name, scenario, "traffic channel kept",
                      state->trunk_vc_freq[0] == GENERIC_TRUNK_VC_HZ && state->trunk_vc_freq[1] == GENERIC_TRUNK_VC_HZ);
    rc |= expect_true(test_case->name, scenario, "control channel kept",
                      state->trunk_cc_freq == (with_cc ? GENERIC_TRUNK_CC_HZ : 0L));
    rc |= expect_true(test_case->name, scenario, "TETRA SM keeps the followed call",
                      tetra_sm_get_state(state) == TETRA_SM_TUNED && tetra_calls_followed(state) != NULL);
    rc |= expect_true(test_case->name, scenario, "carrier loss ended canonical call", canonical_call_ended(state));

    clear_return_to_cc_guard();
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
    static const generic_return_case tetra_cases[] = {
        {"tetra-ndb-pos", DSD_SYNC_TETRA_NDB_POS},
        {"tetra-sb-neg", DSD_SYNC_TETRA_SB_NEG},
        {"tetra-ndb-neg", DSD_SYNC_TETRA_NDB_NEG},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rc |= run_recent_vc_case(&cases[i]);
        rc |= run_stale_vc_with_cc_case(&cases[i]);
        rc |= run_stale_vc_without_cc_case(&cases[i]);
        rc |= run_stale_vc_with_matching_p25_cc_alias_case(&cases[i]);
        rc |= run_sync_source_case(&cases[i], 1);
        rc |= run_sync_source_case(&cases[i], 0);
    }
    for (size_t i = 0; i < sizeof(tetra_cases) / sizeof(tetra_cases[0]); i++) {
        rc |= run_recent_vc_case(&tetra_cases[i]);
        rc |= run_tetra_owner_case(&tetra_cases[i], 1);
        rc |= run_tetra_owner_case(&tetra_cases[i], 0);
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
