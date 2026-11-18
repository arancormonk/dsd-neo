// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA location information protocol (TS 100 392-18-1) in SDS: protocol identifier 10 (EN 300 392-2 table 29.21) with
 * no SDS-TL header, the LIP PDU right after the PID (table 29.49), whole or as a concatenated message (clause
 * 29.5.14.1). A short location report reaches the event history, the location file (-L) and the position feed only
 * when it is admitted: the block CRC passed (the MAC delivers nothing else), the LLC FCS too when the PDU carries one,
 * an earlier CRC-passed block proved the carrier, and a concatenated message was received on one reception and
 * addressed to one party. The SDS
 * sender is the report's source, labelled from the local radio list only when it sends from the network the decoder
 * is on. Blocks are built field by field (tetra_test_pdu.h) and decoded by the real MAC, LLC, MLE and event layer.
 * Coordinates are made up.
 */

#include <assert.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_log.h"
#include "tetra_test_pdu.h"

#define SECOND_NS    1000000000ULL

#define HOME_MCC     100u
#define HOME_MNC     20u
#define FOREIGN_MCC  200u
#define FOREIGN_MNC  30u
#define OTHER_MCC    300u
#define OTHER_MNC    40u
#define SENDER_SSI   4242u
#define ADDRESSED    0x101u

/* Two's complement codes of table 6.1: 22.5 degrees of latitude is 2^21 units of 180/2^24, 45.0 degrees of longitude
 * 2^22 units of 360/2^25. */
#define LAT_22_5     0x200000u
#define LON_45_0     0x400000u
#define LAT_MINUS_22 (0x1000000u - 0x200000u)
#define LON_MINUS_45 (0x2000000u - 0x400000u)

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    char lfile[DSD_TEST_PATH_MAX];
    char log[8192];
} session;

/* A clear SDS on a proved carrier of network HOME_MCC/HOME_MNC, with the event history, the position feed and -L. */
static session
session_open(void) {
    session s;
    DSD_MEMSET(&s, 0, sizeof s);
    dsd_decode_clock_use_test(SECOND_NS * 1000ULL);
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    const int fd = dsd_test_mkstemp(s.lfile, sizeof s.lfile, "tetra_lip");
    assert(fd >= 0 && dsd_close(fd) == 0);
    DSD_SNPRINTF(s.opts->lrrp_out_file, sizeof s.opts->lrrp_out_file, "%s", s.lfile);
    s.opts->lrrp_file_output = 1;
    s.state->event_history_s = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    assert(s.state->event_history_s != NULL);
    init_event_history(&s.state->event_history_s[0], 0, 255);
    init_event_history(&s.state->event_history_s[1], 0, 255);
    tetra_state_t* const ts = tetra_st(s.state);
    assert(ts != NULL);
    ts->decode_ok = 1U;
    ts->net_known = 1U;
    ts->mcc = HOME_MCC;
    ts->mnc = HOME_MNC;
    ts->colour = 25U;
    s.state->synctype = DSD_SYNC_TETRA_NDB_POS;
    s.state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    tetra_publish(s.state);
    dsd_rxid_note_tetra_cell(s.state, HOME_MCC, HOME_MNC, 25U); /* the cell's checked BSCH */
    assert(dsd_position_feed_start(s.state) == 0);
    tetra_confirm_note_signalling(s.state);
    return s;
}

static void
session_close(session* s) {
    dsd_state_ext_free_all(s->state);
    free(s->state->event_history_s);
    free(s->state);
    free(s->opts);
    (void)remove(s->lfile);
}

/* A LIP short location report (table 6.1): 76 bits. */
static ttb_block
lip_short(uint32_t lat, uint32_t lon, uint32_t pos_err, uint32_t velocity, uint32_t direction) {
    ttb_block b = {0};
    ttb_put(&b, 0, 2);         /* PDU type: short location report */
    ttb_put(&b, 0, 2);         /* time elapsed: < 5 s */
    ttb_put(&b, lon, 25);      /* longitude */
    ttb_put(&b, lat, 24);      /* latitude */
    ttb_put(&b, pos_err, 3);   /* position error */
    ttb_put(&b, velocity, 7);  /* horizontal velocity */
    ttb_put(&b, direction, 4); /* direction of travel */
    ttb_put(&b, 0, 1);         /* type of additional data: reason for sending */
    ttb_put(&b, 0, 8);         /* reason: subscriber unit is powered ON */
    assert(b.n == (int)LIP_SHORT_REPORT_BITS);
    return b;
}

/* D-SDS-DATA (table 14.13) head: calling party of type @p type (1 SSI, 2 SSI and its network's MCC and MNC), user
 * defined data-4 of @p user_bits. */
