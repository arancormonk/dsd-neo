// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA MLE routing and the downlink CMCE, SDS and SNDCP decoders (EN 300 392-2 clauses 14, 18, 28 and 29).
 *
 * Every parser reads its PDU through a tetra_bit_reader into a local snapshot and publishes the snapshot to the TETRA
 * state only when every declared element fitted, so a truncated or malformed PDU leaves the last good state in place.
 * CMCE PDUs follow the Annex E.1.1 encoding (type-1 elements, an O-bit, one P-bit per type-2 element, then an M-bit
 * chain of length-delimited type-3/4 elements); MLE PDUs follow Annex E.2.1 (no type-3/4 elements).
 */

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tetra_calls.h"
#include "tetra_log.h"
#include "tetra_mle_pdu.h"
#include "tetra_state.h"

/* What every parser needs besides its bit reader. */
typedef struct {
    dsd_opts* opts; /* may be NULL */
    dsd_state* state;
    tetra_state_t* ts;
    tetra_mac_sdu* sdu;  /* the MAC PDU the TM-SDU came in; NULL without one */
    const uint8_t* bits; /* the whole TM-SDU, from its protocol discriminator */
    int cc;
    int nbits; /* bits in the whole TM-SDU, for the truncation notes */
} mle_ctx;

/* Mark the reader failed: a malformed element is handled like a truncated one, so nothing is published. */
static void
bits_reject(tetra_bit_reader* r) {
    r->failed = 1;
}

/* Annex E.1.1 / E.2.1: type-2 elements in table order, each preceded by its P-bit. values[i] receives element i (0
 * when absent); bit i of the result is set when it was present. */
static unsigned
read_type2_elements(tetra_bit_reader* r, const uint8_t* widths, unsigned count, uint32_t* values) {
    unsigned present = 0;
    for (unsigned i = 0; i < count; i++) {
        values[i] = 0;
        if (tetra_bits_present(r)) {
            values[i] = tetra_bits_read(r, widths[i]);
            present |= 1u << i;
        }
    }
    return present;
}

/* 1 when bit @p index of a read_type2_elements() result is set. */
static uint8_t
type2_present(unsigned present, unsigned index) {
    return (uint8_t)((present >> index) & 1u);
}

/* Reserved type-2 elements the present edition says shall not be used: their P-bits must be clear. */
static void
require_absent_type2(tetra_bit_reader* r, int count) {
    for (int i = 0; i < count; i++) {
        if (tetra_bits_present(r)) {
            bits_reject(r);
        }
    }
}

/* CMCE (clause 14). */

static void
cmce_read_party_address(tetra_bit_reader* r, tetra_cmce_party* party) {
    if (party->type == 1 || party->type == 2) {
        party->ssi = tetra_bits_read(r, 24);
        party->ssi_valid = 1;
    }
    if (party->type == 2) {
        party->extension = tetra_bits_read(r, 24);
        party->extension_valid = 1;
    }
}

/* The type-2 calling/transmitting party type identifier with its conditional address. */
static void
cmce_read_party_type2(tetra_bit_reader* r, tetra_cmce_party* party) {
    if (tetra_bits_present(r)) {
        party->type = (uint8_t)tetra_bits_read(r, 2);
        party->type_valid = 1;
        cmce_read_party_address(r, party);
    }
}

/* Notes a PDU that did not fit or broke its encoding rules; nothing of it is published. */
static int
cmce_reject(const mle_ctx* ctx, const char* name) {
    TETRA_LOG_PAYLOAD(ctx->ts, "[TETRA CMCE %s] CC=%d (truncated or malformed: %d bits)\n", name, ctx->cc, ctx->nbits);
    return 0;
}

/* Hand one call PDU to the call records (tetra_calls.h), with the MAC PDU it came in. */
static void
cmce_call_event(const mle_ctx* ctx, tetra_call_event* ev, uint8_t kind, uint16_t call_id) {
    ev->kind = kind;
    ev->call_id = call_id;
    tetra_calls_on_event(ctx->opts, ctx->state, ev, ctx->sdu);
}

/* D-SETUP, table 14.15. */

typedef struct {
    tetra_cmce_party calling;
    uint16_t call_id;
    uint8_t timeout;
    uint8_t basic;
    uint8_t grant;
    uint8_t permission;
    uint8_t priority;
} cmce_setup;

static void
cmce_read_setup(tetra_bit_reader* r, cmce_setup* s) {
    /* Type-2: notification indicator, temporary address; the calling party follows. */
    static const uint8_t k_type2[] = {6, 24};
    uint32_t values[2];
    s->call_id = (uint16_t)tetra_bits_read(r, 14);
    s->timeout = (uint8_t)tetra_bits_read(r, 4);
    (void)tetra_bits_skip(r, 2); /* hook method selection, simplex/duplex selection */
    s->basic = (uint8_t)tetra_bits_read(r, 8);
    s->grant = (uint8_t)tetra_bits_read(r, 2);
    s->permission = (uint8_t)tetra_bits_read(r, 1);
    s->priority = (uint8_t)tetra_bits_read(r, 4);
    if (!tetra_bits_present(r)) {
        return;
    }
    (void)read_type2_elements(r, k_type2, 2, values);
    cmce_read_party_type2(r, &s->calling);
    (void)tetra_bits_skip_type34(r);
}

static int
parse_cmce_d_setup(tetra_bit_reader* r, const mle_ctx* ctx) {
    cmce_setup s;
    DSD_MEMSET(&s, 0, sizeof s);
    cmce_read_setup(r, &s);
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-SETUP");
    }
    DSD_FPRINTF(stderr,
                "[TETRA CMCE D-SETUP] CC=%d call=%u timeout=%u circuit=%u enc=%u com=%u service=%u grant=%u "
                "request=%u priority=%u",
                ctx->cc, s.call_id, s.timeout, (unsigned)(s.basic >> 5), (unsigned)((s.basic >> 4) & 1u),
                (unsigned)((s.basic >> 2) & 3u), (unsigned)(s.basic & 3u), s.grant, s.permission, s.priority);
    if (s.calling.ssi_valid) {
        DSD_FPRINTF(stderr, " calling_SSI=%u", s.calling.ssi);
    }
    DSD_FPRINTF(stderr, "\n");

    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)(TETRA_CALL_HAS_BASIC | TETRA_CALL_HAS_TIMEOUT | TETRA_CALL_HAS_GRANT);
    ev.basic_service = s.basic;
    ev.priority = s.priority;
    ev.timeout = s.timeout;
    ev.grant = s.grant;
    if (s.calling.ssi_valid) {
        ev.has = (uint8_t)(ev.has | TETRA_CALL_HAS_CALLING);
        ev.calling_ssi = s.calling.ssi;
    }
    if (s.calling.extension_valid) {
        ev.has = (uint8_t)(ev.has | TETRA_CALL_HAS_CALLING_EXT);
        ev.calling_ext = s.calling.extension;
    }
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_SETUP, s.call_id);
    return 1;
}

/* D-RELEASE and D-DISCONNECT, tables 14.12 and 14.9. */

static int
parse_cmce_release(tetra_bit_reader* r, const mle_ctx* ctx, int is_disconnect) {
    static const uint8_t k_type2[] = {6}; /* notification indicator */
    const char* const name = is_disconnect ? "D-DISCONNECT" : "D-RELEASE";
    uint32_t notification = 0;
    const uint16_t call_id = (uint16_t)tetra_bits_read(r, 14);
    const uint8_t cause = (uint8_t)tetra_bits_read(r, 5);
    if (tetra_bits_present(r)) {
        (void)read_type2_elements(r, k_type2, 1, &notification);
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, name);
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE %s] CC=%d call=%u cause=%u\n", name, ctx->cc, call_id, cause);
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_RELEASE, call_id);
    return 1;
}

static int
parse_cmce_d_release(tetra_bit_reader* r, const mle_ctx* ctx) {
    return parse_cmce_release(r, ctx, 0);
}

static int
parse_cmce_d_disconnect(tetra_bit_reader* r, const mle_ctx* ctx) {
    return parse_cmce_release(r, ctx, 1);
}

/* D-CONNECT, table 14.7. */

typedef struct {
    uint32_t optional[4]; /* call priority, basic service information, temporary address, notification indicator */
    unsigned present;
    uint16_t call_id;
    uint8_t timeout;
    uint8_t grant;
    uint8_t permission;
} cmce_connect;

static int
parse_cmce_d_connect(tetra_bit_reader* r, const mle_ctx* ctx) {
    static const uint8_t k_type2[] = {4, 8, 24, 6};
    cmce_connect c;
    DSD_MEMSET(&c, 0, sizeof c);
    c.call_id = (uint16_t)tetra_bits_read(r, 14);
    c.timeout = (uint8_t)tetra_bits_read(r, 4);
    (void)tetra_bits_skip(r, 2); /* hook method selection, simplex/duplex selection */
    c.grant = (uint8_t)tetra_bits_read(r, 2);
    c.permission = (uint8_t)tetra_bits_read(r, 1);
    (void)tetra_bits_skip(r, 1); /* call ownership */
    if (tetra_bits_present(r)) {
        c.present = read_type2_elements(r, k_type2, 4, c.optional);
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-CONNECT");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-CONNECT] CC=%d call=%u timeout=%u grant=%u request=%u\n", ctx->cc, c.call_id,
                c.timeout, c.grant, c.permission);

    const uint8_t basic = (uint8_t)c.optional[1];
    const uint8_t basic_valid = type2_present(c.present, 1);
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)(TETRA_CALL_HAS_TIMEOUT | TETRA_CALL_HAS_GRANT | (basic_valid ? TETRA_CALL_HAS_BASIC : 0u));
    ev.timeout = c.timeout;
    ev.grant = c.grant;
    ev.basic_service = basic;
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_CONNECT, c.call_id);
    return 1;
}

/* D-CONNECT ACKNOWLEDGE, table 14.8. */

static int
parse_cmce_d_connect_ack(tetra_bit_reader* r, const mle_ctx* ctx) {
    static const uint8_t k_type2[] = {6}; /* notification indicator */
    uint32_t notification = 0;
    const uint16_t call_id = (uint16_t)tetra_bits_read(r, 14);
    const uint8_t timeout = (uint8_t)tetra_bits_read(r, 4);
    const uint8_t grant = (uint8_t)tetra_bits_read(r, 2);
    (void)tetra_bits_skip(r, 1); /* transmission request permission */
    if (tetra_bits_present(r)) {
        (void)read_type2_elements(r, k_type2, 1, &notification);
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-CONNECT-ACK");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-CONNECT-ACK] CC=%d call=%u timeout=%u grant=%u\n", ctx->cc, call_id, timeout,
                grant);
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)(TETRA_CALL_HAS_TIMEOUT | TETRA_CALL_HAS_GRANT);
    ev.timeout = timeout;
    ev.grant = grant;
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_CONNECT_ACK, call_id);
    return 1;
}

