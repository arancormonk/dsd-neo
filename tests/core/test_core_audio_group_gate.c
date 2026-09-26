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
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
append_policy_group(dsd_state* st, uint32_t tg, const char* mode, const char* name, uint8_t audio, uint8_t record,
                    uint8_t stream) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(tg, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    row.audio = audio ? 1u : 0u;
    row.record = record ? 1u : 0u;
    row.stream = stream ? 1u : 0u;
    return dsd_tg_policy_append_exact(st, &row);
}

static int
seed_policy_group(dsd_state* st, uint32_t tg, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(tg, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_append_exact(st, &row);
}

static void
seed_active_patch_member(dsd_state* st, uint16_t sg, uint16_t wg) {
    st->p25_patch_count = 1;
    st->p25_patch_sgid[0] = sg;
    st->p25_patch_is_patch[0] = 1U;
    st->p25_patch_active[0] = 1U;
    st->p25_patch_last_update[0] = time(NULL);
    st->p25_patch_wgid_count[0] = 1U;
    st->p25_patch_wgid[0][0] = wg;
}

static void
reset_state(dsd_state* st) {
    if (!st) {
        return;
    }
    dsd_state_ext_free_all(st);
    DSD_MEMSET(st, 0, sizeof(*st));
}

static int
seed_call(dsd_state* st, uint8_t slot, int protocol, dsd_call_kind kind, uint64_t ota_target, uint64_t policy_target,
          uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = slot,
        .kind = kind,
        .ota_target_id = ota_target,
        .policy_target_id = policy_target,
        .ota_source_id = source,
        .observed_m = 1.0,
    };
    return dsd_call_state_observe(st, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

static int
test_private_audio_policy(dsd_opts* opts, dsd_state* st) {
    const int protocols[] = {DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_NXDN_POS, DSD_SYNC_P25P1_POS, DSD_SYNC_P25P2_POS};
    int rc = 0;
    for (size_t protocol = 0; protocol < sizeof(protocols) / sizeof(protocols[0]); ++protocol) {
        for (uint8_t slot = 0; slot < 2; ++slot) {
            DSD_MEMSET(opts, 0, sizeof(*opts));
            reset_state(st);
            st->synctype = protocols[protocol];
            st->currentslot = slot;
            rc |= expect_eq("private-policy-call",
                            seed_call(st, slot, st->synctype, DSD_CALL_KIND_PRIVATE_VOICE, 123, 123, 456), 1);
            // The companion group call has the same target, but the source RID block must not apply to it.
            rc |= expect_eq("private-policy-companion",
                            seed_call(st, slot ^ 1U, st->synctype, DSD_CALL_KIND_GROUP_VOICE, 123, 123, 456), 1);
            for (int temporary = 0; temporary < 2; ++temporary) {
                rc |= expect_eq("private-policy-clear", dsd_tg_policy_clear(st), 0);
                rc |= expect_eq("private-source-block",
                                temporary ? dsd_tg_policy_session_avoid_add(st, 456)
                                          : seed_policy_group(st, 456, "B", "Blocked source"),
                                0);
                int muted = -1;
                int left = -1;
                int right = -1;
                rc |= expect_eq("private-source-mono", dsd_audio_group_gate_mono(opts, st, 123, 0, &muted), 0);
                rc |= expect_eq("private-source-mono-muted", muted, 1);
                rc |= expect_eq("private-source-dual",
                                dsd_audio_group_gate_dual(opts, st, 123, 123, 0, 0, &left, &right), 0);
                rc |= expect_eq("private-source-left", left, slot == 0);
                rc |= expect_eq("private-source-right", right, slot == 1);
            }
            rc |= expect_eq("private-policy-reset", dsd_tg_policy_clear(st), 0);
            opts->trunk_use_allow_list = 1;
            rc |= expect_eq("private-source-allow", seed_policy_group(st, 456, "A", "Allowed source"), 0);
            int muted = -1;
            rc |= expect_eq("private-source-allow-mono", dsd_audio_group_gate_mono(opts, st, 123, 0, &muted), 0);
            rc |= expect_eq("private-source-allow-audible", muted, 0);
        }
    }
    return rc;
}

static int
test_session_avoid_overrides_hold(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        DSD_MEMSET(opts, 0, sizeof(*opts));
        reset_state(st);
        st->synctype = DSD_SYNC_P25P2_POS;
        st->currentslot = slot;
        st->tg_hold = 123;
        st->p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
        st->p25_p2_audio_allowed[slot] = 1;
        rc |= expect_eq("avoid-held-call",
                        seed_call(st, slot, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 123, 123, 456), 1);
        rc |= expect_eq("avoid-held-add", dsd_tg_policy_session_avoid_add(st, 123), 0);
        for (int avoided = 1; avoided >= 0; --avoided) {
            int muted = -1;
            int left = -1;
            int right = -1;
            int record = -1;
            rc |= expect_eq("avoid-held-mono", dsd_audio_group_gate_mono(opts, st, 123, 0, &muted), 0);
            rc |= expect_eq("avoid-held-mono-muted", muted, avoided);
            rc |= expect_eq("avoid-held-dual", dsd_audio_group_gate_dual(opts, st, 123, 123, 0, 0, &left, &right), 0);
            rc |= expect_eq("avoid-held-left-muted", left, avoided);
            rc |= expect_eq("avoid-held-right-muted", right, avoided);
            rc |= expect_eq("avoid-held-p25-decode", dsd_p25p2_decode_audio_allowed(opts, st, slot, 0), !avoided);
            rc |= expect_eq("avoid-held-record", dsd_audio_record_gate_mono(opts, st, &record), 0);
            rc |= expect_eq("avoid-held-record-allowed", record, !avoided);
            dsd_tg_policy_session_avoid_clear(st);
        }
        // A held private source must not unmute an avoided destination, or vice versa.
        rc |= expect_eq("avoid-private-call",
                        seed_call(st, slot, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_PRIVATE_VOICE, 123, 123, 456), 1);
        for (uint32_t endpoint = 123; endpoint <= 456; endpoint += 333) {
            st->tg_hold = endpoint == 123 ? 456 : 123;
            rc |= expect_eq("avoid-private-add", dsd_tg_policy_session_avoid_add(st, endpoint), 0);
            rc |= expect_eq("avoid-private-decode", dsd_p25p2_decode_audio_allowed(opts, st, slot, 0), 0);
            int record = -1;
            rc |= expect_eq("avoid-private-record", dsd_audio_record_gate_mono(opts, st, &record), 0);
            rc |= expect_eq("avoid-private-record-blocked", record, 0);
            dsd_tg_policy_session_avoid_clear(st);
            rc |= expect_eq("avoid-private-cleared", dsd_p25p2_decode_audio_allowed(opts, st, slot, 0), 1);
        }
    }
    return rc;
}

static int
test_call_skip_mutes_slot(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        for (int kind = 0; kind < 4; ++kind) {
            DSD_MEMSET(opts, 0, sizeof(*opts));
            reset_state(st);
            opts->trunk_tune_group_calls = opts->trunk_tune_private_calls = 1;
            st->synctype = DSD_SYNC_P25P2_POS;
            st->currentslot = slot;
            st->tg_hold = 123;
            st->p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
            st->p25_p2_audio_allowed[slot] = 1;
            const uint32_t ota = kind == 3 ? 9000 : 123;
            const uint32_t skipped = kind == 2 ? 456 : ota;
            if (kind == 3) {
                seed_active_patch_member(st, 9000, 123);
            }
            rc |= expect_eq("skip seed call",
                            seed_call(st, slot, DSD_SYNC_P25P2_POS,
                                      kind == 1 || kind == 2 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE,
                                      ota, 123, 456),
                            1);
            for (int blocked = 1; blocked >= 0; --blocked) {
                const double now = dsd_time_now_monotonic_s();
                rc |= expect_eq("skip arm current or expired",
                                dsd_tg_policy_call_skip_arm(st, skipped, 456, 0,
                                                            blocked ? now : now - DSD_TG_CALL_SKIP_QUIET_S - 1.0),
                                0);
                int muted = -1, left = -1, right = -1, record = -1;
                rc |= expect_eq("skip mono", dsd_audio_group_gate_mono(opts, st, ota, 0, &muted), 0);
                rc |= expect_eq("skip mono verdict", muted, blocked);
                rc |= expect_eq(
                    "skip dual",
                    dsd_audio_group_gate_dual(opts, st, slot == 0 ? ota : 0, slot == 1 ? ota : 0, 0, 0, &left, &right),
                    0);
                rc |= expect_eq("skip dual verdict", slot == 0 ? left : right, blocked);
                rc |= expect_eq("skip decode verdict", dsd_p25p2_decode_audio_allowed(opts, st, slot, 0), !blocked);
                rc |= expect_eq("skip record", dsd_audio_record_gate_mono(opts, st, &record), 0);
                rc |= expect_eq("skip record verdict", record, !blocked);
            }
        }
    }
    return rc;
}