static void
sds_head(ttb_block* c, uint32_t type, uint32_t ssi, uint32_t mcc, uint32_t mnc, int user_bits) {
    ttb_put(c, TETRA_MLE_PD_CMCE, 3);
    ttb_put(c, TETRA_CMCE_D_SDS_DATA, 5);
    ttb_put(c, type, 2);
    if (type == 1u || type == 2u) {
        ttb_put(c, ssi, 24); /* the reserved types 0 and 3 carry no address */
    }
    if (type == 2u) {
        ttb_put(c, (mcc << 14) | mnc, 24); /* calling party extension (table 14.44) */
    }
    ttb_put(c, 3, 2);                    /* short data type identifier: user defined data-4 */
    ttb_put(c, (uint32_t)user_bits, 11); /* length indicator */
}

/* A whole SDS message under PID 10 carrying @p lip. */
static ttb_block
sds_lip(uint32_t type, uint32_t ssi, uint32_t mcc, uint32_t mnc, const ttb_block* lip) {
    ttb_block c = {0};
    sds_head(&c, type, ssi, mcc, mnc, 8 + lip->n);
    ttb_put(&c, 10, 8); /* protocol identifier: location information protocol, no SDS-TL header */
    ttb_put_bits(&c, lip->bits, lip->n);
    ttb_put(&c, 0, 1); /* O-bit */
    return c;
}

/* Part @p sequence of 2 of concatenated message @p reference (tables 29.60, 29.62) under PID 12, or with
 * @p transport under PID 140 after an SDS-TRANSFER header (table 29.11), holding @p n bits of @p payload from SSI
 * @p ssi; part 1 names payload PID @p payload_pid. */
static ttb_block
sds_concat_part_pid(uint32_t ssi, int transport, uint32_t reference, uint32_t sequence, uint32_t payload_pid,
                    const uint8_t* payload, int n) {
    ttb_block c = {0};
    const int head = 8 + (transport ? 16 : 0) + 3 + 1 + 4 + 8 + 8 + (sequence == 1u ? 8 : 0);
    sds_head(&c, 1, ssi, 0, 0, head + n);
    ttb_put(&c, transport ? 140u : 12u, 8); /* protocol identifier: concatenated SDS message */
    if (transport) {
        ttb_put(&c, 0, 4);        /* SDS-TL message type: SDS-TRANSFER */
        ttb_put(&c, 0, 3);        /* delivery report request, service selection */
        ttb_put(&c, 0, 1);        /* no storage/forward control */
        ttb_put(&c, sequence, 8); /* message reference */
    }
    ttb_put(&c, 0, 3);         /* concatenation control: transfer */
    ttb_put(&c, 0, 1);         /* no reference extension */
    ttb_put(&c, reference, 4); /* concatenated message reference */
    ttb_put(&c, 2, 8);         /* parts */
    ttb_put(&c, sequence, 8);
    if (sequence == 1u) {
        ttb_put(&c, payload_pid, 8); /* PID of the whole message */
    }
    ttb_put_bits(&c, payload, n);
    ttb_put(&c, 0, 1); /* O-bit */
    return c;
}

/* sds_concat_part_pid() whose part 1 names payload PID 10 (location information protocol). */
static ttb_block
sds_concat_part(uint32_t ssi, int transport, uint32_t reference, uint32_t sequence, const uint8_t* payload, int n) {
    return sds_concat_part_pid(ssi, transport, reference, sequence, 10u, payload, n);
}

/* A MAC-RESOURCE to the SSI @p addressed (address type @p addr_type; for an event label alone, @p addressed is the
 * label) carrying @p cmce in a BL-UDATA, or with @p fcs a BL-UDATA-FCS (table 21.1) whose FCS (clause 22.3.3) is the
 * right one, or one bit off with @p bad_fcs; decoded as one CRC-passed SCH/F block. */
static void
send_sds_to(session* s, uint32_t addr_type, uint32_t addressed, const ttb_block* cmce, int fcs, int bad_fcs) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, addr_type, addressed);
    if (addr_type == TETRA_MAC_ADDR_EVENT_LABEL) {
        ttb_put(&b, addressed, 10);
    }
    ttb_resource_flags(&b, 0u);
    ttb_put(&b, fcs ? 6u : 2u, 4); /* LLC PDU type: BL-UDATA-FCS or BL-UDATA */
    ttb_put_bits(&b, cmce->bits, cmce->n);
    if (fcs) {
        uint32_t value = tetra_llc_fcs32(cmce->bits, cmce->n);
        if (bad_fcs) {
            value ^= 1u;
        }
        ttb_put(&b, value, 32);
    }
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_test_mac_logged(b.bits, b.n, 1, s->opts, s->state, s->log, sizeof s->log);
}

/* send_sds_to() to ADDRESSED. */
static void
send_sds(session* s, const ttb_block* cmce, int fcs, int bad_fcs) {
    send_sds_to(s, TETRA_MAC_ADDR_SSI, ADDRESSED, cmce, fcs, bad_fcs);
}

static const Event_History*
notice_row(const session* s) {
    return &s->state->event_history_s[0].Event_History_Items[1];
}

/* The -L file's content. */
static void
read_lfile(const session* s, char* out, size_t cap) {
    out[0] = '\0';
    FILE* f = dsd_fopen_private(s->lfile, "r");
    assert(f != NULL);
    const size_t n = fread(out, 1U, cap - 1U, f);
    out[n] = '\0';
    fclose(f);
}

