// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression: Motorola P25 systems transmit the talker alias during hangtime,
 * after MAC_END_PTT has already ended the call epoch. The alias attach gate
 * must accept the retained ended epoch when the FQ-SUID source matches, or
 * every hangtime alias is deferred and lost (observed live: 21/21 CRC-valid
 * aliases dropped with "current call source mismatch").
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_fail;

/* The GPS decoders' Cellocator path prints through the DMR text helper, which this core-only binary does not link. */
void
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

static void
expect_true(const char* label, int got) {
    if (!got) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_fail = 1;
    }
}

static void
set_bits(uint8_t* bits, int start, int len, uint32_t value) {
    for (int i = 0; i < len; i++) {
        bits[start + i] = (uint8_t)((value >> (len - 1 - i)) & 1U);
    }
}

/* Build the post-CRC input handed to apx_embedded_alias_dump: the FQ-SUID
 * occupies bits 72..127 (WACN 20, SYSID 12, RID 24). */
static void
build_alias_input(uint8_t* input, size_t input_bits, uint32_t wacn, uint32_t sys, uint32_t rid) {
    DSD_MEMSET(input, 0, input_bits);
    set_bits(input, 72, 20, wacn);
    set_bits(input, 92, 12, sys);
    set_bits(input, 104, 24, rid);
}

/* Decoded alias characters live in the odd bytes of the decoded buffer. */
static uint16_t
build_alias_decoded(uint8_t* decoded, size_t decoded_sz, const char* text) {
    DSD_MEMSET(decoded, 0, decoded_sz);
    size_t len = strlen(text);
    for (size_t i = 0; i < len && (i * 2 + 1) < decoded_sz; i++) {
        decoded[(i * 2) + 1] = (uint8_t)text[i];
    }
    return (uint16_t)(len * 2);
}

static int
observe_call(dsd_state* state, uint64_t tg, uint64_t src, double observed_m, dsd_call_boundary boundary) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = tg,
        .policy_target_id = tg,
        .ota_source_id = src,
        .service_options = 0U,
        .has_service_metadata = 1U,
        .observed_m = observed_m,
    };
    return dsd_call_state_observe(state, &observation, boundary);
}

/* Harris Phase 1 alias block LCW: opcode (0x32..0x35) in bits 0..7, seven
 * payload characters starting at bit 16. */
static void
build_l3h_block(uint8_t* lcw, size_t lcw_bits, uint8_t opcode, const char payload[7]) {
    DSD_MEMSET(lcw, 0, lcw_bits);
    set_bits(lcw, 0, 8, opcode);
    for (int i = 0; i < 7; i++) {
        set_bits(lcw, 16 + (i * 8), 8, (uint8_t)payload[i]);
    }
}

