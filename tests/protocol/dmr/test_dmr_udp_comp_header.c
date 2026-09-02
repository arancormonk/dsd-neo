// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression for issue #450: the DMR UDP/IPv4 compressed header must be labelled
 * exactly as ETSI TS 102 361-3 V1.3.1 clause 7.2 (tables 7.14-7.21) defines it, keep
 * the 7-bit port indices apart from the UDP ports they resolve to, dispatch the
 * payload on the resolved port, and refuse a header whose Header Compression
 * Opcode is reserved.
 *
 * Byte layout of every vector (table 7.14): b0 b1 = IPv4 Identification;
 * b2 = SAID << 4 | DAID; b3 = opcode MSB << 7 | SPID; b4 = opcode LSB << 7 | DPID;
 * then the optional 16-bit extended port(s), then payload.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static unsigned int g_lip_calls;
static unsigned int g_datacall_calls;
static uint32_t g_datacall_src;
static uint32_t g_datacall_dst;
static uint8_t g_datacall_slot;
static dsd_event_category g_datacall_category;
static char g_datacall_text[512];
static char g_datacall_gps[256];

static void
reset_spies(void) {
    g_lip_calls = 0;
    g_datacall_calls = 0;
    g_datacall_src = 0;
    g_datacall_dst = 0;
    g_datacall_slot = 0;
    g_datacall_category = DSD_EVENT_CATEGORY_UNKNOWN;
    DSD_MEMSET(g_datacall_text, 0, sizeof(g_datacall_text));
    DSD_MEMSET(g_datacall_gps, 0, sizeof(g_datacall_gps));
}

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
    (void)input;
    g_lip_calls++;
    uint8_t slot = (state->currentslot == 1) ? 1U : 0U;
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof(state->dmr_embedded_gps[slot]), "%s",
                 "LIP: 41.500000, -87.250000");
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
dsd_event_emit_data_notice_classified(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                      const dsd_call_observation* observation, dsd_event_category category,
                                      const char* notice) {
    (void)opts;
    (void)state;
    g_datacall_calls++;
    g_datacall_src = observation->ota_source_id;
    g_datacall_dst = observation->ota_target_id;
    g_datacall_slot = slot;
    g_datacall_category = category;
    DSD_SNPRINTF(g_datacall_text, sizeof(g_datacall_text), "%s", notice ? notice : "");
    return 0;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    return dsd_event_emit_data_notice_classified(opts, state, slot, observation, DSD_EVENT_CATEGORY_DATA, notice);
}

int
dsd_event_emit_data_notice_classified_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                               const dsd_call_observation* observation, dsd_event_category category,
                                               const char* notice, const char* gps) {
    (void)dsd_event_emit_data_notice_classified(opts, state, slot, observation, category, notice);
    DSD_SNPRINTF(g_datacall_gps, sizeof(g_datacall_gps), "%s", gps ? gps : "");
    return 0;
}

int
dsd_event_emit_data_notice_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                    const dsd_call_observation* observation, const char* notice, const char* gps) {
    return dsd_event_emit_data_notice_classified_with_gps(opts, state, slot, observation, DSD_EVENT_CATEGORY_DATA,
                                                          notice, gps);
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "1999/01/02" : "11:22:33";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

// Under test
void dmr_udp_comp_pdu(dsd_opts* opts, dsd_state* state, uint16_t len, const uint8_t* DMR_PDU);

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!buf || !strstr(buf, needle)) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, buf ? buf : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_no_substr(const char* buf, const char* needle, const char* tag) {
    if (buf && strstr(buf, needle)) {
        DSD_FPRINTF(stderr, "%s: unexpected '%s' in '%s'\n", tag, needle, buf);
        return 1;
    }
    return 0;
}

static int
expect_str_eq(const char* buf, const char* want, const char* tag) {
    if (!buf || strcmp(buf, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, buf ? buf : "(null)", want);
        return 1;
    }
    return 0;
}

static int
expect_category(dsd_event_category got, dsd_event_category want, const char* tag) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: category got %d want %d\n", tag, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static int
expect_count(unsigned int got, unsigned int want, const char* tag) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: count got %u want %u\n", tag, got, want);
        return 1;
    }
    return 0;
}

static const char*
staged_text(const dsd_state* st) {
    return st->event_history_s[0].Event_History_Items[0].text_message;
}

