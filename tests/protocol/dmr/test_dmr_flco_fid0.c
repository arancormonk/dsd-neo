// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Full LC identity gates (issue #903).
 *
 * - Under FID 0 only FLCO 0x00 (Grp_V_Ch_Usr) and 0x03 (UU_V_Ch_Usr) are voice channel users (TS 102 361-2 Table B.1).
 *   Any other FID 0 FLCO opens no call, holds no scan target and arms no encryption lockout. The vendor FIDs keep the
 *   voice FLCOs they had.
 * - The Hytera XPT branding changes how later LCs are read (a 16-bit target), so an LC accepted under relaxed RAS
 *   handling must not set it: a later clean standard header keeps its 24-bit target.
 * - The XPT group call alert names 250-254 as emergency groups and 255 as All Call; 249 is an ordinary group.
 * - The same holds for an XPT CSBK: one accepted under relaxed RAS handling does not brand the site.
 * - A FID 0 LC that is no voice channel user carries no service options: it neither moves the slot's encryption
 *   classification nor ends a voice hold, and the terminal shows no encryption, emergency or service options from it.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int s_scan_activity_calls = 0;

static void
count_scan_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source, int is_private,
                    int encrypted, int data_call) {
    (void)opts;
    (void)state;
    (void)target;
    (void)source;
    (void)is_private;
    (void)encrypted;
    (void)data_call;
    s_scan_activity_calls++;
}

static void
write_bits(uint8_t* bits, size_t start, uint64_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

static void
build_lc(uint8_t bits[80], uint8_t flco, uint8_t fid, uint8_t so, uint32_t target, uint32_t source) {
    DSD_MEMSET(bits, 0, 80U);
    write_bits(bits, 2U, flco, 6U);
    write_bits(bits, 8U, fid, 8U);
    write_bits(bits, 16U, so, 8U);
    write_bits(bits, 24U, target, 24U);
    write_bits(bits, 48U, source, 24U);
}

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->currentslot = 0;
    return state;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* A checked voice LC header on slot 0; returns 1 and fills @p call when the slot has a call afterwards. */
static int
run_header(dsd_opts* opts, dsd_state* state, uint8_t flco, uint8_t fid, uint8_t so, uint32_t target, uint32_t source,
           dsd_call_snapshot* call) {
    uint8_t bits[80];
    build_lc(bits, flco, fid, so, target, source);
    uint32_t irr = 0;
    dmr_flco(opts, state, bits, 1U, &irr, 1U);
    return dsd_call_state_get(state, 0U, call) > 0;
}

/* FID 0 FLCO 0x01 is no voice channel user: no call, no scan hold and no encryption lockout from its fields. */
static void
fid0_flco_01_opens_no_call(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    opts->trunk_enable = 1;
    opts->trunk_tune_enc_calls = 0;
    dsd_state* state = new_state();

    s_scan_activity_calls = 0;
    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_conventional_activity = count_scan_activity;
    dsd_trunk_scan_hooks_set(&hooks);

    dsd_call_snapshot call;
    const int have_call = run_header(opts, state, 0x01U, 0x00U, 0x40U, 0x123U, 0x456U, &call);
    dsd_trunk_scan_hooks_set(NULL);

    assert(!have_call || call.phase != DSD_CALL_PHASE_ACTIVE);
    assert(s_scan_activity_calls == 0);
    assert(!dsd_enc_lockout_lookup(state, 0x123U, 1, NULL));
    free_state(state);

    // A type-1 header carrying a talker alias or GPS FLCO is no voice user either.
    for (uint8_t flco = 0x04U; flco <= 0x08U; flco++) {
        state = new_state();
        assert(!run_header(opts, state, flco, 0x00U, 0x00U, 0x123U, 0x456U, &call)
               || call.phase != DSD_CALL_PHASE_ACTIVE);
        free_state(state);
    }
    // Nor is an undefined FLCO in the vendor bit range.
    state = new_state();
    assert(!run_header(opts, state, 0x13U, 0x00U, 0x00U, 0x123U, 0x456U, &call) || call.phase != DSD_CALL_PHASE_ACTIVE);
    free_state(state);
    free(opts);
}

static void
fid0_flco_00_opens_group(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = new_state();

    s_scan_activity_calls = 0;
    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_conventional_activity = count_scan_activity;
    dsd_trunk_scan_hooks_set(&hooks);

    dsd_call_snapshot call;
    assert(run_header(opts, state, 0x00U, 0x00U, 0x00U, 0x123U, 0x456U, &call));
    dsd_trunk_scan_hooks_set(NULL);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.kind == DSD_CALL_KIND_GROUP_VOICE);
    assert(call.ota_target_id == 0x123U && call.ota_source_id == 0x456U);
    assert(s_scan_activity_calls == 1);
    free_state(state);
    free(opts);
}