static int
history_has_alias(const dsd_state* state, const char* needle) {
    for (int i = 0; i < 8; i++) {
        if (strstr(state->event_history_s[0].Event_History_Items[i].alias, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* A fresh state whose slot @p slot holds an open call of @p protocol, @p tg / @p src, its epoch opened on the event
   row: another protocol's call whose end never decoded (-fa, or a -Y step), which slot 0 keeps until the next call
   begins. */
static dsd_state*
foreign_call_fixture(dsd_opts* opts, int protocol, uint8_t slot, uint64_t tg, uint64_t src) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    Event_History_I* history = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    if (!state || !history) {
        free(state);
        free(history);
        return NULL;
    }
    state->event_history_s = history;
    init_event_history(&state->event_history_s[0], 0, 255);
    init_event_history(&state->event_history_s[1], 0, 255);
    state->currentslot = slot;
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = tg,
        .policy_target_id = tg,
        .ota_source_id = src,
        .observed_m = 1.0,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(opts, state, slot);
    return state;
}

static void
free_fixture(dsd_state* state) {
    if (state) {
        Event_History_I* history = state->event_history_s;
        dsd_state_ext_free_all(state);
        free(history);
        free(state);
    }
}

/* Whether a radio alias was learned for @p id (the talkgroup policy's runtime alias rows). */
static int
alias_learned_for(dsd_state* state, uint32_t id) {
    dsd_tg_policy_lookup lookup;
    return dsd_tg_policy_lookup_id(state, id, &lookup) == 0 && lookup.match != DSD_TG_POLICY_MATCH_NONE;
}

/* Whether the open call's row (the staged row of @p slot) took an alias or a position. */
static int
call_row_took_detail(const dsd_state* state, uint8_t slot) {
    const Event_History* row = &state->event_history_s[slot].Event_History_Items[0];
    return row->alias[0] != '\0' || row->gps_s[0] != '\0';
}

/* An alias or a position decoded before its own protocol's call opened on the slot is that protocol's, never the open
   call of another protocol the slot still holds: that call's row takes nothing, whichever writer it comes from. */
static void
test_enrichment_skips_another_protocols_call(dsd_opts* opts) {
    uint8_t input[3200];
    uint8_t decoded[200];
    uint8_t bits[512];
    char label[96];

    /* P25 Motorola (APX) talker alias naming a radio whose ID an open DMR call happens to carry. */
    dsd_state* state = foreign_call_fixture(opts, DSD_SYNC_DMR_BS_VOICE_POS, 0U, 100U, 10976U);
    build_alias_input(input, sizeof(input), 0xBEE00, 0x4C9, 10976U);
    uint16_t num_bytes = build_alias_decoded(decoded, sizeof(decoded), "APXUNIT");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("p25 apx alias leaves the dmr call's row alone", !call_row_took_detail(state, 0U));
    expect_true("p25 apx alias shows no alias for the dmr call", state->generic_talker_alias[0][0] == '\0');
    free_fixture(state);

    /* P25 Phase 2 Harris talker alias (it names no source: the slot's call is taken as its talker). */
    state = foreign_call_fixture(opts, DSD_SYNC_DMR_BS_VOICE_POS, 1U, 100U, 810001U);
    uint8_t l3h_msg[24];
    DSD_MEMSET(l3h_msg, 0, sizeof(l3h_msg));
    DSD_MEMCPY(l3h_msg + 4, "HARRIS1", 7);
    l3h_embedded_alias_decode(opts, state, 1U, 10, l3h_msg);
    expect_true("p25 harris alias leaves the dmr call's row alone", !call_row_took_detail(state, 1U));
    expect_true("p25 harris alias shows no alias for the dmr call", state->generic_talker_alias[1][0] == '\0');
    expect_true("p25 harris alias teaches the dmr radio no alias", !alias_learned_for(state, 810001U));
    free_fixture(state);

    /* P25 Tait talker alias (it names no source either). */
    state = foreign_call_fixture(opts, DSD_SYNC_NXDN_POS, 0U, 100U, 4242U);
    DSD_MEMSET(bits, 0, sizeof(bits));
    const char tait[] = "TAITUNIT";
    for (int i = 0; i < 8; i++) {
        set_bits(bits, 16 + (i * 7), 7, (uint32_t)tait[i]);
    }
    tait_iso7_embedded_alias_decode(opts, state, 0U, 8, bits);
    expect_true("p25 tait alias leaves the nxdn call's row alone", !call_row_took_detail(state, 0U));
    expect_true("p25 tait alias teaches the nxdn radio no alias", !alias_learned_for(state, 4242U));
    free_fixture(state);

    /* DMR talker alias, its voice LC not decoded yet, with a P25 call still open on the slot. */
    state = foreign_call_fixture(opts, DSD_SYNC_P25P1_POS, 0U, 100U, 3333U);
    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    const char ta[] = "DMRUNIT7";
    for (int i = 0; i < 8; i++) {
        set_bits(state->dmr_pdu_sf[0], i * 8, 8, (uint32_t)ta[i]);
    }
    dmr_talker_alias_lc_decode(opts, state, 0U, 0U, 8U, 8U);
    expect_true("dmr talker alias leaves the p25 call's row alone", !call_row_took_detail(state, 0U));
    free_fixture(state);

    /* Positions: DMR embedded GPS and LIP, P25 Motorola and Harris GPS, NXDN GPS, DMR UDT NMEA. Each payload is a
       valid fix the feed admits (22.5 S 45 W, or 22.5 N for the P25 reports), decoded on its own protocol's carrier;
       the open call is another protocol's (with the source the report names, where it names one). */
    static const struct {
        const char* name;
        int call_protocol;
        int report_protocol;
        int writer;
    } gps[] = {
        {"dmr embedded gps", DSD_SYNC_P25P1_POS, DSD_SYNC_DMR_BS_VOICE_POS, 0},
        {"p25 apx gps", DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_P25P1_POS, 1},
        {"p25 harris gps", DSD_SYNC_NXDN_POS, DSD_SYNC_P25P1_POS, 2},
        {"nxdn gps", DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_NXDN_POS, 3},
        {"dmr lip report", DSD_SYNC_P25P1_POS, DSD_SYNC_DMR_BS_DATA_POS, 4},
        {"dmr udt nmea", DSD_SYNC_NXDN_POS, DSD_SYNC_DMR_BS_DATA_POS, 5},
    };

    for (size_t i = 0; i < sizeof(gps) / sizeof(gps[0]); i++) {
        const uint32_t src = 5555U;
        state = foreign_call_fixture(opts, gps[i].call_protocol, 0U, 100U, src);
        if (state == NULL || dsd_position_feed_start(state) != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s fixture\n", gps[i].name);
            g_fail = 1;
            free_fixture(state);
            continue;
        }
        state->synctype = gps[i].report_protocol;
        state->lastsynctype = gps[i].report_protocol;
        DSD_MEMSET(bits, 0, sizeof(bits));
        /* The P25 link-control reports as a checked message of the report's source would carry them, to the slot's
           ACTIVE call; the carried reports addressed from that source. */
        dsd_gps_report_ctx p25_ctx;
        DSD_MEMSET(&p25_ctx, 0, sizeof(p25_ctx));
        p25_ctx.source_id = src;
        p25_ctx.protocol = DSD_SYNC_P25P1_POS;
        p25_ctx.pos_verified = 1U;
        p25_ctx.source_verified = 1U;
        const dsd_gps_address addr = {src, 0U, 0U};
        /* L3Harris body: 22 30 N, 45 E, 02:00:00, heading 90.0, at rest, quality %01, 5 satellites. */
        static const uint8_t harris[14] = {0, 0, 30, 22, 0, 0, 0, 45, 0x1C, 0x20, 0x84, 0x00, 0x03, 0x15};
        switch (gps[i].writer) {
            case 0:
                /* Embedded GPS LC (FLCO 0x08): 20 m, 22.5 S, 45 W. */
                set_bits(bits, 2, 6, 0x08);
                set_bits(bits, 20, 3, 1);
                set_bits(bits, 23, 1, 1);
                set_bits(bits, 24, 24, 0xC00000);
                set_bits(bits, 48, 1, 1);
                set_bits(bits, 49, 23, 0x600000);
                dmr_embedded_gps(opts, state, bits);
                break;
            case 1:
                /* MFID $90 LCO $06: latitude magnitude 0x200000 (22.5 N), longitude magnitude 0x300000 (67.5 E). */
                set_bits(bits, 0, 8, 0x06);
                set_bits(bits, 8, 8, 0x90);
                set_bits(bits, 24, 24, 0x200000);
                set_bits(bits, 48, 24, 0x300000);
                (void)apx_embedded_gps(opts, state, bits, &p25_ctx);
                break;
            case 2: (void)nmea_harris(opts, state, harris, &p25_ctx); break;
            case 3:
                /* After the 0xFFFC marker: longitude and latitude DDDMM/DDMM, then the date. */
                set_bits(bits, 168 - 16, 16, 2230U);
                set_bits(bits, 200 - 16, 16, 1000U);
                set_bits(bits, 152 - 16, 7, 25U);
                set_bits(bits, 159 - 16, 4, 6U);
                set_bits(bits, 163 - 16, 5, 20U);
                nxdn_gps_report(opts, state, bits, sizeof(bits), src, NULL);
                break;
            case 4:
                /* LIP short location report: 22.5 S, 45 W. */
                set_bits(bits, 4, 1, 1);
                set_bits(bits, 5, 24, 0xC00000);
                set_bits(bits, 29, 1, 1);
                set_bits(bits, 30, 23, 0x600000);
                set_bits(bits, 68, 8, 32);
                lip_pdu_decoder(opts, state, bits, 76U, &addr);
                break;
            default:
                /* Long-form binary NMEA: 22 30 S, 45 W. */
                set_bits(bits, 3, 1, 1);
                set_bits(bits, 11, 7, 22);
                set_bits(bits, 18, 6, 30);
                set_bits(bits, 38, 8, 45);
                set_bits(bits, 66, 5, 11);
                set_bits(bits, 71, 6, 22);
                set_bits(bits, 77, 6, 33);
                nmea_iec_61162_1(opts, state, bits, NMEA_UDT_LONG_BITS, &addr, 2);
                break;
        }
        dsd_position_fix fix;
        dsd_position_feed_meta meta;
        DSD_SNPRINTF(label, sizeof(label), "%s is admitted", gps[i].name);
        expect_true(label, dsd_position_feed_copy_since(state, 0U, &fix, 1U, &meta) == 1);
        DSD_SNPRINTF(label, sizeof(label), "%s leaves the other protocol's call row alone", gps[i].name);
        const Event_History* row = &state->event_history_s[0].Event_History_Items[0];
        expect_true(label, row->gps_s[0] == '\0' && row->pos.valid == 0U);
        free_fixture(state);
    }
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    Event_History_I* history = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    if (!opts || !state || !history) {
        DSD_FPRINTF(stderr, "FAIL: allocation\n");
        free(history);
        free(state);
        free(opts);
        return 1;
    }
    state->event_history_s = history;
    init_event_history(&state->event_history_s[0], 0, 255);
    init_event_history(&state->event_history_s[1], 0, 255);

    const uint32_t wacn = 0xBEE00;
    const uint32_t sys = 0x4C9;
    const uint32_t rid = 10976;
    const uint64_t tg = 5204;

    uint8_t input[3200];
    uint8_t decoded[200];

    /* Sanity: alias during the active call attaches. */
    expect_true("observe begins epoch", observe_call(state, tg, rid, 1.0, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    build_alias_input(input, sizeof(input), wacn, sys, rid);
    uint16_t num_bytes = build_alias_decoded(decoded, sizeof(decoded), "ACTIVE1");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("active-call alias attaches", strstr(state->generic_talker_alias[0], "ACTIVE1") != NULL);

    /* Regression: Motorola hangtime alias arrives after the epoch ended. */
    expect_true("end call", dsd_call_state_end_ex(state, 0U, 2.0, DSD_CALL_END_TERMINATOR) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    state->generic_talker_alias[0][0] = '\0';
    num_bytes = build_alias_decoded(decoded, sizeof(decoded), "HANGTIME");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("hangtime alias attaches to retained ended call",
                strstr(state->generic_talker_alias[0], "HANGTIME") != NULL);
    expect_true("hangtime alias reaches event history", history_has_alias(state, "HANGTIME"));

    /* A hangtime alias for a different radio must not attach. */
    state->generic_talker_alias[0][0] = '\0';
    build_alias_input(input, sizeof(input), wacn, sys, rid + 1U);
    num_bytes = build_alias_decoded(decoded, sizeof(decoded), "WRONGRID");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("mismatched source is still deferred", state->generic_talker_alias[0][0] == '\0');
    expect_true("mismatched source never reaches history", !history_has_alias(state, "WRONGRID"));

    /* Once a new call begins with a different source, the stale alias must not attach. */
    expect_true("next call begins", observe_call(state, tg, rid + 2U, 3.0, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    state->generic_talker_alias[0][0] = '\0';
    build_alias_input(input, sizeof(input), wacn, sys, rid);
    num_bytes = build_alias_decoded(decoded, sizeof(decoded), "STALE999");
    apx_embedded_alias_dump(opts, state, 0U, num_bytes, input, decoded);
    expect_true("stale alias does not attach to the next call", state->generic_talker_alias[0][0] == '\0');

    /* Harris Phase 1: alias blocks that straddle the end of the transmission
     * must finish assembling against the retained ended epoch. */
    const uint64_t l3h_src = 810001;
    uint8_t lcw[72];
    expect_true("harris call begins", observe_call(state, tg, l3h_src, 4.0, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    state->generic_talker_alias[0][0] = '\0';
    build_l3h_block(lcw, sizeof(lcw), 0x32U, "ENGINE ");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    build_l3h_block(lcw, sizeof(lcw), 0x33U, "51     ");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    expect_true("harris partial alias attaches while active",
                strstr(state->generic_talker_alias[0], "ENGINE51") != NULL);

    expect_true("harris call ends", dsd_call_state_end_ex(state, 0U, 5.0, DSD_CALL_END_TERMINATOR) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    build_l3h_block(lcw, sizeof(lcw), 0x34U, "STATION");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    build_l3h_block(lcw, sizeof(lcw), 0x35U, "9      ");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    expect_true("harris alias completes during hangtime",
                strstr(state->generic_talker_alias[0], "ENGINE51STATION9") != NULL);
    expect_true("harris hangtime completion reaches event history", history_has_alias(state, "ENGINE51STATION9"));

    /* Regression: a phase-2 Harris alias (single self-contained MAC message, no
     * source in the payload) that arrives while only the retained ended epoch is
     * on the slot may belong to the *next* talker, whose grant has not been
     * observed yet. Attaching it would overwrite the ended call's history row
     * with the wrong alias (observed live as event-history rows carrying the
     * following call's alias). It must defer instead. */
    uint8_t l3h_msg[24];
    DSD_MEMSET(l3h_msg, 0, sizeof(l3h_msg));
    DSD_MEMCPY(l3h_msg + 4, "NEXTGUY", 7);
    state->generic_talker_alias[0][0] = '\0';
    l3h_embedded_alias_decode(opts, state, 0U, 10, l3h_msg);
    expect_true("phase-2 alias with no active call is deferred", state->generic_talker_alias[0][0] == '\0');
    expect_true("phase-2 alias never lands on the ended call's row", !history_has_alias(state, "NEXTGUY"));
    expect_true("ended harris call keeps its own alias", history_has_alias(state, "ENGINE51STATION9"));

    /* Once the next call is active, the repeated phase-2 alias attaches to it. */
    expect_true("phase-2 follow-on call begins",
                observe_call(state, tg, l3h_src + 1U, 6.0, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    l3h_embedded_alias_decode(opts, state, 0U, 10, l3h_msg);
    expect_true("phase-2 alias attaches to the active call", strstr(state->generic_talker_alias[0], "NEXTGUY") != NULL);

    /* Regression: phase-1 fragments whose block 0 arrived after the epoch ended
     * carry no proof they belong to that epoch; the assembled alias must defer
     * rather than rewrite the ended call's row. */
    expect_true("phase-1 follow-on call ends", dsd_call_state_end_ex(state, 0U, 7.0, DSD_CALL_END_TERMINATOR) > 0);
    dsd_event_sync_slot(opts, state, 0U);
    state->generic_talker_alias[0][0] = '\0';
    build_l3h_block(lcw, sizeof(lcw), 0x32U, "LATCHED");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    build_l3h_block(lcw, sizeof(lcw), 0x33U, "WRONG  ");
    l3h_embedded_alias_blocks_phase1(opts, state, 0U, lcw);
    expect_true("phase-1 fragments started after end are deferred", state->generic_talker_alias[0][0] == '\0');
    expect_true("phase-1 late fragments never reach history", !history_has_alias(state, "LATCHEDWRONG"));

    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    test_enrichment_skips_another_protocols_call(opts);
    free(opts);
    return g_fail;
}
