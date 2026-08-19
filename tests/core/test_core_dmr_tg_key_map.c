// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR talkgroup -> key ID override map (--dmr-tg-key-csv), issue #351 follow-up.
 *
 * A mapped talkgroup selects its key id in place of the OTA-signaled one at key
 * activation time; unmapped talkgroups keep the signaled key id, and the OTA
 * payload_keyid is never rewritten (it stays the truth for logs and history).
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_eq(const char* tag, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
observe_group_call(dsd_state* state, uint8_t slot, uint32_t tg) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof obs);
    obs.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    obs.slot = slot;
    obs.kind = DSD_CALL_KIND_GROUP_VOICE;
    obs.ota_target_id = tg;
    (void)dsd_call_state_observe(state, &obs, DSD_CALL_BOUNDARY_BEGIN);
}

static void
map_one(dsd_state* state, uint32_t tg, uint8_t kid) {
    state->dmr_tg_key_map_tg[state->dmr_tg_key_map_count] = tg;
    state->dmr_tg_key_map_kid[state->dmr_tg_key_map_count] = kid;
    state->dmr_tg_key_map_count++;
}

static int
test_lookup_hit_and_miss(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;
    uint8_t kid = 0;

    map_one(&state, 123U, 0x7B);
    map_one(&state, 4567U, 0x03);

    rc |= expect_eq("lookup-hit", keyring_dmr_tg_map_kid(&state, 123U, &kid), 1);
    rc |= expect_eq("lookup-hit-kid", kid, 0x7B);
    rc |= expect_eq("lookup-hit2", keyring_dmr_tg_map_kid(&state, 4567U, &kid), 1);
    rc |= expect_eq("lookup-hit2-kid", kid, 0x03);
    rc |= expect_eq("lookup-miss", keyring_dmr_tg_map_kid(&state, 999U, &kid), 0);
    return rc;
}

static int
test_mapped_tg_overrides_signaled_kid(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);

    rc |= expect_eq("override-applies", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 1);
    rc |= expect_eq("override-key", (long long)state.R, 0xBBBBBLL);
    rc |= expect_eq("override-ota-kid-untouched", state.payload_keyid, 0x03);

    // The applied notice latches on the call epoch: repeated voice frames of the same
    // call keep applying the key without re-announcing it.
    rc |= expect_eq("override-note-latched", state.dmr_tg_key_note_valid[0], 1);
    const unsigned long long first_epoch = state.dmr_tg_key_note_epoch[0];
    rc |= expect_eq("override-reapplies", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 1);
    rc |= expect_eq("override-note-epoch-stable", (long long)state.dmr_tg_key_note_epoch[0], (long long)first_epoch);

    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_unmapped_tg_leaves_slot_alone(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 999U);

    rc |= expect_eq("unmapped-skipped", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    rc |= expect_eq("unmapped-key-untouched", (long long)state.R, 0);

    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_aes_segments_via_mapped_kid_slot1(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algidR = 0x25;
    state.payload_keyidR = 0x01;
    state.rkey_array[0x05 + 0x000] = 0x1111111111111111ULL;
    state.rkey_array[0x05 + 0x101] = 0x2222222222222222ULL;
    state.rkey_array[0x05 + 0x201] = 0x3333333333333333ULL;
    state.rkey_array[0x05 + 0x301] = 0x4444444444444444ULL;
    state.rkey_array_loaded[0x05 + 0x000] = 1U;
    state.rkey_array_loaded[0x05 + 0x101] = 1U;
    state.rkey_array_loaded[0x05 + 0x201] = 1U;
    state.rkey_array_loaded[0x05 + 0x301] = 1U;
    map_one(&state, 200U, 0x05);
    observe_group_call(&state, 1U, 200U);

    rc |= expect_eq("aes-applies", keyring_dmr_tg_map_activate_slot(&opts, &state, 1), 1);
    rc |= expect_eq("aes-a1", (long long)(state.A1[1] >> 32), 0x11111111LL);
    rc |= expect_eq("aes-a4", (long long)(state.A4[1] >> 32), 0x44444444LL);
    rc |= expect_eq("aes-loaded", state.aes_key_loaded[1], 1);
    rc |= expect_eq("aes-segments", state.aes_key_segments[1], 4);
    rc |= expect_eq("aes-ota-kid-untouched", state.payload_keyidR, 0x01);

    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_gates_hold_activation_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    // Non-DMR sync: the map is DMR-only.
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_P25P1_POS;
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);
    rc |= expect_eq("non-dmr-sync-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    dsd_state_ext_free_all(&state);

    // Keyloader off: no CSV keyring to index into.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.payload_algid = 0x21;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);
    rc |= expect_eq("keyloader-off-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    dsd_state_ext_free_all(&state);

    // No ALG ID yet: same gate as the signaled-KID activation path, and it
    // keeps the basic-privacy TG autoload (algid == 0) unshadowed.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);
    rc |= expect_eq("no-alg-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    dsd_state_ext_free_all(&state);

    // ALG 0x80 (scrambler family) is excluded, mirroring the signaled-KID gate.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x80;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);
    rc |= expect_eq("alg80-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    dsd_state_ext_free_all(&state);

    // No active call on the slot: nothing to match a talkgroup against.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    map_one(&state, 123U, 0x7B);
    rc |= expect_eq("no-call-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    dsd_state_ext_free_all(&state);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_lookup_hit_and_miss();
    rc |= test_mapped_tg_overrides_signaled_kid();
    rc |= test_unmapped_tg_leaves_slot_alone();
    rc |= test_aes_segments_via_mapped_kid_slot1();
    rc |= test_gates_hold_activation_back();
    if (rc == 0) {
        printf("CORE_DMR_TG_KEY_MAP: OK\n");
    }
    return rc;
}