/* D-ALERT and D-CALL PROCEEDING, tables 14.4 and 14.5. */

static int
parse_cmce_d_alert(tetra_bit_reader* r, const mle_ctx* ctx) {
    static const uint8_t k_type2[] = {8, 6}; /* basic service information, notification indicator */
    uint32_t values[2] = {0, 0};
    unsigned present = 0;
    const uint16_t call_id = (uint16_t)tetra_bits_read(r, 14);
    const uint8_t timeout = (uint8_t)tetra_bits_read(r, 3);
    (void)tetra_bits_skip(r, 2); /* reserved (set to 1), simplex/duplex selection */
    const uint8_t queued = (uint8_t)tetra_bits_read(r, 1);
    if (tetra_bits_present(r)) {
        present = read_type2_elements(r, k_type2, 2, values);
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-ALERT");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-ALERT] CC=%d call=%u setup_timeout=%u queued=%u\n", ctx->cc, call_id, timeout,
                queued);
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)(type2_present(present, 0) ? TETRA_CALL_HAS_BASIC : 0u);
    ev.basic_service = (uint8_t)values[0];
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_ALERT, call_id);
    return 1;
}

static int
parse_cmce_d_call_proceeding(tetra_bit_reader* r, const mle_ctx* ctx) {
    static const uint8_t k_type2[] = {8, 3, 6}; /* basic service information, call status, notification indicator */
    uint32_t values[3] = {0, 0, 0};
    unsigned present = 0;
    const uint16_t call_id = (uint16_t)tetra_bits_read(r, 14);
    const uint8_t timeout = (uint8_t)tetra_bits_read(r, 3);
    (void)tetra_bits_skip(r, 2); /* hook method selection, simplex/duplex selection */
    if (tetra_bits_present(r)) {
        present = read_type2_elements(r, k_type2, 3, values);
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-CALL-PROCEEDING");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-CALL-PROCEEDING] CC=%d call=%u setup_timeout=%u\n", ctx->cc, call_id, timeout);
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)(type2_present(present, 0) ? TETRA_CALL_HAS_BASIC : 0u);
    ev.basic_service = (uint8_t)values[0];
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_PROCEEDING, call_id);
    return 1;
}

/* Floor control: D-TX GRANTED, D-TX INTERRUPT, D-TX CONTINUE, D-TX WAIT and D-TX CEASED, tables 14.16-14.20. */

typedef struct {
    tetra_cmce_party party; /* transmitting party (D-TX GRANTED and D-TX INTERRUPT) */
    uint16_t call_id;
    uint8_t grant;          /* transmission grant (D-TX GRANTED and D-TX INTERRUPT) */
    uint8_t permission;     /* transmission request permission */
    uint8_t encryption;     /* encryption control (D-TX GRANTED and D-TX INTERRUPT) */
    uint8_t reserved;       /* reserved, set to 0 (D-TX GRANTED and D-TX INTERRUPT) */
    uint8_t continue_value; /* continue (D-TX CONTINUE) */
    uint8_t notification;
    uint8_t notification_valid;
} cmce_floor;

/* The type-1 elements D-TX GRANTED and D-TX INTERRUPT share after the call identifier. */
static void
cmce_read_grant_fields(tetra_bit_reader* r, cmce_floor* f) {
    f->grant = (uint8_t)tetra_bits_read(r, 2);
    f->permission = (uint8_t)tetra_bits_read(r, 1);
    f->encryption = (uint8_t)tetra_bits_read(r, 1);
    f->reserved = (uint8_t)tetra_bits_read(r, 1);
}

/* The optional part: the notification indicator, the transmitting party when @p with_party, then type-3/4. */
static void
cmce_read_floor_optional(tetra_bit_reader* r, cmce_floor* f, int with_party) {
    static const uint8_t k_type2[] = {6}; /* notification indicator */
    uint32_t notification = 0;
    if (!tetra_bits_present(r)) {
        return;
    }
    f->notification_valid = (uint8_t)read_type2_elements(r, k_type2, 1, &notification);
    f->notification = (uint8_t)notification;
    if (with_party) {
        cmce_read_party_type2(r, &f->party);
    }
    (void)tetra_bits_skip_type34(r);
}

/* The call event of a D-TX GRANTED or D-TX INTERRUPT: its grant, encryption control and transmitting party. */
static void
cmce_floor_event(const mle_ctx* ctx, const cmce_floor* f, uint8_t kind) {
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)(TETRA_CALL_HAS_GRANT | TETRA_CALL_HAS_ENCRYPTION);
    ev.grant = f->grant;
    ev.encryption = f->encryption;
    if (f->party.ssi_valid) {
        ev.has = (uint8_t)(ev.has | TETRA_CALL_HAS_TALKER);
        ev.talker_ssi = f->party.ssi;
    }
    cmce_call_event(ctx, &ev, kind, f->call_id);
}

static int
parse_cmce_d_tx_granted(tetra_bit_reader* r, const mle_ctx* ctx) {
    cmce_floor f;
    DSD_MEMSET(&f, 0, sizeof f);
    f.call_id = (uint16_t)tetra_bits_read(r, 14);
    cmce_read_grant_fields(r, &f);
    cmce_read_floor_optional(r, &f, 1);
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-TX-GRANTED");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-TX-GRANTED] CC=%d call=%u grant=%u request=%u enc=%u", ctx->cc, f.call_id,
                f.grant, f.permission, f.encryption);
    if (f.party.ssi_valid) {
        DSD_FPRINTF(stderr, " granted_SSI=%u", f.party.ssi);
    }
    if (f.reserved) {
        DSD_FPRINTF(stderr, " reserved=%u", f.reserved);
    }
    DSD_FPRINTF(stderr, "\n");
    cmce_floor_event(ctx, &f, TETRA_CALL_EV_TX_GRANTED);
    return 1;
}

static int
parse_cmce_d_tx_ceased(tetra_bit_reader* r, const mle_ctx* ctx) {
    cmce_floor f;
    DSD_MEMSET(&f, 0, sizeof f);
    f.call_id = (uint16_t)tetra_bits_read(r, 14);
    f.permission = (uint8_t)tetra_bits_read(r, 1);
    cmce_read_floor_optional(r, &f, 0);
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-TX-CEASED");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-TX-CEASED] CC=%d call=%u request=%u", ctx->cc, f.call_id, f.permission);
    if (f.notification_valid) {
        DSD_FPRINTF(stderr, " notification=%u", f.notification);
    }
    DSD_FPRINTF(stderr, " (floor released)\n");
    /* The floor is released; the assigned traffic channel stays. */
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_TX_CEASED, f.call_id);
    return 1;
}

/* D-TX CONTINUE (table 14.17), D-TX INTERRUPT (table 14.19) and D-TX WAIT (table 14.20). */
static int
parse_cmce_tx_event(tetra_bit_reader* r, const mle_ctx* ctx, uint32_t pdu_type, const char* name) {
    cmce_floor f;
    DSD_MEMSET(&f, 0, sizeof f);
    f.call_id = (uint16_t)tetra_bits_read(r, 14);
    if (pdu_type == TETRA_CMCE_D_TX_INTERRUPT) {
        cmce_read_grant_fields(r, &f);
    } else {
        if (pdu_type == TETRA_CMCE_D_TX_CONTINUE) {
            f.continue_value = (uint8_t)tetra_bits_read(r, 1);
        }
        f.permission = (uint8_t)tetra_bits_read(r, 1);
    }
    cmce_read_floor_optional(r, &f, pdu_type == TETRA_CMCE_D_TX_INTERRUPT);
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, name);
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE %s] CC=%d call=%u request=%u notif=%u continue=%u grant=%u enc=%u\n", name,
                ctx->cc, f.call_id, f.permission, f.notification, f.continue_value, f.grant, f.encryption);
    if (pdu_type == TETRA_CMCE_D_TX_INTERRUPT) {
        cmce_floor_event(ctx, &f, TETRA_CALL_EV_TX_INTERRUPT);
    } else {
        tetra_call_event ev;
        DSD_MEMSET(&ev, 0, sizeof ev);
        cmce_call_event(ctx, &ev,
                        pdu_type == TETRA_CMCE_D_TX_CONTINUE ? TETRA_CALL_EV_TX_CONTINUE : TETRA_CALL_EV_TX_WAIT,
                        f.call_id);
    }
    return 1;
}

static int
parse_cmce_d_tx_continue(tetra_bit_reader* r, const mle_ctx* ctx) {
    return parse_cmce_tx_event(r, ctx, TETRA_CMCE_D_TX_CONTINUE, "D-TX-CONTINUE");
}

static int
parse_cmce_d_tx_interrupt(tetra_bit_reader* r, const mle_ctx* ctx) {
    return parse_cmce_tx_event(r, ctx, TETRA_CMCE_D_TX_INTERRUPT, "D-TX-INTERRUPT");
}

static int
parse_cmce_d_tx_wait(tetra_bit_reader* r, const mle_ctx* ctx) {
    return parse_cmce_tx_event(r, ctx, TETRA_CMCE_D_TX_WAIT, "D-TX-WAIT");
}

/* D-INFO, table 14.11. */

enum {
    DINFO_NEW_CALL_ID,
    DINFO_TIMEOUT,
    DINFO_SETUP_TIMEOUT,
    DINFO_OWNERSHIP,
    DINFO_MODIFY,
    DINFO_STATUS,
    DINFO_TEMPORARY_ADDRESS,
    DINFO_NOTIFICATION,
    DINFO_POLL_PERCENTAGE,
    DINFO_POLL_NUMBER,
    DINFO_TYPE2_COUNT,
};

static int
parse_cmce_d_info(tetra_bit_reader* r, const mle_ctx* ctx) {
    static const uint8_t k_type2[DINFO_TYPE2_COUNT] = {14, 4, 3, 1, 9, 3, 24, 6, 6, 6};
    uint32_t values[DINFO_TYPE2_COUNT] = {0};
    unsigned present = 0;
    const uint16_t call_id = (uint16_t)tetra_bits_read(r, 14);
    const uint8_t reset = (uint8_t)tetra_bits_read(r, 1);
    const uint8_t poll = (uint8_t)tetra_bits_read(r, 1);
    if (tetra_bits_present(r)) {
        present = read_type2_elements(r, k_type2, DINFO_TYPE2_COUNT, values);
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-INFO");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-INFO] CC=%d call=%u reset=%u poll=%u\n", ctx->cc, call_id, reset, poll);
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)((type2_present(present, DINFO_NEW_CALL_ID) ? TETRA_CALL_HAS_NEW_ID : 0u)
                       | (type2_present(present, DINFO_TIMEOUT) ? TETRA_CALL_HAS_TIMEOUT : 0u));
    ev.new_call_id = (uint16_t)values[DINFO_NEW_CALL_ID];
    ev.timeout = (uint8_t)values[DINFO_TIMEOUT];
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_INFO, call_id);
    return 1;
}

