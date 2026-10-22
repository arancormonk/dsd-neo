// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Signal search site notes from DMR: a C_ALOHA CSBK that passed its CRC and a C_SYS_PARMS / P_SYS_PARMS short LC that
 * passed Hamming(17,12,3) and its CRC-8 note the Tier III identity they carry; the same messages failing their checks
 * note nothing.
 */

#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_search_running = 1;

static int
search_running(const dsd_state* state) {
    (void)state;
    return g_search_running;
}

static int g_notes;
static dsd_signal_search_site_note g_note;

static void
capture_site(const dsd_state* state, const dsd_signal_search_site_note* note) {
    (void)state;
    ++g_notes;
    g_note = *note;
}

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_text(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
put_bits(uint8_t* bits, int at, uint32_t value, int width) {
    for (int i = 0; i < width; ++i) {
        bits[at + i] = (uint8_t)((value >> (width - 1 - i)) & 1U);
    }
}

/* Small model, network 12, site 3: 01 0001100 00011. */
enum { SYSCODE_SMALL_12_3 = (1 << 12) | (12 << 5) | 3 };

static void
aloha(dsd_opts* opts, dsd_state* state, int crc_ok) {
    uint8_t bits[196];
    uint8_t bytes[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 25; // C_ALOHA
    put_bits(bits, 0, 25, 8);
    put_bits(bits, 40, SYSCODE_SMALL_12_3, 14);
    dmr_cspdu(opts, state, bits, bytes, (uint32_t)crc_ok, 0);
}

/* The same C_ALOHA as a BPTC-encoded CSBK burst through the data burst handler: with its CRC, or with the CRC off by one
   bit and the RAS reserved-bit marker, which -F accepts as RAS. */
static void
aloha_burst(dsd_opts* opts, dsd_state* state, int ras) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));
    put_bits(bits, 0, 25, 8);
    put_bits(bits, 40, SYSCODE_SMALL_12_3, 14);
    uint16_t crc = (uint16_t)(ComputeCrcCCITT(bits) ^ 0xA5A5U);
    if (ras) {
        crc ^= 0x0001U;
    }
    put_bits(bits, 80, crc, 16);
    static const uint8_t k_ras_reserved[3] = {0, 0, 1};
    static const uint8_t k_no_reserved[3] = {0, 0, 0};
    uint8_t info[196];
    dmr_test_encode_bptc_196x96(bits, ras ? k_ras_reserved : k_no_reserved, info);
    dmr_data_burst_handler(opts, state, info, 0x03, NULL);
}

static void
hamming17_parity(uint8_t* slc) {
    slc[12] = (uint8_t)(slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[6] ^ slc[7] ^ slc[9]);
    slc[13] = (uint8_t)(slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[7] ^ slc[8] ^ slc[10]);
    slc[14] = (uint8_t)(slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[5] ^ slc[8] ^ slc[9] ^ slc[11]);
    slc[15] = (uint8_t)(slc[0] ^ slc[1] ^ slc[4] ^ slc[5] ^ slc[7] ^ slc[10]);
    slc[16] = (uint8_t)(slc[0] ^ slc[1] ^ slc[2] ^ slc[5] ^ slc[6] ^ slc[8] ^ slc[11]);
}

/* The TACT for @p lcss: the Hamming(7,4) codeword the decoder takes as it is. */
static void
tact(uint8_t cach[25], int lcss) {
    for (int p = 0; p < 8; ++p) {
        uint8_t bits[7] = {0,
                           0,
                           (uint8_t)((lcss >> 1) & 1),
                           (uint8_t)(lcss & 1),
                           (uint8_t)((p >> 2) & 1),
                           (uint8_t)((p >> 1) & 1),
                           (uint8_t)(p & 1)};
        uint8_t check[7];
        DSD_MEMCPY(check, bits, sizeof(check));
        if (Hamming_7_4_decode(check) && memcmp(check, bits, sizeof(bits)) == 0) {
            DSD_MEMCPY(cach, bits, sizeof(bits));
            return;
        }
    }
    abort();
}

/* Feed a four-fragment short LC whose 28 data bits are @p payload; @p corrupt flips one data bit after the CRC-8 is
   computed, so the message fails it. */