static void
clear_lfile(const session* s) {
    FILE* f = dsd_fopen_private(s->lfile, "w");
    assert(f != NULL);
    fclose(f);
}

/* The feed records kept after @p after_seq. */
static int
feed_since(const session* s, uint64_t after_seq, dsd_position_fix* out, size_t cap) {
    dsd_position_feed_meta meta;
    return dsd_position_feed_copy_since(s->state, after_seq, out, cap, &meta);
}

/* A short report under PID 10, with and without the LLC FCS: the notice row carries the position and its text, the
 * location file a row, the feed the record, all as the SDS sender's to the addressed party. A wrong FCS discards the
 * PDU before the MLE, so nothing of it is published. */
static void
test_direct_with_and_without_fcs(void) {
    for (int fcs = 0; fcs <= 1; fcs++) {
        session s = session_open();
        const ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);
        const ttb_block c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
        send_sds(&s, &c, fcs, 0);
        assert(strstr(s.log, "[TETRA SDS LIP] CC=1 src_SSI=4242 dst_SSI=257 short location report; LIP: 22.50000")
               != NULL);
        const Event_History* row = notice_row(&s);
        assert(row->category == DSD_EVENT_CATEGORY_DATA && row->source_id == SENDER_SSI && row->target_id == ADDRESSED);
        assert(strstr(row->event_string, "SDS SRC: 4242; TGT: 257; LIP: short location report;") != NULL);
        assert(row->pos.valid == 1U && row->pos.kind == DSD_POS_TETRA_LIP);
        assert(row->pos.lat_e7 == 225000000 && row->pos.lon_e7 == 450000000 && row->pos.err_m == 20U);
        assert((row->pos.flags & (DSD_POS_FLAG_SOURCE_UNVERIFIED | DSD_POS_FLAG_TARGET_GROUP)) == 0U);
        assert(row->pos.scope.kind == DSD_POS_SCOPE_TETRA && row->pos.scope.a == HOME_MCC
               && row->pos.scope.b == HOME_MNC);
        assert(strstr(row->gps_s, "LIP: 22.50000") != NULL && strstr(row->gps_s, "Err: 20m") != NULL
               && strstr(row->gps_s, "Spd: 10 km/h") != NULL && strstr(row->gps_s, "Dir: 90") != NULL);
        char lfile[512];
        read_lfile(&s, lfile, sizeof lfile);
        assert(strstr(lfile, "\t00004242\t22.500000\t45.000000\t10\t 90\t") != NULL);
        dsd_position_fix rec[2];
        assert(feed_since(&s, 0, rec, 2) == 1);
        assert(rec[0].kind == DSD_POS_TETRA_LIP && DSD_SYNC_IS_TETRA(rec[0].protocol) && rec[0].slot == 0U
               && rec[0].source_id == SENDER_SSI && rec[0].target_id == ADDRESSED && rec[0].age_class == 0U
               && rec[0].speed_dkmh == 100U && rec[0].heading_ddeg == 900U);
        assert(rec[0].sender_scope.kind == DSD_POS_SCOPE_TETRA && rec[0].sender_scope.a == HOME_MCC);
        assert(dsd_event_staged_position(s.state, 0U)->rev == 0U); /* the notice took it */

        if (fcs) {
            /* One FCS bit off: the LLC discards the PDU, so no notice, no position, no -L row. */
            clear_lfile(&s);
            const ttb_block south = lip_short(LAT_MINUS_22, LON_MINUS_45, 1, 10, 4);
            const ttb_block bad = sds_lip(1, SENDER_SSI + 1u, 0, 0, &south);
            const uint64_t rev = row->pos.rev;
            send_sds(&s, &bad, 1, 1);
            assert(strstr(s.log, "TETRA SDS LIP") == NULL && strstr(s.log, "D-SDS-DATA") == NULL);
            assert(notice_row(&s)->pos.rev == rev && notice_row(&s)->source_id == SENDER_SSI);
            read_lfile(&s, lfile, sizeof lfile);
            assert(lfile[0] == '\0');
            assert(feed_since(&s, rec[0].seq, rec, 2) == 0);
        }
        session_close(&s);
    }
}

/* The south-west quarter, and the codes that are bounds or unknown (tables 6.93, 6.51, 6.45): error 7 keeps the
 * report with its validity unknown (decision D11), speed 126 is a lower bound, 127 and direction 15 unknown or
 * ambiguous. */
