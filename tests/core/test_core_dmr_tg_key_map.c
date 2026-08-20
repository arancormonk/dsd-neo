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

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if DSD_PLATFORM_WIN_NATIVE
#define TG_KEY_NULL_DEVICE "NUL"
#else
#define TG_KEY_NULL_DEVICE "/dev/null"
#endif

static int
expect_eq(const char* tag, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
observe_call(dsd_state* state, uint8_t slot, uint32_t target, dsd_call_kind kind, int protocol) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof obs);
    obs.protocol = protocol;
    obs.slot = slot;
    obs.kind = kind;
    obs.ota_target_id = target;
    (void)dsd_call_state_observe(state, &obs, DSD_CALL_BOUNDARY_BEGIN);
}

static void
observe_group_call(dsd_state* state, uint8_t slot, uint32_t tg) {
    observe_call(state, slot, tg, DSD_CALL_KIND_GROUP_VOICE, DSD_SYNC_DMR_BS_VOICE_POS);
}

static void
map_one(dsd_state* state, uint32_t tg, uint8_t kid) {
    state->dmr_tg_key_map_tg[state->dmr_tg_key_map_count] = tg;
    state->dmr_tg_key_map_kid[state->dmr_tg_key_map_count] = kid;
    state->dmr_tg_key_map_count++;
}

// Stands in for what mbe_prepare_frame_state() now does once its ALG ID gate has passed: fetch
// the slot's call snapshot, resolve one key id, activate it. Returns 0xFF -- not a valid key id
// range operators would configure, but not out of band for a uint8_t either -- when the slot has
// no call state at all, mirroring processMbeFrameInternal() passing a NULL snapshot.
static uint8_t
activate_via_map(dsd_state* state, int slot) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0) {
        return 0xFFU;
    }
    const uint8_t signaled = (uint8_t)((slot == 0) ? state->payload_keyid : state->payload_keyidR);
    const uint8_t kid = keyring_dmr_slot_kid_for_call(state, slot, &call, signaled);
    keyring_activate_slot_with_kid(state, slot, (int)kid);
    return kid;
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
    static dsd_state state;
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

    rc |= expect_eq("override-resolves-mapped-kid", activate_via_map(&state, 0), 0x7B);
    rc |= expect_eq("override-key", (long long)state.R, 0xBBBBBLL);
    rc |= expect_eq("override-ota-kid-untouched", state.payload_keyid, 0x03);

    // The applied notice latches on the call epoch: repeated voice frames of the same
    // call keep applying the key without re-announcing it.
    const unsigned long long first_epoch = state.dmr_tg_key_note_epoch[0];
    rc |= expect_eq("override-note-latched", first_epoch != 0ULL, 1);
    rc |= expect_eq("override-reapplies", activate_via_map(&state, 0), 0x7B);
    rc |= expect_eq("override-note-epoch-stable", (long long)state.dmr_tg_key_note_epoch[0], (long long)first_epoch);

    dsd_state_ext_free_all(&state);
    return rc;
}

// "Leaves the slot alone" now means the map does not steer the key id -- the slot still
// activates, same as any other voice frame, but with the OTA-signaled kid and its own material
// rather than the mapped row for a different talkgroup (123, not the call's 999).
static int
test_unmapped_tg_leaves_slot_alone(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    // 0x7B has real material too: without it, a broken lookup that matched TG 999 to the 123 row
    // by accident would still fall back to 0x03 for lack of material, and this test would not
    // notice the lookup was wrong -- the exact gap the review flagged.
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 999U);

    rc |= expect_eq("unmapped-returns-signaled-kid", activate_via_map(&state, 0), 0x03);
    rc |= expect_eq("unmapped-loads-signaled-key", (long long)state.R, 0xAAAAALL);

    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_aes_segments_via_mapped_kid_slot1(void) {
    static dsd_state state;
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

    rc |= expect_eq("aes-resolves-mapped-kid", activate_via_map(&state, 1), 0x05);
    rc |= expect_eq("aes-a1", (long long)(state.A1[1] >> 32), 0x11111111LL);
    rc |= expect_eq("aes-a4", (long long)(state.A4[1] >> 32), 0x44444444LL);
    rc |= expect_eq("aes-loaded", state.aes_key_loaded[1], 1);
    rc |= expect_eq("aes-segments", state.aes_key_segments[1], 4);
    rc |= expect_eq("aes-ota-kid-untouched", state.payload_keyidR, 0x01);

    dsd_state_ext_free_all(&state);
    return rc;
}

