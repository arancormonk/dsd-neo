// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression (#453): an LRRP message whose length byte is zero is still a
 * message of the type its first byte declares. A triggered-location report
 * or a stop acknowledgement with no tokens must be labelled "Response to
 * TGT" and an immediate location request with no tokens "Request from TGT",
 * exactly as the same types are labelled when tokens follow. Only a type the
 * classifier does not know keeps the "Unknown Format" label.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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
    return 0;
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

// Deterministic system time (not used: file output disabled)
int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "1999/01/02" : "11:22:33";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

// Under test
void dmr_lrrp(const dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest,
              const uint8_t* DMR_PDU, uint8_t pdu_crc_ok);

typedef struct {
    const uint8_t* pdu;
    uint16_t len;
    const char* expect;
    const char* tag;
} lrrp_case;

// Type byte, length byte, then tokens. Source 123, target 456 in every case.
static const uint8_t k_triggered_report_empty[] = {0x0D, 0x00};
static const uint8_t k_triggered_report_with_id[] = {0x0D, 0x02, 0x22, 0x01};
static const uint8_t k_stop_response_empty[] = {0x11, 0x00};
static const uint8_t k_immediate_request_empty[] = {0x05, 0x00};
static const uint8_t k_unknown_type_empty[] = {0x33, 0x00};

static const lrrp_case k_cases[] = {
    {k_triggered_report_empty, sizeof k_triggered_report_empty, "LRRP SRC: 123; Response to TGT: 456;",
     "0x0D triggered location, zero length"},
    {k_triggered_report_with_id, sizeof k_triggered_report_with_id, "LRRP SRC: 123; Response to TGT: 456;",
     "0x0D triggered location, identity token (control)"},
    {k_stop_response_empty, sizeof k_stop_response_empty, "LRRP SRC: 123; Response to TGT: 456;",
     "0x11 triggered location stop response, zero length"},
    {k_immediate_request_empty, sizeof k_immediate_request_empty, "LRRP SRC: 123; Request from TGT: 456;",
     "0x05 immediate location request, zero length"},
    {k_unknown_type_empty, sizeof k_unknown_type_empty, "LRRP SRC: 123; Unknown Format 33; TGT: 456;",
     "0x33 unknown type, zero length (control)"},
};

int
main(void) {
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    opts.lrrp_file_output = 0;

    int rc = 0;
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        const lrrp_case* c = &k_cases[i];
        DSD_MEMSET(st.dmr_lrrp_gps[0], 0, sizeof st.dmr_lrrp_gps[0]);
        dmr_lrrp(&opts, &st, c->len, /*src*/ 123, /*dst*/ 456, c->pdu, /*pdu_crc_ok*/ 1);
        if (strcmp(st.dmr_lrrp_gps[0], c->expect) != 0) {
            DSD_FPRINTF(stderr, "%s: got '%s' expected '%s'\n", c->tag, st.dmr_lrrp_gps[0], c->expect);
            rc = 1;
        }
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