static void
test_codes(void) {
    session s = session_open();
    ttb_block lip = lip_short(LAT_MINUS_22, LON_MINUS_45, 7, 127, 15);
    ttb_block c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    send_sds(&s, &c, 0, 0);
    const Event_History* row = notice_row(&s);
    assert(row->pos.valid == 1U && row->pos.lat_e7 == -225000000 && row->pos.lon_e7 == -450000000);
    assert((row->pos.flags & DSD_POS_FLAG_VALIDITY_UNKNOWN) != 0U && (row->pos.flags & DSD_POS_FLAG_HEADING_AMBIGUOUS));
    assert(row->pos.err_m == DSD_POSITION_ERR_UNKNOWN);
    assert(strstr(row->gps_s, "fix may be old or invalid") != NULL && strstr(row->gps_s, "Spd: unknown") != NULL
           && strstr(row->gps_s, "or unknown") != NULL && strstr(row->gps_s, "S ") != NULL);
    char lfile[512];
    read_lfile(&s, lfile, sizeof lfile);
    assert(strstr(lfile, "\t00004242\t-22.500000\t-45.000000\t0\t 0\t") != NULL);
    dsd_position_fix rec[2];
    assert(feed_since(&s, 0, rec, 2) == 1 && rec[0].speed_dkmh == DSD_POSITION_SPEED_UNKNOWN);

    clear_lfile(&s);
    lip = lip_short(LAT_22_5, LON_45_0, 6, 126, 2);
    c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    send_sds(&s, &c, 0, 0);
    row = notice_row(&s);
    assert(row->pos.valid == 1U && (row->pos.flags & DSD_POS_FLAG_ERR_LOWER_BOUND) != 0U
           && (row->pos.flags & DSD_POS_FLAG_SPEED_LOWER_BOUND) != 0U && row->pos.err_m == 200000U);
    assert(strstr(row->gps_s, "Spd: >1043 km/h") != NULL && strstr(row->gps_s, "Err: >200km") != NULL);
    read_lfile(&s, lfile, sizeof lfile);
    assert(strstr(lfile, "\t22.500000\t45.000000\t1043\t 45\t") != NULL);
    session_close(&s);
}

/* The type of additional data and its value (table 6.1) of @p lip: 0 a reason for sending, 1 user-defined data. */
static void
lip_set_additional(ttb_block* lip, uint32_t user_data, uint32_t value) {
    lip->bits[67] = (uint8_t)user_data;
    for (int i = 0; i < 8; i++) {
        lip->bits[68 + i] = (uint8_t)((value >> (7 - i)) & 1U);
    }
}

/* Reason for sending 2, "Emergency condition is detected" (table 6.94), marks the record as an emergency, so the same
 * place reported again as an emergency is no repeat of the ordinary report; user-defined data 2 is no reason. */
static void
test_emergency_reason(void) {
    session s = session_open();
    ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 0, 0);
    ttb_block c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    send_sds(&s, &c, 0, 0);
    lip_set_additional(&lip, 0U, 2U);
    c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    send_sds(&s, &c, 0, 0);
    dsd_position_fix rec[4];
    assert(feed_since(&s, 0, rec, 4) == 2);
    assert((rec[0].flags & DSD_POS_FLAG_EMERGENCY) == 0U && (rec[1].flags & DSD_POS_FLAG_EMERGENCY) != 0U);
    assert(rec[1].lat_e7 == rec[0].lat_e7 && rec[1].lon_e7 == rec[0].lon_e7);
    assert((notice_row(&s)->pos.flags & DSD_POS_FLAG_EMERGENCY) != 0U);
    session_close(&s);

    s = session_open();
    lip_set_additional(&lip, 1U, 2U);
    c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    send_sds(&s, &c, 0, 0);
    assert(feed_since(&s, 0, rec, 4) == 1 && (rec[0].flags & DSD_POS_FLAG_EMERGENCY) == 0U);
    session_close(&s);
}

/* What is not published: a report at exactly (0, 0) is refused, a report on a carrier no earlier CRC-passed block
 * proved waits like the notice, and a long report is named, not decoded. None prints coordinates. */
static void
test_not_published(void) {
    session s = session_open();
    char lfile[512];
    ttb_block lip = lip_short(0, 0, 1, 0, 0);
    ttb_block c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    send_sds(&s, &c, 0, 0);
    assert(strstr(s.log, "short location report; Position: (refused)") != NULL && strstr(s.log, "0.00000") == NULL);
    assert(strstr(notice_row(&s)->event_string, "LIP: short location report;") != NULL
           && notice_row(&s)->pos.valid == 0U && notice_row(&s)->gps_s[0] == '\0');
    read_lfile(&s, lfile, sizeof lfile);
    assert(lfile[0] == '\0');

    /* PDU type 01, extension 0011: a long location report (table 6.92). */
    ttb_block longrep = {0};
    ttb_put(&longrep, 1, 2);
    ttb_put(&longrep, 3, 4);
    ttb_put(&longrep, 0, 2); /* time elapsed */
    ttb_put(&longrep, 0, 4); /* location shape: no shape */
    ttb_put(&longrep, 0, 2);
    c = sds_lip(1, SENDER_SSI, 0, 0, &longrep);
    send_sds(&s, &c, 0, 0);
    assert(strstr(s.log, "long location report (not decoded)") != NULL);
    assert(strstr(notice_row(&s)->event_string, "LIP: long location report (not decoded);") != NULL
           && notice_row(&s)->pos.valid == 0U);

    tetra_confirm_reset(s.state);
    lip = lip_short(LAT_22_5, LON_45_0, 1, 0, 0);
    c = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    send_sds(&s, &c, 0, 0);
    assert(strstr(s.log, "short location report; Position: (carrier not proven)") != NULL
           && strstr(s.log, "22.5") == NULL);
    assert(strstr(notice_row(&s)->event_string, "long location report") != NULL); /* no new notice */
    read_lfile(&s, lfile, sizeof lfile);
    assert(lfile[0] == '\0');
    dsd_position_fix rec[2];
    assert(feed_since(&s, 0, rec, 2) == 0);
    assert(dsd_event_staged_position(s.state, 0U)->rev == 0U);
    session_close(&s);
}

