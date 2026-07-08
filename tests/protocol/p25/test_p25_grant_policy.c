// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify policy-backed P25 grant filtering behavior in the trunk SM path. */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

struct RtlSdrContext;

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
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
enc_tg_cache_is_absent(const dsd_state* st, uint32_t tg) {
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (st->p25_enc_tg_cache_tg[i] == tg && st->p25_enc_tg_cache_until[i] > 0) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);

    opts.p25_trunk = 1;
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
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1100, /*src*/ 2100);
    rc |= expect_true("group unknown blocked in allow-list", st.p25_sm_tune_count == before);

    // Known allowed group tunes.
    rc |= expect_true("seed group A", seed_exact(&st, 1101, "A", "ALLOW", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1101, /*src*/ 2101);
    rc |= expect_true("group known A tunes", st.p25_sm_tune_count == before + 1);

    // Active patch members can satisfy grant policy while the OTA target remains the supergroup.
    p25_sm_on_release(&opts, &st);
    opts.trunk_use_allow_list = 0;
    st.tg_hold = 1401;
    p25_patch_add_wgid(&st, 1400, 1401);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1400, /*src*/ 2400);
    rc |= expect_true("patch member hold tunes supergroup", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("patch member hold stores policy tg", st.p25_policy_tg[0] == 1401U);
    st.synctype = DSD_SYNC_P25P1_POS;
    st.lasttg = 1400;
    st.lastsrc = 2400;
    int enc = 1;
    rc |= expect_true("patch member hold audio gate call", dsd_audio_group_gate_mono(&opts, &st, 1400, enc, &enc) == 0);
    rc |= expect_true("patch member hold audio gate opens", enc == 0);
    st.tg_hold = 0;

    p25_sm_on_release(&opts, &st);
    opts.trunk_use_allow_list = 1;
    rc |= expect_true("seed patch member allow", seed_exact(&st, 1403, "A", "PATCH-MEMBER", 0, 0) == 0);
    p25_patch_add_wgid(&st, 1402, 1403);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1402, /*src*/ 2402);
    rc |= expect_true("patch member allowlist tunes supergroup", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("patch member allowlist policy tg", st.p25_policy_tg[0] == 1403U);
    st.synctype = DSD_SYNC_P25P2_POS;
    st.lasttg = 1402;
    st.lastsrc = 2402;
    st.gi[0] = 0;
    rc |= expect_true("patch member allowlist p2 media gate", dsd_p25p2_decode_audio_allowed(&opts, &st, 0, 0) == 1);

    p25_sm_on_release(&opts, &st);
    p25_patch_add_wgid(&st, 1404, 1405);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1404, /*src*/ 2404);
    rc |= expect_true("patch no policy match blocked by allowlist", st.p25_sm_tune_count == before);

    // Explicit mode blocks remain enforced.
    rc |= expect_true("seed group B", seed_exact(&st, 1102, "B", "BLOCK", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1102, /*src*/ 2102);
    rc |= expect_true("group mode B blocked", st.p25_sm_tune_count == before);

    rc |= expect_true("seed group DE", seed_exact(&st, 1103, "DE", "ENC", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1103, /*src*/ 2103);
    rc |= expect_true("group mode DE blocked", st.p25_sm_tune_count == before);

    // Runtime encrypted-call lockout must not persist as a TG policy block.
    rc |= expect_true("seed mixed-mode group", seed_exact(&st, 1104, "A", "MIXED", 0, 0) == 0);
    p25_sm_on_release(&opts, &st);
    opts.trunk_tune_enc_calls = 0;
    opts.p25_is_tuned = 0;
    before = st.p25_sm_tune_count;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x40, /*tg*/ 1104, /*src*/ 2104);
    rc |= expect_true("encrypted mixed-mode grant blocked", st.p25_sm_tune_count == before);
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1104, /*src*/ 2105);
    rc |= expect_true("later clear mixed-mode grant tunes", st.p25_sm_tune_count == before + 1);
    opts.trunk_tune_enc_calls = 1;

    // Transient encrypted-call memory blocks only ambiguous no-SVC grants and never writes TG policy.
    {
        static dsd_opts cache_opts;
        static dsd_state cache_st;
        DSD_MEMSET(&cache_opts, 0, sizeof cache_opts);
        DSD_MEMSET(&cache_st, 0, sizeof cache_st);
        cache_opts.p25_trunk = 1;
        cache_opts.trunk_tune_group_calls = 1;
        cache_opts.trunk_tune_private_calls = 1;
        cache_opts.trunk_tune_data_calls = 1;
        cache_opts.trunk_tune_enc_calls = 0;
        cache_st.p25_cc_freq = 851000000;
        seed_fdma_iden(&cache_st, id);
        p25_sm_init(&cache_opts, &cache_st);

        before = cache_st.p25_sm_tune_count;
        p25_sm_on_group_grant(&cache_opts, &cache_st, ch, P25_SM_SVC_UNKNOWN, /*tg*/ 1300, /*src*/ 2300);
        rc |= expect_true("unknown-svc grant initially tunes", cache_st.p25_sm_tune_count == before + 1);

        p25_emit_enc_lockout_once(&cache_opts, &cache_st, 0, 1300, /*svc*/ 0);
        p25_sm_on_release(&cache_opts, &cache_st);
        before = cache_st.p25_sm_tune_count;
        cache_opts.p25_is_tuned = 0;
        p25_sm_on_group_grant(&cache_opts, &cache_st, ch, P25_SM_SVC_UNKNOWN, /*tg*/ 1300, /*src*/ 2301);
        rc |= expect_true("unknown-svc grant skipped by transient enc cache", cache_st.p25_sm_tune_count == before);
        rc |= expect_true("transient enc cache does not add TG policy", tg_policy_is_absent(&cache_st, 1300U));

        cache_opts.p25_is_tuned = 0;
        p25_sm_on_group_grant(&cache_opts, &cache_st, ch, /*svc*/ 0x00, /*tg*/ 1300, /*src*/ 2302);
        rc |=
            expect_true("explicit clear grant bypasses transient enc cache", cache_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("explicit clear grant clears transient enc cache", enc_tg_cache_is_absent(&cache_st, 1300U));
        rc |= expect_true("explicit clear grant does not add TG policy", tg_policy_is_absent(&cache_st, 1300U));
        p25_sm_on_release(&cache_opts, &cache_st);
        p25_sm_init(&opts, &st);
    }

    // Matching hold does not override explicit B/DE blocks in grant-compatible hold mode.
    st.tg_hold = 1102;
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1102, /*src*/ 2202);
    rc |= expect_true("hold match still blocked by mode B", st.p25_sm_tune_count == before);
    st.tg_hold = 0;

    // TG 0 is evaluated like any other exact ID.
    p25_sm_on_release(&opts, &st);
    opts.p25_is_tuned = 0;
    rc |= expect_true("seed tg0 A", seed_exact(&st, 0, "A", "ZERO-ALLOW", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 0, /*src*/ 2300);
    rc |= expect_true("tg0 allowed row tunes", st.p25_sm_tune_count == before + 1);

    rc |= expect_true("upsert tg0 B", seed_exact(&st, 0, "B", "ZERO-BLOCK", 0, 0) == 0);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 0, /*src*/ 2301);
    rc |= expect_true("tg0 blocked row blocks", st.p25_sm_tune_count == before);

    // SM private-grant path keeps unknown private IDs allowed under allow-list mode.
    p25_sm_on_release(&opts, &st);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_indiv_grant(&opts, &st, ch, /*svc*/ 0x00, /*dst*/ 9001, /*src*/ 9002);
    rc |= expect_true("private unknown allowed in allow-list", st.p25_sm_tune_count == before + 1);

    // Explicit data grant wrappers force data-call policy without rewriting service bits.
    {
        static dsd_opts data_opts;
        static dsd_state data_st;
        DSD_MEMSET(&data_opts, 0, sizeof data_opts);
        DSD_MEMSET(&data_st, 0, sizeof data_st);
        data_opts.p25_trunk = 1;
        data_opts.trunk_tune_group_calls = 1;
        data_opts.trunk_tune_private_calls = 1;
        data_opts.trunk_tune_data_calls = 0;
        data_opts.trunk_tune_enc_calls = 1;
        data_st.p25_cc_freq = 851000000;
        seed_fdma_iden(&data_st, id);
        p25_sm_init(&data_opts, &data_st);

        before = data_st.p25_sm_tune_count;
        p25_sm_on_group_data_grant(&data_opts, &data_st, ch, /*svc*/ 0x00, /*tg*/ 3100, /*src*/ 4100);
        rc |= expect_true("group data clear svc blocked when data disabled", data_st.p25_sm_tune_count == before);
        p25_sm_on_indiv_data_grant(&data_opts, &data_st, ch, P25_SM_SVC_UNKNOWN, /*dst*/ 3101, /*src*/ 4101);
        rc |= expect_true("indiv data unknown svc blocked when data disabled", data_st.p25_sm_tune_count == before);

        data_opts.trunk_tune_data_calls = 1;
        data_opts.p25_is_tuned = 0;
        p25_sm_on_group_data_grant(&data_opts, &data_st, ch, /*svc*/ 0x00, /*tg*/ 3102, /*src*/ 4102);
        rc |= expect_true("group data clear svc tunes when data enabled", data_st.p25_sm_tune_count == before + 1);
        p25_sm_on_release(&data_opts, &data_st);
        data_opts.p25_is_tuned = 0;
        before = data_st.p25_sm_tune_count;
        p25_sm_on_indiv_data_grant(&data_opts, &data_st, ch, P25_SM_SVC_UNKNOWN, /*dst*/ 3103, /*src*/ 4103);
        rc |= expect_true("indiv data unknown svc tunes when data enabled", data_st.p25_sm_tune_count == before + 1);

        p25_sm_on_release(&data_opts, &data_st);
        data_opts.trunk_tune_enc_calls = 0;
        data_opts.p25_is_tuned = 0;
        before = data_st.p25_sm_tune_count;
        p25_sm_on_group_data_grant(&data_opts, &data_st, ch, /*svc*/ 0x40, /*tg*/ 3104, /*src*/ 4104);
        rc |= expect_true("group data raw enc bit blocks when enc disabled", data_st.p25_sm_tune_count == before);
        rc |=
            expect_true("group data raw enc bit does not arm voice enc cache", enc_tg_cache_is_absent(&data_st, 3104U));
        p25_sm_on_group_grant(&data_opts, &data_st, ch, P25_SM_SVC_UNKNOWN, /*tg*/ 3104, /*src*/ 4105);
        rc |= expect_true("svc-less voice grant not skipped after encrypted data grant",
                          data_st.p25_sm_tune_count == before + 1);
        p25_sm_on_release(&data_opts, &data_st);

        p25_emit_enc_lockout_once(&data_opts, &data_st, 0, 3105, /*svc*/ 0x40);
        rc |= expect_true("seed transient voice enc cache", !enc_tg_cache_is_absent(&data_st, 3105U));
        before = data_st.p25_sm_tune_count;
        data_opts.p25_is_tuned = 0;
        p25_sm_on_group_data_grant(&data_opts, &data_st, ch, P25_SM_SVC_UNKNOWN, /*tg*/ 3105, /*src*/ 4106);
        rc |= expect_true("svc-less group data grant ignores voice enc cache", data_st.p25_sm_tune_count == before + 1);
        rc |= expect_true("svc-less group data grant preserves voice enc cache",
                          !enc_tg_cache_is_absent(&data_st, 3105U));
        p25_sm_on_release(&data_opts, &data_st);

        data_opts.trunk_tune_data_calls = 0;
        data_opts.p25_is_tuned = 0;
        before = data_st.p25_sm_tune_count;
        p25_sm_on_group_data_grant(&data_opts, &data_st, ch, /*svc*/ 0x00, /*tg*/ 3105, /*src*/ 4107);
        rc |= expect_true("clear group data grant blocked when data disabled", data_st.p25_sm_tune_count == before);
        rc |= expect_true("clear group data grant preserves voice enc cache", !enc_tg_cache_is_absent(&data_st, 3105U));
        p25_sm_on_group_grant(&data_opts, &data_st, ch, P25_SM_SVC_UNKNOWN, /*tg*/ 3105, /*src*/ 4108);
        rc |= expect_true("svc-less voice grant still skipped after clear data grant",
                          data_st.p25_sm_tune_count == before);
        p25_sm_init(&opts, &st);
    }

    // Priority preemption: preempt-flagged low-priority candidate does not displace.
    rc |= expect_true("seed active high", seed_exact(&st, 1200, "A", "ACTIVE-HIGH", 80, 0) == 0);
    rc |= expect_true("seed candidate low preempt", seed_exact(&st, 1201, "A", "CAND-LOW", 10, 1) == 0);
    p25_sm_on_release(&opts, &st);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1200, /*src*/ 2200);
    rc |= expect_true("active high tuned", st.p25_sm_tune_count == before + 1);
    before = st.p25_sm_tune_count;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1201, /*src*/ 2201);
    rc |= expect_true("low preempt candidate blocked", st.p25_sm_tune_count == before);

    // Priority preemption: higher-priority candidate without preempt flag does not displace.
    rc |= expect_true("seed candidate high no-preempt", seed_exact(&st, 1203, "A", "CAND-HIGH-NP", 95, 0) == 0);
    before = st.p25_sm_tune_count;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1203, /*src*/ 2203);
    rc |= expect_true("high candidate without preempt flag blocked", st.p25_sm_tune_count == before);

    // Priority preemption: higher-priority preempt candidate displaces.
    rc |= expect_true("seed candidate high preempt", seed_exact(&st, 1202, "A", "CAND-HIGH", 95, 1) == 0);
    before = st.p25_sm_tune_count;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1202, /*src*/ 2202);
    rc |= expect_true("high preempt candidate tuned", st.p25_sm_tune_count == before + 1);

    // Patch-member priority/preempt displaces using the matched member policy, not the OTA SG's default priority.
    p25_sm_on_release(&opts, &st);
    rc |= expect_true("seed patch active base", seed_exact(&st, 1500, "A", "PATCH-ACTIVE", 80, 0) == 0);
    rc |= expect_true("seed patch member preempt", seed_exact(&st, 1502, "A", "PATCH-PREEMPT", 95, 1) == 0);
    p25_patch_add_wgid(&st, 1501, 1502);
    before = st.p25_sm_tune_count;
    opts.p25_is_tuned = 0;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1500, /*src*/ 2500);
    rc |= expect_true("patch preempt active tuned", st.p25_sm_tune_count == before + 1);
    before = st.p25_sm_tune_count;
    p25_sm_on_group_grant(&opts, &st, ch, /*svc*/ 0x00, /*tg*/ 1501, /*src*/ 2501);
    rc |= expect_true("patch member preempt tunes", st.p25_sm_tune_count == before + 1);
    rc |= expect_true("patch member preempt policy tg", st.p25_policy_tg[0] == 1502U);

    // Same-frequency TDMA grants update one slot without clearing the other slot's patch policy mapping.
    {
        static dsd_opts dual_opts;
        static dsd_state dual_st;
        DSD_MEMSET(&dual_opts, 0, sizeof dual_opts);
        DSD_MEMSET(&dual_st, 0, sizeof dual_st);
        dual_opts.p25_trunk = 1;
        dual_opts.trunk_tune_group_calls = 1;
        dual_opts.trunk_tune_private_calls = 1;
        dual_opts.trunk_tune_data_calls = 1;
        dual_opts.trunk_tune_enc_calls = 1;
        dual_opts.trunk_use_allow_list = 1;
        dual_st.p25_cc_freq = 851000000;
        seed_tdma_iden(&dual_st, id);
        p25_sm_init(&dual_opts, &dual_st);

        rc |= expect_true("seed dual slot member 0", seed_exact(&dual_st, 1502, "A", "SLOT0-MEMBER", 0, 0) == 0);
        rc |= expect_true("seed dual slot member 1", seed_exact(&dual_st, 1602, "A", "SLOT1-MEMBER", 0, 0) == 0);
        p25_patch_add_wgid(&dual_st, 1501, 1502);
        p25_patch_add_wgid(&dual_st, 1601, 1602);

        p25_sm_on_group_grant(&dual_opts, &dual_st, 0x100B, /*svc*/ 0x00, /*tg*/ 1601, /*src*/ 2601);
        p25_sm_ctx_t* dual_ctx = p25_sm_get_ctx();
        unsigned dual_tunes_after_slot1 = dual_st.p25_sm_tune_count;
        rc |= expect_true("dual slot1 policy tg stored", dual_st.p25_policy_tg[1] == 1602U);
        rc |= expect_true("dual slot1 grant context",
                          dual_ctx->slots[1].grant_active && dual_ctx->slots[1].target_id == 1602);
        dual_ctx->slots[1].voice_active = 1;
        dual_ctx->slots[1].last_active_m = 1.0;
        dual_st.p25_p2_audio_allowed[1] = 1;

        p25_sm_on_group_grant(&dual_opts, &dual_st, 0x100A, /*svc*/ 0x00, /*tg*/ 1501, /*src*/ 2600);
        rc |= expect_true("dual slot0 same-carrier no retune", dual_st.p25_sm_tune_count == dual_tunes_after_slot1);
        rc |= expect_true("dual slot0 same-carrier active slot", dual_st.p25_p2_active_slot == 0);
        rc |= expect_true("dual slot0 policy tg stored", dual_st.p25_policy_tg[0] == 1502U);
        rc |= expect_true("dual slot1 policy tg preserved", dual_st.p25_policy_tg[1] == 1602U);
        rc |= expect_true("dual slot1 active preserved",
                          dual_ctx->slots[1].voice_active == 1 && dual_ctx->slots[1].target_id == 1602);
        rc |= expect_true("dual slot0 grant context",
                          dual_ctx->slots[0].grant_active && dual_ctx->slots[0].target_id == 1502);

        rc |= expect_true("seed same-slot replacement", seed_exact(&dual_st, 1701, "A", "SLOT0-REPL", 0, 0) == 0);
        p25_sm_on_group_grant(&dual_opts, &dual_st, 0x100A, /*svc*/ 0x00, /*tg*/ 1701, /*src*/ 2701);
        rc |= expect_true("dual same-slot replacement no retune", dual_st.p25_sm_tune_count == dual_tunes_after_slot1);
        rc |= expect_true("dual same-slot replacement target",
                          dual_ctx->slots[0].grant_active && dual_ctx->slots[0].target_id == 1701);
        rc |= expect_true("dual same-slot preserves other slot",
                          dual_ctx->slots[1].voice_active == 1 && dual_ctx->slots[1].target_id == 1602);

        dual_ctx->slots[1].voice_active = 0;
        dual_ctx->slots[1].last_active_m = 0.0;
        dual_st.p25_p2_audio_allowed[1] = 0;
        p25_sm_on_group_grant(&dual_opts, &dual_st, 0x100B, /*svc*/ 0x00, /*tg*/ 1701, /*src*/ 2702);
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
        namespace_opts.p25_trunk = 1;
        namespace_opts.trunk_tune_group_calls = 1;
        namespace_opts.trunk_tune_private_calls = 1;
        namespace_opts.trunk_tune_data_calls = 1;
        namespace_opts.trunk_tune_enc_calls = 1;
        namespace_opts.trunk_use_allow_list = 1;
        namespace_st.p25_cc_freq = 851000000;
        seed_tdma_iden(&namespace_st, id);
        p25_sm_init(&namespace_opts, &namespace_st);

        rc |= expect_true("seed namespace group", seed_exact(&namespace_st, 1234, "A", "GROUP-SAME-RID", 0, 0) == 0);
        p25_sm_on_indiv_grant(&namespace_opts, &namespace_st, 0x100A, /*svc*/ 0x00, /*dst*/ 1234, /*src*/ 4234);
        p25_sm_ctx_t* namespace_ctx = p25_sm_get_ctx();
        unsigned namespace_tunes_after_private = namespace_st.p25_sm_tune_count;
        rc |= expect_true("namespace private slot0 stored",
                          namespace_ctx->slots[0].grant_active && namespace_ctx->slots[0].target_id == 1234
                              && namespace_ctx->slots[0].is_group == 0 && namespace_ctx->slots[0].dst == 1234);
        namespace_ctx->slots[0].voice_active = 1;
        namespace_ctx->slots[0].last_active_m = 1.0;
        namespace_st.p25_p2_audio_allowed[0] = 1;

        p25_sm_on_group_grant(&namespace_opts, &namespace_st, 0x100B, /*svc*/ 0x00, /*tg*/ 1234, /*src*/ 5234);
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

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
