// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Issue #496: exercise real header/assembler/service/event-log paths, not emitter stubs. */
#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/bit_packing.h"
#include "test_support.h"

static void
unpack(const uint8_t* bytes, uint8_t* bits, size_t count) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - i % 8U)) & 1U);
    }
}

static void
header(dsd_opts* opts, dsd_state* state, uint8_t bytes[12], int crc_ok) {
    uint8_t bits[96];
    unpack(bytes, bits, 12);
    dmr_dheader(opts, state, bytes, bits, (uint32_t)crc_ok, 0);
}

static void
crc32(uint8_t* bytes, size_t count) {
    uint8_t bits[8 * 48];
    assert(count <= 48 && count >= 4 && count % 2 == 0);
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[(i / 8U) ^ 1U] >> (7U - i % 8U)) & 1U);
    }
    uint32_t crc = ComputeCrc32Bit(bits, (uint32_t)(count * 8U - 32U));
    for (size_t i = 0; i < 4; ++i) {
        bytes[count - 4 + i] = (uint8_t)(crc >> (24U - i * 8U));
    }
}

static void
send_udt(dsd_opts* opts, dsd_state* state, int header_ok, int payload_ok) {
    uint8_t hdr[12] = {0x00, 0x04, 0, 0, 42, 0, 0, 24, 0, 0, 0, 0};
    uint8_t block[12] = {'H', 'E', 'L', 'L', 'O', 0, 0, 0, 0, 0, 0, 0};
    uint8_t bits[96];
    unpack(block, bits, 12);
    uint16_t crc = (uint16_t)dsd_crc_ccitt16_bits(bits, 80);
    block[10] = (uint8_t)(crc >> 8);
    block[11] = (uint8_t)(crc ^ (payload_ok ? 0U : 1U));
    header(opts, state, hdr, header_ok);
    dmr_block_assembler(opts, state, block, 12, 0x07, 3);
}

static void
send_mnis(dsd_opts* opts, dsd_state* state, int header_ok, int extension_ok, int payload_ok) {
    /* Standard header plus actual DPF=15/SAP=1 extension, not the old DPF=2 test shortcut. */
    uint8_t hdr[12] = {0x02, 0x90, 0, 0, 42, 0, 0, 24, 0x82, 0, 0, 0};
    /* Independently computed using node-dmr-lib's span and CRC32 algorithm. */
    uint8_t pdu[22] = {0x1F, 0x10, 0x02, 0x01, 0x88, 0x55, 0x01, 0,    0,    0,    0,
                       0,    0,    0,    0,    0,    0,    0,    0x14, 0x43, 0xAE, 0x84};
    uint8_t ext[12] = {0};
    pdu[21] ^= (uint8_t)(payload_ok ? 0 : 1);
    DSD_MEMCPY(ext, pdu, 10);
    header(opts, state, hdr, header_ok);
    header(opts, state, ext, extension_ok);
    dmr_block_assembler(opts, state, pdu + 10, 12, 0x07, 1);
}

static void
send_captured_mnis(dsd_opts* opts, dsd_state* state, int corrupt) {
    // Untouched LRRP packet from https://github.com/lwvmobile/dsd-fme/issues/283.
    // The dump's last two zeros are buffer fill: first ten header octets + three data blocks = 46.
    uint8_t hdr[12] = {0x02, 0x99, 0, 0, 0x64, 0, 0x04, 0x42, 0x84, 0, 0xF4, 0xF0};
    uint8_t ext[12] = {0x1F, 0x10, 0x02, 0x01, 0x11, 0xF7, 0x32, 0x0D, 0x18, 0x23, 0x8F, 0x80};
    uint8_t blocks[36] = {0x41, 0x34, 0x1F, 0x9A, 0x36, 0x71, 0xCF, 0x51, 0x49, 0x22, 0x23, 0x00,
                          0x0F, 0xA0, 0x02, 0x0E, 0x21, 0x26, 0x6C, 0x00, 0x25, 0x56, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE8, 0x1D, 0x14};
    blocks[5] ^= (uint8_t)(corrupt ? 1 : 0);
    header(opts, state, hdr, 1);
    header(opts, state, ext, 1);
    for (size_t i = 0; i < sizeof blocks; i += 12) {
        dmr_block_assembler(opts, state, blocks + i, 12, 0x07, 1);
    }
}