/* The LIP PDUs that carry no position this decoder publishes are named on the console and in the notice and give no
 * position: a short report shorter than its 76 bits, other PDU type extensions (table 6.92), the reserved PDU types
 * (table 6.91), a PDU too short for its type; and a short report after a reserved calling party type (table 14.43),
 * whose address length is unknown, so nothing after it can be placed. */
static void
test_undecoded(void) {
    session s = session_open();
    const ttb_block whole = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);
    ttb_block cut = whole;
    cut.n = 75;
    ttb_block request = {0};
    ttb_put(&request, 1, 2); /* PDU type extension follows */
    ttb_put(&request, 1, 4); /* 0001: immediate location report request */
    ttb_put(&request, 0, 8);
    ttb_block reserved = {0};
    ttb_put(&reserved, 2, 2); /* reserved PDU type */
    ttb_put(&reserved, 0, 14);
    ttb_block one_bit = {0};
    ttb_put(&one_bit, 0, 1);
    ttb_block ext_cut = {0};
    ttb_put(&ext_cut, 1, 2);
    ttb_put(&ext_cut, 0, 3); /* the 4-bit extension cut short */

    const struct {
        const ttb_block* pdu;
        const char* what;
    } k_cases[] = {
        {&cut, "short location report truncated (75 bits)"},
        {&request, "PDU type extension 1 (not decoded)"},
        {&reserved, "reserved PDU type 2 (not decoded)"},
        {&one_bit, "truncated (1 bits)"},
        {&ext_cut, "truncated (5 bits)"},
    };

    char want[160];
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        const ttb_block c = sds_lip(1, SENDER_SSI, 0, 0, k_cases[i].pdu);
        send_sds(&s, &c, 0, 0);
        DSD_SNPRINTF(want, sizeof want, "dst_SSI=257 %s\n", k_cases[i].what);
        assert(strstr(s.log, want) != NULL && strstr(s.log, "22.5") == NULL);
        DSD_SNPRINTF(want, sizeof want, "SDS SRC: 4242; TGT: 257; LIP: %s;", k_cases[i].what);
        assert(strstr(notice_row(&s)->event_string, want) != NULL && notice_row(&s)->pos.valid == 0U);
    }

    const ttb_block anon = sds_lip(0, 0, 0, 0, &whole);
    send_sds(&s, &anon, 0, 0);
    assert(strstr(s.log, "short location report; Position: (calling party type reserved)") != NULL
           && strstr(s.log, "22.5") == NULL);
    assert(notice_row(&s)->pos.valid == 0U);
    char lfile[512];
    read_lfile(&s, lfile, sizeof lfile);
    assert(lfile[0] == '\0');
    dsd_position_fix rec[2];
    assert(feed_since(&s, 0, rec, 2) == 0);
    session_close(&s);
}

/* A concatenated message (PID 12, or 140 with SDS-TL) whose payload PID is 10 is a LIP PDU once complete. Its parts
 * must come from one reception: across a carrier boundary between them, nothing of the message is published under the
 * reception the receiver is on now, neither its position nor its notice. The part left from the other reception is
 * dropped before the new part is taken: the message retransmitted whole on the new reception completes, its part 1 no
 * longer paired with the old part 2. */