// keyring_dmr_tg_map_slot_eligible() is gone, and with it two of this function's former cases:
// the non-DMR-sync case tested state->synctype/lastsynctype directly, a check issue #351 review
// finding 4 called out as wrong (it ignored the call's own protocol) and which
// test_non_dmr_call_snapshot_is_rejected now covers correctly via the snapshot's protocol field;
// and the ALG ID gate (algid == 0, algid == 0x80) moved to mbe_prepare_frame_state, which decides
// whether to call the resolver at all -- keyring_dmr_slot_kid_for_call() itself never reads
// payload_algid, so there is no gate left here to exercise. That gate's coverage lives in
// tests/core/test_core_mbe_transform_context.c's test_process_mbe_frame_activation_gate_and_wide_kid(),
// which drives processMbeFrame() directly with keyloader armed -- no test in that file set
// keyloader before that case was added, so until then the gate had no coverage anywhere.
static int
test_gates_hold_activation_back(void) {
    static dsd_state state;
    int rc = 0;

    // Keyloader off: no CSV keyring to index into, so the map is never consulted -- the slot
    // still activates, but with the signaled kid and its own material, not the mapped kid's.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);
    rc |= expect_eq("keyloader-off-returns-signaled-kid", activate_via_map(&state, 0), 0x03);
    rc |= expect_eq("keyloader-off-loads-signaled-not-mapped-key", (long long)state.R, 0xAAAAALL);
    dsd_state_ext_free_all(&state);

    // No active call on the slot: production hits this exact call when mark_vocoder_call_media()
    // reports no active call and mbe_prepare_frame_state() passes a NULL snapshot
    // (processMbeFrameInternal(): have_call ? &call : NULL) -- exercise
    // keyring_dmr_slot_kid_for_call() with call == NULL directly rather than through
    // activate_via_map()'s own dsd_call_state_get() guard, which is test-only scaffolding: that
    // guard's 0xFF sentinel is invariant under any change to keyring.c or dsd_mbe.c, so asserting
    // only it left the real NULL-call contract untested.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    map_one(&state, 123U, 0x7B);
    rc |= expect_eq("no-call-returns-signaled-kid", keyring_dmr_slot_kid_for_call(&state, 0, NULL, 0x03), 0x03);
    dsd_state_ext_free_all(&state);

    return rc;
}

static int
count_substring_in_file(const char* path, const char* needle) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    char line[512];
    int hits = 0;
    while (fgets(line, (int)sizeof line, fp) != NULL) {
        const char* p = line;
        while ((p = strstr(p, needle)) != NULL) {
            hits++;
            p++;
        }
    }
    fclose(fp);
    return hits;
}

// Redirects stderr to a fresh temp file, calls activate_via_map(state, slot) five times, and
// returns how many times `needle` appears in what was written, or -1 if the capture itself could
// not be set up. Points stderr back at the null device before deleting the temp file: Windows
// refuses remove() on a file that is still open, so unlinking while stderr holds it would orphan
// one temp file per capture in the CTest working directory.
static int
capture_notice_hits(dsd_state* state, int slot, const char* needle) {
    char tmpl[] = "dsd-neo-test-tg-key-note-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    fflush(stderr);
    if (freopen(tmpl, "w", stderr) == NULL) {
        (void)remove(tmpl);
        return -1;
    }
    for (int i = 0; i < 5; i++) {
        (void)activate_via_map(state, slot);
    }
    fflush(stderr);
    (void)freopen(TG_KEY_NULL_DEVICE, "w", stderr);

    const int hits = count_substring_in_file(tmpl, needle);
    (void)remove(tmpl);
    return hits;
}