static void
fid0_flco_03_opens_unit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = new_state();

    dsd_call_snapshot call;
    assert(run_header(opts, state, 0x03U, 0x00U, 0x00U, 0x123U, 0x456U, &call));
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.kind == DSD_CALL_KIND_PRIVATE_VOICE);
    assert(call.ota_target_id == 0x123U && call.ota_source_id == 0x456U);
    free_state(state);
    free(opts);
}

/* The predicate keyed on the raw FID: FID 0 names two voice users, the vendor FIDs keep theirs. */
static void
opens_voice_call_table(void) {
    for (unsigned int flco = 0U; flco < 0x40U; flco++) {
        const int fid0 = dmr_flco_opens_voice_call(0x00U, (uint8_t)flco);
        assert(fid0 == (flco == 0x00U || flco == 0x03U));
        // A Kenwood scrambler LC (raw FID 0x20) is a standard call under that FID.
        assert(dmr_flco_opens_voice_call(0x20U, (uint8_t)flco) == fid0);
    }
    // Motorola (Capacity Plus, Capacity Max): the voice users stay; its non-voice LCs and the Capacity Max alias/GPS
    // FLCOs (0x14..0x18, the FID 0 0x04..0x08 with bit 0x10 set) do not open a call.
    assert(dmr_flco_opens_voice_call(0x10U, 0x04U));
    assert(dmr_flco_opens_voice_call(0x10U, 0x07U));
    assert(dmr_flco_opens_voice_call(0x10U, 0x10U));
    assert(dmr_flco_opens_voice_call(0x10U, 0x13U));
    assert(dmr_flco_opens_voice_call(0x10U, 0x20U));
    assert(dmr_flco_opens_voice_call(0x10U, 0x23U));
    assert(!dmr_flco_opens_voice_call(0x10U, 0x08U));
    assert(!dmr_flco_opens_voice_call(0x10U, 0x28U));
    assert(!dmr_flco_opens_voice_call(0x10U, 0x29U));
    for (uint8_t flco = 0x14U; flco <= 0x18U; flco++) {
        assert(!dmr_flco_opens_voice_call(0x10U, flco));
    }
    // Hytera: group and private voice users; the XPT alert, Enhanced privacy and the other Hytera LCs do not.
    assert(dmr_flco_opens_voice_call(0x68U, 0x00U));
    assert(dmr_flco_opens_voice_call(0x68U, 0x03U));
    assert(!dmr_flco_opens_voice_call(0x68U, 0x02U));
    assert(!dmr_flco_opens_voice_call(0x68U, 0x09U));
    assert(!dmr_flco_opens_voice_call(0x68U, 0x13U));
    assert(!dmr_flco_opens_voice_call(0x68U, 0x2EU));
    assert(!dmr_flco_opens_voice_call(0x68U, 0x2FU));
    assert(!dmr_flco_opens_voice_call(0x68U, 0x31U));
    assert(dmr_flco_opens_voice_call(0x08U, 0x00U));
    // FIDs whose LCs dsd-neo does not read as calls (Tait, Kirisun, unknown).
    assert(!dmr_flco_opens_voice_call(0x58U, 0x00U));
    assert(!dmr_flco_opens_voice_call(0x0AU, 0x00U));
    assert(!dmr_flco_opens_voice_call(0x33U, 0x00U));
}