static void
send_unknown(dsd_opts* opts, dsd_state* state, int header_ok, int payload_ok, int encrypted) {
    uint8_t hdr[12] = {0x02, 0x50, 0, 0, 42, 0, 0, 24, 0x81, 0, 0, 0};
    uint8_t block[12] = {0};
    crc32(block, sizeof block);
    block[11] ^= (uint8_t)(payload_ok ? 0 : 1);
    header(opts, state, hdr, header_ok);
    if (encrypted) {
        state->dmr_so = 0x100;
        state->payload_algid = 0x07;
    }
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
}

static void
expect_event(dsd_state* state, uint8_t slot, uint64_t before, int emitted, int invalid, const char* payload) {
    assert(state->event_history_s[slot].push_seq == before + (uint64_t)emitted);
    if (!emitted) {
        return;
    }
    const Event_History* row = &state->event_history_s[slot].Event_History_Items[1];
    assert(strstr(row->event_string, payload) != NULL || strstr(row->text_message, payload) != NULL);
    assert((strstr(row->event_string, "[CRC ERR]") != NULL) == invalid);
    assert(row->severity == (invalid ? DSD_EVENT_SEVERITY_WARNING : DSD_EVENT_SEVERITY_INFO));
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    state->event_history_s = calloc(2, sizeof(Event_History_I));
    assert(state->event_history_s);
    for (int slot = 0; slot < 2; ++slot) {
        init_event_history(&state->event_history_s[slot], 0, 255);
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dmr-event-crc");
    assert(fd >= 0);
    assert(dsd_close(fd) == 0);
    DSD_SNPRINTF(opts->event_out_file, sizeof opts->event_out_file, "%s", path);

    opts->aggressive_framesync = 1;
    uint64_t before = state->event_history_s[0].push_seq;
    send_udt(opts, state, 1, 0);
    expect_event(state, 0, before, 0, 0, "");
    send_mnis(opts, state, 1, 1, 0);
    expect_event(state, 0, before, 0, 0, "");
    send_unknown(opts, state, 1, 0, 1);
    expect_event(state, 0, before, 0, 0, "");

    /* A CRC-valid next packet remains usable after strict rejection. */
    send_udt(opts, state, 1, 1);
    expect_event(state, 0, before, 1, 0, "HELLO");
    before++;
    send_mnis(opts, state, 1, 1, 1);
    expect_event(state, 0, before, 1, 0, "IP ID: 5501");
    before++;
    send_captured_mnis(opts, state, 0);
    expect_event(state, 0, before++, 1, 0, "IP ID: F732");
    send_captured_mnis(opts, state, 1);
    expect_event(state, 0, before, 0, 0, "");

    opts->aggressive_framesync = 0;
    send_udt(opts, state, 1, 0);
    expect_event(state, 0, before++, 1, 1, "HELLO");
    send_mnis(opts, state, 1, 1, 0);
    expect_event(state, 0, before++, 1, 1, "IP ID: 5501");
    send_mnis(opts, state, 0, 1, 1);
    expect_event(state, 0, before++, 1, 1, "IP ID: 5501");
    send_mnis(opts, state, 1, 0, 1);
    expect_event(state, 0, before++, 1, 1, "IP ID: 5501");
    send_udt(opts, state, 0, 1);
    expect_event(state, 0, before++, 1, 1, "HELLO");
    send_unknown(opts, state, 0, 1, 0);
    expect_event(state, 0, before++, 1, 1, "Unknown PDU");
    send_unknown(opts, state, 1, 0, 1);
    expect_event(state, 0, before++, 1, 1, "ENC PDU");
    send_udt(opts, state, 1, 1);
    expect_event(state, 0, before++, 1, 0, "HELLO");

    // Fully BPTC-encoded header 025000002A00001881000000, with reserved R bits = 4.
    // Its ordinary header CRC fails, but no FEC error prevents the relaxed RAS heuristic.
    // The following packet CRC passes: only the original header verdict can mark this event.
    static const uint8_t ras_burst[25] = {0x48, 0x14, 0x82, 0x24, 0x24, 0x3A, 0x08, 0xA8, 0x01, 0x60, 0x11, 0x21, 0x01,
                                          0x52, 0x8B, 0x85, 0x08, 0x60, 0x0C, 0x40, 0x19, 0x20, 0x46, 0x0C, 0xA0};
    uint8_t info[200];
    uint8_t block[12] = {0};
    InitAllFecFunction();
    unpack(ras_burst, info, sizeof ras_burst);
    dmr_data_burst_handler(opts, state, info, 0x06, NULL);
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
    expect_event(state, 0, before++, 1, 1, "Unknown PDU");
    send_unknown(opts, state, 1, 1, 0);
    expect_event(state, 0, before++, 1, 0, "Unknown PDU");

    // A rejected replacement header must disarm an older, still-armed assembly.
    opts->aggressive_framesync = 1;
    uint8_t hdr[12] = {0x02, 0x50, 0, 0, 42, 0, 0, 24, 0x81, 0, 0, 0};
    header(opts, state, hdr, 1);
    header(opts, state, hdr, 0);
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
    expect_event(state, 0, before, 0, 0, "");
    opts->aggressive_framesync = 0;

    // Preserve RAS MBC decoding only with a checked continuation, without laundering its header.
    // This BPTC codeword carries C_ALOHA (190000000000000000000000), reserved bits = 4.
    static const uint8_t ras_mbc[25] = {0x40, 0x10, 0x00, 0x25, 0x00, 0x04, 0x00, 0x20, 0x00, 0x38, 0x01, 0x10, 0x01,
                                        0x80, 0x08, 0x80, 0x0D, 0x00, 0x04, 0x00, 0x28, 0x00, 0x24, 0x00, 0x40};
    for (int mode = 0; mode < 3; mode++) {
        opts->aggressive_framesync = mode == 1;
        state->dmr_t3_syscode = 0xFFFF;
        unpack(ras_mbc, info, sizeof ras_mbc);
        dmr_data_burst_handler(opts, state, info, 0x04, NULL);
        DSD_MEMSET(block, 0, sizeof block);
        block[0] = 0x80; // Last continuation.
        uint8_t bits[96];
        unpack(block, bits, sizeof block);
        const uint16_t crc = (uint16_t)dsd_crc_ccitt16_bits(bits, 80);
        block[10] = (uint8_t)(crc >> 8);
        block[11] = (uint8_t)(crc ^ (mode == 2));
        dmr_block_assembler(opts, state, block, 12, 0x05, 2);
        assert(state->dmr_t3_syscode == (mode == 0 ? 0 : 0xFFFF));
    }
    opts->aggressive_framesync = 0;

    state->currentslot = 1;
    send_udt(opts, state, 1, 1);
    expect_event(state, 1, 0, 1, 0, "HELLO");
    /* The separately configured DMR relaxation has the same publication policy as -F. */
    opts->aggressive_framesync = 1;
    opts->dmr_crc_relaxed_default = 1;
    send_udt(opts, state, 1, 0);
    expect_event(state, 1, 1, 1, 1, "HELLO");

    FILE* log = fopen(path, "rb");
    assert(log);
    char line[4096];
    unsigned marked = 0;
    unsigned clean = 0;
    unsigned marked_text = 0;
    while (fgets(line, sizeof line, log)) {
        if (strstr(line, "[CRC ERR]")) {
            marked++;
            marked_text += strstr(line, "Text:") != NULL && strstr(line, "HELLO") != NULL;
        } else {
            clean++;
        }
    }
    fclose(log);
    assert(marked == 12 && clean == 9 && marked_text == 3);
    assert(remove(path) == 0);
    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(state);
    free(opts);
    puts("DMR_EVENT_CRC: PASS");
    return 0;
}
