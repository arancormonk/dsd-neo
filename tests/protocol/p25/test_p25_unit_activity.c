// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 unit activity rows: every message family of the plan's P25 table, through the decoder's own dispatch. A Phase 1
 * TSBK goes in the MAC form the TSBK decoder hands over (process_MAC_VPDU_bridged_p1()), a native Phase 2 PDU through
 * process_MAC_VPDU() / process_MAC_VPDU_crc(), an MBT through p25_decode_pdu_trunking() and a link control word
 * through p25_lcw(). The rows are read back from the ring (dsd_unit_activity_read()).
 *
 * Made-up systems only: WACN 0xBEE00 / SYS 0x123 serving, WACN 0xBEE01 / SYS 0x3A5 another.
 */

#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#define SERVING_WACN 0xBEE00U
#define SERVING_SYS  0x123U
#define OTHER_WACN   0xBEE01U
#define OTHER_SYS    0x3A5U

enum { MAX_ROWS = 32, T0_MS = 50000 };

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    dsd_unit_activity rows[MAX_ROWS];
    int count;
} fixture;

static int
fixture_open(fixture* f) {
    DSD_MEMSET(f, 0, sizeof *f);
    f->opts = (dsd_opts*)calloc(1, sizeof(*f->opts));
    f->state = (dsd_state*)calloc(1, sizeof(*f->state));
    if (f->opts == NULL || f->state == NULL) {
        free(f->opts);
        free(f->state);
        return -1;
    }
    f->state->p2_wacn = SERVING_WACN;
    f->state->p2_sysid = SERVING_SYS;
    dsd_rxid_note_p25_network(f->state, SERVING_WACN, SERVING_SYS);
    return 0;
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
}

/* Read every row the ring holds, in revision order. */
static int
read_rows(fixture* f) {
    dsd_unit_activity_read_meta meta;
    f->count = dsd_unit_activity_read(f->state, 0U, 0U, f->rows, MAX_ROWS, &meta);
    return f->count;
}

static const dsd_unit_activity*
last_row(fixture* f) {
    return read_rows(f) > 0 ? &f->rows[f->count - 1] : NULL;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_u(const char* tag, unsigned got, unsigned want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u (0x%X), want %u (0x%X)\n", tag, got, got, want, want);
        return 1;
    }
    return 0;
}

/* An identity's id, subtype and validity. */
static int
expect_id(const char* tag, const dsd_ua_identity* id, uint32_t want_id, uint8_t want_subtype) {
    char t[96];
    int rc = 0;
    DSD_SNPRINTF(t, sizeof t, "%s valid", tag);
    rc |= expect_u(t, id->valid, 1U);
    DSD_SNPRINTF(t, sizeof t, "%s id", tag);
    rc |= expect_u(t, id->id, want_id);
    DSD_SNPRINTF(t, sizeof t, "%s subtype", tag);
    rc |= expect_u(t, id->subtype, want_subtype);
    return rc;
}

static int
expect_home(const char* tag, const dsd_ua_identity* id, uint32_t wacn, uint32_t sys) {
    int rc = expect_u(tag, id->home_valid, 1U);
    rc |= expect_u(tag, id->home_kind, DSD_UA_SCOPE_P25);
    rc |= expect_u(tag, id->home_a, wacn);
    rc |= expect_u(tag, id->home_b, sys);
    return rc;
}

static int
expect_row(const char* tag, const dsd_unit_activity* r, uint8_t protocol, uint8_t kind, uint8_t role, uint8_t result) {
    if (r == NULL) {
        DSD_FPRINTF(stderr, "%s: no row\n", tag);
        return 1;
    }
    int rc = expect_u(tag, r->protocol, protocol);
    rc |= expect_u(tag, r->kind, kind);
    rc |= expect_u(tag, r->role, role);
    rc |= expect_u(tag, r->result, result);
    return rc;
}

static void
put_u24(unsigned long long int* mac, int pos, uint32_t value) {
    mac[pos + 0] = (value >> 16) & 0xFFU;
    mac[pos + 1] = (value >> 8) & 0xFFU;
    mac[pos + 2] = value & 0xFFU;
}

static void
put_u16(unsigned long long int* mac, int pos, uint32_t value) {
    mac[pos + 0] = (value >> 8) & 0xFFU;
    mac[pos + 1] = value & 0xFFU;
}

/* WACN (20 bits) and System ID (12 bits) from octet @p pos, as in an SUID. */
static void
put_wacn_sys(unsigned long long int* mac, int pos, uint32_t wacn, uint32_t sys) {
    mac[pos + 0] = (wacn >> 12) & 0xFFU;
    mac[pos + 1] = (wacn >> 4) & 0xFFU;
    mac[pos + 2] = ((wacn & 0x0FU) << 4) | ((sys >> 8) & 0x0FU);
    mac[pos + 3] = sys & 0xFFU;
}

/* stderr goes to the null device: the rows are what is checked. */
static void
quiet_begin(dsd_test_quiet_stderr* cap) {
    if (dsd_test_quiet_stderr_begin(cap) != 0) {
        DSD_FPRINTF(stderr, "quieting stderr failed: %s\n", strerror(errno));
    }
}

static void
quiet_end(dsd_test_quiet_stderr* cap) {
    (void)dsd_test_quiet_stderr_end(cap);
}

/* A Phase 1 TSBK in MAC form: MAC opcode (TSBK opcode ^ 0x40) at MAC[1], TSBK octet n at MAC[n]. */
static void
run_tsbk(fixture* f, unsigned long long int mac[24]) {
    dsd_test_quiet_stderr cap;
    mac[0] = 0x07;
    f->state->synctype = DSD_SYNC_P25P1_POS;
    quiet_begin(&cap);
    process_MAC_VPDU_bridged_p1(f->opts, f->state, mac);
    quiet_end(&cap);
}

/* A native Phase 2 MAC_SIGNAL PDU on a SACCH; @p crc_ok 0 is the PDU -F let through with a failed CRC. */
static void
run_p2_crc(fixture* f, unsigned long long int mac[24], int crc_ok) {
    dsd_test_quiet_stderr cap;
    f->state->synctype = DSD_SYNC_P25P2_POS;
    quiet_begin(&cap);
    process_MAC_VPDU_crc(f->opts, f->state, 1, P25_MAC_PDU_SIGNAL, mac, crc_ok);
    quiet_end(&cap);
}

static void
run_p2(fixture* f, unsigned long long int mac[24]) {
    run_p2_crc(f, mac, 1);
}

/* TSBK 0x2C Unit Registration Response - Abbreviated: RV octet 2 bits 5-4, System ID 2-3, Source ID 4-6, Source
   Address 7-9 (TIA-102.AABC-D §6.2.21.1). */
static void
tsbk_reg_rsp(unsigned long long int mac[24], int rv, uint32_t sys, uint32_t id, uint32_t assigned) {
    DSD_MEMSET(mac, 0, 24 * sizeof mac[0]);
    mac[1] = 0x6C;
    mac[2] = (((unsigned)rv & 3U) << 4) | ((sys >> 8) & 0x0FU);
    mac[3] = sys & 0xFFU;
    put_u24(mac, 4, id);
    put_u24(mac, 7, assigned);
}