// The notice is a stderr write, so counting it is the only way to observe the latch: its only state
// write assigns the same epoch it compares against, which no in-memory assertion can distinguish
// from an unlatched notice. Without this, dropping the early return would flood the console with one
// line per voice frame -- and corrupt the ncurses display -- with nothing failing. Both notice
// variants share the same latch helper (keyring_dmr_tg_map_note_should_print()), so both are
// exercised here rather than just the applied one: the skipped notice is the CSV-typo path this
// feature added the notice for in the first place, and losing its latch carries the identical risk.
//
// Runs last, and reports on stdout: restoring stderr after the capture is not portable, so it is
// left pointing at the null device and no later test may depend on it.
static int
test_notice_is_emitted_once_per_epoch(void) {
    int rc = 0;

    static dsd_state applied_state;
    DSD_MEMSET(&applied_state, 0, sizeof(applied_state));
    applied_state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    applied_state.keyloader = 1;
    applied_state.payload_algid = 0x21;
    applied_state.rkey_array[0x7B] = 0xBBBBBULL;
    applied_state.rkey_array_loaded[0x7B] = 1U;
    map_one(&applied_state, 123U, 0x7B);
    observe_group_call(&applied_state, 0U, 123U);
    const int applied_hits = capture_notice_hits(&applied_state, 0, "DMR TG Key Map");
    dsd_state_ext_free_all(&applied_state);
    if (applied_hits < 0) {
        printf("note-capture: could not set up applied-notice capture\n");
        rc = 1;
    } else if (applied_hits != 1) {
        printf("note-once-per-epoch: got %d notices want 1\n", applied_hits);
        rc = 1;
    }

    static dsd_state skipped_state;
    DSD_MEMSET(&skipped_state, 0, sizeof(skipped_state));
    skipped_state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    skipped_state.keyloader = 1;
    skipped_state.payload_algid = 0x21;
    skipped_state.payload_keyid = 0x03;
    // 0x7B is mapped but never imported -- the skipped-notice path.
    map_one(&skipped_state, 123U, 0x7B);
    observe_group_call(&skipped_state, 0U, 123U);
    const int skipped_hits = capture_notice_hits(&skipped_state, 0, "has no imported key");
    dsd_state_ext_free_all(&skipped_state);
    if (skipped_hits < 0) {
        printf("note-capture: could not set up skipped-notice capture\n");
        rc = 1;
    } else if (skipped_hits != 1) {
        printf("note-skipped-once-per-epoch: got %d notices want 1\n", skipped_hits);
        rc = 1;
    }

    const int restored = (freopen(TG_KEY_NULL_DEVICE, "w", stderr) != NULL);
    if (!restored) {
        printf("note-capture: could not reopen stderr to null device\n");
        rc = 1;
    }
    return rc;
}

// A private call carries the destination RADIO ID in ota_target_id, and DMR radio ids share the
// talkgroup's 24-bit space -- a colliding id must not pull the talkgroup's key onto a unit call.
static int
test_private_call_is_not_a_talkgroup(void) {
    static dsd_state state;
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
    observe_call(&state, 0U, 123U, DSD_CALL_KIND_PRIVATE_VOICE, DSD_SYNC_DMR_BS_VOICE_POS);

    // The colliding radio ID 123 must not pull in TG 123's mapped key (0x7B / 0xBBBBB): the
    // slot activates with the truly-signaled kid and its own material instead.
    rc |= expect_eq("private-call-returns-signaled-kid", activate_via_map(&state, 0), 0x03);
    rc |= expect_eq("private-call-loads-signaled-not-mapped-key", (long long)state.R, 0xAAAAALL);

    dsd_state_ext_free_all(&state);
    return rc;
}

// dsd_call_state_get() reports a hit for any non-zero epoch, so an ENDED call keeps its talkgroup;
// steering off it would key the next transmission on the slot with the previous call's mapping.
static int
test_ended_call_stops_steering_key_selection(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);
    rc |= expect_eq("active-call-resolves-mapped-kid", activate_via_map(&state, 0), 0x7B);

    (void)dsd_call_state_end(&state, 0U, 0.0);
    state.R = 0ULL;
    // payload_keyid was never set (0), and key id 0 has no material, so the ended call falling
    // through to the signaled kid is what keeps R at zero here -- not the map staying applied.
    rc |= expect_eq("ended-call-returns-signaled-kid", activate_via_map(&state, 0), 0);
    rc |= expect_eq("ended-call-key-stays-zero", (long long)state.R, 0);

    dsd_state_ext_free_all(&state);
    return rc;
}