static void
test_concatenated(void) {
    const ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);
    for (int run = 0; run < 3; run++) {
        const int moved = run == 1;
        const int transport = run == 2;
        session s = session_open();
        ttb_block c = sds_concat_part(SENDER_SSI, transport, 5, 2, lip.bits + 40, lip.n - 40);
        send_sds(&s, &c, 0, 0);
        assert(strstr(s.log, "TETRA SDS LIP") == NULL);
        if (moved) {
            dsd_rxid_reset(s.state); /* the engine saw a carrier boundary */
            dsd_rxid_note_tetra_cell(s.state, HOME_MCC, HOME_MNC, 25U);
        }
        c = sds_concat_part(SENDER_SSI, transport, 5, 1, lip.bits, 40);
        send_sds(&s, &c, 1, 0);
        const Event_History* row = notice_row(&s);
        dsd_position_fix rec[2];
        if (moved) {
            assert(strstr(s.log, "TETRA SDS LIP") == NULL);
            for (int i = 0; i < 255; i++) {
                const Event_History* any = &s.state->event_history_s[0].Event_History_Items[i];
                assert(strstr(any->event_string, "SDS SRC: 4242") == NULL && any->pos.valid == 0U);
            }
            assert(feed_since(&s, 0, rec, 2) == 0);
            char lfile[512];
            read_lfile(&s, lfile, sizeof lfile);
            assert(lfile[0] == '\0');
            /* The rest of the retransmission, on the reception it began on: the whole message is the report. */
            c = sds_concat_part(SENDER_SSI, transport, 5, 2, lip.bits + 40, lip.n - 40);
            send_sds(&s, &c, 0, 0);
            row = notice_row(&s);
            assert(strstr(s.log, "short location report; LIP: 22.50000") != NULL);
            assert(row->pos.valid == 1U && row->pos.lat_e7 == 225000000 && row->source_id == SENDER_SSI);
            assert(feed_since(&s, 0, rec, 2) == 1 && rec[0].source_id == SENDER_SSI);
        } else {
            assert(strstr(row->event_string, "SDS SRC: 4242; TGT: 257; LIP: short location report;") != NULL);
            assert(strstr(s.log, "short location report; LIP: 22.50000") != NULL);
            assert(row->pos.valid == 1U && row->pos.lat_e7 == 225000000 && row->source_id == SENDER_SSI);
            assert(feed_since(&s, 0, rec, 2) == 1 && rec[0].source_id == SENDER_SSI);
        }
        session_close(&s);
    }
}

/* A concatenated message's parts go from one sender to one party: part 2 of a report to ADDRESSED + 1 and part 1 of
 * another report to ADDRESSED, with the same sender and reference, assemble into nothing; the message completes from
 * its own parts to one party. Parts addressed by an event label alone name no party: their message's position is
 * refused (the notice still goes in). */
static void
test_concatenated_one_recipient(void) {
    const ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);
    const ttb_block other = lip_short(LAT_MINUS_22, LON_MINUS_45, 1, 10, 4);
    session s = session_open();
    ttb_block c = sds_concat_part(SENDER_SSI, 0, 5, 2, other.bits + 40, other.n - 40);
    send_sds_to(&s, TETRA_MAC_ADDR_SSI, ADDRESSED + 1u, &c, 0, 0);
    c = sds_concat_part(SENDER_SSI, 0, 5, 1, lip.bits, 40);
    send_sds(&s, &c, 0, 0);
    assert(strstr(s.log, "TETRA SDS LIP") == NULL);
    dsd_position_fix rec[2];
    assert(feed_since(&s, 0, rec, 2) == 0);
    char lfile[512];
    read_lfile(&s, lfile, sizeof lfile);
    assert(lfile[0] == '\0');

    c = sds_concat_part(SENDER_SSI, 0, 5, 2, lip.bits + 40, lip.n - 40);
    send_sds(&s, &c, 0, 0);
    assert(strstr(s.log, "dst_SSI=257 short location report; LIP: 22.50000") != NULL);
    assert(feed_since(&s, 0, rec, 2) == 1 && rec[0].lat_e7 == 225000000 && rec[0].target_id == ADDRESSED);
    session_close(&s);

    s = session_open();
    c = sds_concat_part(SENDER_SSI, 0, 6, 2, lip.bits + 40, lip.n - 40);
    send_sds_to(&s, TETRA_MAC_ADDR_EVENT_LABEL, 9u, &c, 0, 0);
    c = sds_concat_part(SENDER_SSI, 0, 6, 1, lip.bits, 40);
    send_sds_to(&s, TETRA_MAC_ADDR_EVENT_LABEL, 9u, &c, 0, 0);
    assert(strstr(s.log, "short location report; Position: (recipient unknown)") != NULL);
    assert(notice_row(&s)->pos.valid == 0U && feed_since(&s, 0, rec, 2) == 0);
    read_lfile(&s, lfile, sizeof lfile);
    assert(lfile[0] == '\0');
    session_close(&s);
}

/* Issue #800 (g62-r9): an SMI and an SSI may hold one value and still be two identities (EN 300 392-1 clauses 7.3.2,
 * 7.3.3). Part 2 of a report addressed to SMI 257 and part 1 of another addressed to SSI 257, with the same sender and
 * reference, assemble into nothing; the SSI's message completes from its own part 2. */
static void
test_concatenated_smi_and_ssi_are_two_parties(void) {
    const ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);
    const ttb_block other = lip_short(LAT_MINUS_22, LON_MINUS_45, 1, 10, 4);
    session s = session_open();
    ttb_block c = sds_concat_part(SENDER_SSI, 0, 5, 2, other.bits + 40, other.n - 40);
    send_sds_to(&s, TETRA_MAC_ADDR_SMI, ADDRESSED, &c, 0, 0);
    c = sds_concat_part(SENDER_SSI, 0, 5, 1, lip.bits, 40);
    send_sds(&s, &c, 0, 0);
    assert(strstr(s.log, "TETRA SDS LIP") == NULL);
    dsd_position_fix rec[2];
    assert(feed_since(&s, 0, rec, 2) == 0);
    char lfile[512];
    read_lfile(&s, lfile, sizeof lfile);
    assert(lfile[0] == '\0');

    c = sds_concat_part(SENDER_SSI, 0, 5, 2, lip.bits + 40, lip.n - 40);
    send_sds(&s, &c, 0, 0);
    assert(strstr(s.log, "dst_SSI=257 short location report; LIP: 22.50000") != NULL);
    assert(feed_since(&s, 0, rec, 2) == 1 && rec[0].lat_e7 == 225000000);
    session_close(&s);
}