/* A Hytera XPT call alert LC on slot 0. */
static void
build_xpt_alert(uint8_t bits[80], int group, uint32_t target) {
    DSD_MEMSET(bits, 0, 80U);
    bits[1] = (uint8_t)(group != 0);
    write_bits(bits, 2U, 0x09U, 6U);
    write_bits(bits, 8U, 0x68U, 8U);
    write_bits(bits, 32U, target, 16U);
    write_bits(bits, 56U, 0x0042U, 16U);
}

static void
ras_relaxed_lc_does_not_brand_xpt(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = new_state();

    uint8_t bits[80];
    build_xpt_alert(bits, 1, 0x0005U);
    uint32_t irr = 0;
    state->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED; // accepted by RAS under -F; its own CRC never verified
    dmr_flco(opts, state, bits, 1U, &irr, 3U);
    state->event_crc_invalid[0] = 0U;
    assert(strcmp(state->dmr_branding_sub, "XPT ") != 0);

    dsd_call_snapshot call;
    assert(run_header(opts, state, 0x00U, 0x00U, 0x00U, 0x123456U, 0x000042U, &call));
    assert(call.kind == DSD_CALL_KIND_GROUP_VOICE);
    assert(call.ota_target_id == 0x123456U);
    free_state(state);

    // A checked XPT LC still brands the site.
    state = new_state();
    irr = 0;
    dmr_flco(opts, state, bits, 1U, &irr, 3U);
    assert(strcmp(state->dmr_branding_sub, "XPT ") == 0);
    free_state(state);
    free(opts);
}

/* An XPT CSBK (FID 0x68) with opcode @p csbk_o on slot 0, its CRC passing or accepted by RAS. */
static void
run_xpt_csbk(dsd_opts* opts, dsd_state* state, uint8_t csbk_o, int ras_relaxed) {
    uint8_t pdu[12];
    uint8_t pdu_bits[96];
    DSD_MEMSET(pdu, 0, sizeof(pdu));
    pdu[0] = csbk_o;
    pdu[1] = 0x68U;
    for (size_t i = 0; i < sizeof(pdu_bits); i++) {
        pdu_bits[i] = (uint8_t)((pdu[i / 8U] >> (7U - (i % 8U))) & 1U);
    }
    state->event_crc_invalid[0] = ras_relaxed ? DSD_CRC_RAS_UNVERIFIED : 0U;
    dmr_cspdu(opts, state, pdu_bits, pdu, 1U, 0U);
    state->event_crc_invalid[0] = 0U;
}

static void
ras_relaxed_csbk_does_not_brand_xpt(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    // 0x0A: XPT site status; 0x0B: XPT adjacent sites.
    const uint8_t opcodes[2] = {0x0AU, 0x0BU};
    for (size_t i = 0; i < 2U; i++) {
        dsd_state* state = new_state();
        run_xpt_csbk(opts, state, opcodes[i], 1);
        assert(strcmp(state->dmr_branding_sub, "XPT ") != 0);
        dsd_call_snapshot call;
        assert(run_header(opts, state, 0x00U, 0x00U, 0x00U, 0x123456U, 0x000042U, &call));
        assert(call.ota_target_id == 0x123456U);
        free_state(state);

        // A checked XPT CSBK still brands the site.
        state = new_state();
        run_xpt_csbk(opts, state, opcodes[i], 0);
        assert(strcmp(state->dmr_branding_sub, "XPT ") == 0);
        free_state(state);
    }
    free(opts);
}

/* A checked LC of the given type on slot 0. */
static void
run_lc(dsd_opts* opts, dsd_state* state, uint8_t type, uint8_t flco, uint8_t so) {
    uint8_t bits[80];
    build_lc(bits, flco, 0x00U, so, 0x123U, 0x456U);
    uint32_t irr = 0;
    dmr_flco(opts, state, bits, 1U, &irr, type);
}