/* D-CALL RESTORE, table 14.6. */

enum {
    RESTORE_NEW_CALL_ID,
    RESTORE_TIMEOUT,
    RESTORE_STATUS,
    RESTORE_MODIFY,
    RESTORE_NOTIFICATION,
    RESTORE_TYPE2_COUNT,
};

static int
cmce_read_call_restore(tetra_bit_reader* r, tetra_cmce_call_restore* c) {
    static const uint8_t k_type2[RESTORE_TYPE2_COUNT] = {14, 4, 3, 9, 6};
    uint32_t v[RESTORE_TYPE2_COUNT] = {0};
    unsigned present = 0;
    c->call_id = (uint16_t)tetra_bits_read(r, 14);
    c->grant = (uint8_t)tetra_bits_read(r, 2);
    c->permission = (uint8_t)tetra_bits_read(r, 1);
    c->reset = (uint8_t)tetra_bits_read(r, 1);
    if (tetra_bits_present(r)) {
        present = read_type2_elements(r, k_type2, RESTORE_TYPE2_COUNT, v);
        (void)tetra_bits_skip_type34(r);
    }
    c->new_call_id = (uint16_t)v[RESTORE_NEW_CALL_ID];
    c->new_call_id_present = type2_present(present, RESTORE_NEW_CALL_ID);
    c->timeout = (uint8_t)v[RESTORE_TIMEOUT];
    c->timeout_present = type2_present(present, RESTORE_TIMEOUT);
    return tetra_bits_ok(r);
}

static void
cmce_report_call_restore(const mle_ctx* ctx, const tetra_cmce_call_restore* c) {
    DSD_FPRINTF(stderr, "[TETRA CMCE D-CALL-RESTORE] CC=%d call=%u grant=%u request=%u reset=%u\n", ctx->cc, c->call_id,
                c->grant, c->permission, c->reset);
    tetra_call_event ev;
    DSD_MEMSET(&ev, 0, sizeof ev);
    ev.has = (uint8_t)((c->new_call_id_present ? TETRA_CALL_HAS_NEW_ID : 0u)
                       | (c->timeout_present ? TETRA_CALL_HAS_TIMEOUT : 0u));
    ev.new_call_id = c->new_call_id;
    ev.timeout = c->timeout;
    cmce_call_event(ctx, &ev, TETRA_CALL_EV_RESTORE, c->call_id);
}

static int
parse_cmce_d_call_restore(tetra_bit_reader* r, const mle_ctx* ctx) {
    tetra_cmce_call_restore c;
    DSD_MEMSET(&c, 0, sizeof c);
    if (!cmce_read_call_restore(r, &c)) {
        return cmce_reject(ctx, "D-CALL-RESTORE");
    }
    cmce_report_call_restore(ctx, &c);
    return 1;
}

/* CMCE FUNCTION NOT SUPPORTED, table 14.33. */

static int
parse_cmce_function_not_supported(tetra_bit_reader* r, const mle_ctx* ctx) {
    uint16_t call_id = 0;
    uint8_t extract_bits = 0;
    const uint8_t rejected = (uint8_t)tetra_bits_read(r, 5);
    const uint8_t call_id_present = (uint8_t)tetra_bits_read(r, 1);
    if (call_id_present) {
        call_id = (uint16_t)tetra_bits_read(r, 14);
    }
    const uint8_t pointer = (uint8_t)tetra_bits_read(r, 8);
    if (pointer != 0) { /* a non-zero pointer brings the length and bits of the received PDU extract */
        extract_bits = (uint8_t)tetra_bits_read(r, 8);
        (void)tetra_bits_skip(r, extract_bits);
    }
    if (tetra_bits_present(r)) {
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "FUNCTION-NOT-SUPPORTED");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE FUNCTION-NOT-SUPPORTED] CC=%d rejected=%u call=%u pointer=%u extract=%u\n",
                ctx->cc, rejected, call_id, pointer, extract_bits);
    return 1;
}

/* D-FACILITY, table 14.10 and Annex E.1.2 tables E.2 to E.4. */

/* A four-bit count of independently encoded SS-PDUs, each an 11-bit length and that many bits, then the O-bit and its
 * type-3/4 chain. The SS type is the first six bits of an SS-PDU. */
static int
parse_cmce_d_facility(tetra_bit_reader* r, const mle_ctx* ctx) {
    uint32_t first_ss_type = 0;
    const uint32_t count = tetra_bits_read(r, 4);
    for (uint32_t i = 0; i < count && tetra_bits_ok(r); i++) {
        const uint32_t length = tetra_bits_read(r, 11);
        if (i == 0 && length >= 6) {
            tetra_bit_reader ss = *r;
            first_ss_type = tetra_bits_read(&ss, 6);
        }
        (void)tetra_bits_skip(r, (int)length);
    }
    if (tetra_bits_present(r)) {
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return cmce_reject(ctx, "D-FACILITY");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-FACILITY] CC=%d ss_pdus=%u first_ss_type=%u\n", ctx->cc, count, first_ss_type);
    return 1;
}

/* SDS (clauses 14.7.1.10 and 29). */

#define SDS_PID_SIMPLE_TEXT           2u
#define SDS_PID_SIMPLE_IMMEDIATE_TEXT 9u
#define SDS_PID_CONCAT                12u
#define SDS_PID_TEXT                  130u
#define SDS_PID_IMMEDIATE_TEXT        137u
#define SDS_PID_CONCAT_TL             140u

/* Table 29.21: protocol identifiers 128 to 254 use the SDS-TL data transfer service (255 is the extension value). */
static int
sds_pid_uses_tl(uint32_t pid) {
    return pid >= 128u && pid < 255u;
}

static int
sds_pid_is_simple_text(uint32_t pid) {
    return pid == SDS_PID_SIMPLE_TEXT || pid == SDS_PID_SIMPLE_IMMEDIATE_TEXT;
}

static int
sds_pid_is_tl_text(uint32_t pid) {
    return pid == SDS_PID_TEXT || pid == SDS_PID_IMMEDIATE_TEXT;
}

/* Append one Unicode scalar value. C0 and C1 controls become a space: radio text must not reach a terminal as an
 * escape sequence (a UTF-8 U+009B is CSI), and table 29.39's LF and CR read as word breaks. The first character that
 * does not fit ends the text. */
static void
sds_text_append(tetra_sds_text* t, uint32_t scalar) {
    char utf8[DSD_UTF8_MAX_BYTES + 1];
    if (t->full) {
        return;
    }
    if (dsd_unicode_scalar_is_control(scalar)) {
        scalar = ' ';
    }
    const size_t n = dsd_utf8_encode_scalar(scalar, utf8, sizeof utf8);
    if ((size_t)t->len + n >= sizeof t->text) {
        t->full = 1;
        return;
    }
    DSD_MEMCPY(t->text + t->len, utf8, n);
    t->len = (uint16_t)(t->len + n);
    t->text[t->len] = '\0';
}

