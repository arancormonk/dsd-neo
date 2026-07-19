// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify policy-backed P25 grant filtering behavior in the trunk SM path. */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_trunk_sm_internal.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    row.priority = priority;
    row.preempt = preempt ? 1u : 0u;
    return dsd_tg_policy_upsert_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

static void
mark_cc_reacquired(dsd_state* st) {
    if (!st) {
        return;
    }
    double now_m = dsd_time_now_monotonic_s();
    if (now_m <= st->last_cc_sync_time_m) {
        now_m = st->last_cc_sync_time_m + 0.001;
    }
    st->last_cc_sync_time = time(NULL);
    st->last_cc_sync_time_m = now_m;
    st->p25_last_cc_msg_time = st->last_cc_sync_time;
    st->p25_last_cc_msg_time_m = now_m;
}

static void
seed_fdma_iden(dsd_state* st, int id) {
    st->p25_chan_iden = id;
    st->p25_iden_fdma[id].base_freq = 851000000 / 5;
    st->p25_iden_fdma[id].chan_type = 1;
    st->p25_iden_fdma[id].chan_spac = 100;
    st->p25_iden_fdma[id].trust = 2;
    st->p25_iden_fdma[id].populated = 1;
    st->p25_chan_tdma_explicit[id] = 1;
}

static void
seed_tdma_iden(dsd_state* st, int id) {
    st->p25_chan_iden = id;
    st->p25_iden_tdma[id].base_freq = 851000000 / 5;
    st->p25_iden_tdma[id].chan_type = 3;
    st->p25_iden_tdma[id].chan_spac = 100;
    st->p25_iden_tdma[id].trust = 2;
    st->p25_iden_tdma[id].populated = 1;
    st->p25_chan_tdma_explicit[id] = 2;
}

static int
tg_policy_is_absent(const dsd_state* st, uint32_t tg) {
    dsd_tg_policy_lookup lookup;
    if (dsd_tg_policy_lookup_id(st, tg, &lookup) != 0) {
        return 0;
    }
    return lookup.match == DSD_TG_POLICY_MATCH_NONE;
}

static int
enc_call_cache_index(const dsd_state* st, uint32_t target, int is_group) {
    if (!st) {
        return -1;
    }
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (st->p25_enc_tg_cache_tg[i] == target && st->p25_enc_tg_cache_is_group[i] == (uint8_t)(is_group ? 1 : 0)
            && st->p25_enc_tg_cache_until[i] > 0) {
            return i;
        }
    }
    return -1;
}

static int
enc_call_cache_is_absent(const dsd_state* st, uint32_t target, int is_group) {
    return enc_call_cache_index(st, target, is_group) < 0;
}

static int
enc_tg_cache_is_absent(const dsd_state* st, uint32_t tg) {
    return enc_call_cache_is_absent(st, tg, 1);
}