// Every case starts from empty spies, an empty staged text payload and no embedded GPS, so a
// negative assertion cannot be satisfied by what the previous case left behind.
static void
run_case(dsd_opts* opts, dsd_state* st, const uint8_t* pdu, size_t len) {
    reset_spies();
    st->currentslot = 0;
    st->event_history_s[0].Event_History_Items[0].text_message[0] = '\0';
    st->dmr_embedded_gps[0][0] = '\0';
    dmr_udp_comp_pdu(opts, st, (uint16_t)len, pdu);
}

int
main(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.dmr_lrrp_source[0] = 1234;
    st.dmr_lrrp_target[0] = 5678;

    st.event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    if (!st.event_history_s) {
        return 100;
    }

    // T1: SAID (table 7.15) and DAID (table 7.16). Reserved runs to 11; Manufacturer
    // Specific starts at 12; only the DAID table has Group Network at 2.
    {
        static const struct {
            uint8_t b2;
            const char* src;
            const char* dst;
        } cases[] = {
            {0x00U, "SRC: 0:63 (Radio Network):(Reserved);", "DST: 0:63 (Radio Network):(Reserved);"},
            {0x11U, "SRC: 1:63 (Ethernet):(Reserved);", "DST: 1:63 (Ethernet):(Reserved);"},
            {0x22U, "SRC: 2:63 (Reserved):(Reserved);", "DST: 2:63 (Group Network):(Reserved);"},
            {0xBBU, "SRC: 11:63 (Reserved):(Reserved);", "DST: 11:63 (Reserved):(Reserved);"},
            {0xCCU, "SRC: 12:63 (Manufacturer Specific):(Reserved);", "DST: 12:63 (Manufacturer Specific):(Reserved);"},
            {0xFFU, "SRC: 15:63 (Manufacturer Specific):(Reserved);", "DST: 15:63 (Manufacturer Specific):(Reserved);"},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const uint8_t pdu[] = {0x00U, 0x01U, cases[i].b2, 0x3FU, 0x3FU};
            run_case(&opts, &st, pdu, sizeof pdu);
            rc |= expect_count(g_datacall_calls, 1U, "t1 notice count");
            rc |= expect_has_substr(g_datacall_text, cases[i].src, "t1 said label");
            rc |= expect_has_substr(g_datacall_text, cases[i].dst, "t1 daid label");
            rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "t1 category");
        }
    }

    // T2: SPID/DPID (tables 7.17/7.18). Reserved is 3-94, Manufacturer Specific 95-127, and
    // indices 1 and 2 name their default ports.
    {
        static const struct {
            uint8_t b3;
            uint8_t b4;
            const char* src;
            const char* dst;
        } cases[] = {
            {0x03U, 0x5EU, "SRC: 0:3 (Radio Network):(Reserved);", "DST: 0:94 (Radio Network):(Reserved);"},
            {0x5FU, 0x7FU, "SRC: 0:95 (Radio Network):(Manufacturer Specific);",
             "DST: 0:127 (Radio Network):(Manufacturer Specific);"},
            {0x61U, 0x61U, "SRC: 0:97 (Radio Network):(Manufacturer Specific);",
             "DST: 0:97 (Radio Network):(Manufacturer Specific);"},
            {0x01U, 0x02U, "SRC: 0:1 (Radio Network):(UTF-16BE Text Message, UDP 5016);",
             "DST: 0:2 (Radio Network):(Location Interface Protocol, UDP 5017);"},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const uint8_t pdu[] = {0x00U, 0x02U, 0x00U, cases[i].b3, cases[i].b4};
            run_case(&opts, &st, pdu, sizeof pdu);
            rc |= expect_count(g_datacall_calls, 1U, "t2 notice count");
            rc |= expect_has_substr(g_datacall_text, cases[i].src, "t2 spid label");
            rc |= expect_has_substr(g_datacall_text, cases[i].dst, "t2 dpid label");
            rc |= expect_count(g_lip_calls, 0U, "t2 no payload, no LIP");
            if (staged_text(&st)[0] != '\0') {
                DSD_FPRINTF(stderr, "t2 no payload, no text: got '%s'\n", staged_text(&st));
                rc |= 1;
            }
        }
    }

    // T3: SPID 0 puts the source port in Extended Header 1 (table 7.20). The index stays
    // 0 in the numeric slot and the port is named in the label, never fed to the index table.
    {
        const uint8_t pdu[] = {0x00U, 0x7CU, 0x10U, 0x00U, 0x3FU, 0x0FU, 0xA4U};
        run_case(&opts, &st, pdu, sizeof pdu);
        rc |= expect_has_substr(g_datacall_text, "SRC: 1:0 (Ethernet):(In Extended Header, UDP 4004);",
                                "t3 extended source");
        rc |= expect_has_substr(g_datacall_text, "DST: 0:63 (Radio Network):(Reserved);", "t3 indexed destination");
        rc |= expect_no_substr(g_datacall_text, "Manufacturer Specific", "t3 port is not an index");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "t3 xcmp category");
        rc |= expect_count(g_lip_calls, 0U, "t3 no LIP");
    }

    // T4: SPID set and DPID 0 puts the destination port in Extended Header 1 (table 7.20).
    {
        const uint8_t pdu[] = {0x00U, 0x7CU, 0x01U, 0x3FU, 0x00U, 0x0FU, 0xA5U};
        run_case(&opts, &st, pdu, sizeof pdu);
        rc |= expect_has_substr(g_datacall_text, "SRC: 0:63 (Radio Network):(Reserved);", "t4 indexed source");
        rc |= expect_has_substr(g_datacall_text, "DST: 1:0 (Ethernet):(In Extended Header, UDP 4005);",
                                "t4 extended destination");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "t4 ars category");
    }

    // T5: both indices 0: source in Extended Header 1, destination in Extended Header 2
    // (table 7.21); the destination port 5017 selects LIP.
    {
        const uint8_t pdu[] = {0x00U, 0x7DU, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x13U, 0x99U, 0xA5U, 0x5AU, 0xC3U};
        run_case(&opts, &st, pdu, sizeof pdu);
        rc |= expect_has_substr(g_datacall_text, "SRC: 0:0 (Radio Network):(In Extended Header, UDP 2);",
                                "t5 extended source");
        rc |= expect_has_substr(g_datacall_text, "DST: 0:0 (Radio Network):(In Extended Header, UDP 5017);",
                                "t5 extended destination");
        rc |= expect_count(g_lip_calls, 1U, "t5 LIP on port 5017");
        rc |= expect_has_substr(g_datacall_gps, "41.500000", "t5 LIP gps");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "t5 category");
    }

    // T6: an extended-header literal port 2 is a UDP port, not index 2, so it is not LIP.
    // SAID 11 is still Reserved.
    {
        const uint8_t pdu[] = {0x00U, 0x7BU, 0xB0U, 0x00U, 0x03U, 0x00U, 0x02U, 0xA5U, 0x5AU, 0xC3U};
        run_case(&opts, &st, pdu, sizeof pdu);
        rc |= expect_count(g_lip_calls, 0U, "t6 literal port 2 is not LIP");
        if (g_datacall_gps[0] != '\0') {
            DSD_FPRINTF(stderr, "t6 literal port 2 produced gps '%s'\n", g_datacall_gps);
            rc |= 1;
        }
        rc |= expect_has_substr(g_datacall_text, "SRC: 11:0 (Reserved):(In Extended Header, UDP 2);",
                                "t6 said 11 reserved");
        rc |= expect_has_substr(g_datacall_text, "DST: 0:3 (Radio Network):(Reserved);", "t6 dpid 3 reserved");
    }

    // T7: an extended-header literal port 5017 is the LIP default (table 7.17 note 1).
    {
        const uint8_t pdu[] = {0x00U, 0x7BU, 0xC0U, 0x00U, 0x03U, 0x13U, 0x99U, 0xA5U, 0x5AU, 0xC3U};
        run_case(&opts, &st, pdu, sizeof pdu);
        rc |= expect_count(g_lip_calls, 1U, "t7 literal port 5017 is LIP");
        rc |= expect_has_substr(g_datacall_text, "SRC: 12:0 (Manufacturer Specific):(In Extended Header, UDP 5017);",
                                "t7 said 12 manufacturer specific");
    }

    // T8: an extended-header literal port 5016 is the UTF-16BE text default.
    {
        const uint8_t pdu[] = {0x12U, 0x35U, 0x00U, 0x3FU, 0x00U, 0x13U, 0x98U, 0x00U, 'O', 0x00U, 'K'};
        run_case(&opts, &st, pdu, sizeof pdu);
        if (strcmp(staged_text(&st), "OK") != 0) {
            DSD_FPRINTF(stderr, "t8 literal port 5016 text: got '%s'\n", staged_text(&st));
            rc |= 1;
        }
        rc |= expect_has_substr(g_datacall_text, "DST: 0:0 (Radio Network):(In Extended Header, UDP 5016);",
                                "t8 extended destination");
        rc |= expect_count(g_lip_calls, 0U, "t8 no LIP");
    }

    // T9: table 7.19 defines opcode 00 only. A reserved opcode has no published layout, so
    // nothing after the IP ID is interpreted: one notice naming the opcode, no field labels,
    // no payload decode, and the observation carries the data header's LLIDs.
    {
        // Opcode 01 with SPID 1: would be UTF-16BE text under the 00 layout.
        const uint8_t op1[] = {0x12U, 0x34U, 0x12U, 0x01U, 0xBFU, 0x00U, 'O', 0x00U, 'K'};
        run_case(&opts, &st, op1, sizeof op1);
        rc |= expect_count(g_datacall_calls, 1U, "t9 op1 single notice");
        rc |= expect_str_eq(g_datacall_text,
                            "IP ID: 1234; OP: 1; Reserved Header Compression Opcode; header not decoded; ",
                            "t9 op1 notice");
        if (staged_text(&st)[0] != '\0') {
            DSD_FPRINTF(stderr, "t9 op1 decoded text '%s' from a reserved layout\n", staged_text(&st));
            rc |= 1;
        }
        if (g_datacall_src != 1234U || g_datacall_dst != 5678U) {
            DSD_FPRINTF(stderr, "t9 op1 observation src=%u dst=%u, want the data header LLIDs 1234/5678\n",
                        g_datacall_src, g_datacall_dst);
            rc |= 1;
        }
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "t9 op1 category");

        // Opcode 10, the shape logged on a live Motorola system in issue #450: SAID/DAID 11,
        // SPID 118, DPID 119 if read as the 00 layout.
        const uint8_t op2[] = {0x01U, 0x02U, 0xBBU, 0xF6U, 0x77U};
        run_case(&opts, &st, op2, sizeof op2);
        rc |= expect_count(g_datacall_calls, 1U, "t9 op2 single notice");
        rc |= expect_has_substr(g_datacall_text, "OP: 2; Reserved Header Compression Opcode", "t9 op2 notice");
        rc |= expect_no_substr(g_datacall_text, "SRC:", "t9 op2 no source label");
        rc |= expect_no_substr(g_datacall_text, "DST:", "t9 op2 no destination label");
        rc |= expect_no_substr(g_datacall_text, "Manufacturer Specific", "t9 op2 no index label");

        // Opcode 11 with SPID 2: would be LIP under the 00 layout.
        const uint8_t op3[] = {0x00U, 0x7BU, 0x10U, 0x82U, 0xBFU, 0xA5U, 0x5AU, 0xC3U};
        run_case(&opts, &st, op3, sizeof op3);
        rc |= expect_has_substr(g_datacall_text, "OP: 3; Reserved Header Compression Opcode", "t9 op3 notice");
        rc |= expect_count(g_lip_calls, 0U, "t9 op3 no LIP");
        if (g_datacall_gps[0] != '\0') {
            DSD_FPRINTF(stderr, "t9 op3 produced gps '%s' from a reserved layout\n", g_datacall_gps);
            rc |= 1;
        }
    }

    // T10: a PDU too short for the extended header it announces still emits one notice,
    // says the port is missing, and decodes nothing.
    {
        const uint8_t one_short[] = {0x00U, 0x7AU, 0x10U, 0x00U, 0x3FU, 0x0FU};
        run_case(&opts, &st, one_short, sizeof one_short);
        rc |= expect_count(g_datacall_calls, 1U, "t10 single notice");
        rc |= expect_count(g_lip_calls, 0U, "t10 no LIP");
        rc |= expect_has_substr(g_datacall_text, "SRC: 1:0 (Ethernet):(In Extended Header, truncated);",
                                "t10 truncated source");
        rc |= expect_has_substr(g_datacall_text, "DST: 0:63 (Radio Network):(Reserved);", "t10 indexed destination");

        const uint8_t both_short[] = {0x00U, 0x7AU, 0x00U, 0x00U, 0x00U};
        run_case(&opts, &st, both_short, sizeof both_short);
        rc |= expect_count(g_datacall_calls, 1U, "t10 both single notice");
        rc |= expect_has_substr(g_datacall_text, "SRC: 0:0 (Radio Network):(In Extended Header, truncated);",
                                "t10 both truncated source");
        rc |= expect_has_substr(g_datacall_text, "DST: 0:0 (Radio Network):(In Extended Header, truncated);",
                                "t10 both truncated destination");
    }

    free(st.event_history_s);
    st.event_history_s = NULL;

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
