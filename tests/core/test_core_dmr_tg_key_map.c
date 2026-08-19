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
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
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
    const unsigned long long first_epoch = state.dmr_tg_key_note_epoch[0];
    rc |= expect_eq("override-note-latched", first_epoch != 0ULL, 1);
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

// The notice is a stderr write, so counting it is the only way to observe the latch: its only state
// write assigns the same epoch it compares against, which no in-memory assertion can distinguish
// from an unlatched notice. Without this, dropping the early return would flood the console with one
// line per voice frame -- and corrupt the ncurses display -- with nothing failing.
//
// Runs last, and reports on stdout: restoring stderr after the capture is not portable, so it is
// left pointing at the null device and no later test may depend on it.
static int
test_notice_is_emitted_once_per_epoch(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);

    char tmpl[] = "dsd-neo-test-tg-key-note-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed for notice capture\n");
        dsd_state_ext_free_all(&state);
        return 1;
    }
    (void)dsd_close(fd);

    fflush(stderr);
    if (freopen(tmpl, "w", stderr) == NULL) {
        (void)remove(tmpl);
        dsd_state_ext_free_all(&state);
        return 1;
    }
    for (int i = 0; i < 5; i++) {
        (void)keyring_dmr_tg_map_activate_slot(&opts, &state, 0);
    }
    fflush(stderr);
    const int restored = (freopen(TG_KEY_NULL_DEVICE, "w", stderr) != NULL);

    const int hits = count_substring_in_file(tmpl, "DMR TG Key Map");
    (void)remove(tmpl);
    dsd_state_ext_free_all(&state);

    if (!restored) {
        printf("note-capture: could not reopen stderr\n");
        return 1;
    }
    if (hits != 1) {
        printf("note-once-per-epoch: got %d notices want 1\n", hits);
        rc = 1;
    }
    return rc;
}

// A private call carries the destination RADIO ID in ota_target_id, and DMR radio ids share the
// talkgroup's 24-bit space -- a colliding id must not pull the talkgroup's key onto a unit call.
static int
test_private_call_is_not_a_talkgroup(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_call(&state, 0U, 123U, DSD_CALL_KIND_PRIVATE_VOICE, DSD_SYNC_DMR_BS_VOICE_POS);

    rc |= expect_eq("private-call-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    rc |= expect_eq("private-call-key-untouched", (long long)state.R, 0);

    dsd_state_ext_free_all(&state);
    return rc;
}

// dsd_call_state_get() reports a hit for any non-zero epoch, so an ENDED call keeps its talkgroup;
// steering off it would key the next transmission on the slot with the previous call's mapping.
static int
test_ended_call_stops_steering_key_selection(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_group_call(&state, 0U, 123U);
    rc |= expect_eq("active-call-applies", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 1);

    (void)dsd_call_state_end(&state, 0U, 0.0);
    state.R = 0ULL;
    rc |= expect_eq("ended-call-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    rc |= expect_eq("ended-call-key-untouched", (long long)state.R, 0);

    dsd_state_ext_free_all(&state);
    return rc;
}

// A resident non-DMR call must not drive the DMR-only map when lastsynctype is the only thing
// still reading DMR (the window between leaving a DMR system and the next protocol's sync).
static int
test_non_dmr_call_snapshot_is_rejected(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    state.synctype = DSD_SYNC_P25P1_POS;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS; // stale: the eligibility gate still passes
    state.keyloader = 1;
    state.payload_algid = 0x84;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 123U, 0x7B);
    observe_call(&state, 0U, 123U, DSD_CALL_KIND_GROUP_VOICE, DSD_SYNC_P25P1_POS);

    rc |= expect_eq("non-dmr-call-noop", keyring_dmr_tg_map_activate_slot(&opts, &state, 0), 0);
    rc |= expect_eq("non-dmr-call-key-untouched", (long long)state.R, 0);

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

int
main(void) {
    int rc = 0;
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
    // Last: it redirects stderr and cannot portably restore it.
    rc |= test_notice_is_emitted_once_per_epoch();
    if (rc == 0) {
        printf("CORE_DMR_TG_KEY_MAP: OK\n");
    }
    return rc;
}
