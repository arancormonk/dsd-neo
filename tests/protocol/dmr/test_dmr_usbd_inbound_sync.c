// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/*
 * Issue #800 spec 2.13 end to end: a USBD LIP Poll Response is decoded only from a burst whose own sync is the
 * MS-sourced (inbound) data pattern, read at the received polarity. The burst goes through the real single-slot data
 * path (dmrMSData() -> dmr_data_sync() -> the burst handler -> the USBD dispatch); only the symbol reader is replaced
 * (linker wraps of getDibitSoft and get_dibit_and_analog_signal), so the polarity correction under test is the real
 * one.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
int __wrap_getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);

/* The burst's live half (54 dibits), then filler for the reads past it. */
static int g_live[54];
static size_t g_live_next;

int
__wrap_getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    const int dibit = g_live_next < 54U ? g_live[g_live_next] : 1;
    g_live_next++;
    if (out_soft != NULL) {
        DSD_MEMSET(out_soft, 0, sizeof(*out_soft));
        out_soft->reliability = 200U;
    }
    return dibit;
}

int
__wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    if (out_analog_signal != NULL) {
        *out_analog_signal = 0;
    }
    return 1;
}

static void
unpack(const uint8_t* bytes, uint8_t* bits, size_t count) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - i % 8U)) & 1U);
    }
}

/* The slot type: colour code 1, data type USBD (0x0B), with its Golay(20,8) parity. */
static int
slot_type_bits(unsigned char st[20]) {
    unsigned char check[20];
    DSD_MEMSET(st, 0, 20);
    const unsigned cc = 1U;
    const unsigned type = 0x0BU;
    for (unsigned b = 0; b < 4U; b++) {
        st[b] = (unsigned char)((cc >> (3U - b)) & 1U);
        st[4U + b] = (unsigned char)((type >> (3U - b)) & 1U);
    }
    for (unsigned parity = 0; parity < 4096U; parity++) {
        for (unsigned b = 0; b < 12U; b++) {
            st[8U + b] = (unsigned char)((parity >> (11U - b)) & 1U);
        }
        DSD_MEMCPY(check, st, sizeof check);
        if (Golay_20_8_decode(check) && memcmp(check, st, sizeof check) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The 144 dibits of the burst as sent (CACH, payload, slot type, @p sync), true polarity. */
static int
build_burst(const char* sync, int dibits[144]) {
    // 22.5 S, 45 W, hash 218 (the LIP vector of DMR_USBD_LIP_LRRP_CRC).
    static const uint8_t lip_bytes[10] = {0x07, 0x80, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x4A, 0x00, 0xDA};
    static const uint8_t reserved[3] = {0, 0, 0};
    uint8_t payload[96] = {0};
    unpack(lip_bytes, payload, sizeof lip_bytes);
    const uint16_t crc = (uint16_t)(ComputeCrcCCITT(payload) ^ 0x3333U);
    for (unsigned i = 0; i < 16; ++i) {
        payload[80 + i] = (uint8_t)((crc >> (15U - i)) & 1U);
    }
    uint8_t info[196];
    dmr_test_encode_bptc_196x96(payload, reserved, info);
    unsigned char st[20];
    if (!slot_type_bits(st)) {
        return 0;
    }
    DSD_MEMSET(dibits, 0, 144 * sizeof(int));
    for (size_t i = 0; i < 49U; i++) {
        dibits[12U + i] = (info[2U * i] << 1) | info[2U * i + 1U];
        dibits[95U + i] = (info[98U + 2U * i] << 1) | info[99U + 2U * i];
    }
    for (size_t i = 0; i < 5U; i++) {
        dibits[61U + i] = (st[2U * i] << 1) | st[2U * i + 1U];
        dibits[90U + i] = (st[10U + 2U * i] << 1) | st[11U + 2U * i];
    }
    for (size_t i = 0; i < 24U; i++) {
        dibits[66U + i] = sync[i] == '3' ? 3 : 1;
    }
    return 1;
}

/* Receive the burst: @p sent_inverted says the carrier's polarity, @p inverted_dmr the receiver's setting. Returns 1
   when a Poll Response notice with its position was committed. */
static int
receive_burst(dsd_opts* opts, dsd_state* state, const char* sync, int sent_inverted, int inverted_dmr) {
    static int payload[90];
    int dibits[144];
    if (!build_burst(sync, dibits)) {
        return -1;
    }
    const int flip = sent_inverted ? 2 : 0;
    for (size_t i = 0; i < 90U; i++) {
        payload[i] = dibits[i] ^ flip;
    }
    for (size_t i = 0; i < 54U; i++) {
        g_live[i] = dibits[90U + i] ^ flip;
    }
    g_live_next = 0;
    Event_History_I* history = state->event_history_s;
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof *state);
    DSD_MEMSET(history, 0, 2 * sizeof *history);
    for (int slot = 0; slot < 2; ++slot) {
        init_event_history(&history[slot], 0, 255);
    }
    state->event_history_s = history;
    state->dmr_payload_p = payload + 90;
    state->dmr_color_code = 16;
    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    opts->inverted_dmr = inverted_dmr;
    dmrMSData(opts, state);
    const Event_History* notice = &history[0].Event_History_Items[1];
    return strstr(notice->event_string, "USBD LIP Poll Response; SRC(Hash): 218;") != NULL && notice->pos.valid == 1U
           && notice->pos.lat_e7 == -225000000 && notice->pos.lon_e7 == -450000000 && notice->source_id == 0U;
}

int
main(void) {
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        return 100;
    }
    static dsd_opts opts;
    static dsd_state state;
    state.event_history_s = calloc(2, sizeof *state.event_history_s);
    if (state.event_history_s == NULL) {
        return 101;
    }
    opts.aggressive_framesync = 1;

    static const struct {
        const char* sync;
        int sent_inverted;
        int inverted_dmr;
        int reply;
        const char* tag;
    } cases[] = {
        {DMR_MS_DATA_SYNC, 0, 0, 1, "inbound, normal polarity"},
        {DMR_MS_DATA_SYNC, 1, 1, 1, "inbound, inverted polarity"},
        {DMR_BS_DATA_SYNC, 1, 1, 0, "outbound (Poll Request direction), inverted polarity"},
        {DMR_DIRECT_MODE_TS1_DATA_SYNC, 1, 1, 0, "direct mode slot 1, inverted polarity"},
        {DMR_DIRECT_MODE_TS2_DATA_SYNC, 0, 0, 0, "direct mode slot 2, normal polarity"},
        // A normal-polarity carrier read as inverted: the corrected sync is no MS data sync, so it is no reply.
        {DMR_MS_DATA_SYNC, 0, 1, 0, "inbound read at the wrong polarity"},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const int got = receive_burst(&opts, &state, cases[i].sync, cases[i].sent_inverted, cases[i].inverted_dmr);
        if (got != cases[i].reply) {
            DSD_FPRINTF(stderr, "\n%s: reply %d, want %d\n", cases[i].tag, got, cases[i].reply);
            failed = 1;
        }
    }
    dsd_state_ext_free_all(&state);
    free(state.event_history_s);
    if (failed == 0) {
        DSD_FPRINTF(stdout, "DMR_USBD_INBOUND_SYNC: PASS\n");
    }
    return failed;
}