/* Two concatenated messages from one sender to one party can share a reference. A part 1 with the same bits as the
 * one held but another payload PID is another message's part 1, never a repeat: the message held is discarded, and
 * the part 2 that follows completes nothing, so no LIP position is read from bits a PID 64 message sent. With the
 * same PID the repeat is a duplicate, and the message completes as one report. */
static void
test_concatenated_first_part_pid(void) {
    const ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);
    for (int transport = 0; transport < 2; transport++) {
        session s = session_open();
        ttb_block c = sds_concat_part_pid(SENDER_SSI, transport, 5, 1, 10u, lip.bits, 40);
        send_sds(&s, &c, 0, 0);
        c = sds_concat_part_pid(SENDER_SSI, transport, 5, 1, 64u, lip.bits, 40);
        send_sds(&s, &c, 0, 0);
        assert(strstr(s.log, "duplicate") == NULL);
        c = sds_concat_part(SENDER_SSI, transport, 5, 2, lip.bits + 40, lip.n - 40);
        send_sds(&s, &c, 0, 0);
        dsd_position_fix rec[2];
        assert(strstr(s.log, "TETRA SDS LIP") == NULL);
        assert(feed_since(&s, 0, rec, 2) == 0);
        char lfile[512];
        read_lfile(&s, lfile, sizeof lfile);
        assert(lfile[0] == '\0');
        session_close(&s);

        s = session_open();
        c = sds_concat_part(SENDER_SSI, transport, 5, 1, lip.bits, 40);
        send_sds(&s, &c, 0, 0);
        send_sds(&s, &c, 0, 0);
        assert(strstr(s.log, "duplicate") != NULL);
        c = sds_concat_part(SENDER_SSI, transport, 5, 2, lip.bits + 40, lip.n - 40);
        send_sds(&s, &c, 0, 0);
        assert(strstr(s.log, "short location report; LIP: 22.50000") != NULL);
        assert(feed_since(&s, 0, rec, 2) == 1 && rec[0].lat_e7 == 225000000);
        session_close(&s);
    }
}

/* One SSI in the local network and in foreign ones, with a populated local radio list: a sender of the network the
 * decoder is on (calling party type 1, or type 2 naming it) takes the radio list's name; a sender naming another
 * network takes only a name learned in that network, never the local list's (spec 2.3 step 5). */
static void
test_sender_network_labels(void) {
    session s = session_open();
    dsd_source_alias_store* radios = dsd_source_alias_store_create();
    assert(radios != NULL);
    dsd_source_alias_entry radio;
    DSD_MEMSET(&radio, 0, sizeof radio);
    radio.id_start = radio.id_end = SENDER_SSI;
    DSD_SNPRINTF(radio.name, sizeof radio.name, "%s", "Local Radio");
    assert(dsd_source_alias_store_append(radios, &radio) == 0);
    dsd_source_alias_install(s.state, radios);
    const dsd_alias_scope foreign = {DSD_ALIAS_SCOPE_TETRA, FOREIGN_MCC, FOREIGN_MNC, 1U};
    assert(dsd_alias_learn_radio(NULL, s.state, &foreign, SENDER_SSI, "Foreign Radio", NULL)
           == DSD_TG_POLICY_LEARN_ADDED);

    const ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);

    const struct {
        uint32_t type, mcc, mnc;
        const char* label;
    } k_cases[] = {
        {1, 0, 0, "Local Radio"},
        {2, HOME_MCC, HOME_MNC, "Local Radio"},
        {2, FOREIGN_MCC, FOREIGN_MNC, "Foreign Radio"},
        {2, OTHER_MCC, OTHER_MNC, ""},
    };

    uint64_t seq = 0;
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        dsd_decode_clock_test_set_ns(SECOND_NS * (1000ULL + 60ULL * (i + 1U))); /* past the repeat window */
        const ttb_block c = sds_lip(k_cases[i].type, SENDER_SSI, k_cases[i].mcc, k_cases[i].mnc, &lip);
        send_sds(&s, &c, 0, 0);
        dsd_position_fix rec[2];
        assert(feed_since(&s, seq, rec, 2) == 1);
        seq = rec[0].seq;
        assert(rec[0].source_id == SENDER_SSI && strcmp(rec[0].label, k_cases[i].label) == 0);
        assert(rec[0].reception_scope.kind == DSD_POS_SCOPE_TETRA && rec[0].reception_scope.a == HOME_MCC
               && rec[0].reception_scope.b == HOME_MNC);
        const uint32_t sender_mcc = k_cases[i].type == 2u ? k_cases[i].mcc : HOME_MCC;
        const uint32_t sender_mnc = k_cases[i].type == 2u ? k_cases[i].mnc : HOME_MNC;
        assert(rec[0].sender_scope.kind == DSD_POS_SCOPE_TETRA && rec[0].sender_scope.a == sender_mcc
               && rec[0].sender_scope.b == sender_mnc);
        /* The row is the sender's own network's (its track), heard here. */
        const Event_History* row = notice_row(&s);
        assert(row->pos.valid == 1U && row->pos.scope.a == sender_mcc && row->rx_scope.scope.a == HOME_MCC);
    }
    session_close(&s);
}