static void
slco_send(dsd_opts* opts, dsd_state* state, const uint8_t payload[28], int corrupt) {
    uint8_t data[44];
    DSD_MEMSET(data, 0, sizeof(data));
    DSD_MEMCPY(data, payload, 28);
    // The CRC-8 whose residue over the 36 bits is zero, as crc8_ok() reads it with the bits after them cleared.
    int crc = 0;
    for (; crc < 256; ++crc) {
        put_bits(data, 28, (uint32_t)crc, 8);
        if (crc8_ok(data, 36)) {
            break;
        }
    }
    if (crc == 256) {
        abort();
    }
    if (corrupt) {
        data[20] ^= 1U;
    }
    uint8_t slco_bits[68];
    DSD_MEMSET(slco_bits, 0, sizeof(slco_bits));
    for (size_t block = 0; block < 3; ++block) {
        for (size_t i = 0; i < 12; ++i) {
            slco_bits[block * 17 + i] = data[block * 12 + i];
        }
        hamming17_parity(&slco_bits[block * 17]);
    }
    uint8_t raw[68];
    for (int i = 0; i < 67; ++i) {
        raw[(i * 4) % 67] = slco_bits[i];
    }
    raw[67] = slco_bits[67];
    static const int lcss[4] = {1, 3, 3, 2};
    for (size_t f = 0; f < 4; ++f) {
        uint8_t cach[25];
        DSD_MEMSET(cach, 0, sizeof(cach));
        tact(cach, lcss[f]);
        DSD_MEMCPY(&cach[7], &raw[f * 17], 17);
        (void)dmr_cach(opts, state, cach);
    }
}

/* A short LC carrying @p slco with SYSCODE_SMALL_12_3. */
static void
slco_sys_parms(dsd_opts* opts, dsd_state* state, int slco, int corrupt) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, (uint32_t)slco, 4);
    put_bits(payload, 4, SYSCODE_SMALL_12_3, 14);
    slco_send(opts, state, payload, corrupt);
}

/* A Connect Plus control (0xA) or traffic (0x9) channel short LC naming @p net and @p site. */
static void
slco_con_plus(dsd_opts* opts, dsd_state* state, int slco, int net, int site) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, (uint32_t)slco, 4);
    put_bits(payload, 8, (uint32_t)net, 8);
    put_bits(payload, 16, (uint32_t)site, 8);
    slco_send(opts, state, payload, 0);
}

/* A Capacity Plus short LC (0xF) naming the rest channel @p rest and site @p site. */
static void
slco_cap_plus(dsd_opts* opts, dsd_state* state, int rest, int site, int corrupt) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, 0xF, 4);
    put_bits(payload, 16, (uint32_t)rest, 4);
    put_bits(payload, 22, (uint32_t)site, 3);
    slco_send(opts, state, payload, corrupt);
}