// Reverse mute (-q) keeps a clear P25 call out of the per-call WAV as it keeps it
// out of the speakers, on Phase 1 and on each Phase 2 slot; a decryptable call
// still records.
static int
test_p25_record_gate_honors_reverse_mute(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int phase2 = 0; phase2 <= 1; ++phase2) {
        const int synctype = phase2 ? DSD_SYNC_P25P2_POS : DSD_SYNC_P25P1_POS;
        for (int slot = 0; slot < (phase2 ? 2 : 1); ++slot) {
            for (int crypto = 0; crypto < 2; ++crypto) {
                for (int reverse = 0; reverse <= 1; ++reverse) {
                    DSD_MEMSET(opts, 0, sizeof(*opts));
                    reset_state(st);
                    opts->reverse_mute = reverse;
                    st->synctype = synctype;
                    st->currentslot = slot;
                    st->p25_crypto_state[slot] = crypto ? DSD_P25_CRYPTO_DECRYPTABLE : DSD_P25_CRYPTO_CLEAR;
                    st->p25_p2_audio_allowed[slot] = 1;
                    rc |=
                        expect_eq("p25 record call",
                                  seed_call(st, (uint8_t)slot, synctype, DSD_CALL_KIND_GROUP_VOICE, 123, 123, 456), 1);
                    int record = -1;
                    rc |= expect_eq("p25 record gate", dsd_audio_record_gate_mono(opts, st, &record), 0);
                    rc |= expect_eq(reverse && !crypto ? "reverse mute keeps clear p25 out of the wav"
                                                       : "p25 call records",
                                    record, !(reverse && !crypto));
                }
            }
        }
    }
    return rc;
}