/* Issue #800 (g62-r7): a whole SDS message under PID 10 in a BL-UDATA without its optional FCS, sent as a TM-SDU in
 * two MAC fragments (EN 300 392-2 clause 23.4.3.1.1): a MAC-RESOURCE first fragment to ADDRESSED, then a MAC-END, both
 * SCH/F blocks of timeslot 1. Heard on one reception, the rebuilt TM-SDU is the sender's report. When the receiver
 * moved between the fragments -- a retune that left the TETRA state machine's fragment context standing (a trunk
 * tuning generation step), or a received identity change -- the MAC-END continues nothing: no position, no notice, no
 * -L row, whatever the colour code and timeslot. */
static void
test_mac_fragmented_report(void) {
    const ttb_block lip = lip_short(LAT_22_5, LON_45_0, 1, 10, 4);
    const ttb_block cmce = sds_lip(1, SENDER_SSI, 0, 0, &lip);
    ttb_block sdu = {0};
    ttb_put(&sdu, 2u, 4); /* LLC PDU type 0010: BL-UDATA, no FCS */
    ttb_put_bits(&sdu, cmce.bits, cmce.n);
    const int first_bits = 64;
    for (int run = 0; run < 3; run++) {
        session s = session_open();
        tetra_state_t* const ts = tetra_st(s.state);
        ts->tdma_valid = 1;
        ts->tn = 1;

        ttb_block b = {0};
        const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, ADDRESSED);
        ttb_set(&b, start + 7, 63, 6); /* length indication 111111: first fragment */
        ttb_resource_flags(&b, 0u);
        ttb_put_bits(&b, sdu.bits, first_bits);
        ttb_finish(&b, TTB_SCH_F_BITS, start + 2);
        tetra_test_mac_logged(b.bits, b.n, 1, s.opts, s.state, s.log, sizeof s.log);
        assert(ts->mac_frag[1].active);

        if (run == 1) {
            dsd_trunk_tuning_generation_advance(); /* a retune the TETRA state machine did not see */
        } else if (run == 2) {
            dsd_rxid_reset(s.state); /* the received identity moved */
            dsd_rxid_note_tetra_cell(s.state, HOME_MCC, HOME_MNC, 25U);
        }

        b = (ttb_block){0};
        const int end = ttb_end(&b);
        ttb_put(&b, 0u, 1); /* channel allocation flag */
        ttb_put_bits(&b, sdu.bits + first_bits, sdu.n - first_bits);
        ttb_close_end(&b, end);
        ttb_finish(&b, TTB_SCH_F_BITS, -1);
        tetra_test_mac_logged(b.bits, b.n, 1, s.opts, s.state, s.log, sizeof s.log);

        dsd_position_fix rec[2];
        char lfile[512];
        read_lfile(&s, lfile, sizeof lfile);
        if (run == 0) {
            assert(strstr(s.log, "src_SSI=4242 dst_SSI=257 short location report; LIP: 22.50000") != NULL);
            assert(feed_since(&s, 0, rec, 2) == 1 && rec[0].source_id == SENDER_SSI);
            assert(notice_row(&s)->pos.valid == 1U && strstr(lfile, "\t00004242\t22.500000\t45.000000") != NULL);
        } else {
            assert(strstr(s.log, "TETRA SDS LIP") == NULL && strstr(s.log, "D-SDS-DATA") == NULL);
            assert(!ts->mac_frag[1].active);
            assert(feed_since(&s, 0, rec, 2) == 0 && lfile[0] == '\0');
            for (int i = 0; i < 255; i++) {
                const Event_History* any = &s.state->event_history_s[0].Event_History_Items[i];
                assert(strstr(any->event_string, "SDS SRC: 4242") == NULL && any->pos.valid == 0U);
            }
        }
        session_close(&s);
    }
}

int
main(void) {
    test_direct_with_and_without_fcs();
    test_codes();
    test_emergency_reason();
    test_not_published();
    test_undecoded();
    test_concatenated();
    test_concatenated_one_recipient();
    test_concatenated_smi_and_ssi_are_two_parties();
    test_concatenated_first_part_pid();
    test_sender_network_labels();
    test_mac_fragmented_report();
    DSD_FPRINTF(stdout, "TETRA_LIP: PASS\n");
    return 0;
}