/* Table 29.39, the GSM 7-bit default alphabet, as Unicode. Index 27 is the escape to the extension table. */
static const uint16_t k_gsm7_alphabet[128] = {
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC, 0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8,
    0x000D, 0x00C5, 0x00E5, 0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8, 0x03A3, 0x0398,
    0x039E, 0x0020, 0x00C6, 0x00E6, 0x00DF, 0x00C9, 0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026,
    0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033,
    0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x00A1,
    0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D,
    0x004E, 0x004F, 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A,
    0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7, 0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073, 0x0074,
    0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

#define GSM7_ESCAPE 27u

/* The GSM 7-bit extension table (ETSI TS 100 900, which table 29.39 adopts); a code without an extension character
 * shows its default-alphabet character. */
static uint32_t
gsm7_extension(unsigned code) {
    static const uint8_t k_codes[] = {10, 20, 40, 41, 47, 60, 61, 62, 64, 101};
    static const uint16_t k_chars[] = {0x000C, '^', '{', '}', '\\', '[', '~', ']', '|', 0x20AC};
    for (size_t i = 0; i < sizeof k_codes; i++) {
        if (k_codes[i] == code) {
            return k_chars[i];
        }
    }
    return k_gsm7_alphabet[code & 0x7fu];
}

/* Clause 29.5.4.2: septets are packed into octets least significant bit first (tables 29.31 to 29.34) and the octets
 * sent most significant bit first; the spare bits of the last octet are padding. A trailing escape without its
 * extension character shows as U+FFFD. */
static int
sds_decode_gsm7(tetra_bit_reader* r, tetra_sds_text* t) {
    uint32_t acc = 0;
    unsigned acc_bits = 0;
    int escaped = 0;
    if (tetra_bits_left(r) % 8 != 0) {
        return 0;
    }
    while (tetra_bits_left(r) > 0 && !t->full) {
        acc |= tetra_bits_read(r, 8) << acc_bits;
        acc_bits += 8;
        while (acc_bits >= 7) {
            const unsigned septet = acc & 0x7fu;
            acc >>= 7;
            acc_bits -= 7;
            if (!escaped && septet == GSM7_ESCAPE) {
                escaped = 1;
                continue;
            }
            sds_text_append(t, escaped ? gsm7_extension(septet) : k_gsm7_alphabet[septet]);
            escaped = 0;
        }
    }
    if (escaped) {
        sds_text_append(t, DSD_UNICODE_REPLACEMENT);
    }
    return 1;
}

/* ISO/IEC 8859-1 octets are the Unicode scalar values 0 to 255. */
static int
sds_decode_latin1(tetra_bit_reader* r, tetra_sds_text* t) {
    if (tetra_bits_left(r) % 8 != 0) {
        return 0;
    }
    while (tetra_bits_left(r) > 0 && !t->full) {
        sds_text_append(t, tetra_bits_read(r, 8));
    }
    return 1;
}

/* UCS-2 with the UTF-16BE extension (clause 29.5.4.1). Surrogate pairs combine; an unpaired half shows as U+FFFD. */
static int
sds_decode_utf16(tetra_bit_reader* r, tetra_sds_text* t) {
    dsd_utf16_decoder decoder;
    uint32_t scalars[DSD_UTF16_MAX_SCALARS_PER_UNIT];
    if (tetra_bits_left(r) % 16 != 0) {
        return 0;
    }
    t->unicode = 1;
    dsd_utf16_decoder_reset(&decoder);
    while (tetra_bits_left(r) > 0 && !t->full) {
        const uint16_t unit = (uint16_t)tetra_bits_read(r, 16);
        const size_t n = dsd_utf16_decoder_push(&decoder, unit, scalars, DSD_UTF16_MAX_SCALARS_PER_UNIT);
        for (size_t i = 0; i < n; i++) {
            sds_text_append(t, scalars[i]);
        }
    }
    if (dsd_utf16_decoder_finish(&decoder, scalars, 1U) > 0U) {
        sds_text_append(t, scalars[0]);
    }
    return 1;
}

/* Text coding schemes, table 29.29. */
#define SDS_CODING_GSM7   0u
#define SDS_CODING_LATIN1 1u
#define SDS_CODING_UTF16  26u

/* A simple text messaging PDU body (table 29.27) or a text message transfer SDU (table 29.28): the time stamp used
 * flag (reserved for simple text) and the text coding scheme, the time stamp when used and allowed, then the text.
 * Text in a coding scheme without a decoder here is left empty. 0 when the PDU is malformed. */
static int
sds_decode_text(tetra_bit_reader* r, int timestamp_allowed, tetra_sds_text* t) {
    const uint32_t timestamp_used = tetra_bits_read(r, 1);
    const uint32_t coding = tetra_bits_read(r, 7);
    if (timestamp_allowed && timestamp_used) {
        (void)tetra_bits_skip(r, 24); /* time stamp, table 29.40 */
    }
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    switch (coding) {
        case SDS_CODING_GSM7: return sds_decode_gsm7(r, t);
        case SDS_CODING_LATIN1: return sds_decode_latin1(r, t);
        case SDS_CODING_UTF16: return sds_decode_utf16(r, t);
        default: return 1;
    }
}

/* The reassembly of a concatenated message (table 29.60): how many parts it holds now, whether this part completed
 * or repeated it, and the reassembled message once complete. */
typedef struct {
    uint8_t* assembled;
    int assembled_bits;
    uint8_t received;
    uint8_t assembled_pid;
    uint8_t complete; /* this part completed the message */
    uint8_t duplicate;
} sds_concat_status;

/* Concatenated SDS reassembly, clause 29.5.14. */

#define SDS_CONCAT_CONTEXTS        4
#define SDS_CONCAT_PARTS           255
#define SDS_CONCAT_PART_BITS       2047
#define SDS_CONCAT_PART_BYTES      ((SDS_CONCAT_PART_BITS + 7) / 8)
#define SDS_CONCAT_TIMEOUT_MS      300000u

/* The network of a calling party of type 1 while the decoder knows none: no 24-bit calling party extension equals it. */
#define SDS_CONCAT_NETWORK_UNKNOWN 0xFFFFFFFFu

/* The calling party a concatenated message comes from, as its TETRA subscriber identity: the SSI with the MCC and MNC
 * of its network (table 14.44), which a calling party of type 2 (TSI, table 14.43) carries as its extension and one of
 * type 1 (SSI) leaves to the current network (EN 300 392-2 clause 14.2.3). Two senders that share an SSI are kept apart;
 * one sender is one, whether it names its network or not. The reserved types carry no address. */
typedef struct {
    uint32_t ssi;
    uint32_t network; /* country code (10 bits) and network code (14 bits), as the extension holds them */
    uint8_t type;     /* 1 for an SSI or TSI calling party; the reserved type identifier otherwise */
} sds_concat_sender;

/* One message being reassembled; the parts are stored packed, MSB first. */
typedef struct {
    uint64_t updated_ms; /* decode-clock time of the last accepted part */
    sds_concat_sender src;
    uint16_t part_bits[SDS_CONCAT_PARTS];
    uint16_t reference;
    uint8_t valid;
    uint8_t with_transport; /* PID 140 (SDS-TL) rather than PID 12 */
    uint8_t total;
    uint8_t received_count;
    uint8_t payload_pid_known;
    uint8_t payload_pid;
    uint8_t received[(SDS_CONCAT_PARTS + 7) / 8];
    uint8_t data[SDS_CONCAT_PARTS][SDS_CONCAT_PART_BYTES];
} sds_concat_ctx;

struct tetra_sds_concat_store {
    sds_concat_ctx ctx[SDS_CONCAT_CONTEXTS];
};

/* One concatenation part as received. */
typedef struct {
    tetra_bit_reader data; /* the part's user data */
    sds_concat_sender src;
    int payload_pid; /* -1 except in part 1 */
    uint16_t reference;
    uint8_t with_transport;
    uint8_t total;
    uint8_t sequence;
} sds_concat_part;

enum { SDS_CONCAT_REJECTED = -1, SDS_CONCAT_PENDING = 0, SDS_CONCAT_COMPLETE = 1, SDS_CONCAT_DUPLICATE = 2 };

static struct tetra_sds_concat_store*
sds_concat_store(tetra_state_t* ts) {
    if (!ts->sds_concat) {
        ts->sds_concat = (struct tetra_sds_concat_store*)calloc(1, sizeof(*ts->sds_concat));
    }
    return ts->sds_concat;
}

static int
sds_concat_has_part(const sds_concat_ctx* c, unsigned index) {
    return (c->received[index >> 3] >> (index & 7u)) & 1u;
}

static void
sds_concat_clear(sds_concat_ctx* c) {
    DSD_MEMSET(c, 0, sizeof(*c));
}

static int
sds_concat_same_sender(const sds_concat_sender* a, const sds_concat_sender* b) {
    return a->type == b->type && a->ssi == b->ssi && a->network == b->network;
}

/* The context collecting @p part's message: the matching one, else a free one, else the stalest. Contexts idle for
 * longer than SDS_CONCAT_TIMEOUT_MS are dropped on the way. */
static sds_concat_ctx*
sds_concat_find(struct tetra_sds_concat_store* store, const sds_concat_part* part, uint64_t now_ms) {
    sds_concat_ctx* free_ctx = NULL;
    sds_concat_ctx* oldest = &store->ctx[0];
    for (unsigned i = 0; i < SDS_CONCAT_CONTEXTS; i++) {
        sds_concat_ctx* c = &store->ctx[i];
        if (c->valid && now_ms - c->updated_ms > SDS_CONCAT_TIMEOUT_MS) {
            sds_concat_clear(c);
        }
        if (c->valid && sds_concat_same_sender(&c->src, &part->src) && c->reference == part->reference
            && c->with_transport == part->with_transport) {
            return c;
        }
        if (!c->valid && !free_ctx) {
            free_ctx = c;
        }
        if (c->valid && c->updated_ms < oldest->updated_ms) {
            oldest = c;
        }
    }
    sds_concat_ctx* c = free_ctx ? free_ctx : oldest;
    sds_concat_clear(c);
    c->valid = 1;
    c->src = part->src;
    c->reference = part->reference;
    c->with_transport = part->with_transport;
    c->total = part->total;
    return c;
}

/* Unpack the complete message into one bit per byte, parts in sequence order. NULL when out of memory. */
static uint8_t*
sds_concat_assemble(const sds_concat_ctx* c, int* nbits) {
    unsigned total_bits = 0;
    for (unsigned i = 0; i < c->total; i++) {
        total_bits += c->part_bits[i];
    }
    uint8_t* out = (uint8_t*)malloc(total_bits > 0 ? total_bits : 1u);
    if (!out) {
        return NULL;
    }
    unsigned pos = 0;
    for (unsigned i = 0; i < c->total; i++) {
        for (unsigned bit = 0; bit < c->part_bits[i] && pos < total_bits; bit++) {
            out[pos++] = (uint8_t)((c->data[i][bit >> 3] >> (7u - (bit & 7u))) & 1u);
        }
    }
    *nbits = (int)pos;
    return out;
}

/* Store one part. An identical repeat is a duplicate; a different part under the same sequence number, or a changed
 * part count, discards the whole message. */
static int
sds_concat_store_part(sds_concat_ctx* c, const sds_concat_part* part) {
    uint8_t packed[SDS_CONCAT_PART_BYTES];
    tetra_bit_reader data = part->data;
    const int part_bits = tetra_bits_left(&data);
    const unsigned index = part->sequence - 1u;
    DSD_MEMSET(packed, 0, sizeof packed);
    for (int i = 0; i < part_bits; i++) {
        packed[i >> 3] = (uint8_t)(packed[i >> 3] | (tetra_bits_read(&data, 1) << (7 - (i & 7))));
    }
    if (c->total != part->total) {
        sds_concat_clear(c);
        return SDS_CONCAT_REJECTED;
    }
    if (sds_concat_has_part(c, index)) {
        if (c->part_bits[index] != (uint16_t)part_bits || memcmp(c->data[index], packed, sizeof packed) != 0) {
            sds_concat_clear(c);
            return SDS_CONCAT_REJECTED;
        }
        return SDS_CONCAT_DUPLICATE;
    }
    DSD_MEMCPY(c->data[index], packed, sizeof packed);
    c->part_bits[index] = (uint16_t)part_bits;
    c->received[index >> 3] = (uint8_t)(c->received[index >> 3] | (1u << (index & 7u)));
    c->received_count++;
    if (part->sequence == 1) {
        c->payload_pid = (uint8_t)part->payload_pid;
        c->payload_pid_known = 1;
    }
    return SDS_CONCAT_PENDING;
}

void
tetra_mle_forget_reassembly(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts && ts->sds_concat) {
        for (unsigned i = 0; i < SDS_CONCAT_CONTEXTS; i++) {
            sds_concat_clear(&ts->sds_concat->ctx[i]);
        }
    }
}

/* Add one well-formed part (sds_read_concat_control()) to the reassembly in @p ts and report the message state in
 * @p status. */
static int
sds_concat_submit(tetra_state_t* ts, const sds_concat_part* part, sds_concat_status* status) {
    struct tetra_sds_concat_store* store = sds_concat_store(ts);
    if (!store) {
        return SDS_CONCAT_REJECTED;
    }
    const uint64_t now_ms = dsd_decode_now_mono_ms();
    sds_concat_ctx* c = sds_concat_find(store, part, now_ms);
    c->updated_ms = now_ms;
    const int result = sds_concat_store_part(c, part);
    status->received = c->received_count;
    if (result != SDS_CONCAT_PENDING || c->received_count != c->total || !c->payload_pid_known) {
        return result;
    }
    status->assembled = sds_concat_assemble(c, &status->assembled_bits);
    status->assembled_pid = c->payload_pid;
    sds_concat_clear(c);
    return status->assembled ? SDS_CONCAT_COMPLETE : SDS_CONCAT_REJECTED;
}