// A resident non-DMR call must not drive the DMR-only map when lastsynctype is the only thing
// still reading DMR (the window between leaving a DMR system and the next protocol's sync).
static int
test_non_dmr_call_snapshot_is_rejected(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_P25P1_POS;
    // lastsynctype no longer matters: keyring_dmr_tg_map_slot_eligible() (and the
    // synctype/lastsynctype check it made) is gone. Only the call snapshot's own protocol field
    // gates the map now, so this is left set to prove that a stale lastsynctype cannot resurrect
    // the old behavior.
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x84;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_call(&state, 0U, 123U, DSD_CALL_KIND_GROUP_VOICE, DSD_SYNC_P25P1_POS);

    rc |= expect_eq("non-dmr-call-returns-signaled-kid", activate_via_map(&state, 0), 0);
    rc |= expect_eq("non-dmr-call-key-stays-zero", (long long)state.R, 0);

    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_effective_kid_resolution(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;
    int mapped = -1;

    state.keyloader = 1;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    map_one(&state, 456U, 0x40); // deliberately: nothing imported for key id 0x40

    // A mapped group target whose key id has material takes the map's key id.
    rc |= expect_eq("eff-mapped", keyring_dmr_effective_kid(&state, 123U, 1, 0x03, &mapped), 0x7B);
    rc |= expect_eq("eff-mapped-flag", mapped, 1);

    // A mapped key id with nothing imported falls back to the signaled id rather than zeroing
    // the slot key and shadowing a signaled id that would have worked.
    rc |= expect_eq("eff-no-material", keyring_dmr_effective_kid(&state, 456U, 1, 0x03, &mapped), 0x03);
    rc |= expect_eq("eff-no-material-flag", mapped, 0);

    // Unmapped target, individual target, and a disarmed keyring all keep the signaled id.
    rc |= expect_eq("eff-unmapped", keyring_dmr_effective_kid(&state, 999U, 1, 0x03, &mapped), 0x03);
    rc |= expect_eq("eff-individual", keyring_dmr_effective_kid(&state, 123U, 0, 0x03, &mapped), 0x03);
    rc |= expect_eq("eff-individual-flag", mapped, 0);
    state.keyloader = 0;
    rc |= expect_eq("eff-keyloader-off", keyring_dmr_effective_kid(&state, 123U, 1, 0x03, &mapped), 0x03);
    state.keyloader = 1;

    // AES-only material counts as material: segments live at key_id + 0x101/0x201/0x301.
    state.rkey_array[0x0C + 0x101] = 0xCCCCULL;
    map_one(&state, 789U, 0x0C);
    rc |= expect_eq("eff-aes-only", keyring_dmr_effective_kid(&state, 789U, 1, 0x03, &mapped), 0x0C);
    rc |= expect_eq("eff-aes-only-flag", mapped, 1);

    // A zero target never matches: it is the "no talkgroup known" sentinel.
    rc |= expect_eq("eff-zero-target", keyring_dmr_effective_kid(&state, 0U, 1, 0x03, &mapped), 0x03);
    return rc;
}

static int
test_kid_material_reports_without_activating(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;
    unsigned long long rkey = 0xDEADULL;
    int aes_loaded = -1;

    state.rkey_array[0x11] = 0x1234ULL;
    state.rkey_array[0x22 + 0x201] = 0x5678ULL;

    rc |= expect_eq("mat-scalar", keyring_kid_material(&state, 0x11, &rkey, &aes_loaded), 1);
    rc |= expect_eq("mat-scalar-rkey", (long long)rkey, 0x1234LL);
    // Segment 0 shares the scalar's index, so a scalar key also reads as AES-loaded. This
    // mirrors keyring_activate_slot_with_kid() exactly: classification must agree with
    // activation, not with a tidier definition.
    rc |= expect_eq("mat-scalar-aes", aes_loaded, 1);

    rc |= expect_eq("mat-segment-only", keyring_kid_material(&state, 0x22, &rkey, &aes_loaded), 1);
    rc |= expect_eq("mat-segment-only-rkey", (long long)rkey, 0LL);
    rc |= expect_eq("mat-segment-only-aes", aes_loaded, 1);

    rc |= expect_eq("mat-none", keyring_kid_material(&state, 0x33, &rkey, &aes_loaded), 0);
    rc |= expect_eq("mat-none-rkey", (long long)rkey, 0LL);
    rc |= expect_eq("mat-none-aes", aes_loaded, 0);

    // The state is untouched: this is a query, not an activation.
    rc |= expect_eq("mat-no-activation-r", (long long)state.R, 0LL);
    rc |= expect_eq("mat-no-activation-a1", (long long)state.A1[0], 0LL);
    return rc;
}

// A mapped row naming a key id with nothing imported behind it (a CSV typo, or a key the
// operator has not loaded yet) must not zero the slot: it falls back to the signaled id, which
// would have decrypted the call had the row not existed at all.
static int
test_mapped_kid_without_material_falls_back(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    // 0x7B is mapped but never imported -- the shape of a CSV typo.
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);

    rc |= expect_eq("fallback-kid", activate_via_map(&state, 0), 0x03);
    // The signaled key is loaded, not zeroed: the slot decrypts exactly as it would have
    // without the map, rather than going silent because of a bad row.
    rc |= expect_eq("fallback-key", (long long)state.R, 0xAAAAALL);

    dsd_state_ext_free_all(&state);
    return rc;
}