// The P25 recording rule is for P25 calls: YSF full rate borrows the Phase 1
// decoder (synctype P25P1) for a YSF call that has no P25 crypto state, and its
// WAV still records; the P25-only unmute override does not reopen a DMR slot
// that reverse mute (or encryption) muted.
static int
test_record_gate_protocol_boundaries(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("ysf reuse call", seed_call(st, 0U, DSD_SYNC_YSF_POS, DSD_CALL_KIND_GROUP_VOICE, 0, 0, 0), 1);
    st->synctype = DSD_SYNC_P25P1_POS;
    opts->reverse_mute = 1;
    int record = -1;
    rc |= expect_eq("ysf reuse record gate", dsd_audio_record_gate_mono(opts, st, &record), 0);
    rc |= expect_eq("ysf full rate through the p25 decoder still records", record, 1);

    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("dmr muted call",
                    seed_call(st, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 123, 123, 456), 1);
    st->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    st->dmr_encL = 1;
    opts->dmr_mute_encL = 1;
    opts->unmute_encrypted_p25 = 1;
    record = -1;
    rc |= expect_eq("dmr muted record gate", dsd_audio_record_gate_mono(opts, st, &record), 0);
    rc |= expect_eq("p25 unmute override leaves a muted dmr slot unrecorded", record, 0);
    return rc;
}