/* SDS-TL, clause 29.4. */

/* External subscriber number digits (table 14.59), as a string; an odd count is followed by a zero dummy digit. */
static void
sds_read_external_number(tetra_bit_reader* r, char* out, size_t out_size) {
    static const char k_digits[] = "0123456789*#+";
    const uint32_t digits = tetra_bits_read(r, 8);
    if (digits == 0 || digits > 24 || digits >= out_size) {
        bits_reject(r);
        return;
    }
    for (uint32_t i = 0; i < digits; i++) {
        const uint32_t value = tetra_bits_read(r, 4);
        if (value > 12) {
            bits_reject(r);
            return;
        }
        out[i] = k_digits[value];
    }
    out[digits] = '\0';
    if ((digits & 1u) != 0 && tetra_bits_read(r, 4) != 0) {
        bits_reject(r);
    }
}

/* Storage/forward control information (tables 29.12 and 29.14): validity period, forward address type (table 29.18)
 * and the address it selects. */
static void
sds_read_forward_address(tetra_bit_reader* r, tetra_sds_tl* h) {
    h->validity_period = (uint8_t)tetra_bits_read(r, 5);
    h->forward_type = (uint8_t)tetra_bits_read(r, 3);
    switch (h->forward_type) {
        case 0: h->forward_sna = (uint8_t)tetra_bits_read(r, 8); break;
        case 1: h->forward_ssi = tetra_bits_read(r, 24); break;
        case 2:
            h->forward_ssi = tetra_bits_read(r, 24);
            h->forward_extension = tetra_bits_read(r, 24);
            break;
        case 3: sds_read_external_number(r, h->forward_external, sizeof h->forward_external); break;
        case 7: break; /* no forward address present */
        default: bits_reject(r); break;
    }
    h->forward_valid = (uint8_t)tetra_bits_ok(r);
}

/* The SDS-TL header after the protocol identifier: SDS-TRANSFER, SDS-REPORT or SDS-ACK. Message types 3 to 15 are
 * reserved or application-defined and carry no header the decoder knows. */
static void
sds_read_tl_header(tetra_bit_reader* r, tetra_sds_tl* h) {
    const uint32_t type = tetra_bits_read(r, 4);
    if (type > TETRA_SDS_TL_ACK) {
        return;
    }
    h->message_type = (uint8_t)type;
    if (type == TETRA_SDS_TL_ACK) {
        (void)tetra_bits_skip(r, 4); /* reserved */
        h->status = (uint8_t)tetra_bits_read(r, 8);
        h->reference = (uint8_t)tetra_bits_read(r, 8);
        return;
    }
    /* TRANSFER: delivery report request, service selection; REPORT: acknowledgement required, reserved. */
    (void)tetra_bits_skip(r, 3);
    h->storage_forward = (uint8_t)tetra_bits_read(r, 1);
    if (type == TETRA_SDS_TL_REPORT) {
        h->status = (uint8_t)tetra_bits_read(r, 8);
    }
    h->reference = (uint8_t)tetra_bits_read(r, 8);
    if (h->storage_forward) {
        sds_read_forward_address(r, h);
    }
}

/* Concatenation control (table 29.60) and, in part 1, the PID of the user payload; then the part's user data. A part
 * is malformed when its count or number is out of range, or when part 1 names a concatenation PID again. */
static void
sds_read_concat_control(tetra_bit_reader* r, tetra_sds_concat_part* part) {
    if (tetra_bits_read(r, 3) != 0) { /* concatenation control PDU type: only transfer is defined (table 29.61) */
        bits_reject(r);
        return;
    }
    const uint32_t extended = tetra_bits_read(r, 1);
    const uint32_t extension = extended ? tetra_bits_read(r, 8) : 0;
    part->reference = (uint16_t)((extension << 4) | tetra_bits_read(r, 4));
    part->total = (uint8_t)tetra_bits_read(r, 8);
    part->sequence = (uint8_t)tetra_bits_read(r, 8);
    part->payload_pid = -1;
    if (part->sequence == 1) {
        part->payload_pid = (int)tetra_bits_read(r, 8);
        if (part->payload_pid == (int)SDS_PID_CONCAT || part->payload_pid == (int)SDS_PID_CONCAT_TL) {
            bits_reject(r);
        }
    }
    part->data_pos = r->pos;
    part->data_bits = tetra_bits_left(r);
    if (part->total < 2 || part->sequence == 0 || part->sequence > part->total
        || part->data_bits > SDS_CONCAT_PART_BITS) {
        bits_reject(r);
    }
}

/* Decode the text of a message whose user data PID is @p pid; @p r is at the text message SDU. */
static int
sds_decode_message_text(tetra_bit_reader* r, uint32_t pid, tetra_sds_text* t) {
    if (sds_pid_is_simple_text(pid)) {
        return sds_decode_text(r, 0, t);
    }
    if (sds_pid_is_tl_text(pid)) {
        return sds_decode_text(r, 1, t);
    }
    return 1;
}

/* A concatenation part (tables 29.62 and 29.63) of the message under @p pid. */
static int
sds_decode_concat(tetra_bit_reader* ud, uint32_t pid, tetra_sds_data* d) {
    if (pid == SDS_PID_CONCAT_TL && d->tl.message_type != TETRA_SDS_TL_TRANSFER) {
        return 0;
    }
    sds_read_concat_control(ud, &d->concat);
    d->concat.with_transport = (uint8_t)(pid == SDS_PID_CONCAT_TL);
    d->concat_present = 1;
    return tetra_bits_ok(ud);
}

/* User defined data-4 (short data type identifier 3): the protocol identifier (table 29.21), the SDS-TL header for the
 * PIDs that use it, then text or a concatenation part. */
static int
sds_decode_user_data(tetra_bit_reader* ud, tetra_sds_data* d) {
    if (tetra_bits_left(ud) < 8) {
        return 1; /* too short to name a protocol: kept as bare user data */
    }
    const uint32_t pid = tetra_bits_read(ud, 8);
    d->pid = (uint8_t)pid;
    d->pid_present = 1;
    if (sds_pid_uses_tl(pid)) {
        sds_read_tl_header(ud, &d->tl);
    }
    if (!tetra_bits_ok(ud)) {
        return 0;
    }
    if (pid == SDS_PID_CONCAT || pid == SDS_PID_CONCAT_TL) {
        return sds_decode_concat(ud, pid, d);
    }
    if (sds_pid_is_tl_text(pid) && d->tl.message_type != TETRA_SDS_TL_TRANSFER) {
        return 1; /* only SDS-TRANSFER carries the text message transfer SDU */
    }
    return sds_decode_message_text(ud, pid, &d->text);
}

/* D-SDS-DATA (table 14.13): calling party, short data type identifier and user data, then the user data decoded. */
static int
sds_decode_data(tetra_bit_reader* r, tetra_sds_data* d) {
    static const int k_fixed_bits[3] = {16, 32, 64}; /* user defined data-1 to -3 */
    d->tl.message_type = TETRA_SDS_TL_NONE;
    d->calling.type = (uint8_t)tetra_bits_read(r, 2);
    cmce_read_party_address(r, &d->calling);
    d->sdti = (uint8_t)tetra_bits_read(r, 2);
    d->data_bits = d->sdti < 3 ? k_fixed_bits[d->sdti] : (int)tetra_bits_read(r, 11);
    const int data_pos = r->pos;
    (void)tetra_bits_skip(r, d->data_bits);
    if (tetra_bits_present(r)) { /* no type-2 elements: type-3 elements follow at once */
        (void)tetra_bits_skip_type34(r);
    }
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    tetra_bit_reader ud = tetra_bits_reader(r->bits, data_pos + d->data_bits, data_pos);
    if (d->sdti == 0) {
        d->short_data = (uint16_t)tetra_bits_read(&ud, 16);
    } else if (d->sdti == 3) {
        return sds_decode_user_data(&ud, d);
    }
    return 1;
}

/* D-STATUS (table 14.14): calling party and pre-coded status. */
static int
sds_decode_status(tetra_bit_reader* r, tetra_cmce_status* st) {
    st->calling.type = (uint8_t)tetra_bits_read(r, 2);
    cmce_read_party_address(r, &st->calling);
    st->status = (uint16_t)tetra_bits_read(r, 16);
    if (tetra_bits_present(r)) { /* no type-2 elements: type-3 elements follow at once */
        (void)tetra_bits_skip_type34(r);
    }
    /* Table 29.13: a pre-coded status starting 011111 is an SDS-TL SDS-SHORT REPORT. */
    if ((st->status >> 10) == 0x1fu) {
        st->short_report = 1;
        st->short_report_type = (uint8_t)((st->status >> 8) & 3u);
        st->short_report_msg_ref = (uint8_t)st->status;
    }
    return tetra_bits_ok(r);
}

int
tetra_sds_decode(const uint8_t* bits, int nbits, tetra_sds_pdu* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    if (tetra_bits_read(&r, 3) != TETRA_MLE_PD_CMCE) {
        return 0;
    }
    out->type = (uint8_t)tetra_bits_read(&r, 5);
    if (!tetra_bits_ok(&r)) {
        return 0;
    }
    if (out->type == TETRA_CMCE_D_STATUS) {
        return sds_decode_status(&r, &out->status);
    }
    return out->type == TETRA_CMCE_D_SDS_DATA && sds_decode_data(&r, &out->data);
}

/* Acting on a complete D-SDS-DATA. */

/* The sender of @p calling's concatenation part, in the network the decoder is on. */
static sds_concat_sender
sds_concat_sender_of(const tetra_state_t* ts, const tetra_cmce_party* calling) {
    sds_concat_sender from = {calling->ssi, SDS_CONCAT_NETWORK_UNKNOWN, calling->type};
    if (calling->type == 2 && calling->extension_valid) {
        from.network = calling->extension;
        from.type = 1;
    } else if (calling->type == 1 && ts->net_known) {
        from.network = ((uint32_t)ts->mcc << 14) | ts->mnc;
    }
    return from;
}

/* Add a concatenation part to the reassembly and, when it completes the message, decode the message's text into
 * @p d. SDS_CONCAT_REJECTED when the part conflicts with the parts already held, or the message text is malformed. */