static void
fid0_non_voice_lc_leaves_privacy_and_hold(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);

    // An open clear call stays clear: a type-1 header with FLCO 0x04 (strong evidence, were it a voice user) and
    // repeated embedded FLCO 0x01 LCs whose service-option byte would read as encrypted.
    dsd_state* state = new_state();
    run_lc(opts, state, 1U, 0x00U, 0x00U);
    assert(dmr_enc_class_established_clear(state, 0U));
    run_lc(opts, state, 1U, 0x04U, 0x40U);
    for (int i = 0; i < 4; i++) {
        run_lc(opts, state, 3U, 0x01U, 0x40U);
    }
    assert(dmr_enc_class_established_clear(state, 0U));
    assert(!dmr_enc_class_established_enc(state, 0U));
    assert((state->dmr_so & 0x40U) == 0U);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.ota_target_id == 0x123U);
    free_state(state);

    // A held slot stays held until a voice user's checked LC says what the call is.
    state = new_state();
    dmr_enc_class_hold(state);
    assert(state->dmr_enc_class_held[0] == 1U);
    run_lc(opts, state, 1U, 0x04U, 0x00U);
    run_lc(opts, state, 3U, 0x01U, 0x00U);
    assert(state->dmr_enc_class_held[0] == 1U);
    run_lc(opts, state, 1U, 0x00U, 0x00U);
    assert(state->dmr_enc_class_held[0] == 0U);
    free_state(state);
    free(opts);
}

/* The terminal line of a checked voice LC header on slot 0. */
static void
header_print(uint8_t flco, uint8_t so, char* out, size_t out_size) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = new_state();
    dsd_call_snapshot call;

    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dsdneo_dmr_fid0_print") == 0);
    (void)run_header(opts, state, flco, 0x00U, so, 0x123U, 0x456U, &call);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, out, out_size) == 0);
    free_state(state);
    free(opts);
}

static void
fid0_non_voice_lc_prints_no_service_options(void) {
    // Emergency, privacy, broadcast, OVCM and priority 3, were the byte service options.
    const uint8_t so = 0x80U | 0x40U | 0x08U | 0x04U | 0x03U;
    char out[2048];
    header_print(0x01U, so, out, sizeof out);
    assert(strstr(out, "TGT=291") != NULL);
    assert(strstr(out, "Encrypted") == NULL);
    assert(strstr(out, "Emergency") == NULL);
    assert(strstr(out, "Priority") == NULL);
    assert(strstr(out, "Broadcast") == NULL);
    assert(strstr(out, "OVCM") == NULL);

    // A voice channel user shows them.
    header_print(0x00U, so, out, sizeof out);
    assert(strstr(out, "Encrypted") != NULL);
    assert(strstr(out, "Emergency") != NULL);
    assert(strstr(out, "Priority 3") != NULL);
    assert(strstr(out, "Broadcast") != NULL);
    assert(strstr(out, "OVCM") != NULL);
}

static void
xpt_alert_label(uint32_t target, char* out, size_t out_size) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    assert(opts != NULL);
    dsd_state* state = new_state();
    uint8_t bits[80];
    build_xpt_alert(bits, 1, target);
    uint32_t irr = 0;

    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dsdneo_dmr_xpt_alert") == 0);
    dmr_flco(opts, state, bits, 1U, &irr, 3U);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, out, out_size) == 0);
    free_state(state);
    free(opts);
}

static void
xpt_alert_labels(void) {
    char out[2048];
    xpt_alert_label(249U, out, sizeof out);
    assert(strstr(out, "Group Call Alert") != NULL);
    assert(strstr(out, "Emergency") == NULL);

    xpt_alert_label(250U, out, sizeof out);
    assert(strstr(out, "Group Emergency Call Alert") != NULL);
    xpt_alert_label(254U, out, sizeof out);
    assert(strstr(out, "Group Emergency Call Alert") != NULL);

    xpt_alert_label(255U, out, sizeof out);
    assert(strstr(out, "Group All Call Alert") != NULL);
    assert(strstr(out, "Emergency") == NULL);
}

int
main(void) {
    fid0_flco_01_opens_no_call();
    fid0_flco_00_opens_group();
    fid0_flco_03_opens_unit();
    opens_voice_call_table();
    ras_relaxed_lc_does_not_brand_xpt();
    xpt_alert_labels();
    ras_relaxed_csbk_does_not_brand_xpt();
    fid0_non_voice_lc_leaves_privacy_and_hold();
    fid0_non_voice_lc_prints_no_service_options();
    printf("DMR FLCO FID 0 gate tests passed.\n");
    return 0;
}