// keyring_kid_kirisun_complete() exists to predict what dsd_dmr_kirisun_slot_key_complete() will
// report once keyring_activate_slot_with_kid() has installed the key id -- classification runs at
// LC/PI time, before that activation. If the two ever disagree, --enc-lockout either drops a
// channel whose mapped key would have decrypted the call, or promises audio the voice path cannot
// deliver.
//
// Exhaustive over the four AES segments x {rkey_array_loaded, value non-zero} -- 4^4 = 256
// combinations, both slots -- because the asymmetric cases are where a plausible-looking predicate
// breaks: keyring_aes_segment_count() returns the loaded-flag count whenever any flag is set and
// the non-zero count otherwise, so "loaded but zero" and "unloaded but non-zero" pull the segment
// count and the A1..A4 non-zero test in opposite directions. keyring_aes_segments_complete(.., 4)
// accepts the loaded-but-zero case and is NOT this predicate.
static int
test_kirisun_kid_predicate_matches_activation(void) {
    static dsd_state state;
    static const int offsets[4] = {0x000, 0x101, 0x201, 0x301};
    const int kid = 0x42;
    int complete_seen = 0;
    int incomplete_seen = 0;
    int rc = 0;

    for (unsigned int combo = 0; combo < 256U; combo++) {
        for (int slot = 0; slot < 2; slot++) {
            DSD_MEMSET(&state, 0, sizeof(state));
            for (int seg = 0; seg < 4; seg++) {
                const unsigned int bits = (combo >> (unsigned int)(seg * 2)) & 0x3U;
                state.rkey_array_loaded[kid + offsets[seg]] = (unsigned char)((bits & 0x1U) != 0U ? 1U : 0U);
                state.rkey_array[kid + offsets[seg]] =
                    (bits & 0x2U) != 0U ? (0x1000ULL + (unsigned long long)seg) : 0ULL;
            }

            // Predict first, then activate, then read the slot back: the order the decoder runs in.
            const int predicted = keyring_kid_kirisun_complete(&state, kid);
            keyring_activate_slot_with_kid(&state, slot, kid);
            const int actual = dsd_dmr_kirisun_slot_key_complete(&state, slot);

            if (predicted != actual) {
                DSD_FPRINTF(stderr, "kirisun-predicate: combo=%u slot=%d predicted %d actual %d\n", combo, slot,
                            predicted, actual);
                rc = 1;
            }
            complete_seen |= actual;
            incomplete_seen |= !actual;
        }
    }

    // The sweep is only proof if activation produced both verdicts; an all-zero fixture would
    // otherwise let an always-0 predicate agree with an always-0 reading.
    rc |= expect_eq("kirisun-predicate-saw-complete", complete_seen, 1);
    rc |= expect_eq("kirisun-predicate-saw-incomplete", incomplete_seen, 1);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_kirisun_kid_predicate_matches_activation();
    rc |= test_lookup_hit_and_miss();
    rc |= test_mapped_tg_overrides_signaled_kid();
    rc |= test_unmapped_tg_leaves_slot_alone();
    rc |= test_aes_segments_via_mapped_kid_slot1();
    rc |= test_gates_hold_activation_back();
    rc |= test_private_call_is_not_a_talkgroup();
    rc |= test_ended_call_stops_steering_key_selection();
    rc |= test_non_dmr_call_snapshot_is_rejected();
    rc |= test_effective_kid_resolution();
    rc |= test_kid_material_reports_without_activating();
    rc |= test_mapped_kid_without_material_falls_back();
    // Last: it redirects stderr and cannot portably restore it.
    rc |= test_notice_is_emitted_once_per_epoch();
    if (rc == 0) {
        printf("CORE_DMR_TG_KEY_MAP: OK\n");
    }
    return rc;
}