static int
sds_reassemble(const mle_ctx* ctx, tetra_sds_data* d, sds_concat_status* concat) {
    sds_concat_part part;
    DSD_MEMSET(&part, 0, sizeof part);
    part.data = tetra_bits_reader(ctx->bits, d->concat.data_pos + d->concat.data_bits, d->concat.data_pos);
    part.src = sds_concat_sender_of(ctx->ts, &d->calling);
    part.payload_pid = d->concat.payload_pid;
    part.reference = d->concat.reference;
    part.with_transport = d->concat.with_transport;
    part.total = d->concat.total;
    part.sequence = d->concat.sequence;
    const int result = sds_concat_submit(ctx->ts, &part, concat);
    concat->duplicate = (uint8_t)(result == SDS_CONCAT_DUPLICATE);
    concat->complete = (uint8_t)(result == SDS_CONCAT_COMPLETE);
    if (concat->complete) {
        tetra_bit_reader assembled = tetra_bits_reader(concat->assembled, concat->assembled_bits, 0);
        if (!sds_decode_message_text(&assembled, concat->assembled_pid, &d->text)) {
            return SDS_CONCAT_REJECTED;
        }
    }
    return result;
}

static void
sds_report_concat(const mle_ctx* ctx, const tetra_sds_data* d, const sds_concat_status* concat) {
    DSD_FPRINTF(stderr, "[TETRA SDS CONCAT] CC=%d src_SSI=%u ref=%u part=%u/%u received=%u%s\n", ctx->cc,
                d->calling.ssi, d->concat.reference, d->concat.sequence, d->concat.total, concat->received,
                concat->complete ? " complete" : (concat->duplicate ? " duplicate" : ""));
}

/* A data notice for the event history (tetra_calls_notice()): who sent the message to whom, its text as the notice's
 * message, or what it held when it had no text. */
static void
sds_notice(const mle_ctx* ctx, const tetra_sds_data* d, const sds_concat_status* concat) {
    const uint32_t dst = tetra_calls_addressed_ssi(ctx->sdu);
    char notice[96];
    if (d->text.len > 0) {
        DSD_SNPRINTF(notice, sizeof notice, "SDS SRC: %u; TGT: %u;", (unsigned)d->calling.ssi, (unsigned)dst);
    } else if (d->sdti == 0) {
        DSD_SNPRINTF(notice, sizeof notice, "SDS SRC: %u; TGT: %u; DATA: 0x%04X;", (unsigned)d->calling.ssi,
                     (unsigned)dst, (unsigned)d->short_data);
    } else {
        DSD_SNPRINTF(notice, sizeof notice, "SDS SRC: %u; TGT: %u; DATA: %d bits;", (unsigned)d->calling.ssi,
                     (unsigned)dst, concat->complete ? concat->assembled_bits : d->data_bits);
    }
    tetra_calls_notice(ctx->opts, ctx->state, 0, d->calling.ssi, dst, notice, d->text.len > 0 ? d->text.text : NULL);
}

static void
sds_report(const mle_ctx* ctx, const tetra_sds_data* d, const sds_concat_status* concat) {
    DSD_FPRINTF(stderr, "[TETRA CMCE D-SDS-DATA] CC=%d src_SSI=%u CPTI=%u SDTI=%u data_bits=%d\n", ctx->cc,
                d->calling.ssi, d->calling.type, d->sdti, d->data_bits);
    sds_notice(ctx, d, concat);
}

/* D-SDS-DATA, table 14.13: a whole message, or a concatenation part, which completes a message when it is the last of
 * its parts to arrive. */
static int
parse_cmce_d_sds_data(tetra_bit_reader* r, const mle_ctx* ctx) {
    tetra_sds_data data;
    tetra_sds_data* const d = &data;
    sds_concat_status concat;
    DSD_MEMSET(d, 0, sizeof *d);
    DSD_MEMSET(&concat, 0, sizeof concat);
    int ok = sds_decode_data(r, d);
    if (ok && d->concat_present) {
        ok = sds_reassemble(ctx, d, &concat) != SDS_CONCAT_REJECTED;
        if (ok) {
            sds_report_concat(ctx, d, &concat);
        }
    }
    if (ok && (!d->concat_present || concat.complete)) {
        sds_report(ctx, d, &concat);
    }
    free(concat.assembled);
    return ok ? 1 : cmce_reject(ctx, "D-SDS-DATA");
}

/* D-STATUS, table 14.14: a control notice with the status. */
static int
parse_cmce_d_status(tetra_bit_reader* r, const mle_ctx* ctx) {
    tetra_cmce_status st;
    DSD_MEMSET(&st, 0, sizeof st);
    if (!sds_decode_status(r, &st)) {
        return cmce_reject(ctx, "D-STATUS");
    }
    DSD_FPRINTF(stderr, "[TETRA CMCE D-STATUS] CC=%d src_SSI=%u status=0x%04X\n", ctx->cc, st.calling.ssi, st.status);
    const uint32_t dst = tetra_calls_addressed_ssi(ctx->sdu);
    char notice[64];
    DSD_SNPRINTF(notice, sizeof notice, "STATUS SRC: %u; TGT: %u; STATUS: 0x%04X;", (unsigned)st.calling.ssi,
                 (unsigned)dst, (unsigned)st.status);
    tetra_calls_notice(ctx->opts, ctx->state, 1, st.calling.ssi, dst, notice, NULL);
    return 1;
}

/* CMCE PDU dispatch, table 14.66. */

typedef int (*cmce_parser)(tetra_bit_reader* r, const mle_ctx* ctx);

static const cmce_parser k_cmce_parsers[32] = {
    [TETRA_CMCE_D_ALERT] = parse_cmce_d_alert,
    [TETRA_CMCE_D_CALL_PROCEEDING] = parse_cmce_d_call_proceeding,
    [TETRA_CMCE_D_CONNECT] = parse_cmce_d_connect,
    [TETRA_CMCE_D_CONNECT_ACK] = parse_cmce_d_connect_ack,
    [TETRA_CMCE_D_DISCONNECT] = parse_cmce_d_disconnect,
    [TETRA_CMCE_D_INFO] = parse_cmce_d_info,
    [TETRA_CMCE_D_RELEASE] = parse_cmce_d_release,
    [TETRA_CMCE_D_SETUP] = parse_cmce_d_setup,
    [TETRA_CMCE_D_STATUS] = parse_cmce_d_status,
    [TETRA_CMCE_D_TX_CEASED] = parse_cmce_d_tx_ceased,
    [TETRA_CMCE_D_TX_CONTINUE] = parse_cmce_d_tx_continue,
    [TETRA_CMCE_D_TX_GRANTED] = parse_cmce_d_tx_granted,
    [TETRA_CMCE_D_TX_WAIT] = parse_cmce_d_tx_wait,
    [TETRA_CMCE_D_TX_INTERRUPT] = parse_cmce_d_tx_interrupt,
    [TETRA_CMCE_D_CALL_RESTORE] = parse_cmce_d_call_restore,
    [TETRA_CMCE_D_SDS_DATA] = parse_cmce_d_sds_data,
    [TETRA_CMCE_D_FACILITY] = parse_cmce_d_facility,
    [TETRA_CMCE_FUNCTION_NOT_SUPPORTED] = parse_cmce_function_not_supported,
};

static void
parse_cmce(tetra_bit_reader* r, const mle_ctx* ctx) {
    const tetra_state_t* const ts = ctx->ts;
    const uint32_t pdu_type = tetra_bits_read(r, 5);
    if (!tetra_bits_ok(r)) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA CMCE] CC=%d (too short: %d bits)\n", ctx->cc, ctx->nbits);
        return;
    }
    const cmce_parser parser = k_cmce_parsers[pdu_type & 31u];
    if (!parser) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA CMCE] CC=%d pdu_type=%u (%d bits)\n", ctx->cc, pdu_type, ctx->nbits);
        return;
    }
    (void)parser(r, ctx);
}

/* SNDCP (clause 28.4.4). */

/* SN PDU types (table 28.121) whose second element is the NSAPI: ACTIVATE PDP CONTEXT ACCEPT, ACTIVATE PDP CONTEXT
 * REJECT, UNITDATA, DATA, DATA TRANSMIT REQUEST, DATA TRANSMIT RESPONSE and PAGE REQUEST. */
static int
sndcp_type_has_nsapi(uint32_t type) {
    static const uint16_t k_mask = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 10);
    return (k_mask >> type) & 1u;
}

/* The SN PDU type and, for the types that carry one, the NSAPI; @p r is after the protocol discriminator. */
static int
sndcp_read_header(tetra_bit_reader* r, tetra_sndcp_pdu* out) {
    out->nbits = (uint16_t)tetra_bits_left(r);
    out->pdu_type = (uint8_t)tetra_bits_read(r, 4);
    out->nsapi = sndcp_type_has_nsapi(out->pdu_type) ? (uint8_t)tetra_bits_read(r, 4) : 0U;
    return tetra_bits_ok(r);
}

int
tetra_sndcp_decode(const uint8_t* bits, int nbits, tetra_sndcp_pdu* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    return tetra_bits_read(&r, 3) == TETRA_MLE_PD_SNDCP && tetra_bits_ok(&r) && sndcp_read_header(&r, out);
}

/* Packet data is not decoded further; an SN-PDU too short for its header is noted with -Z. */
static void
parse_sndcp(tetra_bit_reader* r, const mle_ctx* ctx) {
    const int nbits = tetra_bits_left(r);
    tetra_sndcp_pdu pdu;
    DSD_MEMSET(&pdu, 0, sizeof pdu);
    if (!sndcp_read_header(r, &pdu)) {
        TETRA_LOG_PAYLOAD(ctx->ts, "[TETRA SNDCP] CC=%d (too short: %d bits)\n", ctx->cc, nbits);
    }
}

/* MLE protocol PDUs (clause 18.4.1.4). */

static void
mle_reject(const mle_ctx* ctx, const char* name) {
    TETRA_LOG_PAYLOAD(ctx->ts, "[TETRA MLE %s] CC=%d (truncated or malformed: %d bits)\n", name, ctx->cc, ctx->nbits);
}

/* D-NEW-CELL (table 18.6): channel command valid. No type-2 elements are defined, so the O-bit must be clear; an MM
 * SDU may follow. */
static int
mle_decode_new_cell(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    pdu->channel_command = (uint8_t)tetra_bits_read(r, 2);
    require_absent_type2(r, 1);
    pdu->sdu_pos = r->pos;
    return tetra_bits_ok(r);
}

/* D-PREPARE-FAIL (table 18.7): fail cause, the clear O-bit, an optional MM SDU. */
static int
mle_decode_prepare_fail(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    pdu->fail_cause = (uint8_t)tetra_bits_read(r, 2);
    require_absent_type2(r, 1);
    pdu->sdu_pos = r->pos;
    return tetra_bits_ok(r);
}