/* Phase 2 0xEC Unit Registration Response - Extended (TIA-102.BBAD-A §3.47 Fig 110). */
static void
p2_reg_rsp_ext(unsigned long long int mac[24], int rv, uint32_t wacn, uint32_t sys, uint32_t id, uint32_t assigned) {
    DSD_MEMSET(mac, 0, 24 * sizeof mac[0]);
    mac[1] = 0xEC;
    mac[2] = 0x0D;
    mac[3] = (unsigned)rv & 3U;
    put_wacn_sys(mac, 4, wacn, sys);
    put_u24(mac, 8, id);
    put_u24(mac, 11, assigned);
}

/* TSBK 0x27 / 0x21 Deny / Queued Response (TIA-102.AABC-D §6.2.5 / §6.2.14): AIV + Service Type octet 2, Reason 3,
   Additional Information 4-6, Target 7-9; Phase 2 0x67 / 0x61 the same. */
static void
deny_rsp(unsigned long long int mac[24], int is_deny, int aiv, uint8_t svc, uint8_t reason, uint32_t info,
         uint32_t target) {
    DSD_MEMSET(mac, 0, 24 * sizeof mac[0]);
    mac[1] = is_deny ? 0x67 : 0x61;
    mac[2] = (aiv ? 0x80U : 0U) | (svc & 0x3FU);
    mac[3] = reason;
    put_u24(mac, 4, info);
    put_u24(mac, 7, target);
}

/* ---- Registration ---- */