/* A Capacity Plus channel status CSBK (opcode 0x3E, FID 0x10) in one block, naming rest LSN @p rest. */
static void
cap_plus_status(dsd_opts* opts, dsd_state* state, int rest) {
    uint8_t bits[196];
    uint8_t bytes[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 0x3E;
    bytes[1] = 0x10;
    put_bits(bits, 0, 0x3E, 8);
    put_bits(bits, 8, 0x10, 8);
    put_bits(bits, 16, 3, 2); // first and last block
    put_bits(bits, 19, (uint32_t)rest, 5);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

/* A Capacity Plus group voice LC (FLCO 0x04, FID 0x10) naming rest LSN @p rest, its check passed or not. */
static void
cap_plus_lc(dsd_opts* opts, dsd_state* state, int rest, uint32_t crc_correct) {
    uint8_t lc[96];
    DSD_MEMSET(lc, 0, sizeof(lc));
    put_bits(lc, 2, 0x04, 6);
    put_bits(lc, 8, 0x10, 8);
    put_bits(lc, 40, 7, 8);
    put_bits(lc, 51, (uint32_t)rest, 5);
    put_bits(lc, 56, 1234, 16);
    uint32_t irrecoverable = 0;
    dmr_flco(opts, state, lc, crc_correct, &irrecoverable, 1);
}

/* A Hytera XPT site status CSBK (opcode 0x0A, FID 0x68) naming free LCN @p free_lcn. */
static void
xpt_site_status(dsd_opts* opts, dsd_state* state, int free_lcn) {
    uint8_t bits[196];
    uint8_t bytes[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 0x0A;
    bytes[1] = 0x68;
    put_bits(bits, 0, 0x0A, 8);
    put_bits(bits, 8, 0x68, 8);
    put_bits(bits, 16, (uint32_t)free_lcn, 4);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

/* A Hytera XPT call alert LC (FLCO 0x09, FID 0x68) naming free LCN @p free_lcn, its check passed or not. */
static void
xpt_call_alert(dsd_opts* opts, dsd_state* state, int free_lcn, uint32_t crc_correct) {
    uint8_t lc[96];
    DSD_MEMSET(lc, 0, sizeof(lc));
    put_bits(lc, 2, 0x09, 6);
    put_bits(lc, 8, 0x68, 8);
    put_bits(lc, 16, 2, 4);
    put_bits(lc, 24, (uint32_t)free_lcn, 4);
    put_bits(lc, 32, 100, 16);
    put_bits(lc, 56, 1234, 16);
    uint32_t irrecoverable = 0;
    dmr_flco(opts, state, lc, crc_correct, &irrecoverable, 1);
}

/* A Hytera XPT short LC (0x8) naming free LCN @p free_lcn. */
static void
slco_xpt(dsd_opts* opts, dsd_state* state, int free_lcn, int corrupt) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, 0x8, 4);
    put_bits(payload, 12, (uint32_t)free_lcn, 4);
    put_bits(payload, 16, 1, 4);
    slco_send(opts, state, payload, corrupt);
}

static void
clear_record(dsd_state* state) {
    dsd_rxid_reset(state);
}

/* The received identity record (dsd_state::rx_id) takes the identity of the same checked messages, whether or not a
   search runs; a message the decoder accepted only through RAS (-F), or one whose check failed, names nothing. */
static int
test_received_identity(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    g_search_running = 0;
    opts->dmr_dmrla_is_set = 1;
    opts->dmr_dmrla_n = 0;

    clear_record(state);
    aloha(opts, state, 1);
    rc |= expect_int("rx-id aloha parts", (long)id->valid, DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT);
    rc |= expect_int("rx-id aloha syscode", id->dmr_t3_syscode, SYSCODE_SMALL_12_3);
    rc |= expect_text("rx-id aloha site", id->dmr_site_text,
                      "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3 \xC2\xB7 SYS 1183");
    // The same message again, after the record went stale: an unchanged repeat proves it again.
    dsd_rxid_mark_stale(state);
    aloha(opts, state, 1);
    rc |= expect_int("rx-id aloha repeat is fresh", (long)id->fresh, DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT);

    static const uint8_t unverified[2] = {DSD_CRC_RAS_UNVERIFIED, DSD_CRC_FAILED};
    for (int i = 0; i < 2; ++i) {
        clear_record(state);
        state->currentslot = 1;
        state->event_crc_invalid[1] = unverified[i];
        aloha(opts, state, 1);
        cap_plus_status(opts, state, 5);
        state->event_crc_invalid[1] = 0;
        state->currentslot = 0;
        rc |= expect_int("rx-id unverified csbks note nothing", (long)id->valid, 0);
    }
    rc |= expect_int("rx-id unverified cap+ status still follows", state->dmr_rest_channel, 5);
    clear_record(state);
    opts->aggressive_framesync = 0;
    aloha_burst(opts, state, 1);
    rc |= expect_int("rx-id ras aloha burst notes nothing", (long)id->valid, 0);
    aloha_burst(opts, state, 0);
    rc |= expect_int("rx-id checked aloha burst notes", (long)id->valid,
                     DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT);

    for (int slco = 2; slco <= 3; ++slco) {
        clear_record(state);
        slco_sys_parms(opts, state, slco, 1);
        rc |= expect_int("rx-id failed sys_parms notes nothing", (long)id->valid, 0);
        slco_sys_parms(opts, state, slco, 0);
        rc |= expect_int("rx-id sys_parms notes", (long)id->valid, DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT);
    }

    // Capacity Plus: the channel status CSBK names the rest LSN, with the brand; the voice LC only with its check.
    clear_record(state);
    state->dmr_rest_channel = 5;
    cap_plus_status(opts, state, 5);
    rc |= expect_int("rx-id cap+ status rest lsn (unchanged repeat)", id->dmr_rest_lsn, 5);
    rc |= expect_int("rx-id cap+ status parts", (long)id->valid, DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_BRAND);
    rc |= expect_text("rx-id cap+ brand", id->dmr_brand, "Capacity Plus");
    clear_record(state);
    cap_plus_lc(opts, state, 9, 0U);
    rc |= expect_int("rx-id failed cap+ lc notes nothing", (long)id->valid, 0);
    state->currentslot = 0;
    state->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    cap_plus_lc(opts, state, 9, 1U);
    state->event_crc_invalid[0] = 0;
    rc |= expect_int("rx-id ras cap+ lc notes nothing", (long)id->valid, 0);
    cap_plus_lc(opts, state, 9, 1U);
    rc |= expect_int("rx-id checked cap+ lc rest lsn",
                     (long)(id->valid & DSD_RXID_DMR_REST_LSN) != 0 ? id->dmr_rest_lsn : -1, 9);
    clear_record(state);
    slco_cap_plus(opts, state, 4, 2, 1);
    rc |= expect_int("rx-id failed cap+ short lc notes nothing", (long)id->valid, 0);
    slco_cap_plus(opts, state, 4, 2, 0);
    rc |= expect_int("rx-id cap+ short lc parts", (long)id->valid,
                     DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND);
    rc |= expect_int("rx-id cap+ short lc rest lsn", id->dmr_rest_lsn, 4);
    rc |= expect_text("rx-id cap+ short lc site", id->dmr_site_text, "Cap+ \xC2\xB7 Site 2");

    // Hytera XPT: the site status CSBK, the call alert LC and the short LC each name the site's free channel, which the
    // record takes from a checked message only.
    for (int i = 0; i < 2; ++i) {
        clear_record(state);
        state->currentslot = 1;
        state->event_crc_invalid[1] = unverified[i];
        xpt_site_status(opts, state, 6);
        state->event_crc_invalid[1] = 0;
        state->currentslot = 0;
        rc |= expect_int("rx-id unverified xpt status notes no free lcn", (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN),
                         0);
    }
    clear_record(state);
    xpt_site_status(opts, state, 6);
    rc |= expect_int("rx-id xpt status free lcn",
                     (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN) != 0 ? id->dmr_xpt_free_lcn : -1, 6);
    rc |= expect_text("rx-id xpt status brand", id->dmr_brand, "Hytera XPT");
    clear_record(state);
    xpt_call_alert(opts, state, 7, 0U);
    rc |= expect_int("rx-id failed xpt alert notes no free lcn", (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN), 0);
    state->currentslot = 0;
    state->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    xpt_call_alert(opts, state, 7, 1U);
    state->event_crc_invalid[0] = 0;
    rc |= expect_int("rx-id ras xpt alert notes no free lcn", (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN), 0);
    rc |= expect_text("rx-id ras xpt alert is still decoded", state->dmr_site_parms, "Free LCN - 7 ");
    xpt_call_alert(opts, state, 7, 1U);
    rc |= expect_int("rx-id checked xpt alert free lcn",
                     (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN) != 0 ? id->dmr_xpt_free_lcn : -1, 7);
    clear_record(state);
    slco_xpt(opts, state, 8, 1);
    rc |= expect_int("rx-id failed xpt short lc notes nothing", (long)id->valid, 0);
    slco_xpt(opts, state, 8, 0);
    rc |= expect_int("rx-id xpt short lc parts", (long)id->valid, DSD_RXID_DMR_XPT_FREE_LCN | DSD_RXID_DMR_BRAND);
    rc |= expect_int("rx-id xpt short lc free lcn", id->dmr_xpt_free_lcn, 8);

    // Connect Plus: control channel A, then an operator move with trunking on to system B's traffic channel. B's traffic
    // channel short LC names B; A's control channel frequency is not B's, and B's traffic channel is no control channel.
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->rtlsdr_center_freq = 451000000U;
    clear_record(state);
    slco_con_plus(opts, state, 0xA, 3, 5);
    rc |= expect_text("rx-id con+ control site", id->dmr_site_text, "Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5");
    rc |= expect_int("rx-id con+ control channel frequency", (long)id->cc_freq_hz, 451000000L);
    rc |= expect_int("rx-id con+ control parts", (long)id->valid,
                     DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND | DSD_RXID_CC_FREQ);
    clear_record(state); // the carrier boundary of the operator's move
    opts->rtlsdr_center_freq = 452500000U;
    slco_con_plus(opts, state, 0x9, 4, 6);
    rc |= expect_text("rx-id con+ traffic site", id->dmr_site_text, "Con+ \xC2\xB7 Net 4 \xC2\xB7 Site 6");
    rc |= expect_int("rx-id con+ traffic has no control channel", (long)(id->valid & DSD_RXID_CC_FREQ), 0);
    // A Tier III control channel names its frequency with the system.
    clear_record(state);
    aloha(opts, state, 1);
    rc |= expect_int("rx-id aloha control channel frequency", (long)id->cc_freq_hz, 452500000L);
    opts->audio_in_type = 0;
    opts->trunk_enable = 0;
    opts->rtlsdr_center_freq = 0;
    g_search_running = 1;
    return rc;
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    dsd_signal_search_site_set(capture_site);
    dsd_signal_search_active_set(search_running);
    int rc = 0;
    // No network/site split, so the identity reads as broadcast.
    opts->dmr_dmrla_is_set = 1;
    opts->dmr_dmrla_n = 0;

    aloha(opts, state, 1);
    rc |= expect_int("crc-valid aloha notes once", g_notes, 1);
    rc |= expect_int("aloha note kind", g_note.kind, DSD_SEARCH_SITE_NOTE_DMR_TIII);
    rc |= expect_text("aloha note text", g_note.text, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3 \xC2\xB7 SYS 1183");
    rc |= expect_text("aloha site parameters agree", state->dmr_site_parms, "TIII Small:12-3;1183; ");
    g_notes = 0;
    aloha(opts, state, 0);
    rc |= expect_int("failed-crc aloha notes nothing", g_notes, 0);

    // dmr_cspdu()'s gate also passes a burst -F accepted by RAS or an MBC whose header CRC failed; the dispatcher flags
    // those for the slot, and neither names a site.
    static const uint8_t unverified[2] = {DSD_CRC_RAS_UNVERIFIED, DSD_CRC_FAILED};
    for (int i = 0; i < 2; ++i) {
        g_notes = 0;
        state->currentslot = 1;
        state->event_crc_invalid[1] = unverified[i];
        aloha(opts, state, 1);
        state->event_crc_invalid[1] = 0;
        rc |= expect_int("unverified aloha notes nothing", g_notes, 0);
        rc |= expect_text("unverified aloha is still decoded", state->dmr_site_parms, "TIII Small:12-3;1183; ");
        state->currentslot = 0;
    }
    // Through the burst handler: a checked CSBK notes the site, a RAS-accepted one under -F does not.
    opts->aggressive_framesync = 0;
    g_notes = 0;
    aloha_burst(opts, state, 0);
    rc |= expect_int("checked aloha burst notes once", g_notes, 1);
    g_notes = 0;
    DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "%s", "");
    aloha_burst(opts, state, 1);
    rc |= expect_text("ras aloha burst is decoded", state->dmr_site_parms, "TIII Small:12-3;1183; ");
    rc |= expect_int("ras aloha burst notes nothing", g_notes, 0);
    // With no search running there is nothing to note.
    g_search_running = 0;
    g_notes = 0;
    aloha(opts, state, 1);
    rc |= expect_int("aloha without a search notes nothing", g_notes, 0);
    g_search_running = 1;

    // A network/site split shows the sub-site, as the site parameters do.
    opts->dmr_dmrla_n = 2;
    aloha(opts, state, 1);
    rc |= expect_text("split aloha note text", g_note.text,
                      "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 1.4 \xC2\xB7 SYS 1183");
    rc |= expect_text("split aloha site parameters agree", state->dmr_site_parms, "TIII Small:13-1.4;1183; ");
    opts->dmr_dmrla_n = 0;

    for (int slco = 2; slco <= 3; ++slco) {
        g_notes = 0;
        DSD_MEMSET(g_note.text, 0, sizeof(g_note.text));
        slco_sys_parms(opts, state, slco, 0);
        rc |= expect_int("checked sys_parms short lc notes once", g_notes, 1);
        rc |= expect_text("sys_parms note text", g_note.text,
                          "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3 \xC2\xB7 SYS 1183");
        g_notes = 0;
        slco_sys_parms(opts, state, slco, 1);
        rc |= expect_int("failed-crc sys_parms short lc notes nothing", g_notes, 0);
    }

    rc |= test_received_identity(opts, state);

    dsd_signal_search_site_set(NULL);
    dsd_signal_search_active_set(NULL);
    dsd_state_ext_free_all(state); // the Capacity Plus status keeps recent activity in a state extension
    free(state);
    free(opts);
    return rc;
}
