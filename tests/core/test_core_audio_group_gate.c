// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test for talkgroup/whitelist/TG-hold audio gating.
 *
 * Ensures dual-slot gating keeps allowed traffic audible while muting blocked
 * or non-held traffic.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
append_policy_group(dsd_state* st, uint32_t tg, const char* mode, const char* name, uint8_t audio, uint8_t record,
                    uint8_t stream) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(tg, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    row.audio = audio ? 1u : 0u;
    row.record = record ? 1u : 0u;
    row.stream = stream ? 1u : 0u;
    return dsd_tg_policy_append_legacy_exact(st, &row);
}

static int
seed_policy_group(dsd_state* st, uint32_t tg, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(tg, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_append_legacy_exact(st, &row);
}

static void
reset_state(dsd_state* st) {
    if (!st) {
        return;
    }
    dsd_state_ext_free_all(st);
    memset(st, 0, sizeof(*st));
}

int
main(void) {
    int rc = 0;

    // dsd_opts is sizeable enough to keep off the function stack in tests.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts) {
        fprintf(stderr, "alloc-failed: dsd_opts\n");
        return 1;
    }

    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!st) {
        fprintf(stderr, "alloc-failed: dsd_state\n");
        free(opts);
        return 1;
    }

    // Case 1: Explicit block list on slot R while slot L remains allowed.
    rc |= expect_eq("case1-seed-a", seed_policy_group(st, 100U, "A", "ALLOW"), 0);
    rc |= expect_eq("case1-seed-b", seed_policy_group(st, 200U, "B", "BLOCK"), 0);
    {
        int outL = -1, outR = -1;
        rc |= expect_eq("case1-ret", dsd_audio_group_gate_dual(opts, st, 100UL, 200UL, 0, 0, &outL, &outR), 0);
        rc |= expect_eq("case1-outL", outL, 0);
        rc |= expect_eq("case1-outR", outR, 1);
    }

    // Case 2: Allow-list mode defaults unknown TGs to blocked.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case2-seed-a", seed_policy_group(st, 300U, "A", "ONLY"), 0);
    {
        int outL = -1, outR = -1;
        rc |= expect_eq("case2-ret", dsd_audio_group_gate_dual(opts, st, 300UL, 301UL, 0, 0, &outL, &outR), 0);
        rc |= expect_eq("case2-outL", outL, 0);
        rc |= expect_eq("case2-outR", outR, 1);
    }

    // Case 2b: "DE" lockout mode should be treated as blocked by audio gate.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case2b-seed-a", seed_policy_group(st, 310U, "DE", "ENC-LOCKOUT"), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2b-ret", dsd_audio_group_gate_mono(opts, st, 310UL, 0, &outL), 0);
        rc |= expect_eq("case2b-outL", outL, 1);
    }

    // Case 2c: Explicit audio=off policy blocks audio.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case2c-append", append_policy_group(st, 311U, "A", "AUDIO-OFF", 0, 1, 1), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2c-ret", dsd_audio_group_gate_mono(opts, st, 311UL, 0, &outL), 0);
        rc |= expect_eq("case2c-outL", outL, 1);
    }

    // Case 2d: Disabling group-call tuning does not mute already-routed audio.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_tune_group_calls = 0;
    rc |= expect_eq("case2d-seed-a", seed_policy_group(st, 312U, "A", "GROUP-DISABLED"), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2d-ret", dsd_audio_group_gate_mono(opts, st, 312UL, 0, &outL), 0);
        rc |= expect_eq("case2d-outL", outL, 0);
    }

    // Case 2e: Disabling data-call tuning does not mute already-routed audio.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_tune_data_calls = 0;
    rc |= expect_eq("case2e-seed-a", seed_policy_group(st, 313U, "A", "DATA-DISABLED"), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2e-ret", dsd_audio_group_gate_mono(opts, st, 313UL, 0, &outL), 0);
        rc |= expect_eq("case2e-outL", outL, 0);
    }

    // Case 3: TG hold mutes non-matching slot and force-unmutes matching slot.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case3-seed-a", seed_policy_group(st, 400U, "A", "LEFT"), 0);
    rc |= expect_eq("case3-seed-b", seed_policy_group(st, 401U, "B", "RIGHT-BLOCKED"), 0);
    st->tg_hold = 401U;
    {
        int outL = -1, outR = -1;
        rc |= expect_eq("case3-ret", dsd_audio_group_gate_dual(opts, st, 400UL, 401UL, 0, 0, &outL, &outR), 0);
        rc |= expect_eq("case3-outL", outL, 1);
        rc |= expect_eq("case3-outR", outR, 0);
    }

    // Case 3b: Encrypted baseline stays muted unless matching TG hold force-unmutes.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case3b-seed-a", seed_policy_group(st, 410U, "A", "BASELINE"), 0);
    {
        int outL = -1;
        rc |= expect_eq("case3b-ret-a", dsd_audio_group_gate_mono(opts, st, 410UL, 1, &outL), 0);
        rc |= expect_eq("case3b-out-a", outL, 1);
        st->tg_hold = 410U;
        rc |= expect_eq("case3b-ret-b", dsd_audio_group_gate_mono(opts, st, 410UL, 1, &outL), 0);
        rc |= expect_eq("case3b-out-b", outL, 0);
    }

    // Defensive API contract checks.
    {
        int out = 0;
        rc |= expect_eq("null-mono", dsd_audio_group_gate_mono(NULL, st, 0UL, 0, &out), -1);
        rc |= expect_eq("null-dual", dsd_audio_group_gate_dual(opts, st, 0UL, 0UL, 0, 0, NULL, &out), -1);
        rc |= expect_eq("null-record-mono", dsd_audio_record_gate_mono(opts, NULL, &out), -1);
    }

    // Case 4: Mono per-call recording gate respects block mode.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    st->lasttg = 500UL;
    st->dmr_encL = 0;
    rc |= expect_eq("case4-seed-a", seed_policy_group(st, 500U, "B", "REC-BLOCK"), 0);
    {
        int allow = -1;
        rc |= expect_eq("case4-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case4-rec-allow", allow, 0);
    }

    // Case 4b: record=off blocks recording while audio remains allowed.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    st->lasttg = 520UL;
    st->dmr_encL = 0;
    rc |= expect_eq("case4b-append", append_policy_group(st, 520U, "A", "REC-OFF", 1, 0, 1), 0);
    {
        int out = -1;
        int allow = -1;
        rc |= expect_eq("case4b-aud-ret", dsd_audio_group_gate_mono(opts, st, 520UL, 0, &out), 0);
        rc |= expect_eq("case4b-aud-out", out, 0);
        rc |= expect_eq("case4b-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case4b-rec-allow", allow, 0);
    }

    // Case 4c: Matching TG hold overrides explicit media-off policy; non-match blocks.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    st->lasttg = 530UL;
    st->dmr_encL = 0;
    rc |= expect_eq("case4c-append", append_policy_group(st, 530U, "A", "MEDIA-OFF", 0, 0, 0), 0);
    {
        int out = -1;
        int allow = -1;
        st->tg_hold = 530U;
        rc |= expect_eq("case4c-aud-ret-a", dsd_audio_group_gate_mono(opts, st, 530UL, 1, &out), 0);
        rc |= expect_eq("case4c-aud-out-a", out, 0);
        rc |= expect_eq("case4c-rec-ret-a", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case4c-rec-allow-a", allow, 1);
        st->tg_hold = 531U;
        rc |= expect_eq("case4c-aud-ret-b", dsd_audio_group_gate_mono(opts, st, 530UL, 0, &out), 0);
        rc |= expect_eq("case4c-aud-out-b", out, 1);
        rc |= expect_eq("case4c-rec-ret-b", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case4c-rec-allow-b", allow, 0);
    }

    // Case 5: Mono per-call recording gate uses slot-specific TG/encryption state.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    st->currentslot = 1;
    st->lasttg = 600UL;
    st->lasttgR = 601UL;
    st->dmr_encL = 1;
    st->dmr_encR = 0;
    rc |= expect_eq("case5-seed-a", seed_policy_group(st, 601U, "A", "RIGHT-ONLY"), 0);
    {
        int allow = -1;
        rc |= expect_eq("case5-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case5-rec-allow", allow, 1);
    }

    // Case 5b: Slot-specific DMR encrypted-mute flags gate recording baseline.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    st->currentslot = 1;
    st->lasttgR = 611UL;
    st->dmr_encR = 1;
    rc |= expect_eq("case5b-seed-a", seed_policy_group(st, 611U, "A", "ENC-RIGHT"), 0);
    {
        int allow = -1;
        opts->dmr_mute_encR = 1;
        rc |= expect_eq("case5b-rec-ret-a", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case5b-rec-allow-a", allow, 0);
        opts->dmr_mute_encR = 0;
        rc |= expect_eq("case5b-rec-ret-b", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case5b-rec-allow-b", allow, 1);
    }

    // Case 6: P25 Phase 2 recording gate follows the per-slot audio-allowed flag.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    st->synctype = DSD_SYNC_P25P2_POS;
    st->currentslot = 1;
    st->p25_p2_audio_allowed[0] = 0;
    st->p25_p2_audio_allowed[1] = 1;
    {
        int allow = -1;
        rc |= expect_eq("case6-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case6-rec-allow", allow, 1);
    }

    // Case 7: P25p2 decode gate preserves matching TG-hold media override.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case7-seed-b", seed_policy_group(st, 700U, "B", "HELD-BLOCKED"), 0);
    st->lasttg = 700;
    st->lastsrc = 701;
    st->gi[0] = 0;
    st->tg_hold = 700U;
    rc |= expect_eq("case7-held-decode", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 1);
    st->tg_hold = 702U;
    rc |= expect_eq("case7-nonheld-decode", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);

    // Case 8: P25p2 private decode gate evaluates source and target RIDs.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case8-seed-src", seed_policy_group(st, 9002U, "A", "SRC-ALLOW"), 0);
    st->gi[0] = 1;
    st->lasttg = 9001;
    st->lastsrc = 9002;
    rc |= expect_eq("case8-src-allow", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 1);
    st->lastsrc = 9003;
    rc |= expect_eq("case8-unknown-private-block", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);
    st->tg_hold = 9003U;
    rc |= expect_eq("case8-source-hold", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 1);

    // Case 9: P25p2 decode gate fails closed under media policy when call metadata is unknown.
    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    st->gi[0] = 0;
    rc |= expect_eq("case9-unknown-group-allowlist", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);
    st->gi[0] = 1;
    rc |= expect_eq("case9-unknown-private-allowlist", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);

    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    st->tg_hold = 9100U;
    rc |= expect_eq("case9-unknown-hold", dsd_p25p2_decode_audio_allowed(opts, st, 1, 0), 0);

    memset(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case9-seed-source", seed_policy_group(st, 9200U, "A", "SRC-ALLOW"), 0);
    st->lastsrc = 9200;
    st->lasttg = 0;
    st->gi[0] = 0;
    rc |= expect_eq("case9-target-zero-gi-unset", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);

    if (rc == 0) {
        printf("CORE_AUDIO_GROUP_GATE: OK\n");
    }
    dsd_state_ext_free_all(st);
    free(opts);
    free(st);
    return rc;
}