// A patched call is judged on the member WG its grant matched, but a user block
// on the over-the-air supergroup itself is final: it mutes the member-matched
// call too (a Lock out whose return to the CC fails leaves the call running).
// An allow-list miss on the supergroup is not a block on it.
static int
test_supergroup_block_mutes_patched_member(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        for (int block = 0; block < 3; ++block) {
            DSD_MEMSET(opts, 0, sizeof(*opts));
            reset_state(st);
            opts->trunk_tune_group_calls = 1;
            st->synctype = DSD_SYNC_P25P2_POS;
            st->currentslot = slot;
            st->p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
            st->p25_p2_audio_allowed[slot] = 1;
            seed_active_patch_member(st, 9000, 123);
            rc |= expect_eq("sg block member row", seed_policy_group(st, 123U, "A", "MEMBER"), 0);
            if (block == 0) {
                rc |= expect_eq("sg block mode row", seed_policy_group(st, 9000U, "B", "SG"), 0);
            } else if (block == 1) {
                rc |= expect_eq("sg block session avoid", dsd_tg_policy_session_avoid_add(st, 9000U), 0);
            } else {
                opts->trunk_use_allow_list = 1;
            }
            const int blocked = block != 2;
            rc |= expect_eq("sg block seed call",
                            seed_call(st, slot, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 9000, 123, 456), 1);
            int muted = -1, left = -1, right = -1, record = -1;
            rc |= expect_eq("sg block mono", dsd_audio_group_gate_mono(opts, st, 9000, 0, &muted), 0);
            rc |= expect_eq("sg block mono verdict", muted, blocked);
            rc |= expect_eq(
                "sg block dual",
                dsd_audio_group_gate_dual(opts, st, slot == 0 ? 9000 : 0, slot == 1 ? 9000 : 0, 0, 0, &left, &right),
                0);
            rc |= expect_eq("sg block dual verdict", slot == 0 ? left : right, blocked);
            rc |= expect_eq("sg block decode verdict", dsd_p25p2_decode_audio_allowed(opts, st, slot, 0), !blocked);
            rc |= expect_eq("sg block record", dsd_audio_record_gate_mono(opts, st, &record), 0);
            rc |= expect_eq("sg block record verdict", record, !blocked);
        }
    }
    return rc;
}

