// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* P25 LCW canonical call attribution and metadata regression tests. */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);

static int g_nmea_harris_calls;
static uint32_t g_nmea_harris_src;
static uint16_t g_nmea_harris_prefix;
static int g_apx_gps_calls;
static int g_apx_alias_header_calls;
static int g_apx_alias_block_calls;
static int g_l3h_alias_block_calls;
static int g_tait_alias_calls;
static uint8_t g_last_alias_slot;
static int16_t g_tait_alias_len;
static uint16_t g_last_alias_prefix;

/* ── Strong stubs (same set used by test_p25_lcw_call_term) ────────────── */

dsd_trunk_tune_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_trunk_tuning_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

/* Alias / GPS stubs — not exercised by the formats under test */

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_apx_alias_header_calls++;
    g_last_alias_slot = slot;
    g_last_alias_prefix = (uint16_t)convert_bits_into_output(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_apx_alias_block_calls++;
    g_last_alias_slot = slot;
    g_last_alias_prefix = (uint16_t)convert_bits_into_output(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_l3h_alias_block_calls++;
    g_last_alias_slot = slot;
    g_last_alias_prefix = (uint16_t)convert_bits_into_output(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    g_tait_alias_calls++;
    g_last_alias_slot = slot;
    g_tait_alias_len = len;
    g_last_alias_prefix = (uint16_t)convert_bits_into_output(input, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    g_apx_gps_calls++;
    g_last_alias_prefix = (uint16_t)convert_bits_into_output(lc_bits, 16);
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    g_nmea_harris_calls++;
    g_nmea_harris_src = src;
    g_nmea_harris_prefix = (uint16_t)convert_bits_into_output(input, 16);
    (void)slot;
}

/* ── Bit-packing helper ───────────────────────────────────────────────── */

static void
set_bits_msb(uint8_t* b, int off, int n, uint32_t v) {
    for (int i = 0; i < n; i++) {
        int bit = (v >> (n - 1 - i)) & 1;
        b[off + i] = (uint8_t)bit;
    }
}

/* ── Test assertion helper ────────────────────────────────────────────── */

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: %s\n", tag);
    return 0;
}

static int
expect_contains(const char* tag, const char* text, const char* needle) {
    if (!text || !needle || strstr(text, needle) == NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s missing '%s' in '%s'\n", tag, needle ? needle : "(null)", text ? text : "(null)");
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: %s\n", tag);
    return 0;
}

static int
capture_lcw_output(dsd_opts* opts, dsd_state* st, uint8_t lcw[96], char* out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';

    FILE* capture = tmpfile();
    if (!capture) {
        return -1;
    }

    (void)fflush(stderr);
    int saved = dup(fileno(stderr));
    if (saved < 0 || dup2(fileno(capture), fileno(stderr)) < 0) {
        if (saved >= 0) {
            (void)close(saved);
        }
        (void)fclose(capture);
        return -1;
    }

    p25_lcw(opts, st, lcw, /*irrecoverable_errors*/ 0);
    (void)fflush(stderr);
    int restored = dup2(saved, fileno(stderr));
    (void)close(saved);
    if (restored < 0) {
        (void)fclose(capture);
        return -1;
    }

    if (fseek(capture, 0, SEEK_SET) != 0) {
        (void)fclose(capture);
        return -1;
    }
    size_t n = fread(out, 1, out_sz - 1, capture);
    if (n < out_sz - 1 && ferror(capture) != 0) {
        (void)fclose(capture);
        return -1;
    }
    out[n] = '\0';
    (void)fclose(capture);
    return 0;
}

static void
reset_lcw_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    p25_sm_init_ctx(p25_sm_get_ctx(), NULL, state);
}

static int
seed_call(dsd_state* state, dsd_call_kind kind, uint64_t target, uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .observed_m = 1.0,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0;
}

static int
get_active_call(const dsd_state* state, dsd_call_snapshot* call) {
    return dsd_call_state_get(state, 0U, call) > 0 && call->phase == DSD_CALL_PHASE_ACTIVE;
}

static int
active_call_is(const dsd_state* state, dsd_call_kind kind, uint64_t target, uint64_t source) {
    dsd_call_snapshot call;
    return get_active_call(state, &call) && call.protocol == DSD_SYNC_P25P1_POS && call.kind == kind
           && call.ota_target_id == target && call.policy_target_id == target && call.ota_source_id == source;
}

static int
same_call_identity(const dsd_call_snapshot* before, const dsd_call_snapshot* after) {
    return before->epoch == after->epoch && before->phase == after->phase && before->protocol == after->protocol
           && before->slot == after->slot && before->kind == after->kind
           && before->ota_target_id == after->ota_target_id && before->policy_target_id == after->policy_target_id
           && before->ota_source_id == after->ota_source_id && before->crypto == after->crypto
           && before->algid == after->algid && before->kid == after->kid && before->mi == after->mi;
}

/* ── Test cases ───────────────────────────────────────────────────────── */

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    install_trunk_tuning_hooks();

    /*
     * Public guard returns must leave state untouched and tolerate NULL inputs.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("NullGuards_seed_call", seed_call(&st, DSD_CALL_KIND_PRIVATE_VOICE, 0x2468, 0x13579B));
        dsd_call_snapshot before;
        dsd_call_snapshot after;
        rc |= expect_true("NullGuards_get_before", get_active_call(&st, &before));

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);

        p25_lcw(NULL, &st, lcw, /*irrecoverable_errors*/ 0);
        p25_lcw(&opts, NULL, lcw, /*irrecoverable_errors*/ 0);
        p25_lcw(&opts, &st, NULL, /*irrecoverable_errors*/ 0);

        rc |= expect_true("NullGuards_get_after", get_active_call(&st, &after));
        rc |= expect_true("NullGuards_preserve_state", same_call_identity(&before, &after));
    }

    /*
     * Test case 1 — Format 0x00 (Group Voice Channel User)
     * Pre-condition: the canonical call has source 1234567.
     * Input:         LCW with format=0x00, group=100, source=0
     * Expected:      the canonical source remains 1234567.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("Fmt0x00_seed_call", seed_call(&st, DSD_CALL_KIND_GROUP_VOICE, 100, 1234567));

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (Group Voice) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00 (standard)   */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00              */
        set_bits_msb(lcw, 32, 16, 100); /* group     = 100               */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0  (bug trigger)  */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x00_src0_preserves_canonical_source",
                          active_call_is(&st, DSD_CALL_KIND_GROUP_VOICE, 100, 1234567));
    }

    /*
     * Test case 2 — Format 0x03 (Unit-to-Unit Voice Channel User)
     * Pre-condition: the canonical call has source 102.
     * Input:         LCW with format=0x03, target=200, source=0
     * Expected:      the canonical source remains 102.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("Fmt0x03_seed_call", seed_call(&st, DSD_CALL_KIND_PRIVATE_VOICE, 200, 102));

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x03);  /* lc_format = 0x03 (Unit-to-Unit) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00               */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00               */
        set_bits_msb(lcw, 24, 24, 200); /* target    = 200                */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0  (bug trigger)   */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x03_src0_preserves_canonical_source",
                          active_call_is(&st, DSD_CALL_KIND_PRIVATE_VOICE, 200, 102));
    }

    /*
     * Test case 3 — MFID90 format 0x00 (Motorola Group Regroup Channel User)
     * Pre-condition: the canonical call has source 54321.
     * Input:         LCW with format=0x00, MFID=0x90, sg=300, src=0
     * Expected:      the canonical source remains 54321.
     *
     * Note: MFID90 0x00 means lc_format byte = 0x00 with SF=0, so
     * lc_mfid = 0x90 and lc_opcode = 0x00.  The is_standard_mfid check
     * fails (0x90 != 0/1 and SF=0), routing to the MFID90 branch.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("MFID90_Fmt0x00_seed_call", seed_call(&st, DSD_CALL_KIND_GROUP_VOICE, 300, 54321));

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (PB=0,SF=0,LCO=0x00) */
        set_bits_msb(lcw, 8, 8, 0x90);  /* lc_mfid   = 0x90 (Motorola)            */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00                        */
        set_bits_msb(lcw, 32, 16, 300); /* sg        = 300                         */
        set_bits_msb(lcw, 48, 24, 0);   /* src       = 0  (bug trigger)            */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_Fmt0x00_src0_preserves_canonical_source",
                          active_call_is(&st, DSD_CALL_KIND_GROUP_VOICE, 300, 54321));
    }

    /*
     * Edge case — source=0 with no prior call.
     * Input:         LCW with format=0x00, group=100, source=0
     * Expected:      an anonymous canonical group call is created.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);  /* lc_format = 0x00 (Group Voice) */
        set_bits_msb(lcw, 8, 8, 0x00);  /* lc_mfid   = 0x00              */
        set_bits_msb(lcw, 16, 8, 0x00); /* lc_svcopt = 0x00              */
        set_bits_msb(lcw, 32, 16, 100); /* group     = 100               */
        set_bits_msb(lcw, 48, 24, 0);   /* source    = 0                 */

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |=
            expect_true("EdgeCase_src0_starts_anonymous_call", active_call_is(&st, DSD_CALL_KIND_GROUP_VOICE, 100, 0));
    }

    /*
     * Source ID Extension (0x49)
     * sdrtrunk parses this as WACN[16..35], SYSTEM[36..47], RADIO[48..71].
     * The radio ID must not be read starting at bit 40.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("SrcIdExtension_seed_call", seed_call(&st, DSD_CALL_KIND_GROUP_VOICE, 0x1111, 111));

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x49);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 20, 0xABCDE);
        set_bits_msb(lcw, 36, 12, 0x123);
        set_bits_msb(lcw, 48, 24, 0x456789);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("SrcIdExtension_WACN_20bit", st.p25_src_nid == 0xABCDE);
        rc |= expect_true("SrcIdExtension_radio_24bit_at_bit48",
                          active_call_is(&st, DSD_CALL_KIND_GROUP_VOICE, 0x1111, 0x456789));
    }

    /*
     * Tait MFID 0xD8 format 0x01 uses the same fully-qualified source layout:
     * WACN[16..35], SYSTEM[36..47], RADIO[48..71].
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("TaitD8Fmt01_seed_call", seed_call(&st, DSD_CALL_KIND_GROUP_VOICE, 0x2222, 222));

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x01);
        set_bits_msb(lcw, 8, 8, 0xD8);
        set_bits_msb(lcw, 16, 20, 0x54321);
        set_bits_msb(lcw, 36, 12, 0x987);
        set_bits_msb(lcw, 48, 24, 0x2468AC);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("TaitD8Fmt01_WACN_20bit", st.p25_src_nid == 0x54321);
        rc |= expect_true("TaitD8Fmt01_radio_24bit_at_bit48",
                          active_call_is(&st, DSD_CALL_KIND_GROUP_VOICE, 0x2222, 0x2468AC));
    }

    /* Format 0x00 service options are retained on the canonical call. */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        opts.payload = 1;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 8, 0xFF);
        set_bits_msb(lcw, 32, 16, 0x1234);
        set_bits_msb(lcw, 48, 24, 0x456789);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        dsd_call_snapshot call;
        rc |= expect_true("Fmt0x00_canonical_call", get_active_call(&st, &call));
        rc |= expect_true("Fmt0x00_emergency_metadata",
                          call.kind == DSD_CALL_KIND_GROUP_VOICE && call.ota_target_id == 0x1234
                              && call.ota_source_id == 0x456789 && call.service_options == 0xFF && call.emergency == 1
                              && call.priority == 7 && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    }

    /* Format 0x03 publishes an encrypted individual/private canonical call. */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x03);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 8, 0x40);
        set_bits_msb(lcw, 24, 24, 0x010203);
        set_bits_msb(lcw, 48, 24, 0x040506);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        dsd_call_snapshot call;
        rc |= expect_true("Fmt0x03_canonical_call", get_active_call(&st, &call));
        rc |= expect_true("Fmt0x03_private_state", call.kind == DSD_CALL_KIND_PRIVATE_VOICE
                                                       && call.ota_target_id == 0x010203
                                                       && call.ota_source_id == 0x040506 && call.service_options == 0x40
                                                       && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    }

    /*
     * Format 0x42 carries two implicit group voice channel updates. When the two
     * groups differ, both structured recent-activity entries should be refreshed.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x42);
        set_bits_msb(lcw, 8, 16, 0x1111);
        set_bits_msb(lcw, 24, 16, 0x2222);
        set_bits_msb(lcw, 40, 16, 0x3333);
        set_bits_msb(lcw, 56, 16, 0x4444);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        dsd_recent_activity_snapshot activity;
        rc |= expect_true("Fmt0x42_activity_snapshot", dsd_recent_activity_copy_snapshot(&st, &activity) > 0);
        rc |= expect_true("Fmt0x42_first_activity",
                          activity.entries[0].updated_m_ms != 0U
                              && activity.entries[0].observation.kind == DSD_CALL_KIND_GROUP_VOICE
                              && activity.entries[0].observation.channel == 0x1111
                              && activity.entries[0].observation.ota_target_id == 0x2222);
        rc |= expect_true("Fmt0x42_second_activity",
                          activity.entries[1].updated_m_ms != 0U
                              && activity.entries[1].observation.kind == DSD_CALL_KIND_GROUP_VOICE
                              && activity.entries[1].observation.channel == 0x3333
                              && activity.entries[1].observation.ota_target_id == 0x4444);
        dsd_call_snapshot call;
        rc |= expect_true("Fmt0x42_does_not_start_call",
                          dsd_call_state_get(&st, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
    }

    /* Format 0x4A is an extended unit-to-unit voice channel user. */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x4A);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 8, 0x00);
        set_bits_msb(lcw, 24, 24, 0x101112);
        set_bits_msb(lcw, 48, 24, 0x202122);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x4A_sets_private_identity",
                          active_call_is(&st, DSD_CALL_KIND_PRIVATE_VOICE, 0x101112, 0x202122));
    }

    /* Format 0x50 updates affiliation only; it is not a voice identity. */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("Fmt0x50_seed_call", seed_call(&st, DSD_CALL_KIND_PRIVATE_VOICE, 0x1111, 0x222222));
        const dsd_call_crypto_update crypto = {
            .classification = DSD_CALL_CRYPTO_ENCRYPTED,
            .algid = 0x84,
            .kid = 0x1234,
            .mi = UINT64_C(0x1122334455667788),
            .audio_permitted = 0U,
            .observed_m = 2.0,
        };
        rc |= expect_true("Fmt0x50_seed_crypto", dsd_call_state_update_crypto(&st, 0U, &crypto) > 0);
        dsd_call_snapshot before;
        dsd_call_snapshot after;
        rc |= expect_true("Fmt0x50_get_before", get_active_call(&st, &before));
        st.payload_algid = 0x84;
        st.payload_keyid = 0x1234;
        st.payload_miP = UINT64_C(0x1122334455667788);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x50);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 32, 16, 0x3456);
        set_bits_msb(lcw, 48, 24, 0x654321);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x50_get_after", get_active_call(&st, &after));
        rc |= expect_true("Fmt0x50_preserves_canonical_call", same_call_identity(&before, &after));
        rc |= expect_true("Fmt0x50_preserves_decoder_crypto", st.payload_algid == 0x84 && st.payload_keyid == 0x1234
                                                                  && st.payload_miP == UINT64_C(0x1122334455667788));
        rc |= expect_true("Fmt0x50_updates_affiliation",
                          st.p25_ga_count == 1 && st.p25_ga_rid[0] == 0x654321 && st.p25_ga_tg[0] == 0x3456);
    }

    /*
     * Exercise the known protection ALGID map, including values that previously
     * only appeared in display text. The persistent state must still record the
     * exact ALGID/KID payload.
     */
    {
        const uint8_t algids[] = {0x81, 0x82, 0x83, 0x84, 0x85, 0x88, 0x89, 0x9F, 0xAA, 0xAF};
        for (size_t i = 0; i < sizeof(algids) / sizeof(algids[0]); i++) {
            DSD_MEMSET(&opts, 0, sizeof opts);
            reset_lcw_state(&st);

            uint8_t lcw[96];
            DSD_MEMSET(lcw, 0, sizeof lcw);
            set_bits_msb(lcw, 0, 8, 0x65);
            set_bits_msb(lcw, 8, 8, 0x00);
            set_bits_msb(lcw, 24, 8, algids[i]);
            set_bits_msb(lcw, 32, 16, (uint32_t)(0x1200U + i));
            set_bits_msb(lcw, 48, 24, 0x010203);

            p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

            rc |= expect_true("Fmt0x65_known_algid_valid", st.p25_prot_valid == 1);
            rc |= expect_true("Fmt0x65_known_algid_value", st.p25_prot_algid == algids[i]);
            rc |= expect_true("Fmt0x65_known_algid_kid", st.p25_prot_kid == (int)(0x1200U + i));
        }
    }

    /*
     * Unknown ALGIDs do not have a printable name, but the raw protection
     * parameters are still meaningful decoder state.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x65);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 24, 8, 0x01);
        set_bits_msb(lcw, 32, 16, 0xBEEF);
        set_bits_msb(lcw, 48, 24, 0x010203);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("Fmt0x65_unknown_algid_valid", st.p25_prot_valid == 1);
        rc |= expect_true("Fmt0x65_unknown_algid_value", st.p25_prot_algid == 0x01);
        rc |= expect_true("Fmt0x65_unknown_algid_kid", st.p25_prot_kid == 0xBEEF);
    }

    /* MFID90 regroup channel users update the canonical call and patch table. */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);
        set_bits_msb(lcw, 8, 8, 0x90);
        lcw[16] = 1;
        lcw[17] = 1;
        lcw[31] = 1;
        set_bits_msb(lcw, 32, 16, 0x345);
        set_bits_msb(lcw, 48, 24, 0x6789AB);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_regroup_canonical_call",
                          active_call_is(&st, DSD_CALL_KIND_GROUP_VOICE, 0x345, 0x6789AB));
        rc |= expect_true("MFID90_regroup_patch_count", st.p25_patch_count == 1);
        rc |= expect_true("MFID90_regroup_patch_sgid", st.p25_patch_sgid[0] == 0x345);
        rc |= expect_true("MFID90_regroup_patch_active", st.p25_patch_is_patch[0] == 1 && st.p25_patch_active[0] == 1);
    }

    /* MFID90 regroup channel updates are recent activity, not active calls. */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x01);
        set_bits_msb(lcw, 8, 8, 0x90);
        lcw[16] = 1;
        lcw[17] = 1;
        set_bits_msb(lcw, 24, 16, 0x456);
        set_bits_msb(lcw, 56, 16, 0x1234);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        dsd_recent_activity_snapshot activity;
        rc |= expect_true("MFID90_regroup_update_activity_snapshot",
                          dsd_recent_activity_copy_snapshot(&st, &activity) > 0);
        rc |= expect_true("MFID90_regroup_update_activity",
                          activity.entries[0].updated_m_ms != 0U
                              && activity.entries[0].observation.kind == DSD_CALL_KIND_GROUP_VOICE
                              && activity.entries[0].observation.ota_target_id == 0x456
                              && activity.entries[0].observation.channel == 0x1234);
        dsd_call_snapshot call;
        rc |= expect_true("MFID90_regroup_update_no_active_call",
                          dsd_call_state_get(&st, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
    }

    /*
     * Motorola LCW regroup add/delete carries two patched talkgroups.  Treat
     * zero and supergroup-as-member as absent, matching sdrtrunk's LCW model.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x03);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 16, 0x0700);
        set_bits_msb(lcw, 32, 16, 0x0101);
        set_bits_msb(lcw, 48, 16, 0x0202);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_lcw_regroup_add_count", st.p25_patch_count == 1);
        rc |= expect_true("MFID90_lcw_regroup_add_sgid", st.p25_patch_sgid[0] == 0x0700);
        rc |= expect_true("MFID90_lcw_regroup_add_members", st.p25_patch_wgid_count[0] == 2);
        rc |= expect_true("MFID90_lcw_regroup_add_member1", st.p25_patch_wgid[0][0] == 0x0101);
        rc |= expect_true("MFID90_lcw_regroup_add_member2", st.p25_patch_wgid[0][1] == 0x0202);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x04);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 16, 0x0700);
        set_bits_msb(lcw, 32, 16, 0x0101);
        set_bits_msb(lcw, 48, 16, 0x0700);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("MFID90_lcw_regroup_delete_one", st.p25_patch_wgid_count[0] == 1);
        rc |= expect_true("MFID90_lcw_regroup_delete_keeps_other", st.p25_patch_wgid[0][0] == 0x0202);
    }

    /*
     * Additional Motorola vendor LCWs are metadata/display-only; they should be
     * recognized without changing call or patch state.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("MFID90_metadata_seed_call", seed_call(&st, DSD_CALL_KIND_GROUP_VOICE, 0xAAAA, 0xBBBBCC));
        dsd_call_snapshot before;
        dsd_call_snapshot after;
        rc |= expect_true("MFID90_metadata_get_before", get_active_call(&st, &before));
        char out[2048];

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x02);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 8, 0xAB);
        if (capture_lcw_output(&opts, &st, lcw, out, sizeof(out)) != 0) {
            return 1;
        }
        rc |= expect_contains("MFID90_failsoft_label", out, "MFID90 (Moto) Failsoft");
        rc |= expect_true("MFID90_failsoft_get_after", get_active_call(&st, &after));
        rc |= expect_true("MFID90_failsoft_no_call_mutation", same_call_identity(&before, &after));

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x0A);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 32, 16, 0x1234);
        set_bits_msb(lcw, 48, 24, 0x456789);
        if (capture_lcw_output(&opts, &st, lcw, out, sizeof(out)) != 0) {
            return 1;
        }
        rc |= expect_contains("MFID90_emergency_label", out, "Emergency Alarm Activation");
        rc |= expect_contains("MFID90_emergency_group", out, "Group: 4660");
        rc |= expect_contains("MFID90_emergency_source", out, "Source: 4548489");
        rc |= expect_true("MFID90_emergency_no_patch", st.p25_patch_count == 0);
        rc |= expect_true("MFID90_emergency_get_after", get_active_call(&st, &after));
        rc |= expect_true("MFID90_emergency_no_call_mutation", same_call_identity(&before, &after));
    }

    /*
     * Harris GPS is assembled from a block-1 prefix plus block-2 continuation.
     * A valid prefix dispatches to the GPS decoder stub and then clears the
     * scratch buffer; a missing block 1 only clears scratch state.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        g_nmea_harris_calls = 0;
        g_nmea_harris_src = 0;
        g_nmea_harris_prefix = 0;
        rc |= expect_true("HarrisGPS_seed_call", seed_call(&st, DSD_CALL_KIND_GROUP_VOICE, 0x3456, 0x123456));

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x2A);
        set_bits_msb(lcw, 8, 8, 0xA4);
        set_bits_msb(lcw, 16, 8, 0x5A);
        set_bits_msb(lcw, 40, 8, 0xC3);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |=
            expect_true("HarrisGPS_block1_prefix", (uint16_t)convert_bits_into_output(st.dmr_pdu_sf[0], 16) == 0x2AA4);
        rc |= expect_true("HarrisGPS_block1_payload_copy", st.dmr_pdu_sf[0][40] == 0 && st.dmr_pdu_sf[0][41] == 1);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x2B);
        set_bits_msb(lcw, 8, 8, 0xA4);
        set_bits_msb(lcw, 16, 8, 0x66);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("HarrisGPS_valid_dispatch", g_nmea_harris_calls == 1);
        rc |= expect_true("HarrisGPS_dispatch_src", g_nmea_harris_src == 0x123456);
        rc |= expect_true("HarrisGPS_dispatch_prefix", g_nmea_harris_prefix == 0x2AA4);
        rc |= expect_true("HarrisGPS_clears_scratch", st.dmr_pdu_sf[0][0] == 0 && st.dmr_pdu_sf[0][40] == 0);

        DSD_MEMSET(st.dmr_pdu_sf[0], 1, sizeof(st.dmr_pdu_sf[0]));
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x2B);
        set_bits_msb(lcw, 8, 8, 0xA4);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("HarrisGPS_missing_block1_no_dispatch", g_nmea_harris_calls == 1);
        rc |= expect_true("HarrisGPS_missing_block1_clears_scratch", st.dmr_pdu_sf[0][0] == 0);
    }

    /*
     * L3Harris opcode 0x0A is observed as a data/return-to-control indication,
     * but sources are inconclusive.  Keep it metadata-only.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        opts.trunk_is_tuned = 1;
        st.p25_vc_freq[0] = 851000000;
        st.p25_vc_freq[1] = 851000000;
        char out[2048];

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x0A);
        set_bits_msb(lcw, 8, 8, 0xA4);
        set_bits_msb(lcw, 16, 8, 0x7E);
        set_bits_msb(lcw, 24, 24, 0x123456);
        set_bits_msb(lcw, 48, 24, 0xABCDEF);

        if (capture_lcw_output(&opts, &st, lcw, out, sizeof(out)) != 0) {
            return 1;
        }
        rc |= expect_contains("MFIDA4_0x0A_neutral_label", out, "Data/Return-to-Control Indication");
        rc |= expect_contains("MFIDA4_0x0A_source", out, "SRC: 1193046");
        rc |= expect_contains("MFIDA4_0x0A_target", out, "TGT: 11259375");
        rc |= expect_true("MFIDA4_0x0A_no_release",
                          opts.trunk_is_tuned == 1 && st.p25_vc_freq[0] == 851000000 && st.p25_vc_freq[1] == 851000000);
    }

    /*
     * Vendor LCW alias/GPS opcodes should delegate to their protocol-specific
     * decoders with the raw LCW bits and voice slot 0, rather than only printing
     * static labels.
     */
    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        g_apx_gps_calls = 0;
        g_apx_alias_header_calls = 0;
        g_apx_alias_block_calls = 0;
        g_l3h_alias_block_calls = 0;
        g_tait_alias_calls = 0;
        g_last_alias_slot = 0xFF;
        g_tait_alias_len = 0;
        g_last_alias_prefix = 0;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x06);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 8, 0xA1);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFID90_GPS_dispatch", g_apx_gps_calls == 1);
        rc |= expect_true("MFID90_GPS_prefix", g_last_alias_prefix == 0x0690);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x15);
        set_bits_msb(lcw, 8, 8, 0x90);
        set_bits_msb(lcw, 16, 8, 0xB2);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFID90_alias_header_dispatch", g_apx_alias_header_calls == 1);
        rc |= expect_true("MFID90_alias_header_slot", g_last_alias_slot == 0);
        rc |= expect_true("MFID90_alias_header_prefix", g_last_alias_prefix == 0x1590);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x17);
        set_bits_msb(lcw, 8, 8, 0x90);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFID90_alias_block_dispatch", g_apx_alias_block_calls == 1);
        rc |= expect_true("MFID90_alias_block_slot", g_last_alias_slot == 0);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x32);
        set_bits_msb(lcw, 8, 8, 0xA4);
        set_bits_msb(lcw, 16, 8, 0xC3);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFIDA4_alias_block_dispatch", g_l3h_alias_block_calls == 1);
        rc |= expect_true("MFIDA4_alias_block_slot", g_last_alias_slot == 0);
        rc |= expect_true("MFIDA4_alias_block_prefix", g_last_alias_prefix == 0x32A4);

        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x00);
        set_bits_msb(lcw, 8, 8, 0xD8);
        set_bits_msb(lcw, 16, 8, 0xD4);
        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);
        rc |= expect_true("MFIDD8_tait_alias_dispatch", g_tait_alias_calls == 1);
        rc |= expect_true("MFIDD8_tait_alias_slot", g_last_alias_slot == 0);
        rc |= expect_true("MFIDD8_tait_alias_len", g_tait_alias_len == 8);
        rc |= expect_true("MFIDD8_tait_alias_prefix", g_last_alias_prefix == 0x00D8);
    }

    /*
     * Unknown and protected LCWs are tolerated as no-op control messages: they
     * must not rewrite remembered call state or invoke vendor payload decoders.
     */
    {
        static const struct {
            const char* name;
            uint8_t format;
            uint8_t mfid;
        } unknown_cases[] = {
            {"UnknownStandard_no_state_mutation", 0x30, 0x00}, {"UnknownMoto_no_state_mutation", 0x30, 0x90},
            {"UnknownHarris_no_state_mutation", 0x30, 0xA4},   {"UnknownTait_no_state_mutation", 0x30, 0xD8},
            {"UnknownVendor_no_state_mutation", 0x30, 0x7F},
        };

        for (size_t i = 0; i < sizeof(unknown_cases) / sizeof(unknown_cases[0]); i++) {
            DSD_MEMSET(&opts, 0, sizeof opts);
            reset_lcw_state(&st);
            rc |= expect_true("UnknownLCW_seed_call", seed_call(&st, DSD_CALL_KIND_PRIVATE_VOICE, 0x3456, 0x123456));
            dsd_call_snapshot before;
            dsd_call_snapshot after;
            rc |= expect_true("UnknownLCW_get_before", get_active_call(&st, &before));
            st.p25_prot_valid = 1;
            st.p25_prot_algid = 0x84;
            st.p25_prot_kid = 0x4321;
            g_apx_gps_calls = 0;
            g_apx_alias_header_calls = 0;
            g_apx_alias_block_calls = 0;
            g_l3h_alias_block_calls = 0;
            g_tait_alias_calls = 0;

            uint8_t lcw[96];
            DSD_MEMSET(lcw, 0, sizeof lcw);
            set_bits_msb(lcw, 0, 8, unknown_cases[i].format);
            set_bits_msb(lcw, 8, 8, unknown_cases[i].mfid);
            set_bits_msb(lcw, 16, 8, 0xFF);
            set_bits_msb(lcw, 48, 24, 0x654321);

            p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

            rc |= expect_true("UnknownLCW_get_after", get_active_call(&st, &after));
            rc |= expect_true(unknown_cases[i].name, same_call_identity(&before, &after) && st.p25_prot_valid == 1
                                                         && st.p25_prot_algid == 0x84 && st.p25_prot_kid == 0x4321
                                                         && g_apx_gps_calls == 0 && g_apx_alias_header_calls == 0
                                                         && g_apx_alias_block_calls == 0 && g_l3h_alias_block_calls == 0
                                                         && g_tait_alias_calls == 0);
        }
    }

    {
        DSD_MEMSET(&opts, 0, sizeof opts);
        reset_lcw_state(&st);
        rc |= expect_true("ProtectedLCW_seed_call", seed_call(&st, DSD_CALL_KIND_PRIVATE_VOICE, 0x4567, 0x234567));
        dsd_call_snapshot before;
        dsd_call_snapshot after;
        rc |= expect_true("ProtectedLCW_get_before", get_active_call(&st, &before));
        st.p25_prot_valid = 0;

        uint8_t lcw[96];
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits_msb(lcw, 0, 8, 0x80);
        set_bits_msb(lcw, 8, 8, 0x00);
        set_bits_msb(lcw, 16, 8, 0xFF);
        set_bits_msb(lcw, 32, 16, 0x1234);
        set_bits_msb(lcw, 48, 24, 0x654321);

        p25_lcw(&opts, &st, lcw, /*irrecoverable_errors*/ 0);

        rc |= expect_true("ProtectedLCW_get_after", get_active_call(&st, &after));
        rc |= expect_true("ProtectedLCW_no_state_mutation",
                          same_call_identity(&before, &after) && st.p25_prot_valid == 0);
    }

    dsd_state_ext_free_all(&st);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(misc-use-internal-linkage)
