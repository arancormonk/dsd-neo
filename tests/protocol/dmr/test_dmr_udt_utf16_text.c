// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test for GitHub issue #358: a DMR UDT whose UTF-16 payload contains a surrogate
 * crashed the Windows build.
 *
 * Each 16-bit unit of the text went to fprintf("%lc"). A lone surrogate - one unit in 32 of the
 * garbage an OTA-encrypted repeater emits, and half of every emoji - cannot be converted on its
 * own, and the UCRT turned that failed conversion into an unbounded stack dump on stderr followed
 * by an access violation. The text now goes through dsd-neo's own UTF-16 decoder: pairs combine,
 * unpaired halves become U+FFFD, and %lc is never used. Both UDT text formats share that path.
 */

#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

/* UTF-16BE: U+4739, U+1F600 as a surrogate pair, a lone high surrogate, then 'A'. */
static const uint8_t kText[] = {0x47, 0x39, 0xD8, 0x3D, 0xDE, 0x00, 0xD8, 0x00, 0x00, 0x41};

/* What that text must look like on stderr: the pair combined, the orphan replaced. */
static const char kExpected[] = "\xE4\x9C\xB9"
                                "\xF0\x9F\x98\x80"
                                "\xEF\xBF\xBD"
                                "A";

static void
seed_udt(dsd_state* state, uint8_t format, size_t text_offset) {
    state->currentslot = 0;
    state->data_header_valid[0] = 1;
    state->data_header_blocks[0] = 2;
    state->data_block_counter[0] = 2;
    state->data_byte_ctr[0] = 0;
    state->data_conf_data[0] = 0;
    state->data_header_format[0] = 0;
    state->data_header_sap[0] = 0;
    state->data_p_head[0] = 0;
    state->udt_uab_reserved[0] = 0;
    state->data_block_crc_valid[0][0] = 1;

    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    /* UDT header octet 1 carries the SAP in its high nibble and the UDT format in the low one. */
    state->dmr_pdu_sf[0][1] = format;
    DSD_MEMCPY(&state->dmr_pdu_sf[0][text_offset], kText, sizeof kText);
}

/*
 * Deliver the last appended block. The assembler stores it at octet 24 before decoding, so hand
 * it the octets already seeded there to keep the PDU intact.
 */
static void
run_udt(dsd_opts* opts, dsd_state* state) {
    uint8_t block[12];
    DSD_MEMCPY(block, &state->dmr_pdu_sf[0][24], sizeof block);
    dmr_block_assembler(opts, state, block, 12, 0x06, 3);
}

static void
capture_udt(dsd_opts* opts, dsd_state* state, char* buf, size_t buf_size) {
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_udt_utf16") == 0);
    run_udt(opts, state);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, buf_size) == 0);
}

static void
expect_text_after(const char* buf, const char* marker) {
    const char* p = strstr(buf, marker);
    if (p == NULL) {
        DSD_FPRINTF(stderr, "missing '%s' in: %s\n", marker, buf);
        assert(p != NULL);
    }
    p += strlen(marker);
    if (memcmp(p, kExpected, sizeof kExpected - 1U) != 0) {
        DSD_FPRINTF(stderr, "unexpected text after '%s': %s\n", marker, p);
        assert(0 && "UTF-16 text was not transcoded as expected");
    }
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts != NULL && state != NULL);

    /* The UDT text paths write into event history, which the app allocates at init. */
    state->event_history_s = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    assert(state->event_history_s != NULL);
    for (int i = 0; i < 2; i++) {
        init_event_history(&state->event_history_s[i], 0, 255);
    }

    assert(dsd_test_setenv("DSD_FORCE_UTF8", "1", 1) == 0);
    assert(dsd_test_unsetenv("DSD_FORCE_ASCII") == 0);

    char buf[4096];

    /* Format 0x0A, mixed address + UTF-16 text: the PDU from the report. Text starts at octet 16. */
    seed_udt(state, 0x0A, 16);
    capture_udt(opts, state, buf, sizeof buf);
    expect_text_after(buf, "UTF16 Text: ");

    /* Format 0x07, plain UTF-16 text starting right after the 12-octet UDT header. */
    seed_udt(state, 0x07, 12);
    capture_udt(opts, state, buf, sizeof buf);
    expect_text_after(buf, "UTF16 Text: ");

    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(state);
    free(opts);
    DSD_FPRINTF(stderr, "DMR_UDT_UTF16_TEXT: PASS\n");
    return 0;
}
