// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Verify policy-backed P25 grant filtering behavior in the trunk SM path. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

struct RtlSdrContext;

bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    row.priority = priority;
    row.preempt = preempt ? 1u : 0u;
    return dsd_tg_policy_upsert_legacy_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);

    opts.p25_trunk = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_private_calls = 1;
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_enc_calls = 1;
    opts.trunk_use_allow_list = 1;
    st.p25_cc_freq = 851000000;

    // FDMA IDEN
    int id = 1;
    st.p25_chan_iden = id;
    st.p25_chan_type[id] = 1;
    st.p25_chan_tdma[id] = 0;
    st.p25_base_freq[id] = 851000000 / 5;
    st.p25_chan_spac[id] = 100;
    st.p25_iden_trust[id] = 2;
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

    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS");
    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS");

    return rc;
}