/* D-RESTORE-ACK (table 18.8): the clear O-bit and a CMCE D-CALL RESTORE SDU (Annex E table E.23). */
static int
mle_decode_restore_ack(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    require_absent_type2(r, 1);
    if (tetra_bits_read(r, 5) != TETRA_CMCE_D_CALL_RESTORE) {
        bits_reject(r);
    }
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    pdu->restore_ack.call_present = 1;
    return cmce_read_call_restore(r, &pdu->restore_ack.call);
}

/* D-RESTORE-FAIL (table 18.9): fail cause and the clear O-bit. */
static int
mle_decode_restore_fail(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    pdu->fail_cause = (uint8_t)tetra_bits_read(r, 2);
    require_absent_type2(r, 1);
    return tetra_bits_ok(r);
}

/* D-CHANNEL RESPONSE (table 18.10). Its two type-2 elements shall not be included, so the O-bit must be clear. */
static int
mle_decode_channel_response(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    tetra_mle_channel_response* const c = &pdu->channel_response;
    c->type = (uint8_t)tetra_bits_read(r, 1);
    c->reason = (uint8_t)tetra_bits_read(r, 3);
    c->retry_delay = (uint8_t)tetra_bits_read(r, 4);
    require_absent_type2(r, 1);
    return tetra_bits_ok(r);
}

/* Network broadcast elements, clause 18.5. */

/* Neighbour cell information for CA (table 18.64): five type-1 elements, then ten type-2 elements of which only their
 * presence is checked. */
static void
mle_read_ca_neighbor(tetra_bit_reader* r, tetra_mle_ca_neighbor* n) {
    static const uint8_t k_type2[10] = {10, 10, 14, 14, 3, 4, 16, 12, 5, 6};
    n->cell_id = (uint8_t)tetra_bits_read(r, 5);
    n->reselect_types = (uint8_t)tetra_bits_read(r, 2);
    (void)tetra_bits_skip(r, 3); /* neighbour cell synchronized, cell load CA */
    n->main_carrier = (uint16_t)tetra_bits_read(r, 12);
    if (tetra_bits_present(r)) {
        uint32_t values[10];
        (void)read_type2_elements(r, k_type2, 10, values);
    }
}

/* Neighbour cell information for DA (table 18.65); its three trailing reserved type-2 elements shall not be used. */
static void
mle_read_da_neighbor(tetra_bit_reader* r, tetra_mle_da_neighbor* n) {
    static const uint8_t k_type2[11] = {10, 10, 14, 14, 8, 3, 4, 16, 7, 3, 6};
    n->cell_id = (uint8_t)tetra_bits_read(r, 8);
    (void)tetra_bits_skip(r, 12); /* reselection types DA, synchronized, wide-area cell, C/I path measurements */
    n->load_known = (uint8_t)tetra_bits_read(r, 1);
    if (n->load_known) {
        n->load = (uint8_t)tetra_bits_read(r, 3);
    }
    n->main_carrier = (uint16_t)tetra_bits_read(r, 12);
    (void)tetra_bits_skip(r, 6); /* main control channel modulation mode, bandwidth */
    if (tetra_bits_present(r)) {
        uint32_t values[11];
        n->optional_mask = (uint16_t)read_type2_elements(r, k_type2, 11, values);
        n->carrier_extension = (uint16_t)values[0];
        n->la = (uint16_t)values[3];
        require_absent_type2(r, 3);
    }
}

static void
mle_read_channel_class(tetra_bit_reader* r, tetra_mle_channel_class* c) {
    c->id = (uint8_t)tetra_bits_read(r, 4);
    c->characteristics = tetra_bits_read(r, 18);
    c->bs_power = (uint8_t)tetra_bits_read(r, 5);
}

static void
mle_read_irregular_channel(tetra_bit_reader* r, tetra_mle_irregular_channel* c) {
    c->channel_id = (uint8_t)tetra_bits_read(r, 5);
    (void)tetra_bits_skip(r, 18); /* channel characteristics */
    c->carrier = (uint16_t)tetra_bits_read(r, 12);
    c->extension_valid = (uint8_t)tetra_bits_read(r, 1);
    c->carrier_extension = c->extension_valid ? (uint16_t)tetra_bits_read(r, 10) : 0;
}

static void
mle_read_serving_classes(tetra_bit_reader* r, tetra_mle_channel_lists* l) {
    l->serving_classes_present = 1;
    l->serving_class_count = (uint8_t)tetra_bits_read(r, 4);
    for (uint8_t i = 0; i < l->serving_class_count; i++) {
        mle_read_channel_class(r, &l->serving_class[i]);
    }
}

static void
mle_read_neighbor_classes(tetra_bit_reader* r, int cell_id_bits, tetra_mle_channel_lists* l) {
    l->neighbor_classes_present = 1;
    l->neighbor_class_count = (uint8_t)tetra_bits_read(r, 5);
    for (uint8_t i = 0; i < l->neighbor_class_count; i++) {
        l->neighbor_class[i].cell_id = (uint8_t)tetra_bits_read(r, cell_id_bits);
        mle_read_channel_class(r, &l->neighbor_class[i].channel_class);
    }
}

static void
mle_read_serving_irregular(tetra_bit_reader* r, tetra_mle_channel_lists* l) {
    l->serving_irregular_present = 1;
    l->serving_irregular_count = (uint8_t)tetra_bits_read(r, 5);
    for (uint8_t i = 0; i < l->serving_irregular_count; i++) {
        mle_read_irregular_channel(r, &l->serving_irregular[i]);
    }
}

static void
mle_read_neighbor_irregular(tetra_bit_reader* r, int cell_id_bits, tetra_mle_channel_lists* l) {
    l->neighbor_irregular_present = 1;
    l->neighbor_irregular_count = (uint8_t)tetra_bits_read(r, 6);
    for (uint8_t i = 0; i < l->neighbor_irregular_count; i++) {
        l->neighbor_irregular[i].cell_id = (uint8_t)tetra_bits_read(r, cell_id_bits);
        mle_read_irregular_channel(r, &l->neighbor_irregular[i].channel);
    }
}

/* The four channel list type-2 elements of tables 18.3 and 18.4; neighbour cells are identified by a cell identifier
 * CA (5 bits) or DA (8 bits). */
static void
mle_read_channel_lists(tetra_bit_reader* r, int cell_id_bits, tetra_mle_channel_lists* l) {
    if (tetra_bits_present(r)) {
        mle_read_serving_classes(r, l);
    }
    if (tetra_bits_present(r)) {
        mle_read_neighbor_classes(r, cell_id_bits, l);
    }
    if (tetra_bits_present(r)) {
        mle_read_serving_irregular(r, l);
    }
    if (tetra_bits_present(r)) {
        mle_read_neighbor_irregular(r, cell_id_bits, l);
    }
}

static void
mle_read_channel_removal(tetra_bit_reader* r, tetra_mle_channel_removal* c) {
    c->class_count = (uint8_t)tetra_bits_read(r, 4);
    for (uint8_t i = 0; i < c->class_count; i++) {
        c->class_id[i] = (uint8_t)tetra_bits_read(r, 4);
    }
    c->channel_count = (uint8_t)tetra_bits_read(r, 5);
    for (uint8_t i = 0; i < c->channel_count; i++) {
        c->channel_id[i] = (uint8_t)tetra_bits_read(r, 5);
    }
}

static void
mle_read_cell_removal(tetra_bit_reader* r, int cell_id_bits, tetra_mle_cell_removal* c) {
    c->cell_id = (uint8_t)tetra_bits_read(r, cell_id_bits);
    c->remove_cell = (uint8_t)tetra_bits_read(r, 1);
    if (!c->remove_cell) {
        mle_read_channel_removal(r, &c->channels);
    }
}

/* D-NWRK-BROADCAST, table 18.2. */

static int
mle_decode_nwrk_broadcast(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    tetra_mle_nwrk_broadcast* const b = &pdu->nwrk_broadcast;
    b->reselect = (uint16_t)tetra_bits_read(r, 16);
    b->load = (uint8_t)tetra_bits_read(r, 2);
    if (tetra_bits_present(r)) {
        if (tetra_bits_present(r)) {
            b->network_time = tetra_bits_read64(r, 48);
            b->network_time_valid = 1;
        }
        if (tetra_bits_present(r)) {
            b->neighbor_count_valid = 1;
            b->neighbor_count = (uint8_t)tetra_bits_read(r, 3);
            for (uint8_t i = 0; i < b->neighbor_count; i++) {
                mle_read_ca_neighbor(r, &b->neighbor[i]);
            }
        }
    }
    return tetra_bits_ok(r);
}

static void
mle_report_nwrk_broadcast(const mle_ctx* ctx, const tetra_mle_pdu* pdu) {
    const tetra_mle_nwrk_broadcast* const b = &pdu->nwrk_broadcast;
    DSD_FPRINTF(stderr, "[TETRA MLE D-NWRK-BROADCAST] CC=%d reselect=0x%04X load=%u neighbours=%u\n", ctx->cc,
                b->reselect, b->load, b->neighbor_count);
}

/* D-NWRK-BROADCAST EXTENSION, table 18.4. */

static int
mle_decode_nwrk_broadcast_ext(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    if (tetra_bits_present(r)) {
        mle_read_channel_lists(r, 5, &pdu->ext);
        require_absent_type2(r, 2); /* two reserved elements that shall not be used */
    }
    return tetra_bits_ok(r);
}

static void
mle_report_nwrk_broadcast_ext(const mle_ctx* ctx, const tetra_mle_pdu* pdu) {
    const tetra_mle_channel_lists* const l = &pdu->ext;
    DSD_FPRINTF(stderr, "[TETRA MLE D-NWRK-BROADCAST-EXT] CC=%d classes=%u/%u irregular=%u/%u\n", ctx->cc,
                l->serving_class_count, l->neighbor_class_count, l->serving_irregular_count,
                l->neighbor_irregular_count);
}

/* D-NWRK-BROADCAST-DA, table 18.3. */

static void
mle_read_da_neighbors(tetra_bit_reader* r, tetra_mle_da_broadcast* b) {
    b->neighbor_list_present = 1;
    b->neighbor_count = (uint8_t)tetra_bits_read(r, 3);
    for (uint8_t i = 0; i < b->neighbor_count; i++) {
        mle_read_da_neighbor(r, &b->neighbor[i]);
    }
}

/* The type-2 elements after the O-bit: a reserved one that shall not be used, cell identifier, re-select parameters,
 * cell load, network time, the local CA cell, DA neighbours, the four channel lists, and two more reserved ones. */