static dsd_p25_crypto_state
classify_current_call_without_key(dsd_opts* opts, dsd_state* state, int target) {
    return p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE1, 0, 0x84, 0x2714, 0x0123456789ABCDEFULL, target);
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    install_trunk_tuning_hooks();
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.trunk_use_allow_list = 1;
    st.p25_cc_freq = 851000000;

    // FDMA IDEN
    int id = 1;
    seed_fdma_iden(&st, id);
    int ch = (id << 12) | 0x000A;
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", "0", 1);
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", "0", 1);

    // Unknown group is blocked in allow-list mode.
    unsigned before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1100,
                                   .src = 2100,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("group unknown blocked in allow-list", st.p25_sm_tune_count == before);

    // Known allowed group tunes.
    rc |= expect_true("seed group A", seed_exact(&st, 1101, "A", "ALLOW", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1101,
                                   .src = 2101,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("group known A tunes", st.p25_sm_tune_count == before + 1);

    // Active patch members can satisfy grant policy while the OTA target remains the supergroup.
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    opts.trunk_use_allow_list = 0;
    st.tg_hold = 1401;
    p25_patch_add_wgid(&st, 1400, 1401);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1400,
                                   .src = 2400,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("patch member hold tunes supergroup", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("patch member hold stores policy tg", st.p25_policy_tg[0] == 1401U);
    st.synctype = DSD_SYNC_P25P1_POS;
    st.lasttg = 1400;
    st.lastsrc = 2400;
    int enc = 1;
    rc |= expect_true("patch member hold audio gate call", dsd_audio_group_gate_mono(&opts, &st, 1400, enc, &enc) == 0);
    rc |= expect_true("patch member hold audio gate opens", enc == 0);
    st.tg_hold = 0;

    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    opts.trunk_use_allow_list = 1;
    rc |= expect_true("seed patch member allow", seed_exact(&st, 1403, "A", "PATCH-MEMBER", 0, 0) == 0);
    p25_patch_add_wgid(&st, 1402, 1403);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1402,
                                   .src = 2402,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("patch member allowlist tunes supergroup", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("patch member allowlist policy tg", st.p25_policy_tg[0] == 1403U);
    st.synctype = DSD_SYNC_P25P2_POS;
    st.lasttg = 1402;
    st.lastsrc = 2402;
    st.gi[0] = 0;
    rc |= expect_true("patch member allowlist p2 media gate", dsd_p25p2_decode_audio_allowed(&opts, &st, 0, 0) == 1);

    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    p25_patch_add_wgid(&st, 1404, 1405);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1404,
                                   .src = 2404,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("patch no policy match blocked by allowlist", st.p25_sm_tune_count == before);

    // Explicit mode blocks remain enforced.
    rc |= expect_true("seed group B", seed_exact(&st, 1102, "B", "BLOCK", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1102,
                                   .src = 2102,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("group mode B blocked", st.p25_sm_tune_count == before);

    rc |= expect_true("seed group DE", seed_exact(&st, 1103, "DE", "ENC", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1103,
                                   .src = 2103,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("group mode DE blocked", st.p25_sm_tune_count == before);

    // Runtime encryption lockout uses a silent probe and must not persist as a
    // TG policy block for a later clear call.
    rc |= expect_true("seed mixed-mode group", seed_exact(&st, 1104, "A", "MIXED", 0, 0) == 0);
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    opts.trunk_tune_enc_calls = 0;
    opts.trunk_is_tuned = 0;
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1104,
                                   .src = 2104,
                                   .svc_bits = 0x40,
                                   .is_group = 1});
    rc |= expect_true("encrypted mixed-mode grant probes", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("encrypted mixed-mode probe is pending",
                      st.p25_crypto_state[0] == DSD_P25_CRYPTO_ENCRYPTED_PENDING);
    rc |= expect_true("encrypted grant alone does not arm blocked-call cache", enc_tg_cache_is_absent(&st, 1104U));
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1104,
                                   .src = 2105,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("later clear mixed-mode grant tunes", st.p25_sm_tune_count == before + 1);
    opts.trunk_tune_enc_calls = 1;

    // A target is probed once, retained only after authoritative crypto
    // metadata proves it cannot be decrypted, and recovered by a clear grant.
    {
        static dsd_opts cache_opts;
        static dsd_state cache_st;
        DSD_MEMSET(&cache_opts, 0, sizeof cache_opts);
        DSD_MEMSET(&cache_st, 0, sizeof cache_st);
        cache_opts.trunk_enable = 1;
        cache_opts.trunk_tune_group_calls = 1;
        cache_opts.trunk_tune_private_calls = 1;
        cache_opts.trunk_tune_data_calls = 1;
        cache_opts.trunk_tune_enc_calls = 0;
        cache_st.p25_cc_freq = 851000000;
        seed_fdma_iden(&cache_st, id);
        p25_sm_init_ctx(p25_sm_get_ctx(), &cache_opts, &cache_st);

        before = cache_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1300,
                                       .src = 2300,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 1});
        rc |= expect_true("unknown-svc grant initially tunes", cache_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("unknown-svc probe starts pending",
                          cache_st.p25_crypto_state[0] == DSD_P25_CRYPTO_ENCRYPTED_PENDING);
        rc |= expect_true("pending probe does not arm cache", enc_tg_cache_is_absent(&cache_st, 1300U));

        rc |= expect_true("missing AES key classifies probe blocked",
                          classify_current_call_without_key(&cache_opts, &cache_st, 1300) == DSD_P25_CRYPTO_BLOCKED);
        int cache_idx = enc_call_cache_index(&cache_st, 1300U, 1);
        rc |= expect_true("blocked group probe arms typed cache", cache_idx >= 0);
        mark_cc_reacquired(&cache_st);
        if (cache_idx >= 0) {
            cache_st.p25_enc_tg_cache_until[cache_idx] = time(NULL) + 1;
        }
        time_t short_until = (cache_idx >= 0) ? cache_st.p25_enc_tg_cache_until[cache_idx] : 0;
        before = cache_st.p25_sm_tune_count;
        cache_opts.trunk_is_tuned = 0;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1300,
                                       .src = 2301,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 1});
        rc |= expect_true("unknown-svc grant suppressed by blocked-call cache", cache_st.p25_sm_tune_count == before);
        rc |= expect_true("suppressed grant refreshes blocked-call expiry",
                          cache_idx >= 0 && cache_st.p25_enc_tg_cache_until[cache_idx] > short_until);
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1300,
                                       .src = 2301,
                                       .svc_bits = 0x40,
                                       .is_group = 1});
        rc |= expect_true("explicit encrypted grant suppressed by blocked-call cache",
                          cache_st.p25_sm_tune_count == before);
        rc |= expect_true("transient enc cache does not add TG policy", tg_policy_is_absent(&cache_st, 1300U));

        cache_opts.trunk_is_tuned = 0;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1300,
                                       .src = 2302,
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        rc |=
            expect_true("explicit clear grant bypasses transient enc cache", cache_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("explicit clear grant clears transient enc cache", enc_tg_cache_is_absent(&cache_st, 1300U));
        rc |= expect_true("explicit clear grant does not add TG policy", tg_policy_is_absent(&cache_st, 1300U));
        p25_sm_release(p25_sm_get_ctx(), &cache_opts, &cache_st, "explicit-release");
        mark_cc_reacquired(&cache_st);

        p25_sm_note_encrypted_call_typed(&cache_opts, &cache_st, 1301, 1);
        cache_idx = enc_call_cache_index(&cache_st, 1301U, 1);
        rc |= expect_true("seed expiring group cache", cache_idx >= 0);
        if (cache_idx >= 0) {
            cache_st.p25_enc_tg_cache_until[cache_idx] = time(NULL) - 1;
        }
        before = cache_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1301,
                                       .src = 2303,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 1});
        rc |= expect_true("expired blocked-call cache permits new probe", cache_st.p25_sm_tune_count == before + 1);
        p25_sm_release(p25_sm_get_ctx(), &cache_opts, &cache_st, "explicit-release");
        mark_cc_reacquired(&cache_st);

        p25_sm_note_encrypted_call_typed(&cache_opts, &cache_st, 1302, 1);
        cache_opts.trunk_tune_enc_calls = 1;
        before = cache_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1302,
                                       .src = 2304,
                                       .svc_bits = 0x40,
                                       .is_group = 1});
        rc |= expect_true("encrypted-follow mode ignores blocked-call cache", cache_st.p25_sm_tune_count == before + 1);
        p25_sm_release(p25_sm_get_ctx(), &cache_opts, &cache_st, "explicit-release");
        mark_cc_reacquired(&cache_st);
        cache_opts.trunk_tune_enc_calls = 0;

        p25_sm_note_encrypted_call_typed(&cache_opts, &cache_st, 1303, 1);
        p25_patch_set_kas(&cache_st, 1303, /*key*/ 0, /*alg*/ 0x84, /*ssn*/ 1);
        before = cache_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1303,
                                       .src = 2305,
                                       .svc_bits = 0x40,
                                       .is_group = 1});
        rc |=
            expect_true("regroup clear override bypasses blocked-call cache", cache_st.p25_sm_tune_count == before + 1);
        rc |=
            expect_true("regroup clear override clears matching cache entry", enc_tg_cache_is_absent(&cache_st, 1303U));
        p25_sm_release(p25_sm_get_ctx(), &cache_opts, &cache_st, "explicit-release");
        mark_cc_reacquired(&cache_st);

        // Group and private calls with the same numeric target retain separate
        // identities, and clear grants remove only the matching type.
        p25_sm_note_encrypted_call_typed(&cache_opts, &cache_st, 1400, 1);
        before = cache_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .dst = 1400,
                                       .src = 2400,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 0});
        rc |= expect_true("group cache does not suppress same-ID private probe",
                          cache_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("private probe classifies blocked",
                          classify_current_call_without_key(&cache_opts, &cache_st, 1400) == DSD_P25_CRYPTO_BLOCKED);
        rc |= expect_true("group same-ID cache retained", !enc_call_cache_is_absent(&cache_st, 1400U, 1));
        rc |= expect_true("private same-ID cache armed", !enc_call_cache_is_absent(&cache_st, 1400U, 0));
        mark_cc_reacquired(&cache_st);

        before = cache_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .dst = 1400,
                                       .src = 2401,
                                       .svc_bits = 0x40,
                                       .is_group = 0});
        rc |= expect_true("private encrypted grant suppressed by private cache", cache_st.p25_sm_tune_count == before);
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1400,
                                       .src = 2402,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 1});
        rc |= expect_true("group unknown grant suppressed by group cache", cache_st.p25_sm_tune_count == before);

        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .dst = 1400,
                                       .src = 2403,
                                       .svc_bits = 0x00,
                                       .is_group = 0});
        rc |= expect_true("private clear grant recovers private call", cache_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("private clear removes only private cache", enc_call_cache_is_absent(&cache_st, 1400U, 0));
        rc |=
            expect_true("private clear preserves same-ID group cache", !enc_call_cache_is_absent(&cache_st, 1400U, 1));
        p25_sm_release(p25_sm_get_ctx(), &cache_opts, &cache_st, "explicit-release");
        mark_cc_reacquired(&cache_st);
        before = cache_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &cache_opts, &cache_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 1400,
                                       .src = 2404,
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        rc |= expect_true("group clear grant recovers group call", cache_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("group clear removes group cache", enc_call_cache_is_absent(&cache_st, 1400U, 1));
        p25_sm_release(p25_sm_get_ctx(), &cache_opts, &cache_st, "explicit-release");
        mark_cc_reacquired(&cache_st);
        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &st);
    }

    // Matching hold does not override explicit B/DE blocks in grant-compatible hold mode.
    st.tg_hold = 1102;
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1102,
                                   .src = 2202,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("hold match still blocked by mode B", st.p25_sm_tune_count == before);
    st.tg_hold = 0;

    // TG 0 is evaluated like any other exact ID.
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    opts.trunk_is_tuned = 0;
    rc |= expect_true("seed tg0 A", seed_exact(&st, 0, "A", "ZERO-ALLOW", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    p25_sm_event(
        p25_sm_get_ctx(), &opts, &st,
        &(p25_sm_event_t){
            .type = P25_SM_EV_GRANT, .slot = -1, .channel = ch, .tg = 0, .src = 2300, .svc_bits = 0x00, .is_group = 1});
    rc |= expect_true("tg0 allowed row tunes", st.p25_sm_tune_count == before + 1);

    rc |= expect_true("upsert tg0 B", seed_exact(&st, 0, "B", "ZERO-BLOCK", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(
        p25_sm_get_ctx(), &opts, &st,
        &(p25_sm_event_t){
            .type = P25_SM_EV_GRANT, .slot = -1, .channel = ch, .tg = 0, .src = 2301, .svc_bits = 0x00, .is_group = 1});
    rc |= expect_true("tg0 blocked row blocks", st.p25_sm_tune_count == before);

    // The SM is the canonical private-grant policy gate.
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .dst = 9001,
                                   .src = 9002,
                                   .svc_bits = 0x00,
                                   .is_group = 0});
    rc |= expect_true("private unknown allowed in allow-list", st.p25_sm_tune_count == before + 1);

    // Explicit data grant wrappers force data-call policy without rewriting service bits.
    {
        static dsd_opts data_opts;
        static dsd_state data_st;
        DSD_MEMSET(&data_opts, 0, sizeof data_opts);
        DSD_MEMSET(&data_st, 0, sizeof data_st);
        data_opts.trunk_enable = 1;
        data_opts.trunk_tune_group_calls = 1;
        data_opts.trunk_tune_private_calls = 1;
        data_opts.trunk_tune_data_calls = 0;
        data_opts.trunk_tune_enc_calls = 1;
        data_st.p25_cc_freq = 851000000;
        seed_fdma_iden(&data_st, id);
        p25_sm_init_ctx(p25_sm_get_ctx(), &data_opts, &data_st);

        before = data_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 3100,
                                       .src = 4100,
                                       .svc_bits = 0x00,
                                       .is_group = 1,
                                       .data_call_override = 1});
        rc |= expect_true("group data clear svc blocked when data disabled", data_st.p25_sm_tune_count == before);
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .dst = 3101,
                                       .src = 4101,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 0,
                                       .data_call_override = 1});
        rc |= expect_true("indiv data unknown svc blocked when data disabled", data_st.p25_sm_tune_count == before);

        data_opts.trunk_tune_data_calls = 1;
        data_opts.trunk_is_tuned = 0;
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 3102,
                                       .src = 4102,
                                       .svc_bits = 0x00,
                                       .is_group = 1,
                                       .data_call_override = 1});
        rc |= expect_true("group data clear svc tunes when data enabled", data_st.p25_sm_tune_count == before + 1);
        p25_sm_release(p25_sm_get_ctx(), &data_opts, &data_st, "explicit-release");
        mark_cc_reacquired(&data_st);
        data_opts.trunk_is_tuned = 0;
        before = data_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .dst = 3103,
                                       .src = 4103,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 0,
                                       .data_call_override = 1});
        rc |= expect_true("indiv data unknown svc tunes when data enabled", data_st.p25_sm_tune_count == before + 1);

        p25_sm_release(p25_sm_get_ctx(), &data_opts, &data_st, "explicit-release");
        mark_cc_reacquired(&data_st);
        data_opts.trunk_tune_enc_calls = 0;
        data_opts.trunk_is_tuned = 0;
        before = data_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 3104,
                                       .src = 4104,
                                       .svc_bits = 0x40,
                                       .is_group = 1,
                                       .data_call_override = 1});
        rc |= expect_true("group data raw enc bit blocks when enc disabled", data_st.p25_sm_tune_count == before);
        rc |=
            expect_true("group data raw enc bit does not arm voice enc cache", enc_tg_cache_is_absent(&data_st, 3104U));
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 3104,
                                       .src = 4105,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 1});
        rc |= expect_true("svc-less voice grant not skipped after encrypted data grant",
                          data_st.p25_sm_tune_count == before + 1);
        p25_sm_release(p25_sm_get_ctx(), &data_opts, &data_st, "explicit-release");
        mark_cc_reacquired(&data_st);

        p25_emit_enc_lockout_once_typed(&data_opts, &data_st, 0, 3105, /*svc*/ 0x40, 1);
        rc |= expect_true("seed transient voice enc cache", !enc_tg_cache_is_absent(&data_st, 3105U));
        before = data_st.p25_sm_tune_count;
        data_opts.trunk_is_tuned = 0;
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 3105,
                                       .src = 4106,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 1,
                                       .data_call_override = 1});
        rc |= expect_true("svc-less group data grant ignores voice enc cache", data_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("svc-less group data grant preserves voice enc cache",
                          !enc_tg_cache_is_absent(&data_st, 3105U));
        p25_sm_release(p25_sm_get_ctx(), &data_opts, &data_st, "explicit-release");
        mark_cc_reacquired(&data_st);

        data_opts.trunk_tune_data_calls = 0;
        data_opts.trunk_is_tuned = 0;
        before = data_st.p25_sm_tune_count;
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 3105,
                                       .src = 4107,
                                       .svc_bits = 0x00,
                                       .is_group = 1,
                                       .data_call_override = 1});
        rc |= expect_true("clear group data grant blocked when data disabled", data_st.p25_sm_tune_count == before);
        rc |= expect_true("clear group data grant preserves voice enc cache", !enc_tg_cache_is_absent(&data_st, 3105U));
        p25_sm_event(p25_sm_get_ctx(), &data_opts, &data_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = ch,
                                       .tg = 3105,
                                       .src = 4108,
                                       .svc_bits = P25_SM_SVC_UNKNOWN,
                                       .is_group = 1});
        rc |= expect_true("svc-less voice grant remains suppressed after clear data grant",
                          data_st.p25_sm_tune_count == before);
        p25_sm_init_ctx(p25_sm_get_ctx(), &opts, &st);
    }

    // Priority preemption: preempt-flagged low-priority candidate does not displace.
    rc |= expect_true("seed active high", seed_exact(&st, 1200, "A", "ACTIVE-HIGH", 80, 0) == 0);
    rc |= expect_true("seed candidate low preempt", seed_exact(&st, 1201, "A", "CAND-LOW", 10, 1) == 0);
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1200,
                                   .src = 2200,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("active high tuned", st.p25_sm_tune_count == before + 1);
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1201,
                                   .src = 2201,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("low preempt candidate blocked", st.p25_sm_tune_count == before);

    // Priority preemption: higher-priority candidate without preempt flag does not displace.
    rc |= expect_true("seed candidate high no-preempt", seed_exact(&st, 1203, "A", "CAND-HIGH-NP", 95, 0) == 0);
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1203,
                                   .src = 2203,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("high candidate without preempt flag blocked", st.p25_sm_tune_count == before);

    // Priority preemption: higher-priority preempt candidate displaces.
    rc |= expect_true("seed candidate high preempt", seed_exact(&st, 1202, "A", "CAND-HIGH", 95, 1) == 0);
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1202,
                                   .src = 2202,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("high preempt candidate reused carrier",
                      st.p25_sm_tune_count == before && p25_sm_get_ctx()->vc_tg == 1202);

    // Patch-member priority/preempt displaces using the matched member policy, not the OTA SG's default priority.
    p25_sm_release(p25_sm_get_ctx(), &opts, &st, "explicit-release");
    mark_cc_reacquired(&st);
    rc |= expect_true("seed patch active base", seed_exact(&st, 1500, "A", "PATCH-ACTIVE", 80, 0) == 0);
    rc |= expect_true("seed patch member preempt", seed_exact(&st, 1502, "A", "PATCH-PREEMPT", 95, 1) == 0);
    p25_patch_add_wgid(&st, 1501, 1502);
    before = st.p25_sm_tune_count;
    opts.trunk_is_tuned = 0;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1500,
                                   .src = 2500,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("patch preempt active tuned", st.p25_sm_tune_count == before + 1);
    before = st.p25_sm_tune_count;
    p25_sm_event(p25_sm_get_ctx(), &opts, &st,
                 &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                   .slot = -1,
                                   .channel = ch,
                                   .tg = 1501,
                                   .src = 2501,
                                   .svc_bits = 0x00,
                                   .is_group = 1});
    rc |= expect_true("patch member preempt reuses carrier",
                      st.p25_sm_tune_count == before && p25_sm_get_ctx()->vc_tg == 1502);
    rc |= expect_true("patch member preempt policy tg", st.p25_policy_tg[0] == 1502U);

    // Same-frequency TDMA grants update one slot without clearing the other slot's patch policy mapping.
    {
        static dsd_opts dual_opts;
        static dsd_state dual_st;
        DSD_MEMSET(&dual_opts, 0, sizeof dual_opts);
        DSD_MEMSET(&dual_st, 0, sizeof dual_st);
        dual_opts.trunk_enable = 1;
        dual_opts.trunk_tune_group_calls = 1;
        dual_opts.trunk_tune_private_calls = 1;
        dual_opts.trunk_tune_data_calls = 1;
        dual_opts.trunk_tune_enc_calls = 1;
        dual_opts.trunk_use_allow_list = 1;
        dual_st.p25_cc_freq = 851000000;
        seed_tdma_iden(&dual_st, id);
        p25_sm_init_ctx(p25_sm_get_ctx(), &dual_opts, &dual_st);

        rc |= expect_true("seed dual slot member 0", seed_exact(&dual_st, 1502, "A", "SLOT0-MEMBER", 0, 0) == 0);
        rc |= expect_true("seed dual slot member 1", seed_exact(&dual_st, 1602, "A", "SLOT1-MEMBER", 0, 0) == 0);
        p25_patch_add_wgid(&dual_st, 1501, 1502);
        p25_patch_add_wgid(&dual_st, 1601, 1602);

        p25_sm_event(p25_sm_get_ctx(), &dual_opts, &dual_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = 0x100B,
                                       .tg = 1601,
                                       .src = 2601,
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        p25_sm_ctx_t* dual_ctx = p25_sm_get_ctx();
        unsigned dual_tunes_after_slot1 = dual_st.p25_sm_tune_count;
        rc |= expect_true("dual slot1 policy tg stored", dual_st.p25_policy_tg[1] == 1602U);
        rc |= expect_true("dual slot1 grant context",
                          dual_ctx->slots[1].grant_active && dual_ctx->slots[1].target_id == 1602);
        dual_ctx->slots[1].voice_active = 1;
        dual_ctx->slots[1].last_active_m = 1.0;
        dual_st.p25_p2_audio_allowed[1] = 1;

        p25_sm_event(p25_sm_get_ctx(), &dual_opts, &dual_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = 0x100A,
                                       .tg = 1501,
                                       .src = 2600,
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        rc |= expect_true("dual slot0 same-carrier no retune", dual_st.p25_sm_tune_count == dual_tunes_after_slot1);
        rc |= expect_true("dual slot0 same-carrier active slot", dual_st.p25_p2_active_slot == 0);
        rc |= expect_true("dual slot0 policy tg stored", dual_st.p25_policy_tg[0] == 1502U);
        rc |= expect_true("dual slot1 policy tg preserved", dual_st.p25_policy_tg[1] == 1602U);
        rc |= expect_true("dual slot1 active preserved",
                          dual_ctx->slots[1].voice_active == 1 && dual_ctx->slots[1].target_id == 1602);
        rc |= expect_true("dual slot0 grant context",
                          dual_ctx->slots[0].grant_active && dual_ctx->slots[0].target_id == 1502);

        rc |= expect_true("seed same-slot replacement", seed_exact(&dual_st, 1701, "A", "SLOT0-REPL", 0, 0) == 0);
        p25_sm_event(p25_sm_get_ctx(), &dual_opts, &dual_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = 0x100A,
                                       .tg = 1701,
                                       .src = 2701,
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        rc |= expect_true("dual same-slot replacement no retune", dual_st.p25_sm_tune_count == dual_tunes_after_slot1);
        rc |= expect_true("dual same-slot replacement target",
                          dual_ctx->slots[0].grant_active && dual_ctx->slots[0].target_id == 1701);
        rc |= expect_true("dual same-slot preserves other slot",
                          dual_ctx->slots[1].voice_active == 1 && dual_ctx->slots[1].target_id == 1602);

        dual_ctx->slots[1].voice_active = 0;
        dual_ctx->slots[1].last_active_m = 0.0;
        dual_st.p25_p2_audio_allowed[1] = 0;
        p25_sm_event(p25_sm_get_ctx(), &dual_opts, &dual_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = 0x100B,
                                       .tg = 1701,
                                       .src = 2702,
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        rc |= expect_true("dual moved target no retune", dual_st.p25_sm_tune_count == dual_tunes_after_slot1);
        rc |= expect_true("dual moved target active slot", dual_st.p25_p2_active_slot == 1);
        rc |= expect_true("dual moved target clears old slot", dual_ctx->slots[0].grant_active == 0);
        rc |= expect_true("dual moved target stores new slot",
                          dual_ctx->slots[1].grant_active && dual_ctx->slots[1].target_id == 1701);
    }

    // Group TG IDs and individual RID destinations are separate namespaces.
    {
        static dsd_opts namespace_opts;
        static dsd_state namespace_st;
        DSD_MEMSET(&namespace_opts, 0, sizeof namespace_opts);
        DSD_MEMSET(&namespace_st, 0, sizeof namespace_st);
        namespace_opts.trunk_enable = 1;
        namespace_opts.trunk_tune_group_calls = 1;
        namespace_opts.trunk_tune_private_calls = 1;
        namespace_opts.trunk_tune_data_calls = 1;
        namespace_opts.trunk_tune_enc_calls = 1;
        namespace_opts.trunk_use_allow_list = 1;
        namespace_st.p25_cc_freq = 851000000;
        seed_tdma_iden(&namespace_st, id);
        p25_sm_init_ctx(p25_sm_get_ctx(), &namespace_opts, &namespace_st);

        rc |= expect_true("seed namespace group", seed_exact(&namespace_st, 1234, "A", "GROUP-SAME-RID", 0, 0) == 0);
        p25_sm_event(p25_sm_get_ctx(), &namespace_opts, &namespace_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = 0x100A,
                                       .dst = 1234,
                                       .src = 4234,
                                       .svc_bits = 0x00,
                                       .is_group = 0});
        p25_sm_ctx_t* namespace_ctx = p25_sm_get_ctx();
        unsigned namespace_tunes_after_private = namespace_st.p25_sm_tune_count;
        rc |= expect_true("namespace private slot0 stored",
                          namespace_ctx->slots[0].grant_active && namespace_ctx->slots[0].target_id == 1234
                              && namespace_ctx->slots[0].is_group == 0 && namespace_ctx->slots[0].dst == 1234);
        namespace_ctx->slots[0].voice_active = 1;
        namespace_ctx->slots[0].last_active_m = 1.0;
        namespace_st.p25_p2_audio_allowed[0] = 1;

        p25_sm_event(p25_sm_get_ctx(), &namespace_opts, &namespace_st,
                     &(p25_sm_event_t){.type = P25_SM_EV_GRANT,
                                       .slot = -1,
                                       .channel = 0x100B,
                                       .tg = 1234,
                                       .src = 5234,
                                       .svc_bits = 0x00,
                                       .is_group = 1});
        rc |= expect_true("namespace group same-carrier no retune",
                          namespace_st.p25_sm_tune_count == namespace_tunes_after_private);
        rc |= expect_true("namespace private slot preserved",
                          namespace_ctx->slots[0].grant_active && namespace_ctx->slots[0].voice_active == 1
                              && namespace_ctx->slots[0].target_id == 1234 && namespace_ctx->slots[0].is_group == 0
                              && namespace_ctx->slots[0].dst == 1234);
        rc |= expect_true("namespace group slot stored",
                          namespace_ctx->slots[1].grant_active && namespace_ctx->slots[1].target_id == 1234
                              && namespace_ctx->slots[1].is_group == 1 && namespace_ctx->slots[1].ota_tg == 1234);
    }

    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS");
    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS");

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