int
main(void) {
    int rc = 0;

    // dsd_opts is sizeable enough to keep off the function stack in tests.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts) {
        DSD_FPRINTF(stderr, "alloc-failed: dsd_opts\n");
        return 1;
    }

    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!st) {
        DSD_FPRINTF(stderr, "alloc-failed: dsd_state\n");
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
    DSD_MEMSET(opts, 0, sizeof(*opts));
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
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case2b-seed-a", seed_policy_group(st, 310U, "DE", "ENC-LOCKOUT"), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2b-ret", dsd_audio_group_gate_mono(opts, st, 310UL, 0, &outL), 0);
        rc |= expect_eq("case2b-outL", outL, 1);
    }

    // Case 2c: Explicit audio=off policy blocks audio.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case2c-append", append_policy_group(st, 311U, "A", "AUDIO-OFF", 0, 1, 1), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2c-ret", dsd_audio_group_gate_mono(opts, st, 311UL, 0, &outL), 0);
        rc |= expect_eq("case2c-outL", outL, 1);
    }

    // Case 2d: Disabling group-call tuning does not mute already-routed audio.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_tune_group_calls = 0;
    rc |= expect_eq("case2d-seed-a", seed_policy_group(st, 312U, "A", "GROUP-DISABLED"), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2d-ret", dsd_audio_group_gate_mono(opts, st, 312UL, 0, &outL), 0);
        rc |= expect_eq("case2d-outL", outL, 0);
    }

    // Case 2e: Disabling data-call tuning does not mute already-routed audio.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_tune_data_calls = 0;
    rc |= expect_eq("case2e-seed-a", seed_policy_group(st, 313U, "A", "DATA-DISABLED"), 0);
    {
        int outL = -1;
        rc |= expect_eq("case2e-ret", dsd_audio_group_gate_mono(opts, st, 313UL, 0, &outL), 0);
        rc |= expect_eq("case2e-outL", outL, 0);
    }

    // Case 3: TG hold mutes non-matching slot and force-unmutes matching slot.
    DSD_MEMSET(opts, 0, sizeof(*opts));
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
    DSD_MEMSET(opts, 0, sizeof(*opts));
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
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    st->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    rc |= expect_eq("case4-call",
                    seed_call(st, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 500U, 500U, 501U), 1);
    st->dmr_encL = 0;
    rc |= expect_eq("case4-seed-a", seed_policy_group(st, 500U, "B", "REC-BLOCK"), 0);
    {
        int allow = -1;
        rc |= expect_eq("case4-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case4-rec-allow", allow, 0);
    }

    // Case 4b: record=off blocks recording while audio remains allowed.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    st->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    rc |= expect_eq("case4b-call",
                    seed_call(st, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 520U, 520U, 521U), 1);
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
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    st->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    rc |= expect_eq("case4c-call",
                    seed_call(st, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 530U, 530U, 531U), 1);
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
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    st->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    opts->trunk_use_allow_list = 1;
    st->currentslot = 1;
    rc |= expect_eq("case5-left-call",
                    seed_call(st, 0U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 600U, 600U, 602U), 1);
    rc |= expect_eq("case5-right-call",
                    seed_call(st, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 601U, 601U, 603U), 1);
    st->dmr_encL = 1;
    st->dmr_encR = 0;
    rc |= expect_eq("case5-seed-a", seed_policy_group(st, 601U, "A", "RIGHT-ONLY"), 0);
    {
        int allow = -1;
        rc |= expect_eq("case5-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case5-rec-allow", allow, 1);
    }

    // Case 5b: Slot-specific DMR encrypted-mute flags gate recording baseline.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    st->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    opts->trunk_use_allow_list = 1;
    st->currentslot = 1;
    rc |= expect_eq("case5b-call",
                    seed_call(st, 1U, DSD_SYNC_DMR_BS_VOICE_POS, DSD_CALL_KIND_GROUP_VOICE, 611U, 611U, 612U), 1);
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
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    st->synctype = DSD_SYNC_P25P2_POS;
    st->currentslot = 1;
    st->p25_p2_audio_allowed[0] = 0;
    st->p25_p2_audio_allowed[1] = 1;
    // A slot whose audio is allowed has classified clear or decryptable.
    st->p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    {
        int allow = -1;
        rc |= expect_eq("case6-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case6-rec-allow", allow, 1);
    }

    // Case 7: P25p2 decode gate preserves matching TG-hold media override.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    rc |= expect_eq("case7-seed-b", seed_policy_group(st, 700U, "B", "HELD-BLOCKED"), 0);
    rc |=
        expect_eq("case7-call", seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 700U, 700U, 701U), 1);
    st->tg_hold = 700U;
    st->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    rc |= expect_eq("case7-held-decode", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 1);
    st->tg_hold = 702U;
    rc |= expect_eq("case7-nonheld-decode", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);

    // Case 8: P25p2 private decode gate evaluates source and target RIDs.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case8-seed-src", seed_policy_group(st, 9002U, "A", "SRC-ALLOW"), 0);
    rc |= expect_eq("case8-call",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_PRIVATE_VOICE, 9001U, 9001U, 9002U), 1);
    st->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    rc |= expect_eq("case8-src-allow", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 1);
    rc |= expect_eq("case8-new-source",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_PRIVATE_VOICE, 9001U, 9001U, 9003U), 1);
    rc |= expect_eq("case8-unknown-private-block", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);
    st->tg_hold = 9003U;
    rc |= expect_eq("case8-source-hold", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 1);

    // Case 9: P25p2 decode gate fails closed under media policy when call metadata is unknown.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    st->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    rc |= expect_eq("case9-anon-group-call",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 0U, 0U, 0U), 1);
    rc |= expect_eq("case9-unknown-group-allowlist", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);
    rc |= expect_eq("case9-anon-private-call",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_PRIVATE_VOICE, 0U, 0U, 0U), 1);
    rc |= expect_eq("case9-unknown-private-allowlist", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);

    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    st->tg_hold = 9100U;
    rc |= expect_eq("case9-unknown-hold", dsd_p25p2_decode_audio_allowed(opts, st, 1, 0), 0);

    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case9-seed-source", seed_policy_group(st, 9200U, "A", "SRC-ALLOW"), 0);
    st->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    rc |= expect_eq("case9-source-only-call",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 0U, 0U, 9200U), 1);
    rc |= expect_eq("case9-target-zero-gi-unset", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);

    // Case 10: P25 patch-member policy targets are only honored while the OTA SG still owns the active member.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case10-seed-member", seed_policy_group(st, 9401U, "A", "PATCH-MEMBER"), 0);
    st->synctype = DSD_SYNC_P25P2_POS;
    st->currentslot = 0;
    rc |= expect_eq("case10-call",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 9400U, 9401U, 777001U), 1);
    st->p25_p2_audio_allowed[0] = 1U;
    st->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    seed_active_patch_member(st, 9400U, 9401U);
    {
        int out = -1;
        int allow = -1;
        rc |= expect_eq("case10-aud-ret-valid", dsd_audio_group_gate_mono(opts, st, 9400UL, 0, &out), 0);
        rc |= expect_eq("case10-aud-out-valid", out, 0);
        rc |= expect_eq("case10-decode-valid", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 1);
        rc |= expect_eq("case10-rec-ret-valid", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case10-rec-allow-valid", allow, 1);

        st->p25_patch_active[0] = 0U;
        rc |= expect_eq("case10-aud-ret-inactive", dsd_audio_group_gate_mono(opts, st, 9400UL, 0, &out), 0);
        rc |= expect_eq("case10-aud-out-inactive", out, 1);
        rc |= expect_eq("case10-decode-inactive", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);
        rc |= expect_eq("case10-rec-ret-inactive", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case10-rec-allow-inactive", allow, 0);
    }

    // Case 10b: A live member from another SG must not be substituted for changed slot metadata.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case10b-seed-member", seed_policy_group(st, 9501U, "A", "OTHER-PATCH-MEMBER"), 0);
    st->synctype = DSD_SYNC_P25P2_POS;
    st->currentslot = 0;
    rc |= expect_eq("case10b-call",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 9502U, 9501U, 777002U), 1);
    st->p25_p2_audio_allowed[0] = 1U;
    seed_active_patch_member(st, 9500U, 9501U);
    {
        int out = -1;
        int allow = -1;
        rc |= expect_eq("case10b-aud-ret", dsd_audio_group_gate_mono(opts, st, 9502UL, 0, &out), 0);
        rc |= expect_eq("case10b-aud-out", out, 1);
        rc |= expect_eq("case10b-decode", dsd_p25p2_decode_audio_allowed(opts, st, 0, 0), 0);
        rc |= expect_eq("case10b-rec-ret", dsd_audio_record_gate_mono(opts, st, &allow), 0);
        rc |= expect_eq("case10b-rec-allow", allow, 0);
    }

    // Case 10c: Dual-slot P25 SG calls must apply each slot's patched policy TG independently.
    DSD_MEMSET(opts, 0, sizeof(*opts));
    reset_state(st);
    opts->trunk_use_allow_list = 1;
    rc |= expect_eq("case10c-seed-right-member", seed_policy_group(st, 9602U, "A", "RIGHT-PATCH-MEMBER"), 0);
    st->synctype = DSD_SYNC_P25P2_POS;
    rc |= expect_eq("case10c-left-call",
                    seed_call(st, 0U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 9600U, 9601U, 777003U), 1);
    rc |= expect_eq("case10c-right-call",
                    seed_call(st, 1U, DSD_SYNC_P25P2_POS, DSD_CALL_KIND_GROUP_VOICE, 9600U, 9602U, 777004U), 1);
    st->p25_patch_count = 1;
    st->p25_patch_sgid[0] = 9600U;
    st->p25_patch_is_patch[0] = 1U;
    st->p25_patch_active[0] = 1U;
    st->p25_patch_last_update[0] = time(NULL);
    st->p25_patch_wgid_count[0] = 2U;
    st->p25_patch_wgid[0][0] = 9601U;
    st->p25_patch_wgid[0][1] = 9602U;
    {
        int outL = -1;
        int outR = -1;
        rc |= expect_eq("case10c-ret", dsd_audio_group_gate_dual(opts, st, 9600UL, 9600UL, 0, 0, &outL, &outR), 0);
        rc |= expect_eq("case10c-outL", outL, 1);
        rc |= expect_eq("case10c-outR", outR, 0);
    }

    rc |= test_session_avoid_overrides_hold(opts, st);
    rc |= test_call_skip_mutes_slot(opts, st);
    rc |= test_supergroup_block_mutes_patched_member(opts, st);
    rc |= test_p25_record_gate_honors_reverse_mute(opts, st);
    rc |= test_record_gate_protocol_boundaries(opts, st);
    rc |= test_private_audio_policy(opts, st);
    if (rc == 0) {
        printf("CORE_AUDIO_GROUP_GATE: OK\n");
    }
    dsd_state_ext_free_all(st);
    free(opts);
    free(st);
    return rc;
}
