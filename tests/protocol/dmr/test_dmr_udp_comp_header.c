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
static dsd_event_category g_datacall_category;
static char g_datacall_text[512];
static char g_datacall_gps[256];

static void
reset_spies(void) {
    g_lip_calls = 0;
    g_datacall_calls = 0;
    g_datacall_src = 0;
    g_datacall_dst = 0;
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
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count, uint32_t src) {
    (void)bit_count;
    (void)src;
    (void)opts;
    (void)bits;
    g_lip_calls++;
    uint8_t slot = (state->currentslot == 1) ? 1U : 0U;
    DSD_SNPRINTF(state->dmr_embedded_gps[slot], sizeof(state->dmr_embedded_gps[slot]), "%s",
                 "LIP: 41.500000, -87.250000");
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_cellocator(dsd_opts* opts, dsd_state* state, const uint8_t* input, int len) {
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
    (void)slot;
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
    return dsd_event_staged_text(st, 0);
}

// Every case starts from empty spies, an empty staged text payload and no embedded GPS, so a
// negative assertion cannot be satisfied by what the previous case left behind.
static void
run_case(dsd_opts* opts, dsd_state* st, const uint8_t* pdu, size_t len) {
    reset_spies();
    st->currentslot = 0;
    dsd_event_stage_text(st, 0, "");
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

    // T11: the notice's observation names the radios from the data header (source and
    // target LLIDs), as every other DMR data notice does; SAID/DAID are 4-bit network
    // indices and were being passed off as radio IDs.
    {
        const uint8_t pdu[] = {0x00U, 0x03U, 0x12U, 0x3FU, 0x3FU};
        run_case(&opts, &st, pdu, sizeof pdu);
        if (g_datacall_src != 1234U || g_datacall_dst != 5678U) {
            DSD_FPRINTF(stderr, "t11 observation src=%u dst=%u, want the data header LLIDs 1234/5678\n", g_datacall_src,
                        g_datacall_dst);
            rc |= 1;
        }
        rc |=
            expect_has_substr(g_datacall_text, "SRC: 1:63 (Ethernet):(Reserved); DST: 2:63 (Group Network):(Reserved);",
                              "t11 labels unchanged");
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

    // T12 (issue #746): the vector the reporter's Motorola system sends. SPID/DPID 97 are manufacturer
    // specific and not linked to a port, so the payload is the only evidence; it is exactly one ARS
    // Device Registration Ack (MOTOTRBO ARS spec 3.4.2: 00 02 BF 01, refresh 1 x 30 min). Both slots.
    {
        for (uint8_t slot = 0; slot < 2; slot++) {
            const uint8_t pdu[] = {0x00U, 0x00U, 0x10U, 0x61U, 0x61U, 0x00U, 0x02U, 0xBFU, 0x01U};
            reset_spies();
            st.currentslot = slot;
            st.dmr_lrrp_source[slot] = 1234;
            st.dmr_lrrp_target[slot] = 5678;
            DSD_SNPRINTF(st.dmr_lrrp_gps[slot], sizeof st.dmr_lrrp_gps[slot], "%s", "Data Call - stale");
            dmr_udp_comp_pdu(&opts, &st, (uint16_t)sizeof pdu, pdu);
            rc |= expect_count(g_datacall_calls, 1U, "t12 single notice");
            rc |= expect_has_substr(g_datacall_text,
                                    "SRC: 1:97 (Ethernet):(Manufacturer Specific); DST: 0:97 (Radio "
                                    "Network):(Manufacturer Specific); ARS SRC: 1234; DST: 5678; ARS Ack: OK; "
                                    "refresh 30 min;",
                                    "t12 notice carries the ARS summary");
            rc |= expect_str_eq(st.dmr_lrrp_gps[slot], "ARS SRC: 1234; DST: 5678; ARS Ack: OK; refresh 30 min; ",
                                "t12 slot string rewritten");
            rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "t12 control");
        }
        st.currentslot = 0;
    }

    // T13: ports in the extended headers. A radio (SAID 0) registering with the ARS server (DAID 1),
    // 4005 -> 4005, with the spec's initial registration example for radio 11 (2.3.1).
    {
        const uint8_t pdu[] = {0x00U, 0x05U, 0x01U, 0x00U, 0x00U, 0x0FU, 0xA5U, 0x0FU, 0xA5U,
                               0x00U, 0x07U, 0xF0U, 0x20U, 0x02U, 0x31U, 0x31U, 0x00U, 0x00U};
        run_case(&opts, &st, pdu, sizeof pdu);
        rc |= expect_has_substr(g_datacall_text,
                                "(In Extended Header, UDP 4005); DST: 1:0 (Ethernet):(In Extended "
                                "Header, UDP 4005); ARS SRC: 1234; DST: 5678; ARS Reg: 11; Initial;",
                                "t13 registration");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "t13 control");
    }

    // T14: the radio's ARS port is fixed at 4005 while the server's is configurable (ARS spec 4.4.2,
    // 4.4.4), so a registration to server port 4025 is still ARS. A claimed destination wins over a
    // source that names a service: 5016 -> 4004 is XCMP, 4004 -> 5016 is text.
    {
        const uint8_t to_4025[] = {0x00U, 0x06U, 0x01U, 0x00U, 0x00U, 0x0FU, 0xA5U,
                                   0x0FU, 0xB9U, 0x00U, 0x03U, 0xBFU, 0x10U, 0x80U};
        run_case(&opts, &st, to_4025, sizeof to_4025);
        rc |= expect_has_substr(g_datacall_text, "ARS SRC: 1234; DST: 5678; ARS Ack: OK; refresh 480 min;",
                                "t14 source 4005 names ARS");

        const uint8_t text_to_xcmp[] = {0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x13U, 0x98U, 0x0FU, 0xA4U, 0x00U, 'H'};
        run_case(&opts, &st, text_to_xcmp, sizeof text_to_xcmp);
        rc |= expect_has_substr(g_datacall_text, "XCMP SRC: 1234; DST: 5678;", "t14 claimed destination wins");
        if (staged_text(&st)[0] != '\0') {
            DSD_FPRINTF(stderr, "t14 5016 -> 4004 decoded text '%s'\n", staged_text(&st));
            rc |= 1;
        }

        const uint8_t xcmp_to_text[] = {0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x0FU, 0xA4U, 0x13U, 0x98U, 0x00U, 'H'};
        run_case(&opts, &st, xcmp_to_text, sizeof xcmp_to_text);
        rc |= expect_str_eq(staged_text(&st), "H", "t14 destination 5016 is text");

        const uint8_t lip_to_lrrp[] = {0x00U, 0x09U, 0x00U, 0x00U, 0x00U, 0x13U, 0x99U, 0x0FU, 0xA1U, 0x0DU};
        run_case(&opts, &st, lip_to_lrrp, sizeof lip_to_lrrp);
        rc |= expect_count(g_lip_calls, 0U, "t14 5017 -> 4001 is not LIP");
        rc |= expect_has_substr(g_datacall_text, "LRRP SRC: 1234; DST: 5678; Truncated;", "t14 destination LRRP");
    }

    // T15: what the content check accepts and refuses on an unlinked manufacturer-specific index.
    {
        static const struct {
            const char* tag;
            uint8_t bytes[24];
            uint8_t len;
            const char* want; // NULL: must stay Unknown Decode Format
        } cases[] = {
            {"reg ack no timer", {0x00, 0x01, 0x3F}, 3, "ARS Ack: OK; refresh off;"},
            {"reg ack fail no reason", {0x00, 0x01, 0x7F}, 3, "ARS Ack: FAIL;"},
            {"reg ack fail reason", {0x00, 0x02, 0xFF, 0x00}, 4, "ARS Ack: FAIL - device not authorized;"},
            {"csbk query ack", {0x00, 0x03, 0x3F, 0x10, 0x80}, 5, "ARS Ack: OK; refresh off;"},
            {"device registration", {0x00, 0x07, 0xF0, 0x40, 0x02, 0x31, 0x31, 0x00, 0x00}, 9, "ARS Reg: 11; Refresh;"},
            {"user registration",
             {0x00, 0x10, 0xF5, 0x00, 0x02, 0x31, 0x31, 0x09, '9', '9', '9', '9', '9', '9', '9', '9', '9', 0x00},
             18,
             "ARS User Reg: 999999999; DEV: 11;"},
            {"deregistration", {0x00, 0x01, 0x31}, 3, "ARS Dereg;"},
            {"query", {0x00, 0x01, 0x74}, 3, "ARS Query;"},
            {"user dereg", {0x00, 0x01, 0x76}, 3, "ARS User Dereg;"},
            {"user ack bare", {0x00, 0x01, 0x37}, 3, "ARS User Ack: OK;"},
            {"user ack session", {0x00, 0x02, 0xB7, 0x00}, 4, "ARS User Ack: OK; session until power cycle;"},
            {"user ack fail no reason", {0x00, 0x01, 0x77}, 3, "ARS User Ack: FAIL;"},
            {"user ack fail reason", {0x00, 0x02, 0xF7, 0x01}, 4, "ARS User Ack: FAIL - user validation failed;"},
            // A TMS acknowledgement shares the 0xBF header octet; a recorded one rode index 98 (ok-dmrlib).
            {"tms ack", {0x00, 0x03, 0xBF, 0x00, 0x07}, 5, NULL},
            {"tms ack no reserved bit", {0x00, 0x04, 0x9F, 0x00, 0x95, 0x20}, 6, NULL},
            {"tms ack short", {0x00, 0x02, 0x1F, 0x00}, 4, NULL},
            {"tms text", {0x00, 0x05, 0xE0, 0x00, 0x01, 'h', 0x00}, 7, NULL},
            {"lrrp", {0x0D, 0x02, 0x22, 0x01}, 4, NULL},
            {"size short of payload", {0x00, 0x02, 0xBF, 0x01, 0x00}, 5, NULL},
            {"size past payload", {0x00, 0x03, 0xBF, 0x01}, 4, NULL},
            {"priority clear", {0x00, 0x01, 0x1F}, 3, NULL},
            {"query with ext", {0x00, 0x02, 0xF4, 0x00}, 4, NULL},
            {"dereg with ack", {0x00, 0x01, 0x71}, 3, NULL},
            {"undefined type", {0x00, 0x01, 0x32}, 3, NULL},
            {"device reg with user id", {0x00, 0x08, 0xF0, 0x20, 0x02, 0x31, 0x31, 0x01, 0x41, 0x00}, 10, NULL},
            {"device reg bad event", {0x00, 0x07, 0xF0, 0x00, 0x02, 0x31, 0x31, 0x00, 0x00}, 9, NULL},
            {"registration encoding", {0x00, 0x07, 0xF0, 0x21, 0x02, 0x31, 0x31, 0x00, 0x00}, 9, NULL},
            {"second header ext", {0x00, 0x02, 0xBF, 0x81}, 4, NULL},
            {"user ack session not zero", {0x00, 0x02, 0xB7, 0x05}, 4, NULL},
            {"lv overrun", {0x00, 0x05, 0x70, 0x05, 0x31, 0x00, 0x00}, 7, NULL},
            {"empty record", {0x00, 0x00, 0x00}, 3, NULL},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            uint8_t pdu[5 + 24];
            pdu[0] = 0x00U;
            pdu[1] = 0x0AU;
            pdu[2] = 0x10U;
            pdu[3] = 0x62U; // 98
            pdu[4] = 0x62U;
            DSD_MEMCPY(pdu + 5, cases[i].bytes, cases[i].len);
            run_case(&opts, &st, pdu, (size_t)(5U + cases[i].len));
            if (cases[i].want != NULL) {
                rc |= expect_has_substr(g_datacall_text, cases[i].want, cases[i].tag);
                rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, cases[i].tag);
            } else {
                rc |= expect_no_substr(g_datacall_text, "ARS", cases[i].tag);
                rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, cases[i].tag);
            }
        }
    }

    // T16: Reserved indices carry nothing a receiver may interpret, ARS-shaped or not. One unlinked
    // manufacturer-specific endpoint is enough when the other endpoint names no decoder (port 6000).
    {
        const uint8_t reserved[] = {0x00U, 0x0BU, 0x10U, 0x3FU, 0x3FU, 0x00U, 0x02U, 0xBFU, 0x01U};
        run_case(&opts, &st, reserved, sizeof reserved);
        rc |= expect_no_substr(g_datacall_text, "ARS", "t16 reserved index");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_DATA, "t16 reserved category");

        const uint8_t one_mfr[] = {0x00U, 0x0CU, 0x10U, 0x61U, 0x00U, 0x17U, 0x70U, 0x00U, 0x02U, 0xBFU, 0x01U};
        run_case(&opts, &st, one_mfr, sizeof one_mfr);
        rc |= expect_has_substr(g_datacall_text, "ARS Ack: OK; refresh 30 min;", "t16 one unlinked endpoint");
    }

    // T17: a port in an extended header reaches every UDP service the IPv4 path decodes: TMS on 4007,
    // and an --lrrp-extra-port port as LRRP. A one-octet LRRP after a decoded one does not inherit
    // its summary.
    {
        const uint8_t tms[] = {0x00U, 0x0DU, 0x00U, 0x3FU, 0x00U, 0x0FU, 0xA7U, 0x00U,
                               0x06U, 0xA0U, 0x00U, 0x80U, 0x04U, 'H',   0x00U};
        run_case(&opts, &st, tms, sizeof tms);
        rc |= expect_has_substr(g_datacall_text, "TMS SRC: 1234; DST: 5678;", "t17 tms");
        rc |= expect_str_eq(staged_text(&st), "H", "t17 tms text");

        opts.lrrp_extra_port_count = 1;
        opts.lrrp_extra_ports[0] = 6001U;
        const uint8_t extra[] = {0x00U, 0x0EU, 0x00U, 0x3FU, 0x00U, 0x17U, 0x71U, 0x0DU};
        DSD_SNPRINTF(st.dmr_lrrp_gps[0], sizeof st.dmr_lrrp_gps[0], "%s", "LRRP SRC: 1; previous fix 41.5, -87.2");
        run_case(&opts, &st, extra, sizeof extra);
        rc |= expect_has_substr(g_datacall_text, "LRRP SRC: 1234; DST: 5678; Truncated;", "t17 extra port truncated");
        rc |= expect_no_substr(g_datacall_text, "previous fix", "t17 no stale summary");
        opts.lrrp_extra_port_count = 0;
        opts.lrrp_extra_ports[0] = 0U;
    }

    // T18: a site that knows its radios' linkage maps an index to a port (ETSI TS 102 361-3 table 7.17
    // note 2). The mapped port decodes as any resolved port, labelled as mapped; an unmapped index
    // still goes by content, and a mapped index whose port has no decoder is not second-guessed.
    {
        opts.dmr_udp_comp_ports[97U - 95U] = 4005U;
        opts.dmr_udp_comp_ports[98U - 95U] = 4007U;
        const uint8_t ars[] = {0x00U, 0x0FU, 0x10U, 0x61U, 0x61U, 0x00U, 0x05U, 0xF0U, 0x20U, 0x02U, 0x31U, 0x31U};
        run_case(&opts, &st, ars, sizeof ars);
        rc |= expect_has_substr(g_datacall_text,
                                "SRC: 1:97 (Ethernet):(Manufacturer Specific, mapped UDP 4005); DST: 0:97 (Radio "
                                "Network):(Manufacturer Specific, mapped UDP 4005); ARS SRC: 1234; DST: 5678;",
                                "t18 mapped ars");
        rc |= expect_category(g_datacall_category, DSD_EVENT_CATEGORY_CONTROL, "t18 mapped category");

        const uint8_t tms_ack[] = {0x00U, 0x10U, 0x00U, 0x62U, 0x62U, 0x00U, 0x03U, 0xBFU, 0x00U, 0x07U};
        run_case(&opts, &st, tms_ack, sizeof tms_ack);
        rc |= expect_has_substr(g_datacall_text, "(Manufacturer Specific, mapped UDP 4007)", "t18 mapped tms label");
        rc |= expect_has_substr(g_datacall_text, "TMS SRC: 1234; DST: 5678; Acknowledgment;", "t18 mapped tms ack");

        opts.dmr_udp_comp_ports[99U - 95U] = 6000U;
        const uint8_t unclaimed[] = {0x00U, 0x11U, 0x00U, 0x63U, 0x63U, 0x00U, 0x02U, 0xBFU, 0x01U};
        run_case(&opts, &st, unclaimed, sizeof unclaimed);
        rc |= expect_no_substr(g_datacall_text, "ARS", "t18 mapped to an undecoded port");
        DSD_MEMSET(opts.dmr_udp_comp_ports, 0, sizeof opts.dmr_udp_comp_ports);
    }

    // T19: the P25 services that print IPv4 addresses name the radios on the compressed path, which
    // carries no addresses.
    {
        const uint8_t atlas[] = {0x00U, 0x12U, 0x00U, 0x3FU, 0x00U, 0x24U, 0x91U, 0x01U};
        run_case(&opts, &st, atlas, sizeof atlas);
        rc |= expect_has_substr(g_datacall_text, "P25 Atlas SRC: 1234; DST: 5678;", "t19 atlas");
        const uint8_t locn[] = {0x00U, 0x13U, 0x00U, 0x3FU, 0x00U, 0xC0U, 0x2EU, 0x0DU};
        run_case(&opts, &st, locn, sizeof locn);
        rc |= expect_has_substr(g_datacall_text, "P25 Tier 2 LOCN SRC: 1234; DST: 5678;", "t19 locn");
    }

    free(st.event_history_s);
    st.event_history_s = NULL;

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