static void
mle_read_da_optional(tetra_bit_reader* r, tetra_mle_da_broadcast* b) {
    static const uint8_t k_type2[3] = {8, 16, 3}; /* cell identifier DA, cell re-select parameters, cell load DA */
    uint32_t values[3];
    require_absent_type2(r, 1);
    const unsigned present = read_type2_elements(r, k_type2, 3, values);
    b->cell_id = (uint8_t)values[0];
    b->cell_id_valid = type2_present(present, 0);
    b->reselect = (uint16_t)values[1];
    b->reselect_valid = type2_present(present, 1);
    b->load = (uint8_t)values[2];
    b->load_valid = type2_present(present, 2);
    if (tetra_bits_present(r)) {
        b->network_time = tetra_bits_read64(r, 48);
        b->network_time_valid = 1;
    }
    if (tetra_bits_present(r)) {
        b->local_ca_valid = 1;
        mle_read_ca_neighbor(r, &b->local_ca);
    }
    if (tetra_bits_present(r)) {
        mle_read_da_neighbors(r, b);
    }
    mle_read_channel_lists(r, 8, &b->lists);
    require_absent_type2(r, 2);
}

static int
mle_decode_nwrk_broadcast_da(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    if (tetra_bits_read(r, 3) != 0) { /* reserved, set to 000 */
        bits_reject(r);
    }
    if (tetra_bits_present(r)) {
        mle_read_da_optional(r, &pdu->da);
    }
    return tetra_bits_ok(r);
}

static void
mle_report_nwrk_broadcast_da(const mle_ctx* ctx, const tetra_mle_pdu* pdu) {
    const tetra_mle_da_broadcast* const b = &pdu->da;
    DSD_FPRINTF(stderr, "[TETRA MLE D-NWRK-BROADCAST-DA] CC=%d neighbours=%u classes=%u/%u irregular=%u/%u\n", ctx->cc,
                b->neighbor_count, b->lists.serving_class_count, b->lists.neighbor_class_count,
                b->lists.serving_irregular_count, b->lists.neighbor_irregular_count);
}

/* D-NWRK-BROADCAST REMOVE, table 18.5. */

static void
mle_read_removal_optional(tetra_bit_reader* r, tetra_mle_broadcast_removal* b) {
    if (tetra_bits_present(r)) {
        b->ca_present = 1;
        b->ca_count = (uint8_t)tetra_bits_read(r, 5);
        for (uint8_t i = 0; i < b->ca_count; i++) {
            mle_read_cell_removal(r, 5, &b->ca[i]);
        }
    }
    if (tetra_bits_present(r)) {
        b->da_present = 1;
        b->da_count = (uint8_t)tetra_bits_read(r, 8);
        for (unsigned i = 0; i < b->da_count; i++) {
            mle_read_cell_removal(r, 8, &b->da[i]);
        }
    }
    if (tetra_bits_present(r)) {
        b->serving_present = 1;
        mle_read_channel_removal(r, &b->serving);
    }
    require_absent_type2(r, 4); /* four reserved elements that shall not be used */
}

static int
mle_decode_nwrk_broadcast_remove(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    if (tetra_bits_present(r)) {
        mle_read_removal_optional(r, &pdu->remove);
    }
    return tetra_bits_ok(r);
}

static void
mle_report_nwrk_broadcast_remove(const mle_ctx* ctx, const tetra_mle_pdu* pdu) {
    const tetra_mle_broadcast_removal* const b = &pdu->remove;
    DSD_FPRINTF(stderr, "[TETRA MLE D-NWRK-BROADCAST-REMOVE] CC=%d CA=%u DA=%u serving=%u\n", ctx->cc, b->ca_count,
                b->da_count, b->serving_present);
}

/* MLE PDU decoding, tables 18.85 and 18.86. */

typedef int (*mle_decoder)(tetra_bit_reader* r, tetra_mle_pdu* pdu);

static int
mle_decode_extended(tetra_bit_reader* r, tetra_mle_pdu* pdu) {
    pdu->extension = (uint8_t)tetra_bits_read(r, 4);
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    if (pdu->extension == TETRA_MLE_D_NWRK_BROADCAST_DA) {
        return mle_decode_nwrk_broadcast_da(r, pdu);
    }
    if (pdu->extension == TETRA_MLE_D_NWRK_BCAST_REMOVE) {
        return mle_decode_nwrk_broadcast_remove(r, pdu);
    }
    return 0;
}

int
tetra_mle_decode(const uint8_t* bits, int nbits, tetra_mle_pdu* out) {
    static const mle_decoder k_decoders[8] = {
        [TETRA_MLE_D_NEW_CELL] = mle_decode_new_cell,
        [TETRA_MLE_D_PREPARE_FAIL] = mle_decode_prepare_fail,
        [TETRA_MLE_D_NWRK_BROADCAST] = mle_decode_nwrk_broadcast,
        [TETRA_MLE_D_NWRK_BCAST_EXT] = mle_decode_nwrk_broadcast_ext,
        [TETRA_MLE_D_RESTORE_ACK] = mle_decode_restore_ack,
        [TETRA_MLE_D_RESTORE_FAIL] = mle_decode_restore_fail,
        [TETRA_MLE_D_CHANNEL_RESPONSE] = mle_decode_channel_response,
        [TETRA_MLE_EXTENDED_PDU] = mle_decode_extended,
    };
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    if (tetra_bits_read(&r, 3) != TETRA_MLE_PD_MLE) {
        return 0;
    }
    out->type = (uint8_t)tetra_bits_read(&r, 3);
    return tetra_bits_ok(&r) && k_decoders[out->type & 7u](&r, out);
}

/* Acting on a complete MLE PDU. */

/* The SDU after the fields of a D-NEW-CELL or D-PREPARE-FAIL is an MM PDU without its protocol discriminator (tables
 * 18.6 and 18.7). */
static void
mle_report_mm_sdu(const mle_ctx* ctx, const tetra_mle_pdu* pdu) {
    if (ctx->nbits - pdu->sdu_pos > 0) {
        tetra_mm_dispatch(ctx->bits + pdu->sdu_pos, ctx->nbits - pdu->sdu_pos, ctx->cc, ctx->opts, ctx->state);
    }
}

static void
mle_report_restore_ack(const mle_ctx* ctx, const tetra_mle_pdu* pdu) {
    cmce_report_call_restore(ctx, &pdu->restore_ack.call);
}

/* How a downlink MLE PDU is named and acted on. D-RESTORE-FAIL and D-CHANNEL RESPONSE concern one MS's cell
 * reselection or channel request; nothing acts on them. */
typedef struct {
    const char* name; /* in the truncation note */
    void (*report)(const mle_ctx* ctx, const tetra_mle_pdu* pdu);
} mle_kind;

static const mle_kind k_mle_kinds[8] = {
    [TETRA_MLE_D_NEW_CELL] = {"D-NEW-CELL", mle_report_mm_sdu},
    [TETRA_MLE_D_PREPARE_FAIL] = {"D-PREPARE-FAIL", mle_report_mm_sdu},
    [TETRA_MLE_D_NWRK_BROADCAST] = {"D-NWRK-BROADCAST", mle_report_nwrk_broadcast},
    [TETRA_MLE_D_NWRK_BCAST_EXT] = {"D-NWRK-BROADCAST-EXT", mle_report_nwrk_broadcast_ext},
    [TETRA_MLE_D_RESTORE_ACK] = {"D-RESTORE-ACK", mle_report_restore_ack},
    [TETRA_MLE_D_RESTORE_FAIL] = {"D-RESTORE-FAIL", NULL},
    [TETRA_MLE_D_CHANNEL_RESPONSE] = {"D-CHANNEL-RESPONSE", NULL},
    [TETRA_MLE_EXTENDED_PDU] = {NULL, NULL},
};

static const mle_kind k_mle_extended_kinds[2] = {
    [TETRA_MLE_D_NWRK_BROADCAST_DA] = {"D-NWRK-BROADCAST-DA", mle_report_nwrk_broadcast_da},
    [TETRA_MLE_D_NWRK_BCAST_REMOVE] = {"D-NWRK-BROADCAST-REMOVE", mle_report_nwrk_broadcast_remove},
};

/* The kind of the decoded PDU; NULL when the TM-SDU ends before its PDU type, or before or at an extension that is not
 * decoded, which goes unremarked. */
static const mle_kind*
mle_kind_of(const mle_ctx* ctx, const tetra_mle_pdu* pdu) {
    if (ctx->nbits < 6) {
        return NULL;
    }
    if (pdu->type != TETRA_MLE_EXTENDED_PDU) {
        return &k_mle_kinds[pdu->type & 7u];
    }
    if (ctx->nbits < 10 || pdu->extension > TETRA_MLE_D_NWRK_BCAST_REMOVE) {
        return NULL;
    }
    return &k_mle_extended_kinds[pdu->extension];
}

static void
parse_mle(const mle_ctx* ctx) {
    tetra_mle_pdu pdu;
    const int complete = tetra_mle_decode(ctx->bits, ctx->nbits, &pdu);
    const mle_kind* const kind = mle_kind_of(ctx, &pdu);
    if (!kind) {
        return;
    }
    if (complete) {
        if (kind->report) {
            kind->report(ctx, &pdu);
        }
    } else if (pdu.type == TETRA_MLE_D_RESTORE_ACK && pdu.restore_ack.call_present) {
        (void)cmce_reject(ctx, "D-CALL-RESTORE");
    } else {
        mle_reject(ctx, kind->name);
    }
}

/* Entry point. */

void
tetra_mle_dispatch_sdu(const uint8_t* bits, int nbits, int cc, tetra_mac_sdu* sdu, dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!bits || !ts) {
        return;
    }
    const mle_ctx ctx = {opts, state, ts, sdu, bits, cc, nbits};
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    const uint32_t protocol = tetra_bits_read(&r, 3); /* protocol discriminator, table 18.87 */
    if (!tetra_bits_ok(&r)) {
        return;
    }
    switch (protocol) {
        case TETRA_MLE_PD_MM: tetra_mm_dispatch(bits + 3, nbits - 3, cc, opts, state); break;
        case TETRA_MLE_PD_CMCE: parse_cmce(&r, &ctx); break;
        case TETRA_MLE_PD_SNDCP: parse_sndcp(&r, &ctx); break;
        case TETRA_MLE_PD_MLE: parse_mle(&ctx); break;
        default:
            TETRA_LOG_PAYLOAD(ts, "[TETRA MLE] CC=%d protocol_discriminator=%u (%d bits)\n", cc, protocol, nbits);
            break;
    }
}

void
tetra_mle_dispatch(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state) {
    tetra_mle_dispatch_sdu(bits, nbits, cc, NULL, opts, state);
}