static int
test_p25_registration_rows(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    /* Abbreviated, accepted: the home ID on the serving WACN, the assigned address beside it. */
    tsbk_reg_rsp(mac, 0, SERVING_SYS, 0x00ABCD, 0x001234);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("0x2C accept", r, DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_ACCEPTED);
    if (r) {
        rc |= expect_id("0x2C subject", &r->subject, 0x00ABCD, DSD_UA_ID_RADIO);
        rc |= expect_home("0x2C home", &r->subject, SERVING_WACN, SERVING_SYS);
        rc |= expect_id("0x2C assigned", &r->assigned, 0x001234, DSD_UA_ID_RADIO);
        rc |= expect_u("0x2C scope kind", r->scope.kind, DSD_UA_SCOPE_P25);
        rc |= expect_u("0x2C scope wacn", r->scope.a, SERVING_WACN);
        rc |= expect_u("0x2C scope sys", r->scope.b, SERVING_SYS);
    }

    /* Refused: no address was assigned. */
    tsbk_reg_rsp(mac, 3, SERVING_SYS, 0x00ABCE, 0x001235);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x2C refused", r, DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_REFUSED);
    if (r) {
        rc |= expect_u("0x2C refused no assigned", r->assigned.valid, 0U);
    }

    /* A unit at home addressed by its own ID: nothing more to say than the home identity. */
    tsbk_reg_rsp(mac, 0, SERVING_SYS, 0x000777, 0x000777);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_true("own ID row", r != NULL && r->subject.id == 0x000777U && !r->assigned.valid);

    /* Extended, a roamer from another system, on a Phase 2 SACCH. */
    f.state->currentslot = 1;
    p2_reg_rsp_ext(mac, 2, OTHER_WACN, OTHER_SYS, 0x00BCDE, 0x002345);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |=
        expect_row("0xEC deny", r, DSD_UA_PROTO_P25P2, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED);
    if (r) {
        rc |= expect_id("0xEC subject", &r->subject, 0x00BCDE, DSD_UA_ID_RADIO);
        rc |= expect_home("0xEC home", &r->subject, OTHER_WACN, OTHER_SYS);
    }
    p2_reg_rsp_ext(mac, 0, OTHER_WACN, OTHER_SYS, 0x00BCDE, 0x002345);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xEC accept", r, DSD_UA_PROTO_P25P2, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_ACCEPTED);
    if (r) {
        rc |= expect_id("0xEC assigned", &r->assigned, 0x002345, DSD_UA_ID_RADIO);
    }
    f.state->currentslot = 0;

    /* Deregistration: the home identity, with the address its registration held (read before it goes). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6F;
    put_wacn_sys(mac, 3, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 7, 0x00BCDE);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x2F dereg", r, DSD_UA_PROTO_P25P1, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_ACK,
                     DSD_UA_RESULT_ACKNOWLEDGED);
    if (r) {
        rc |= expect_id("0x2F subject", &r->subject, 0x00BCDE, DSD_UA_ID_RADIO);
        rc |= expect_home("0x2F home", &r->subject, OTHER_WACN, OTHER_SYS);
        rc |= expect_id("0x2F assigned", &r->assigned, 0x002345, DSD_UA_ID_RADIO);
    }

    /* Location Registration Response 0x6B (length octet 2, RV 3, group 4-5, RFSS 6, site 7, target 8-10). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6B;
    mac[2] = 0x0A;
    mac[3] = 0x01;
    put_u16(mac, 4, 0x0101);
    mac[6] = 1;
    mac[7] = 2;
    put_u24(mac, 8, 0x003456);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x6B fail", r, DSD_UA_PROTO_P25P2, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_FAILED);
    if (r) {
        rc |= expect_id("0x6B subject", &r->subject, 0x003456, DSD_UA_ID_RADIO);
        rc |= expect_true("0x6B group", r->group_valid && r->group == 0x0101U);
    }

    /* Unit Registration Command: native 0x6D (target 2-4) and TSBK 0x2D (target 4-6). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6D;
    put_u24(mac, 2, 0x004567);
    put_u24(mac, 5, 0xFFFFFD);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x6D command", r, DSD_UA_PROTO_P25P2, DSD_UA_REGISTRATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x6D subject", &r->subject, 0x004567, DSD_UA_ID_RADIO);
    }
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6D;
    put_u24(mac, 4, 0x004568);
    put_u24(mac, 7, 0xFFFFFD);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_true("0x2D subject", r != NULL && r->kind == DSD_UA_REGISTRATION && r->role == DSD_UA_ROLE_COMMAND
                                          && r->subject.id == 0x004568U);
    fixture_close(&f);
    return rc;
}

/* ---- Talkgroup joins ---- */

static int
test_p25_affiliation_rows(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    /* TSBK 0x28 (LG/GAV octet 2, AGA 3-4, GA 5-6, target 7-9). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x68;
    mac[2] = 0x00;
    put_u16(mac, 3, 0x0200);
    put_u16(mac, 5, 0x0201);
    put_u24(mac, 7, 0x001111);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("0x28 accept", r, DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_ACCEPTED);
    if (r) {
        rc |= expect_id("0x28 subject", &r->subject, 0x001111, DSD_UA_ID_RADIO);
        rc |= expect_true("0x28 group", r->group_valid && r->group == 0x0201U);
    }

    /* Phase 2 0xE8 (length 2, LG/GAV 3, AGA 4-5, GA 6-7, source group SUID 8-13, target 14-16). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xE8;
    mac[2] = 0x10;
    mac[3] = 0x02;
    put_u16(mac, 4, 0x0300);
    put_u16(mac, 6, 0x0301);
    put_wacn_sys(mac, 8, SERVING_WACN, SERVING_SYS);
    put_u16(mac, 12, 0x0301);
    put_u24(mac, 14, 0x001112);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xE8 deny", r, DSD_UA_PROTO_P25P2, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_DENIED);
    if (r) {
        rc |= expect_id("0xE8 subject", &r->subject, 0x001112, DSD_UA_ID_RADIO);
        rc |= expect_true("0xE8 group", r->group_valid && r->group == 0x0301U);
    }
    fixture_close(&f);
    return rc;
}

/* ---- MBT (alternate multiple block trunking) ---- */

/* An outbound AMBTC of @p opcode with header address @p addr (octets 3-5) and @p blocks data blocks. */
static void
mbt_header(uint8_t* m, uint8_t opcode, uint32_t addr, int blocks) {
    m[0] = 0x37;        /* AN 0, IO 1 (outbound), format %10111 (AMBTC) */
    m[1] = 0xC0 | 0x3D; /* SAP: trunking control */
    m[2] = 0x00;        /* MFID standard */
    m[3] = (uint8_t)(addr >> 16);
    m[4] = (uint8_t)(addr >> 8);
    m[5] = (uint8_t)addr;
    m[6] = 0x80 | (uint8_t)blocks; /* blocks to follow */
    m[7] = opcode;
}

static void
mbt_put_wacn_sys(uint8_t* m, uint32_t wacn, uint32_t sys) {
    m[8] = (uint8_t)(wacn >> 12);
    m[9] = (uint8_t)(wacn >> 4);
    m[12] = (uint8_t)(((wacn & 0x0FU) << 4) | ((sys >> 8) & 0x0FU));
    m[13] = (uint8_t)sys;
}

static void
run_mbt(fixture* f, const uint8_t* m, size_t len) {
    dsd_test_quiet_stderr cap;
    quiet_begin(&cap);
    (void)p25_decode_pdu_trunking(f->opts, f->state, m, len);
    quiet_end(&cap);
}

static int
test_p25_mbt_rows(void) {
    fixture f;
    uint8_t m[36];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    /* Unit Registration Response - Extended (TIA-102.AABC-D §6.2.21.2): assigned address in the header, home SUID and
       RV in the data block. */
    DSD_MEMSET(m, 0, sizeof m);
    mbt_header(m, 0x2C, 0x002222, 1);
    mbt_put_wacn_sys(m, OTHER_WACN, OTHER_SYS);
    m[14] = 0x00;
    m[15] = 0xCD;
    m[16] = 0xEF;
    m[17] = 0x00; /* RV accept */
    run_mbt(&f, m, 24);
    const dsd_unit_activity* r = last_row(&f);
    rc |=
        expect_row("MBT reg", r, DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED);
    if (r) {
        rc |= expect_id("MBT reg subject", &r->subject, 0x00CDEF, DSD_UA_ID_RADIO);
        rc |= expect_home("MBT reg home", &r->subject, OTHER_WACN, OTHER_SYS);
        rc |= expect_id("MBT reg assigned", &r->assigned, 0x002222, DSD_UA_ID_RADIO);
    }

    /* Group Affiliation Response - Extended (TIA-102.AABC-D §6.2.8.2): target in the header, GA octets 18-19, GAV 20. */
    DSD_MEMSET(m, 0, sizeof m);
    mbt_header(m, 0x28, 0x003333, 1);
    mbt_put_wacn_sys(m, SERVING_WACN, SERVING_SYS);
    m[14] = 0x04;
    m[15] = 0x01;
    m[18] = 0x04;
    m[19] = 0x01;
    m[20] = 0x01; /* GAV fail */
    run_mbt(&f, m, 24);
    r = last_row(&f);
    rc |= expect_row("MBT aff", r, DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_FAILED);
    if (r) {
        rc |= expect_id("MBT aff subject", &r->subject, 0x003333, DSD_UA_ID_RADIO);
        rc |= expect_true("MBT aff group", r->group_valid && r->group == 0x0401U);
    }
    fixture_close(&f);
    return rc;
}

/* ---- Deny and queued ---- */

static int
test_p25_deny_queued_rows(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    deny_rsp(mac, 1, 1, 0x00, 0x31, 0x000101, 0x005555);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("0x27 deny", r, DSD_UA_PROTO_P25P1, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED);
    if (r) {
        rc |= expect_id("0x27 subject", &r->subject, 0x005555, DSD_UA_ID_RADIO);
        rc |= expect_true("0x27 service", r->value_valid && r->value == 0x00U);
        rc |= expect_u("0x27 reason set", r->reason_set, DSD_UA_REASON_P25_DENY);
        rc |= expect_u("0x27 reason", r->reason_code, 0x31U);
        rc |= expect_true("0x27 info", r->value2_valid && r->value2 == 0x000101U);
    }

    deny_rsp(mac, 0, 0, 0x04, 0x40, 0x000000, 0x005556);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x61 queued", r, DSD_UA_PROTO_P25P2, DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_QUEUED);
    if (r) {
        rc |= expect_u("0x61 reason set", r->reason_set, DSD_UA_REASON_P25_QUEUED);
        rc |= expect_true("0x61 no info", !r->value2_valid);
        rc |= expect_true("0x61 service", r->value_valid && r->value == 0x04U);
    }

    /* Motorola Phase 2 0xA7 (MFID 0x90, length octet 3): the standard fields two octets later. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xA7;
    mac[2] = 0x90;
    mac[3] = 0x0B;
    mac[4] = 0x00;
    mac[5] = 0x60;
    put_u24(mac, 9, 0x005557);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xA7 deny", r, DSD_UA_PROTO_P25P2, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED);
    if (r) {
        rc |= expect_id("0xA7 subject", &r->subject, 0x005557, DSD_UA_ID_RADIO);
        rc |= expect_u("0xA7 reason", r->reason_code, 0x60U);
    }

    /* Two denies alike but for different groups (the additional information) are two rows (plan G8). */
    const int before = read_rows(&f);
    deny_rsp(mac, 1, 1, 0x00, 0x31, 0x000102, 0x005555);
    run_tsbk(&f, mac);
    rc |= expect_u("deny for another group is its own row", (unsigned)read_rows(&f), (unsigned)before + 1U);
    fixture_close(&f);
    return rc;
}

/* ---- Radio commands ---- */

static void
ext_fnct(unsigned long long int mac[24], uint8_t class_id, uint8_t operand, uint32_t argument, uint32_t target) {
    DSD_MEMSET(mac, 0, 24 * sizeof mac[0]);
    mac[1] = 0x64;
    mac[2] = class_id;
    mac[3] = operand;
    put_u24(mac, 4, argument);
    put_u24(mac, 7, target);
}

static int
test_p25_radio_command_rows(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    /* Inhibit command: the argument is the requester (actor), the target the radio commanded. */
    ext_fnct(mac, 0x00, 0x7F, 0x000042, 0x006666);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("inhibit", r, DSD_UA_PROTO_P25P1, DSD_UA_INHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("inhibit subject", &r->subject, 0x006666, DSD_UA_ID_RADIO);
        rc |= expect_id("inhibit actor", &r->actor, 0x000042, DSD_UA_ID_RADIO);
    }
    /* Inhibit ACK: the argument is the radio that acknowledged, the target the party told. */
    ext_fnct(mac, 0x00, 0xFF, 0x006666, 0x000042);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("inhibit ack", r, DSD_UA_PROTO_P25P2, DSD_UA_INHIBIT, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED);
    if (r) {
        rc |= expect_id("inhibit ack subject", &r->subject, 0x006666, DSD_UA_ID_RADIO);
        rc |= expect_id("inhibit ack actor", &r->actor, 0x000042, DSD_UA_ID_RADIO);
    }
    ext_fnct(mac, 0x00, 0x7E, 0xFFFFFD, 0x006667);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("uninhibit", r, DSD_UA_PROTO_P25P1, DSD_UA_UNINHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("uninhibit actor is the system", &r->actor, 0xFFFFFD, DSD_UA_ID_INFRASTRUCTURE);
    }
    ext_fnct(mac, 0x00, 0x00, 0x000000, 0x006668);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("radio check", r, DSD_UA_PROTO_P25P1, DSD_UA_RADIO_CHECK, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_u("radio check null argument", r->actor.valid, 0U);
    }
    ext_fnct(mac, 0x00, 0x7D, 0x000042, 0x006669);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("detach", r, DSD_UA_PROTO_P25P1, DSD_UA_DETACH, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);

    /* A reserved operand and another class add no row. */
    int before = read_rows(&f);
    ext_fnct(mac, 0x00, 0x10, 0x000042, 0x006670);
    run_tsbk(&f, mac);
    ext_fnct(mac, 0x02, 0x00, 0x000101, 0x006670);
    run_tsbk(&f, mac);
    rc |= expect_u("reserved operand / class 2 no row", (unsigned)read_rows(&f), (unsigned)before);

    /* Phase 2 0xE4 Extended VCH (length 2, class 3, operand 4, argument 5-7, target 8-10, source SUID 11-17). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xE4;
    mac[2] = 0x11;
    mac[3] = 0x00;
    mac[4] = 0x7F;
    put_u24(mac, 5, 0x000043);
    put_u24(mac, 8, 0x006671);
    put_wacn_sys(mac, 11, SERVING_WACN, SERVING_SYS);
    put_u24(mac, 15, 0x000043);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xE4 inhibit", r, DSD_UA_PROTO_P25P2, DSD_UA_INHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0xE4 subject", &r->subject, 0x006671, DSD_UA_ID_RADIO);
        rc |= expect_home("0xE4 command requester home", &r->actor, SERVING_WACN, SERVING_SYS);
    }
    /* The ACK form: the argument is the radio acknowledging, which the Source SUID does not name. */
    mac[4] = 0xFF;
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xE4 inhibit ack", r, DSD_UA_PROTO_P25P2, DSD_UA_INHIBIT, DSD_UA_ROLE_ACK,
                     DSD_UA_RESULT_ACKNOWLEDGED);
    if (r) {
        rc |= expect_id("0xE4 ack subject", &r->subject, 0x000043, DSD_UA_ID_RADIO);
        rc |= expect_u("0xE4 ack subject has no home", r->subject.home_valid, 0U);
    }

    /* Motorola 0x84 (MFID 0x90, length 3, function 4-5, argument 6-8, target 9-11), class $00. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x84;
    mac[2] = 0x90;
    mac[3] = 0x0B;
    mac[4] = 0x00;
    mac[5] = 0xFE;
    put_u24(mac, 6, 0x006672);
    put_u24(mac, 9, 0x000044);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x84 uninhibit ack", r, DSD_UA_PROTO_P25P2, DSD_UA_UNINHIBIT, DSD_UA_ROLE_ACK,
                     DSD_UA_RESULT_ACKNOWLEDGED);
    if (r) {
        rc |= expect_id("0x84 subject", &r->subject, 0x006672, DSD_UA_ID_RADIO);
    }

    /* Radio Unit Monitor: TSBK 0x1D (source 4-6, target 7-9), Phase 2 0x4C (target 5-7, source 8-10), 0x5E for a
       talkgroup. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x5D;
    mac[2] = 0x05;
    mac[3] = 0x01;
    put_u24(mac, 4, 0x000045);
    put_u24(mac, 7, 0x006673);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |=
        expect_row("0x1D monitor", r, DSD_UA_PROTO_P25P1, DSD_UA_RADIO_MONITOR, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x1D subject", &r->subject, 0x006673, DSD_UA_ID_RADIO);
        rc |= expect_id("0x1D actor", &r->actor, 0x000045, DSD_UA_ID_RADIO);
    }
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x4C;
    mac[2] = 0x0A;
    mac[3] = 0x05;
    mac[4] = 0x01;
    put_u24(mac, 5, 0x006674);
    put_u24(mac, 8, 0x000046);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_true("0x4C monitor", r != NULL && r->kind == DSD_UA_RADIO_MONITOR && r->subject.id == 0x006674U
                                          && r->actor.id == 0x000046U);
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x5E;
    put_u24(mac, 2, 0x006675);
    put_u16(mac, 5, 0x0501);
    put_u24(mac, 7, 0x000047);
    mac[10] = 0x40; /* talkgroup mode */
    run_p2(&f, mac);
    r = last_row(&f);
    rc |=
        expect_true("0x5E talkgroup monitor", r != NULL && r->kind == DSD_UA_RADIO_MONITOR && r->subject.id == 0x006675U
                                                  && r->group_valid && r->group == 0x0501U && !r->actor.valid);

    /* Authentication Command TSBK 0x2E: the target SUID (WACN/SYS octets 3-6, target 7-9). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6E;
    put_wacn_sys(mac, 3, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 7, 0x006676);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x2E auth", r, DSD_UA_PROTO_P25P1, DSD_UA_AUTHENTICATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x2E subject", &r->subject, 0x006676, DSD_UA_ID_RADIO);
        rc |= expect_home("0x2E home", &r->subject, OTHER_WACN, OTHER_SYS);
    }
    fixture_close(&f);
    return rc;
}

/* ---- Status, message, call alert, answer requests ---- */

static int
test_p25_report_rows(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    /* Status Update TSBK 0x18: the reporting source is the subject (plan G13), the target the recipient. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x58;
    mac[2] = 0xA1;
    mac[3] = 0x0B;
    put_u24(mac, 4, 0x007001);
    put_u24(mac, 7, 0x007002);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("0x18 status", r, DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x18 subject is the source", &r->subject, 0x007002, DSD_UA_ID_RADIO);
        rc |= expect_id("0x18 recipient", &r->recipient, 0x007001, DSD_UA_ID_RADIO);
        rc |= expect_true("0x18 value", r->value_valid && r->value == 0xA10BU);
    }
    /* Another status value is another row (plan G8). */
    int before = read_rows(&f);
    mac[3] = 0x0C;
    run_tsbk(&f, mac);
    rc |= expect_u("other status value, other row", (unsigned)read_rows(&f), (unsigned)before + 1U);

    /* Phase 2 0xD8 Status Update - Extended VCH: the source SUID's home. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xD8;
    mac[2] = 0x0E;
    mac[3] = 0x01;
    mac[4] = 0x02;
    put_u24(mac, 5, 0x007003);
    put_wacn_sys(mac, 8, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 12, 0x007004);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xD8 status", r, DSD_UA_PROTO_P25P2, DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0xD8 subject", &r->subject, 0x007004, DSD_UA_ID_RADIO);
        rc |= expect_home("0xD8 home", &r->subject, OTHER_WACN, OTHER_SYS);
        rc |= expect_true("0xD8 value", r->value == 0x0102U);
    }

    /* Status Query TSBK 0x1A (target 4-6, source 7-9): a command to the queried target. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x5A;
    put_u24(mac, 4, 0x007005);
    put_u24(mac, 7, 0x007006);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x1A query", r, DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x1A subject", &r->subject, 0x007005, DSD_UA_ID_RADIO);
        rc |= expect_id("0x1A actor", &r->actor, 0x007006, DSD_UA_ID_RADIO);
    }

    /* Message Update TSBK 0x1C (message 2-3, target 4-6, source 7-9). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x5C;
    put_u16(mac, 2, 0x1234);
    put_u24(mac, 4, 0x007007);
    put_u24(mac, 7, 0x007008);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x1C message", r, DSD_UA_PROTO_P25P1, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x1C subject", &r->subject, 0x007008, DSD_UA_ID_RADIO);
        rc |= expect_id("0x1C recipient", &r->recipient, 0x007007, DSD_UA_ID_RADIO);
        rc |= expect_true("0x1C value", r->value_valid && r->value == 0x1234U);
    }

    /* Call Alert TSBK 0x1F (target 4-6, source 7-9). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x5F;
    put_u24(mac, 4, 0x007009);
    put_u24(mac, 7, 0x00700A);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x1F alert", r, DSD_UA_PROTO_P25P1, DSD_UA_CALL_ALERT, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x1F subject", &r->subject, 0x007009, DSD_UA_ID_RADIO);
        rc |= expect_id("0x1F actor", &r->actor, 0x00700A, DSD_UA_ID_RADIO);
    }

    /* Unit-to-Unit Answer Request TSBK 0x05 (service 2, target 4-6, source 7-9). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x45;
    mac[2] = 0x40;
    put_u24(mac, 4, 0x00700B);
    put_u24(mac, 7, 0x00700C);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x05 answer", r, DSD_UA_PROTO_P25P1, DSD_UA_ANSWER_REQUEST, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x05 subject", &r->subject, 0x00700B, DSD_UA_ID_RADIO);
        rc |= expect_id("0x05 actor", &r->actor, 0x00700C, DSD_UA_ID_RADIO);
        rc |= expect_true("0x05 service", r->value_valid && r->value == 0x40U);
    }

    /* Telephone Interconnect Answer Request TSBK 0x0A (digits 2-6, target 7-9): no caller unit. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x4A;
    mac[2] = 0x55;
    put_u24(mac, 7, 0x00700D);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0x0A answer", r, DSD_UA_PROTO_P25P1, DSD_UA_ANSWER_REQUEST, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0x0A subject", &r->subject, 0x00700D, DSD_UA_ID_RADIO);
        rc |= expect_true("0x0A no caller", !r->actor.valid && !r->value_valid);
    }

    /* Link control Message Update 0x55 (TIA-102.AABF-D §7.3.19: message octets 1-2, target 3-5, source 6-8). */
    uint8_t lc[9] = {0x55, 0x43, 0x21, 0x00, 0x70, 0x0E, 0x00, 0x70, 0x0F};
    uint8_t bits[72];
    for (int i = 0; i < 72; i++) {
        bits[i] = (uint8_t)((lc[i / 8] >> (7 - (i % 8))) & 1U);
    }
    dsd_test_quiet_stderr cap;
    quiet_begin(&cap);
    p25_lcw(f.opts, f.state, bits, 0);
    quiet_end(&cap);
    r = last_row(&f);
    rc |= expect_row("LC 0x55 message", r, DSD_UA_PROTO_P25P1, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("LC 0x55 subject", &r->subject, 0x00700F, DSD_UA_ID_RADIO);
        rc |= expect_id("LC 0x55 recipient", &r->recipient, 0x00700E, DSD_UA_ID_RADIO);
        rc |= expect_true("LC 0x55 value", r->value == 0x4321U);
    }

    /* A Group Affiliation Query is no row (the plan's table has none). */
    before = read_rows(&f);
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6A;
    put_u24(mac, 4, 0x007010);
    put_u24(mac, 7, 0x007011);
    run_tsbk(&f, mac);
    rc |= expect_u("GA query no row", (unsigned)read_rows(&f), (unsigned)before);
    fixture_close(&f);
    return rc;
}

/* ---- Provenance ---- */

/* A Phase 2 MAC PDU -F let through with a failed CRC changes no table and makes no row, whatever it says. */
static int
test_p25_relaxed_crc_no_row(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    p2_reg_rsp_ext(mac, 0, OTHER_WACN, OTHER_SYS, 0x00BCDE, 0x002345);
    run_p2_crc(&f, mac, 0);
    deny_rsp(mac, 1, 0, 0x00, 0x31, 0, 0x005555);
    run_p2_crc(&f, mac, 0);
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6F;
    put_wacn_sys(mac, 3, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 7, 0x00BCDE);
    run_p2_crc(&f, mac, 0);
    ext_fnct(mac, 0x00, 0x7F, 0x000042, 0x006666);
    run_p2_crc(&f, mac, 0);
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x58;
    mac[2] = 0x0A;
    mac[3] = 0xA1;
    put_u24(mac, 5, 0x007001);
    put_u24(mac, 8, 0x007002);
    run_p2_crc(&f, mac, 0);
    rc |= expect_u("failed-CRC PDUs make no row", (unsigned)read_rows(&f), 0U);

    /* The same registration with its CRC passed does. */
    p2_reg_rsp_ext(mac, 0, OTHER_WACN, OTHER_SYS, 0x00BCDE, 0x002345);
    run_p2_crc(&f, mac, 1);
    rc |= expect_u("checked PDU makes a row", (unsigned)read_rows(&f), 1U);
    fixture_close(&f);
    return rc;
}

/* A busy control channel repeats the same registration every few hundred ms: one row with a repeat count. A changed
   assignment is another row (plan G8). */
static int
test_p25_repeat_tsbk_one_row(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    dsd_decode_clock_use_test((uint64_t)T0_MS * UINT64_C(1000000));
    for (int i = 0; i < 5; i++) {
        dsd_decode_clock_test_set_ns((uint64_t)(T0_MS + (i * 300)) * UINT64_C(1000000));
        tsbk_reg_rsp(mac, 0, SERVING_SYS, 0x00ABCD, 0x001234);
        run_tsbk(&f, mac);
    }
    rc |= expect_u("five repeats, one row", (unsigned)read_rows(&f), 1U);
    rc |= expect_u("repeat count", f.count > 0 ? f.rows[0].repeat : 0U, 5U);
    tsbk_reg_rsp(mac, 0, SERVING_SYS, 0x00ABCD, 0x001299);
    run_tsbk(&f, mac);
    rc |= expect_u("changed assignment, new row", (unsigned)read_rows(&f), 2U);
    dsd_decode_clock_use_system();
    fixture_close(&f);
    return rc;
}

/* ---- Names ---- */

/* name_frozen: the scoped name for the current system, through the assigned address first; a roamer's home ID is
   another system's number and never names it here. */
static int
test_p25_names_at_push(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    (void)dsd_alias_learn_radio(NULL, f.state, NULL, 0x001234, "Alice", NULL);
    (void)dsd_alias_learn_radio(NULL, f.state, NULL, 0x00BCDE, "Bob", NULL);
    tsbk_reg_rsp(mac, 0, SERVING_SYS, 0x00ABCD, 0x001234);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_true("assigned names the radio", r != NULL && strcmp(r->name_frozen, "Alice") == 0);

    p2_reg_rsp_ext(mac, 0, OTHER_WACN, OTHER_SYS, 0x00BCDE, 0x002345);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_true("roamer's home ID is not named", r != NULL && r->name_frozen[0] == '\0');

    deny_rsp(mac, 1, 0, 0x00, 0x31, 0, 0x00BCDE);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_true("local ID named", r != NULL && strcmp(r->name_frozen, "Bob") == 0);

    /* A home unit whose assigned address has no name is named by its own ID, as the view names a live row. */
    (void)dsd_alias_learn_radio(NULL, f.state, NULL, 0x00ABCF, "Carol", NULL);
    tsbk_reg_rsp(mac, 0, SERVING_SYS, 0x00ABCF, 0x001299);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_true("unnamed assigned falls back to the home ID",
                      r != NULL && r->assigned.valid && strcmp(r->name_frozen, "Carol") == 0);
    fixture_close(&f);
    return rc;
}

/* ---- Fix round 1 ---- */

static void
tsbk_dereg(unsigned long long int mac[24], uint8_t opcode, uint32_t wacn, uint32_t sys, uint32_t id) {
    DSD_MEMSET(mac, 0, 24 * sizeof mac[0]);
    mac[1] = opcode;
    put_wacn_sys(mac, 3, wacn, sys);
    put_u24(mac, 7, id);
}

/* A busy control channel repeats a deregistration acknowledgement: the first removes the registration it reads the
   assigned address from, and the repeats still merge into its row, which keeps that address (plan G8). */
static int
test_p25_repeat_dereg_one_row(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    dsd_decode_clock_use_test((uint64_t)T0_MS * UINT64_C(1000000));
    p2_reg_rsp_ext(mac, 0, OTHER_WACN, OTHER_SYS, 0x00BCDE, 0x002345);
    run_p2(&f, mac);
    for (int i = 0; i < 3; i++) {
        dsd_decode_clock_test_set_ns((uint64_t)(T0_MS + 100 + (i * 400)) * UINT64_C(1000000));
        tsbk_dereg(mac, 0x6F, OTHER_WACN, OTHER_SYS, 0x00BCDE);
        run_tsbk(&f, mac);
    }
    rc |= expect_u("registration + one dereg row", (unsigned)read_rows(&f), 2U);
    const dsd_unit_activity* r = f.count == 2 ? &f.rows[1] : NULL;
    rc |= expect_row("dereg row", r, DSD_UA_PROTO_P25P1, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_ACK,
                     DSD_UA_RESULT_ACKNOWLEDGED);
    if (r) {
        rc |= expect_u("dereg repeat", r->repeat, 3U);
        rc |= expect_id("dereg keeps the assigned address", &r->assigned, 0x002345, DSD_UA_ID_RADIO);
    }
    /* Phase 2 0xEF Extended form: the same fields. */
    tsbk_dereg(mac, 0xEF, SERVING_WACN, SERVING_SYS, 0x00BCDF);
    mac[0] = 0x00;
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xEF dereg", r, DSD_UA_PROTO_P25P2, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_ACK,
                     DSD_UA_RESULT_ACKNOWLEDGED);
    if (r) {
        rc |= expect_id("0xEF subject", &r->subject, 0x00BCDF, DSD_UA_ID_RADIO);
        rc |= expect_u("0xEF no assigned", r->assigned.valid, 0U);
    }
    dsd_decode_clock_use_system();
    fixture_close(&f);
    return rc;
}

/* Native 0x6C (length octet 2, RV octet 3 bits 5-4, System ID 3-4, Source ID 5-7, Source Address 8-10), with and
   without the site's WACN. */
static int
test_p25_native_0x6c_rows(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6C;
    mac[2] = 0x0A;
    mac[3] = (SERVING_SYS >> 8) & 0x0FU;
    mac[4] = SERVING_SYS & 0xFFU;
    put_u24(mac, 5, 0x00ABCD);
    put_u24(mac, 8, 0x001234);
    run_p2(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("0x6C accept", r, DSD_UA_PROTO_P25P2, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_ACCEPTED);
    if (r) {
        rc |= expect_home("0x6C home", &r->subject, SERVING_WACN, SERVING_SYS);
        rc |= expect_id("0x6C assigned", &r->assigned, 0x001234, DSD_UA_ID_RADIO);
    }

    /* Before the site's WACN is known the subject has no home, and an own-ID accept on this System ID adds no
       assigned identity. */
    f.state->p2_wacn = 0;
    put_u24(mac, 8, 0x00ABCD);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_true("0x6C no WACN: no home", r != NULL && r->subject.valid && !r->subject.home_valid);
    rc |= expect_true("0x6C no WACN: own ID not assigned", r != NULL && !r->assigned.valid);
    fixture_close(&f);
    return rc;
}

static int
test_p25_more_forms(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    /* TSBK 0x2B Location Registration Response (RV 2, group 3-4, RFSS 5, site 6, target 7-9). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x6B;
    mac[2] = 0x00;
    put_u16(mac, 3, 0x0102);
    put_u24(mac, 7, 0x003457);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("0x2B accept", r, DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE,
                     DSD_UA_RESULT_ACCEPTED);
    rc |= expect_true("0x2B fields", r != NULL && r->subject.id == 0x003457U && r->group == 0x0102U);

    /* Motorola 0xA6 queued. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xA6;
    mac[2] = 0x90;
    mac[3] = 0x0B;
    mac[4] = 0x00;
    mac[5] = 0x40;
    put_u24(mac, 9, 0x005558);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xA6 queued", r, DSD_UA_PROTO_P25P2, DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_QUEUED);

    /* 0xC5 Unit-to-Unit Answer Request - Extended (service 2, target 3-5, source SUID 6-12). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xC5;
    mac[2] = 0x40;
    put_u24(mac, 3, 0x007101);
    put_wacn_sys(mac, 6, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 10, 0x007102);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xC5 answer", r, DSD_UA_PROTO_P25P2, DSD_UA_ANSWER_REQUEST, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0xC5 caller", &r->actor, 0x007102, DSD_UA_ID_RADIO);
        rc |= expect_home("0xC5 caller home", &r->actor, OTHER_WACN, OTHER_SYS);
    }

    /* 0xDA Status Query / 0xDF Call Alert - Extended VCH (target 2-4, source SUID 5-11). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xDA;
    put_u24(mac, 2, 0x007103);
    put_wacn_sys(mac, 5, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 9, 0x007104);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xDA query", r, DSD_UA_PROTO_P25P2, DSD_UA_STATUS, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0xDA subject", &r->subject, 0x007103, DSD_UA_ID_RADIO);
        rc |= expect_home("0xDA actor home", &r->actor, OTHER_WACN, OTHER_SYS);
    }
    mac[1] = 0xDF;
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xDF alert", r, DSD_UA_PROTO_P25P2, DSD_UA_CALL_ALERT, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);

    /* 0xDC Message Update - Extended VCH (length 2, message 3-4, target 5-7, source SUID 8-14). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xDC;
    mac[2] = 0x0E;
    put_u16(mac, 3, 0x0BAD);
    put_u24(mac, 5, 0x007105);
    put_wacn_sys(mac, 8, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 12, 0x007106);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xDC message", r, DSD_UA_PROTO_P25P2, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0xDC subject", &r->subject, 0x007106, DSD_UA_ID_RADIO);
        rc |= expect_home("0xDC home", &r->subject, OTHER_WACN, OTHER_SYS);
        rc |= expect_u("0xDC value", r->value, 0x0BADU);
    }

    /* 0xCC Radio Unit Monitor - Extended VCH (length 2, time 3, SM/mult 4, target 5-7, source SUID 8-14). */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xCC;
    mac[2] = 0x0E;
    put_u24(mac, 5, 0x007107);
    put_wacn_sys(mac, 8, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 12, 0x007108);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |=
        expect_row("0xCC monitor", r, DSD_UA_PROTO_P25P2, DSD_UA_RADIO_MONITOR, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0xCC actor", &r->actor, 0x007108, DSD_UA_ID_RADIO);
        rc |= expect_home("0xCC actor home", &r->actor, OTHER_WACN, OTHER_SYS);
    }

    /* 0xE5 Extended Function - Extended LCCH (length 2, class 3, operand 4, argument 5-7, target 8-10, source WACN /
       System ID 11-14): the argument's home. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xE5;
    mac[2] = 0x0E;
    mac[3] = 0x00;
    mac[4] = 0x7F;
    put_u24(mac, 5, 0x007109);
    put_u24(mac, 8, 0x00710A);
    put_wacn_sys(mac, 11, OTHER_WACN, OTHER_SYS);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_row("0xE5 inhibit", r, DSD_UA_PROTO_P25P2, DSD_UA_INHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("0xE5 subject", &r->subject, 0x00710A, DSD_UA_ID_RADIO);
        rc |= expect_id("0xE5 actor", &r->actor, 0x007109, DSD_UA_ID_RADIO);
        rc |= expect_u("0xE5 actor has no home (no Source ID ties it)", r->actor.home_valid, 0U);
    }

    /* 0x5E Radio Unit Monitor Enhanced in source mode: the source is the actor. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0x5E;
    put_u24(mac, 2, 0x00710B);
    put_u16(mac, 5, 0x0501);
    put_u24(mac, 7, 0x00710C);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_true("0x5E source mode", r != NULL && r->kind == DSD_UA_RADIO_MONITOR && !r->group_valid
                                              && r->actor.valid && r->actor.id == 0x00710CU);

    /* LC 0x5B Message Update - Source ID Extension Required (same fields as 0x55). */
    uint8_t lc[9] = {0x5B, 0x00, 0x42, 0x00, 0x71, 0x0D, 0x00, 0x71, 0x0E};
    uint8_t bits[72];
    for (int i = 0; i < 72; i++) {
        bits[i] = (uint8_t)((lc[i / 8] >> (7 - (i % 8))) & 1U);
    }
    dsd_test_quiet_stderr cap;
    quiet_begin(&cap);
    p25_lcw(f.opts, f.state, bits, 0);
    quiet_end(&cap);
    r = last_row(&f);
    rc |= expect_row("LC 0x5B message", r, DSD_UA_PROTO_P25P1, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    rc |= expect_true("LC 0x5B fields",
                      r != NULL && r->subject.id == 0x00710EU && r->recipient.id == 0x00710DU && r->value == 0x0042U);
    fixture_close(&f);
    return rc;
}

/* The extended MBT Status Update, Status Query, Message Update and Call Alert (TIA-102.AABC-D Figs 6.2.18-2,
   6.2.17-2, 6.2.10-2, 6.2.4-2): two data blocks. */
static void
mbt_command(uint8_t* m, uint8_t opcode, uint16_t value) {
    DSD_MEMSET(m, 0, 36);
    mbt_header(m, opcode, 0x000201, 2); /* Target Address (WUID) */
    mbt_put_wacn_sys(m, OTHER_WACN, OTHER_SYS);
    m[14] = 0x00; /* Source ID 0x000301 */
    m[15] = 0x03;
    m[16] = 0x01;
    m[17] = (uint8_t)(value >> 8);
    m[18] = (uint8_t)value;
    m[22] = 0x00; /* Source Address (WUID) 0x000302 */
    m[23] = 0x03;
    m[24] = 0x02;
    m[25] = (uint8_t)(SERVING_WACN >> 12); /* Target SUID */
    m[26] = (uint8_t)((SERVING_WACN >> 4) & 0xFFU);
    m[27] = (uint8_t)(((SERVING_WACN & 0x0FU) << 4) | ((SERVING_SYS >> 8) & 0x0FU));
    m[28] = (uint8_t)(SERVING_SYS & 0xFFU);
    m[29] = 0x00; /* Target ID 0x000202 */
    m[30] = 0x02;
    m[31] = 0x02;
}

static int
test_p25_mbt_report_rows(void) {
    fixture f;
    uint8_t m[36];
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    mbt_command(m, 0x18, 0xA10B);
    run_mbt(&f, m, 36);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("MBT status", r, DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("MBT status subject", &r->subject, 0x000301, DSD_UA_ID_RADIO);
        rc |= expect_home("MBT status home", &r->subject, OTHER_WACN, OTHER_SYS);
        rc |= expect_id("MBT status assigned", &r->assigned, 0x000302, DSD_UA_ID_RADIO);
        rc |= expect_id("MBT status recipient", &r->recipient, 0x000202, DSD_UA_ID_RADIO);
        rc |= expect_u("MBT status value", r->value, 0xA10BU);
    }
    mbt_command(m, 0x1A, 0);
    run_mbt(&f, m, 36);
    r = last_row(&f);
    rc |= expect_row("MBT query", r, DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_id("MBT query subject", &r->subject, 0x000202, DSD_UA_ID_RADIO);
        rc |= expect_id("MBT query assigned", &r->assigned, 0x000201, DSD_UA_ID_RADIO);
        rc |= expect_id("MBT query actor", &r->actor, 0x000301, DSD_UA_ID_RADIO);
    }
    mbt_command(m, 0x1C, 0x1234);
    run_mbt(&f, m, 36);
    r = last_row(&f);
    rc |= expect_row("MBT message", r, DSD_UA_PROTO_P25P1, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    rc |= expect_true("MBT message value", r != NULL && r->value == 0x1234U);
    mbt_command(m, 0x1F, 0);
    run_mbt(&f, m, 36);
    r = last_row(&f);
    rc |= expect_row("MBT alert", r, DSD_UA_PROTO_P25P1, DSD_UA_CALL_ALERT, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    fixture_close(&f);
    return rc;
}

/* ---- Final review fixes: names that belong to another system ---- */

/* The name every frontend shows for the newest row (the app-control view over the live ring). */
static void
view_newest_name(const fixture* f, char* out, size_t cap) {
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_meta meta;
    dsd_app_unit_activity_row rows[MAX_ROWS];
    const int n = dsd_app_unit_activity_rows(f->state, &cursor, rows, MAX_ROWS, &meta);
    uint64_t newest = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (rows[i].seq > newest) {
            newest = rows[i].seq;
            DSD_SNPRINTF(out, cap, "%s", rows[i].subject_name);
        }
    }
}

/* A link control word (9 octets) through p25_lcw(). */
static void
run_lcw(fixture* f, const uint8_t lc[9]) {
    uint8_t bits[72];
    for (int i = 0; i < 72; i++) {
        bits[i] = (uint8_t)((lc[i / 8] >> (7 - (i % 8))) & 1U);
    }
    dsd_test_quiet_stderr cap;
    quiet_begin(&cap);
    p25_lcw(f->opts, f->state, bits, 0);
    quiet_end(&cap);
}

/* LC 0x5B Message Update - Source ID Extension Required (TIA-102.AABF-D §7.3.31): its Source ID is qualified by the
   Source ID Extension that follows, so it is not a local radio's address. A local radio with that number never names
   it, at push or in the view; the same source in a plain 0x55 is a local radio and is named. */
static int
test_p25_lc_source_awaiting_extension_not_named(void) {
    fixture f;
    int rc = 0;
    char name[64];
    if (fixture_open(&f) != 0) {
        return 1;
    }
    (void)dsd_alias_learn_radio(NULL, f.state, NULL, 0x00710E, "Engine 7", NULL);
    const uint8_t lc5b[9] = {0x5B, 0x00, 0x42, 0x00, 0x71, 0x0D, 0x00, 0x71, 0x0E};
    run_lcw(&f, lc5b);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_true("0x5B row", r != NULL && r->subject.id == 0x00710EU);
    rc |= expect_true("0x5B source not named at push", r != NULL && r->name_frozen[0] == '\0');
    view_newest_name(&f, name, sizeof name);
    rc |= expect_true("0x5B source not named in the view", name[0] == '\0');

    const uint8_t lc55[9] = {0x55, 0x00, 0x43, 0x00, 0x71, 0x0D, 0x00, 0x71, 0x0E};
    run_lcw(&f, lc55);
    r = last_row(&f);
    rc |= expect_true("0x55 source named at push", r != NULL && strcmp(r->name_frozen, "Engine 7") == 0);
    view_newest_name(&f, name, sizeof name);
    rc |= expect_true("0x55 source named in the view", strcmp(name, "Engine 7") == 0);
    fixture_close(&f);
    return rc;
}

/* A visitor whose home SUID already holds local address 100 on the serving system sends a native Phase 2 report
   naming only its home SUID: the row takes address 100 as its assigned identity (plan G5), and the radio is named by
   it, at push and in the view. */
static int
test_p25_visitor_report_named_by_serving_assignment(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    char name[64];
    if (fixture_open(&f) != 0) {
        return 1;
    }
    p2_reg_rsp_ext(mac, 0, OTHER_WACN, OTHER_SYS, 0x00BCDE, 100U);
    run_p2(&f, mac);
    (void)dsd_alias_learn_radio(NULL, f.state, NULL, 100U, "VISITOR", NULL);
    (void)dsd_alias_learn_radio(NULL, f.state, NULL, 0x00BCDE, "Local namesake", NULL);

    /* 0xDC Message Update - Extended VCH: message 3-4, target 5-7, source WACN / System ID 8-11, source ID 12-14. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xDC;
    mac[2] = 0x0E;
    put_u16(mac, 3, 0x0042);
    put_u24(mac, 5, 0x000202);
    put_wacn_sys(mac, 8, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 12, 0x00BCDE);
    run_p2(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_row("0xDC visitor", r, DSD_UA_PROTO_P25P2, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    if (r) {
        rc |= expect_home("0xDC visitor home", &r->subject, OTHER_WACN, OTHER_SYS);
        rc |= expect_id("0xDC visitor assigned", &r->assigned, 100U, DSD_UA_ID_RADIO);
        rc |= expect_true("0xDC visitor named by its assigned address", strcmp(r->name_frozen, "VISITOR") == 0);
    }
    view_newest_name(&f, name, sizeof name);
    rc |= expect_true("the view names the visitor by its assigned address", strcmp(name, "VISITOR") == 0);

    /* 0xD8 Status Update - Extended VCH: the same rule. */
    DSD_MEMSET(mac, 0, sizeof mac);
    mac[1] = 0xD8;
    mac[2] = 0x0E;
    mac[3] = 0x01;
    mac[4] = 0x02;
    put_u24(mac, 5, 0x000202);
    put_wacn_sys(mac, 8, OTHER_WACN, OTHER_SYS);
    put_u24(mac, 12, 0x00BCDE);
    run_p2(&f, mac);
    r = last_row(&f);
    rc |= expect_true("0xD8 visitor named by its assigned address", r != NULL && r->kind == DSD_UA_STATUS
                                                                        && r->assigned.valid && r->assigned.id == 100U
                                                                        && strcmp(r->name_frozen, "VISITOR") == 0);
    fixture_close(&f);
    return rc;
}

/* An abbreviated registration response of a sibling system's unit (System ID other than the serving one) before the
   site's WACN is known: its Source ID is that system's number, not a local radio's, so a local radio with the same
   number never names it. */
static int
test_p25_sibling_unit_before_wacn_not_named(void) {
    fixture f;
    unsigned long long int mac[24];
    int rc = 0;
    char name[64];
    if (fixture_open(&f) != 0) {
        return 1;
    }
    (void)dsd_alias_learn_radio(NULL, f.state, NULL, 0x00ABCF, "Carol", NULL);
    f.state->p2_wacn = 0;
    tsbk_reg_rsp(mac, 0, OTHER_SYS, 0x00ABCF, 0x001299);
    run_tsbk(&f, mac);
    const dsd_unit_activity* r = last_row(&f);
    rc |= expect_true("sibling row", r != NULL && r->subject.id == 0x00ABCFU && !r->subject.home_valid
                                         && r->assigned.valid && r->assigned.id == 0x001299U);
    rc |= expect_true("sibling unit not named at push", r != NULL && r->name_frozen[0] == '\0');
    view_newest_name(&f, name, sizeof name);
    rc |= expect_true("sibling unit not named in the view", name[0] == '\0');

    /* A unit of the serving System ID is still named by its own ID. */
    tsbk_reg_rsp(mac, 0, SERVING_SYS, 0x00ABCF, 0x001299);
    run_tsbk(&f, mac);
    r = last_row(&f);
    rc |= expect_true("serving-system unit named", r != NULL && strcmp(r->name_frozen, "Carol") == 0);
    fixture_close(&f);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_p25_registration_rows();
    rc |= test_p25_affiliation_rows();
    rc |= test_p25_mbt_rows();
    rc |= test_p25_deny_queued_rows();
    rc |= test_p25_radio_command_rows();
    rc |= test_p25_report_rows();
    rc |= test_p25_relaxed_crc_no_row();
    rc |= test_p25_repeat_tsbk_one_row();
    rc |= test_p25_names_at_push();
    rc |= test_p25_repeat_dereg_one_row();
    rc |= test_p25_native_0x6c_rows();
    rc |= test_p25_more_forms();
    rc |= test_p25_mbt_report_rows();
    rc |= test_p25_lc_source_awaiting_extension_not_named();
    rc |= test_p25_visitor_report_named_by_serving_assignment();
    rc |= test_p25_sibling_unit_before_wacn_not_named();
    if (rc == 0) {
        printf("P25_UNIT_ACTIVITY: OK\n");
    }
    return rc;
}
