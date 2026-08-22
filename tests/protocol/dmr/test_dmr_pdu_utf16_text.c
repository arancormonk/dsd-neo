// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * utf16_to_text() renders DMR SMS/TMS text from UTF-16BE octets (issue #358 covers the same
 * %lc hazard for UDT text). Surrogate pairs must combine into one character, an unpaired half
 * must become U+FFFD, and the historical markers for padding and the 0x040D line break stay.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Minimal stubs for direct link with dmr_pdu.c
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_unicode_supported(void) {
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "1999/01/02" : "11:22:33";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

/* UTF-16BE: U+4739, U+1F600 as a pair, a lone high surrogate, 'A', padding, then 0x040D. */
static const uint8_t kText[] = {0x47, 0x39, 0xD8, 0x3D, 0xDE, 0x00, 0xD8, 0x00, 0x00, 0x41, 0x00, 0x00, 0x04, 0x0D};

static const char kExpected[] = "\xE4\x9C\xB9"
                                "\xF0\x9F\x98\x80"
                                "\xEF\xBF\xBD"
                                "A_ / ";

int
main(void) {
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    st.event_history_s = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    assert(st.event_history_s != NULL);

    char buf[256];
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_pdu_utf16") == 0);
    utf16_to_text(&st, 1, (uint16_t)sizeof kText, kText);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, sizeof buf) == 0);

    if (strcmp(buf, kExpected) != 0) {
        DSD_FPRINTF(stderr, "utf16_to_text printed: %s\n", buf);
        assert(0 && "UTF-16 text was not transcoded as expected");
    }
    /* The event log keeps only the ASCII characters, as before. */
    assert(strcmp(st.event_history_s[0].Event_History_Items[0].text_message, "A") == 0);

    /* An odd trailing octet is ignored rather than read past. */
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_pdu_utf16") == 0);
    utf16_to_text(&st, 0, 3, kText);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, sizeof buf) == 0);
    assert(strcmp(buf, "\xE4\x9C\xB9") == 0);

    free(st.event_history_s);
    DSD_FPRINTF(stderr, "DMR_PDU_UTF16_TEXT: PASS\n");
    return 0;
}
