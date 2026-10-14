// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MLE routing and the CMCE, SDS and SNDCP decoders (EN 300 392-2 clauses 14, 18, 28 and 29): every PDU against
 * its ETSI table, and the rule that a truncated or malformed PDU publishes nothing.
 */

#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_mle_pdu.h"
#include "tetra_state.h"
#include "tetra_test_log.h"
#include "tetra_test_pdu.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Write @nbits bits from @val (MSB-first) into out[offset..]. */
static void
pack_bits(uint8_t* out, uint32_t val, int offset, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        out[offset++] = (uint8_t)((val >> i) & 1u);
    }
}

/* Expand a frozen MSB-first hexadecimal PDU into the decoder's one-byte-per-bit
 * representation. Golden vectors use this instead of the field builder above
 * so a shared offset error cannot make both the input and expectation pass. */
static void
unpack_hex_bits(const char* hex, uint8_t* bits, int nbits) {
    for (int bit = 0; bit < nbits; bit++) {
        unsigned char ch = (unsigned char)hex[bit / 4];
        unsigned value = ch - 'a' + 10u;
        if (ch >= '0' && ch <= '9') {
            value = ch - '0';
        } else if (ch >= 'A' && ch <= 'F') {
            value = ch - 'A' + 10u;
        }
        bits[bit] = (uint8_t)((value >> (3 - (bit & 3))) & 1u);
    }
}

static dsd_state*
alloc_state(void) {
    return (dsd_state*)calloc(1, sizeof(dsd_state));
}

static dsd_opts*
alloc_opts(void) {
    return (dsd_opts*)calloc(1, sizeof(dsd_opts));
}

/* Frees the TETRA state extension (and the SDS reassembly it owns) and the event history with the decoder state. */
static void
free_state(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(state);
}

/* A decoder state with an event history and a carrier a CRC-passed block proved: decoded short data and status reach
 * the history as notices (tetra_calls_notice()). */
static dsd_state*
alloc_notice_state(void) {
    dsd_state* state = alloc_state();
    assert(state != NULL);
    state->event_history_s = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (state->event_history_s) {
        init_event_history(&state->event_history_s[0], 0, 255);
        init_event_history(&state->event_history_s[1], 0, 255);
    }
    tetra_confirm_note_signalling(state);
    return state;
}

/* Notices pushed to @p state's history so far. */
static uint64_t
notices(const dsd_state* state) {
    return state->event_history_s ? state->event_history_s[0].push_seq : 0U;
}

/* The notice @p back pushes ago (1 for the last). */
static const Event_History*
notice(const dsd_state* state, int back) {
    return &state->event_history_s[0].Event_History_Items[back];
}

/* The message of the notice @p back pushes ago; empty without an event history. */
static const char*
notice_text(const dsd_state* state, int back) {
    return state->event_history_s ? state->event_history_s[0].Event_History_Items[back].text_message : "";
}

/* A D-SETUP (table 14.15) for group call @p call_id without optional elements, decoded without its MAC PDU. */
static void
dispatch_setup(dsd_opts* opts, dsd_state* state, uint16_t call_id) {
    uint8_t bits[64];
    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, TETRA_CMCE_D_SETUP, 3, 5);
    pack_bits(bits, call_id, 8, 14);
    pack_bits(bits, 4u, 22, 4);                   /* call time-out */
    pack_bits(bits, 0x04u, 28, 8);                /* basic service: clear speech, point to multipoint */
    pack_bits(bits, 3u, 36, 2);                   /* transmission granted to another user */
    tetra_mle_dispatch(bits, 44, 0, opts, state); /* the O-bit, 0, is bit 43 */
}

/* Table 14.18 D-TX GRANTED, transmission grant 3 (granted to another user), with TPTI 1 and the transmitting SSI. */
static int
test_d_tx_granted(void) {
    const uint32_t TEST_GRANTED_SSI = 0x4C1A77u; /* 5053047 */

    enum { NBITS = 57 };

    uint8_t bits[NBITS];
    DSD_MEMSET(bits, 0, sizeof bits);

    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, 11u, 3, 5);
    pack_bits(bits, 7u, 8, 14);
    pack_bits(bits, 3u, 22, 2);
    pack_bits(bits, 1u, 24, 1);
    pack_bits(bits, 0u, 25, 1);
    pack_bits(bits, 0u, 26, 1);
    pack_bits(bits, 1u, 27, 1); /* O-bit */
    pack_bits(bits, 0u, 28, 1); /* notification absent */
    pack_bits(bits, 1u, 29, 1); /* TPTI present */
    pack_bits(bits, 1u, 30, 2);
    pack_bits(bits, TEST_GRANTED_SSI, 32, 24);
    pack_bits(bits, 0u, 56, 1); /* terminating M-bit */

    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();

    dispatch_setup(opts, state, 7);
    tetra_mle_dispatch(bits, NBITS, 0, opts, state);

    int ok = 1;
    const tetra_call_t* const c = tetra_calls_peek(state, 7);
    if (!c || c->phase != TETRA_CALL_TX_GRANTED) {
        DSD_FPRINTF(stderr, "FAIL(d_tx_granted): the call's transmission is not granted\n");
        ok = 0;
    }
    if (c && c->talker_ssi != TEST_GRANTED_SSI) {
        DSD_FPRINTF(stderr, "FAIL(d_tx_granted): talker=0x%06X expect=0x%06X\n", (unsigned)c->talker_ssi,
                    (unsigned)TEST_GRANTED_SSI);
        ok = 0;
    }

    free_state(state);
    free(opts);
    return ok;
}

/* Table 14.16 D-TX CEASED without optional elements ends an active grant. */
static int
test_d_tx_ceased(void) {
    enum { NBITS = 24 };

    uint8_t bits[NBITS];
    DSD_MEMSET(bits, 0, sizeof bits);

    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, 9u, 3, 5);
    pack_bits(bits, 42u, 8, 14);
    pack_bits(bits, 1u, 22, 1);
    pack_bits(bits, 0u, 23, 1); /* O-bit: no optionals */

    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    dispatch_setup(opts, state, 42); /* pre-condition: call 42 has a transmission */
    tetra_calls_find(state, 42)->phase = TETRA_CALL_TX_GRANTED;
    tetra_calls_find(state, 42)->talker_ssi = 0x123456u;

    tetra_mle_dispatch(bits, NBITS, 0, opts, state);

    int ok = 1;
    const tetra_call_t* const c = tetra_calls_peek(state, 42);
    if (!c || c->phase != TETRA_CALL_CEASED || c->talker_ssi != 0u) {
        DSD_FPRINTF(stderr, "FAIL(d_tx_ceased): the call's transmission did not cease\n");
        ok = 0;
    }

    free_state(state);
    free(opts);
    return ok;
}

static int
test_d_setup_truncated_optional_rejected(void) {
    uint8_t cmce[48] = {0};
    uint8_t mle[64] = {0};
    int off = 0;
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();

    pack_bits(cmce, TETRA_CMCE_D_SETUP, off, 5);
    off += 5;
    pack_bits(cmce, 0x1234, off, 14);
    off += 14;
    pack_bits(cmce, 9, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off += 1;
    pack_bits(cmce, 1, off, 1);
    off += 1;
    pack_bits(cmce, 0x32, off, 8);
    off += 8;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off += 1;
    pack_bits(cmce, 7, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off += 1; /* notification claimed, payload absent */

    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);

    int ok = tetra_calls_peek(state, 0x1234) == NULL;
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL(d_setup_truncated): malformed optional IE changed call state\n");
    }
    free_state(state);
    free(opts);
    return ok;
}

static void
wrap_cmce(uint8_t* mle, const uint8_t* cmce, int cmce_nbits) {
    DSD_MEMSET(mle, 0, 128);
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)cmce_nbits);
}

static int
test_floor_control_truncated_optionals_rejected(void) {
    uint8_t cmce[96] = {0};
    uint8_t mle[128] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int ok = 1;
    int off = 0;

    /* D-TX-GRANTED: a present notification must carry all six bits. */
    pack_bits(cmce, TETRA_CMCE_D_TX_GRANTED, off, 5);
    off += 5;
    pack_bits(cmce, 7, off, 14);
    off += 14;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off += 1;
    pack_bits(cmce, 0, off, 1);
    off += 1;
    pack_bits(cmce, 0, off, 1);
    off += 1;
    pack_bits(cmce, 1, off, 1);
    off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off += 1; /* notification present, value absent */
    dispatch_setup(opts, state, 7);
    dispatch_setup(opts, state, 8);
    tetra_calls_find(state, 8)->phase = TETRA_CALL_TX_GRANTED;
    dispatch_setup(opts, state, 9);
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    if (tetra_calls_peek(state, 7)->phase != TETRA_CALL_SETUP || tetra_calls_peek(state, 7)->talker_ssi != 0u) {
        ok = 0;
    }

    /* D-TX-CEASED: malformed notification must not clear an active grant. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_CEASED, off, 5);
    off += 5;
    pack_bits(cmce, 8, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 1);
    off += 1;
    pack_bits(cmce, 1, off, 1);
    off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off += 1; /* notification present, value absent */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    if (tetra_calls_peek(state, 8)->phase != TETRA_CALL_TX_GRANTED) {
        ok = 0;
    }

    /* D-TX-WAIT: malformed notification must not publish an event. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, off, 5);
    off += 5;
    pack_bits(cmce, 9, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 1);
    off += 1;
    pack_bits(cmce, 1, off, 1);
    off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off += 1; /* notification present, value absent */
    char log[512];
    const double seen_m = tetra_calls_peek(state, 9)->seen_m;
    wrap_cmce(mle, cmce, off);
    tetra_test_mle_logged(mle, off + 3, 0, opts, state, log, sizeof log);
    if (strstr(log, "D-TX-WAIT] CC=0 call=9") != NULL || tetra_calls_peek(state, 9)->seen_m > seen_m) {
        ok = 0;
    }

    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL(floor_control_truncated): malformed optional IE changed state\n");
    }
    free_state(state);
    free(opts);
    return ok;
}

/* A release ends the call it names, and only that one: a release of a call no record holds does nothing. */
static int
test_release_is_scoped_to_active_call(void) {
    uint8_t cmce[32] = {0};
    uint8_t mle[128] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;
    int ok = 1;

    dispatch_setup(opts, state, 0x1234u);
    tetra_calls_find(state, 0x1234u)->phase = TETRA_CALL_TX_GRANTED;
    tetra_calls_find(state, 0x1234u)->talker_ssi = 0x112233u;

    pack_bits(cmce, TETRA_CMCE_D_RELEASE, off, 5);
    off += 5;
    pack_bits(cmce, 0x1235u, off, 14);
    off += 14;
    pack_bits(cmce, 19, off, 5);
    off += 5;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    const tetra_call_t* c = tetra_calls_peek(state, 0x1234u);
    if (!c || c->phase != TETRA_CALL_TX_GRANTED || c->talker_ssi != 0x112233u || tetra_calls_peek(state, 0x1235u)) {
        ok = 0;
    }

    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_DISCONNECT, off, 5);
    off += 5;
    pack_bits(cmce, 0x1234u, off, 14);
    off += 14;
    pack_bits(cmce, 7, off, 5);
    off += 5;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    char log[256];
    wrap_cmce(mle, cmce, off);
    tetra_test_mle_logged(mle, off + 3, 0, opts, state, log, sizeof log);
    if (tetra_calls_peek(state, 0x1234u) != NULL
        || strstr(log, "[TETRA CMCE D-DISCONNECT] CC=0 call=4660 cause=7") == NULL) {
        ok = 0;
    }

    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL(release_scope): unrelated release changed active call\n");
    }
    free_state(state);
    free(opts);
    return ok;
}

/* Tables 14.15, 14.14, 14.12, 14.4, 14.5, 14.7, 14.8, 14.11, 14.6 and 14.33: mandatory and optional fields. */
static int
test_standard_setup_release_status(void) {
    uint8_t cmce[160] = {0}, mle[192];
    char log[512];
    int n = 0;
    int ok = 1;
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    /* Table 14.15 mandatory D-SETUP plus absent notification/temp and CPTI=1. */
    int off = 0;
    pack_bits(cmce, TETRA_CMCE_D_SETUP, off, 5);
    off += 5;
    pack_bits(cmce, 0x1234, off, 14);
    off += 14;
    pack_bits(cmce, 9, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x32, off, 8);
    off += 8; /* circuit=1, enc=1, com=0, slots=2 */
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 7, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 2);
    off += 2;
    pack_bits(cmce, 0x654321, off, 24);
    off += 24;
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    n = off + 3;
    tetra_mle_dispatch(mle, n, 2, opts, state);
    const tetra_call_t* c = tetra_calls_peek(state, 0x1234);
    if (!c || c->timeout != 9 || c->calling_ssi != 0x654321 || c->talker_ssi != 0x654321 || c->basic_service != 0x32
        || !c->e2ee || c->priority != 7) {
        ok = 0;
    }

    /* Table 14.15 optionals: notification, temporary address and CPTI=2 (calling SSI plus calling-party extension), in
     * table order: the calling SSI is read past the other two. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_SETUP, off, 5);
    off += 5;
    pack_bits(cmce, 0x1235, off, 14);
    off += 14;
    pack_bits(cmce, 6, off, 4);
    off += 4;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x20, off, 8);
    off += 8;
    pack_bits(cmce, 1, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 3, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 17, off, 6);
    off += 6;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0xABCDEF, off, 24);
    off += 24;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 2, off, 2);
    off += 2;
    pack_bits(cmce, 0x112233, off, 24);
    off += 24;
    pack_bits(cmce, 0x445566, off, 24);
    off += 24;
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_mle_dispatch(mle, off + 3, 2, opts, state);
    c = tetra_calls_peek(state, 0x1235);
    if (!c || c->calling_ssi != 0x112233 || c->priority != 3 || c->timeout != 6) {
        ok = 0;
    }

    /* A later setup without optionals must not expose stale identities. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_SETUP, off, 5);
    off += 5;
    pack_bits(cmce, 0x1236, off, 14);
    off += 14;
    pack_bits(cmce, 5, off, 4);
    off += 4;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0x20, off, 8);
    off += 8;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 2, off, 4);
    off += 4;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_mle_dispatch(mle, off + 3, 2, opts, state);
    c = tetra_calls_peek(state, 0x1236);
    if (!c || c->calling_ssi != 0u) {
        ok = 0;
    }

    /* Table 14.14 D-STATUS with CPTI=1. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_STATUS, off, 5);
    off += 5;
    pack_bits(cmce, 1, off, 2);
    off += 2;
    pack_bits(cmce, 0x654321, off, 24);
    off += 24;
    pack_bits(cmce, 0xBEEF, off, 16);
    off += 16;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_sds_pdu sds;
    if (!tetra_sds_decode(mle, off + 3, &sds) || sds.type != TETRA_CMCE_D_STATUS || sds.status.calling.ssi != 0x654321
        || sds.status.status != 0xBEEF || sds.status.short_report) {
        ok = 0;
    }

    /* Table 14.12 D-RELEASE. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_RELEASE, off, 5);
    off += 5;
    pack_bits(cmce, 0x1236, off, 14);
    off += 14;
    pack_bits(cmce, 19, off, 5);
    off += 5;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (tetra_calls_peek(state, 0x1236) != NULL || tetra_calls_peek(state, 0x1235) == NULL
        || strstr(log, "[TETRA CMCE D-RELEASE] CC=2 call=4662 cause=19") == NULL) {
        ok = 0;
    }
    /* Tables 14.4, 14.5, 14.7, 14.8 and 14.11 mandatory fields. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_ALERT, off, 5);
    off += 5;
    pack_bits(cmce, 0x2345, off, 14);
    off += 14;
    pack_bits(cmce, 6, off, 3);
    off += 3;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (strstr(log, "[TETRA CMCE D-ALERT] CC=2 call=9029 setup_timeout=6 queued=1") == NULL) {
        ok = 0;
    }

    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CALL_PROCEEDING, off, 5);
    off += 5;
    pack_bits(cmce, 0x2345, off, 14);
    off += 14;
    pack_bits(cmce, 5, off, 3);
    off += 3;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (strstr(log, "[TETRA CMCE D-CALL-PROCEEDING] CC=2 call=9029 setup_timeout=5") == NULL) {
        ok = 0;
    }

    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CONNECT, off, 5);
    off += 5;
    pack_bits(cmce, 0x2345, off, 14);
    off += 14;
    pack_bits(cmce, 8, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (strstr(log, "[TETRA CMCE D-CONNECT] CC=2 call=9029 timeout=8 grant=3 request=1") == NULL) {
        ok = 0;
    }

    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CONNECT_ACK, off, 5);
    off += 5;
    pack_bits(cmce, 0x2345, off, 14);
    off += 14;
    pack_bits(cmce, 4, off, 4);
    off += 4;
    pack_bits(cmce, 2, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (strstr(log, "[TETRA CMCE D-CONNECT-ACK] CC=2 call=9029 timeout=4 grant=2") == NULL) {
        ok = 0;
    }

    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_INFO, off, 5);
    off += 5;
    pack_bits(cmce, 0x2345, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (strstr(log, "[TETRA CMCE D-INFO] CC=2 call=9029 reset=1 poll=1") == NULL) {
        ok = 0;
    }

    /* Table 14.6 D-CALL-RESTORE. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CALL_RESTORE, off, 5);
    off += 5;
    pack_bits(cmce, 0x3456, off, 14);
    off += 14;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (strstr(log, "[TETRA CMCE D-CALL-RESTORE] CC=2 call=13398 grant=3 request=1 reset=1") == NULL) {
        ok = 0;
    }

    /* Table 14.33 FUNCTION NOT SUPPORTED with call identifier and pointer=0. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    off = 0;
    pack_bits(cmce, TETRA_CMCE_FUNCTION_NOT_SUPPORTED, off, 5);
    off += 5;
    pack_bits(cmce, TETRA_CMCE_D_SETUP, off, 5);
    off += 5;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x3456, off, 14);
    off += 14;
    pack_bits(cmce, 0, off, 8);
    off += 8;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    DSD_MEMSET(mle, 0, sizeof(mle));
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(mle + 3, cmce, (size_t)off);
    tetra_test_mle_logged(mle, off + 3, 2, opts, state, log, sizeof log);
    if (strstr(log, "[TETRA CMCE FUNCTION-NOT-SUPPORTED] CC=2 rejected=7 call=13398 pointer=0 extract=0") == NULL) {
        ok = 0;
    }

    free_state(state);
    free(opts);
    return ok;
}

/* Real table 18.87 envelopes: a direct CMCE PDU has only the 3-bit protocol
 * discriminator; an MLE PDU adds the table 18.85 3-bit PDU type. */
static int
test_standard_mle_envelopes(void) {
    uint8_t bits[32] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();

    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, TETRA_CMCE_D_INFO, 3, 5);
    pack_bits(bits, 0x1234, 8, 14);
    pack_bits(bits, 1, 22, 1);
    pack_bits(bits, 1, 23, 1);
    pack_bits(bits, 0, 24, 1); /* O-bit */
    char log[512];
    tetra_test_mle_logged(bits, 25, 4, opts, state, log, sizeof log);
    int ok = strstr(log, "[TETRA CMCE D-INFO] CC=4 call=4660 reset=1 poll=1") != NULL;

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(bits, TETRA_MLE_D_RESTORE_FAIL, 3, 3);
    pack_bits(bits, 2, 6, 2);
    pack_bits(bits, 0, 8, 1); /* mandatory O-bit */
    tetra_mle_pdu pdu;
    ok = ok && tetra_mle_decode(bits, 9, &pdu) && pdu.type == TETRA_MLE_D_RESTORE_FAIL && pdu.fail_cause == 2;

    free_state(state);
    free(opts);
    return ok;
}

/* ETSI EN 300 392-2 V3.8.1 Annex E.19-E.24: downlink cell-reselection
 * responses, including their mandatory outer O-bit and protocol-specific SDU. */
static int
test_annex_e19_e24_mle_responses(void) {
    uint8_t bits[80] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;

    /* E.19: D-NEW-CELL without an MM SDU. */
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_NEW_CELL, off, 3);
    off += 3;
    pack_bits(bits, 2, off, 2);
    off += 2;
    pack_bits(bits, 0, off, 1);
    off++;
    tetra_mle_pdu pdu;
    int ok = tetra_mle_decode(bits, off, &pdu) && pdu.type == TETRA_MLE_D_NEW_CELL && pdu.channel_command == 2
             && pdu.sdu_pos == off;

    /* E.20: D-NEW-CELL carrying a minimal D-LOCATION UPDATE ACCEPT. */
    DSD_MEMSET(bits, 0, sizeof bits);
    off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_NEW_CELL, off, 3);
    off += 3;
    pack_bits(bits, 1, off, 2);
    off += 2;
    pack_bits(bits, 0, off, 1);
    off++;
    pack_bits(bits, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, off, 4);
    off += 4;
    pack_bits(bits, 5, off, 3);
    off += 3;
    pack_bits(bits, 0, off, 1);
    off++;
    ok = ok && tetra_mle_decode(bits, off, &pdu) && pdu.channel_command == 1 && pdu.sdu_pos == 9;
    char log[512];
    tetra_test_mle_logged(bits, off, 2, opts, state, log, sizeof log);
    ok = ok && strstr(log, "[TETRA MM D-LU-ACCEPT] CC=2 type=5") != NULL;

    /* E.21: D-PREPARE-FAIL without an MM SDU. */
    DSD_MEMSET(bits, 0, sizeof bits);
    off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_PREPARE_FAIL, off, 3);
    off += 3;
    pack_bits(bits, 3, off, 2);
    off += 2;
    pack_bits(bits, 0, off, 1);
    off++;
    ok = ok && tetra_mle_decode(bits, off, &pdu) && pdu.type == TETRA_MLE_D_PREPARE_FAIL && pdu.fail_cause == 3;

    /* E.22: D-PREPARE-FAIL carrying D-LOCATION UPDATE REJECT. */
    DSD_MEMSET(bits, 0, sizeof bits);
    off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_PREPARE_FAIL, off, 3);
    off += 3;
    pack_bits(bits, 1, off, 2);
    off += 2;
    pack_bits(bits, 0, off, 1);
    off++;
    pack_bits(bits, TETRA_MM_D_LOCATION_UPDATING_REJECT, off, 4);
    off += 4;
    pack_bits(bits, 2, off, 3);
    off += 3;
    pack_bits(bits, 17, off, 5);
    off += 5;
    pack_bits(bits, 0, off, 1);
    off++;
    pack_bits(bits, 0, off, 1);
    off++;
    ok = ok && tetra_mle_decode(bits, off, &pdu) && pdu.fail_cause == 1 && pdu.sdu_pos == 9;
    tetra_test_mle_logged(bits, off, 3, opts, state, log, sizeof log);
    ok = ok && strstr(log, "[TETRA MM D-LU-REJECT] CC=3 type=2 cause=17 cipher=0") != NULL;

    /* E.23: D-RESTORE-ACK carrying CMCE D-CALL-RESTORE. */
    DSD_MEMSET(bits, 0, sizeof bits);
    off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_RESTORE_ACK, off, 3);
    off += 3;
    pack_bits(bits, 0, off, 1);
    off++; /* MLE O-bit */
    pack_bits(bits, TETRA_CMCE_D_CALL_RESTORE, off, 5);
    off += 5;
    pack_bits(bits, 0x2345, off, 14);
    off += 14;
    pack_bits(bits, 2, off, 2);
    off += 2;
    pack_bits(bits, 1, off, 1);
    off++;
    pack_bits(bits, 1, off, 1);
    off++;
    pack_bits(bits, 0, off, 1);
    off++; /* CMCE O-bit */
    ok = ok && tetra_mle_decode(bits, off, &pdu) && pdu.restore_ack.call.call_id == 0x2345;
    tetra_test_mle_logged(bits, off, 4, opts, state, log, sizeof log);
    ok = ok && strstr(log, "[TETRA CMCE D-CALL-RESTORE] CC=4 call=9029 grant=2 request=1 reset=1") != NULL;

    /* Without the CMCE O-bit the ACK and its call restore are incomplete together: nothing acts on either. */
    ok = ok && !tetra_mle_decode(bits, off - 1, &pdu);
    tetra_test_mle_logged(bits, off - 1, 4, opts, state, log, sizeof log);
    ok = ok && strstr(log, "D-CALL-RESTORE] CC=4") == NULL;

    /* E.24: D-RESTORE-FAIL. */
    DSD_MEMSET(bits, 0, sizeof bits);
    off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_RESTORE_FAIL, off, 3);
    off += 3;
    pack_bits(bits, 2, off, 2);
    off += 2;
    pack_bits(bits, 0, off, 1);
    off++;
    ok = ok && tetra_mle_decode(bits, off, &pdu) && pdu.type == TETRA_MLE_D_RESTORE_FAIL && pdu.fail_cause == 2;

    /* No listed response defines Type 2 elements; O=1 is malformed. */
    DSD_MEMSET(bits, 0, sizeof bits);
    off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_NEW_CELL, off, 3);
    off += 3;
    pack_bits(bits, 0, off, 2);
    off += 2;
    pack_bits(bits, 1, off, 1);
    off++;
    ok = ok && !tetra_mle_decode(bits, off, &pdu);

    /* Table 18.10: D-CHANNEL RESPONSE with its reserved optionals absent. */
    DSD_MEMSET(bits, 0, sizeof bits);
    off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3);
    off += 3;
    pack_bits(bits, TETRA_MLE_D_CHANNEL_RESPONSE, off, 3);
    off += 3;
    pack_bits(bits, 1, off, 1);
    off++;
    pack_bits(bits, 5, off, 3);
    off += 3;
    pack_bits(bits, 9, off, 4);
    off += 4;
    pack_bits(bits, 0, off, 1);
    off++;
    ok = ok && tetra_mle_decode(bits, off, &pdu) && pdu.channel_response.type == 1 && pdu.channel_response.reason == 5
         && pdu.channel_response.retry_delay == 9;

    bits[off - 1] = 1; /* reserved Type 2 fields claimed */
    ok = ok && !tetra_mle_decode(bits, off, &pdu);

    free_state(state);
    free(opts);
    return ok;
}

/* Tables 14.4 and 14.5: ordered type-2 fields are committed only when every
 * presence flag and every declared value in the received tail is complete. */
static int
test_alert_proceeding_optional_atomicity(void) {
    uint8_t cmce[80] = {0}, tm_sdu[96] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;
    dispatch_setup(opts, state, 0x1234);
    dispatch_setup(opts, state, 0x2222);
    dispatch_setup(opts, state, 0x3456);

    pack_bits(cmce, TETRA_CMCE_D_ALERT, off, 5);
    off += 5;
    pack_bits(cmce, 0x1234, off, 14);
    off += 14;
    pack_bits(cmce, 5, off, 3);
    off += 3;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0xA5, off, 8);
    off += 8;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x2A, off, 6);
    off += 6;
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 3, opts, state);
    int ok = tetra_calls_peek(state, 0x1234)->basic_service == 0xA5 && !tetra_calls_peek(state, 0x1234)->e2ee;

    /* A D-ALERT that declares Basic Service but ends before its eight bits changes nothing. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    DSD_MEMSET(tm_sdu, 0, sizeof tm_sdu);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_ALERT, off, 5);
    off += 5;
    pack_bits(cmce, 0x2222, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 3);
    off += 3;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 3, opts, state);
    ok = ok && tetra_calls_peek(state, 0x2222)->basic_service == 0x04;

    DSD_MEMSET(cmce, 0, sizeof cmce);
    DSD_MEMSET(tm_sdu, 0, sizeof tm_sdu);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CALL_PROCEEDING, off, 5);
    off += 5;
    pack_bits(cmce, 0x3456, off, 14);
    off += 14;
    pack_bits(cmce, 6, off, 3);
    off += 3;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x53, off, 8);
    off += 8;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 5, off, 3);
    off += 3;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 17, off, 6);
    off += 6;
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 3, opts, state);
    ok = ok && tetra_calls_peek(state, 0x3456)->basic_service == 0x53 && tetra_calls_peek(state, 0x3456)->e2ee
         && tetra_st(state)->enc_mode == 0;

    free_state(state);
    free(opts);
    return ok;
}

static int
test_call_restore_optionals_and_atomicity(void) {
    uint8_t cmce[96] = {0}, tm_sdu[112] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;
    dispatch_setup(opts, state, 0x1111);

    pack_bits(cmce, TETRA_CMCE_D_CALL_RESTORE, off, 5);
    off += 5;
    pack_bits(cmce, 0x1111, off, 14);
    off += 14;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x2222, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 9, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 5, off, 3);
    off += 3;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x12A, off, 9);
    off += 9;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0x2D, off, 6);
    off += 6;
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 6, opts, state);

    int ok = tetra_calls_peek(state, 0x1111) == NULL && tetra_calls_peek(state, 0x2222) != NULL
             && tetra_calls_peek(state, 0x2222)->phase == TETRA_CALL_CONNECTED
             && tetra_calls_peek(state, 0x2222)->timeout == 9;

    /* New identifier is declared but incomplete: the call it names stays as it was. */
    dispatch_setup(opts, state, 0x3333);
    DSD_MEMSET(cmce, 0, sizeof cmce);
    DSD_MEMSET(tm_sdu, 0, sizeof tm_sdu);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CALL_RESTORE, off, 5);
    off += 5;
    pack_bits(cmce, 0x3333, off, 14);
    off += 14;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 7, off, 3);
    off += 3;
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 6, opts, state);
    ok = ok && tetra_calls_peek(state, 0x2222) != NULL && tetra_calls_peek(state, 0x3333) != NULL
         && tetra_calls_peek(state, 0x3333)->phase == TETRA_CALL_SETUP;

    free_state(state);
    free(opts);
    return ok;
}

static int
test_d_info_optionals_and_atomicity(void) {
    uint8_t cmce[160] = {0}, tm_sdu[176] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;
    dispatch_setup(opts, state, 0x1010);

    pack_bits(cmce, TETRA_CMCE_D_INFO, off, 5);
    off += 5;
    pack_bits(cmce, 0x1010, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
#define PUT_DINFO_OPT(value, width)                                                                                    \
    do {                                                                                                               \
        pack_bits(cmce, 1, off, 1);                                                                                    \
        off++;                                                                                                         \
        pack_bits(cmce, (value), off, (width));                                                                        \
        off += (width);                                                                                                \
    } while (0)
    PUT_DINFO_OPT(0x2020, 14);
    PUT_DINFO_OPT(11, 4);
    PUT_DINFO_OPT(6, 3);
    PUT_DINFO_OPT(1, 1);
    PUT_DINFO_OPT(0x155, 9);
    PUT_DINFO_OPT(3, 3);
    PUT_DINFO_OPT(0xABCDEF, 24);
    PUT_DINFO_OPT(42, 6);
    PUT_DINFO_OPT(37, 6);
    PUT_DINFO_OPT(12, 6);
#undef PUT_DINFO_OPT
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 5, opts, state);

    /* Every type-2 element present: the PDU is read to its M-bit and acted on. */
    int ok = tetra_calls_peek(state, 0x1010) == NULL && tetra_calls_peek(state, 0x2020) != NULL
             && tetra_calls_peek(state, 0x2020)->timeout == 11;

    /* Declared new call identifier is incomplete: the call it names stays as it was. */
    dispatch_setup(opts, state, 0x3030);
    DSD_MEMSET(cmce, 0, sizeof cmce);
    DSD_MEMSET(tm_sdu, 0, sizeof tm_sdu);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_INFO, off, 5);
    off += 5;
    pack_bits(cmce, 0x3030, off, 14);
    off += 14;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 3, off, 3);
    off += 3;
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 5, opts, state);
    ok = ok && tetra_calls_peek(state, 0x2020) != NULL && tetra_calls_peek(state, 0x3030) != NULL
         && tetra_calls_peek(state, 0x3030)->timeout == 4;

    free_state(state);
    free(opts);
    return ok;
}

static int
test_connect_optionals_and_atomicity(void) {
    uint8_t cmce[112] = {0}, tm_sdu[128] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;
    dispatch_setup(opts, state, 0x1111);
    dispatch_setup(opts, state, 0x2222);
    dispatch_setup(opts, state, 0x3333);
    pack_bits(cmce, TETRA_CMCE_D_CONNECT, off, 5);
    off += 5;
    pack_bits(cmce, 0x1111, off, 14);
    off += 14;
    pack_bits(cmce, 8, off, 4);
    off += 4;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
#define PUT_CONNECT_OPT(value, width)                                                                                  \
    do {                                                                                                               \
        pack_bits(cmce, 1, off, 1);                                                                                    \
        off++;                                                                                                         \
        pack_bits(cmce, (value), off, (width));                                                                        \
        off += (width);                                                                                                \
    } while (0)
    PUT_CONNECT_OPT(14, 4);
    PUT_CONNECT_OPT(0xA6, 8);
    PUT_CONNECT_OPT(0x654321, 24);
    PUT_CONNECT_OPT(33, 6);
#undef PUT_CONNECT_OPT
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 4, opts, state);
    const tetra_call_t* const c1 = tetra_calls_peek(state, 0x1111);
    int ok = c1 && c1->phase == TETRA_CALL_CONNECTED && c1->basic_service == 0xA6 && !c1->e2ee && c1->timeout == 8
             && tetra_st(state)->enc_mode == 0;

    /* Priority is declared but truncated: keep the established call intact. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    DSD_MEMSET(tm_sdu, 0, sizeof tm_sdu);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CONNECT, off, 5);
    off += 5;
    pack_bits(cmce, 0x2222, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 4);
    off += 4;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 4, opts, state);
    ok = ok && tetra_calls_peek(state, 0x2222)->phase == TETRA_CALL_SETUP
         && tetra_calls_peek(state, 0x2222)->timeout == 4;

    DSD_MEMSET(cmce, 0, sizeof cmce);
    DSD_MEMSET(tm_sdu, 0, sizeof tm_sdu);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CONNECT_ACK, off, 5);
    off += 5;
    pack_bits(cmce, 0x3333, off, 14);
    off += 14;
    pack_bits(cmce, 7, off, 4);
    off += 4;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 29, off, 6);
    off += 6;
    pack_bits(cmce, 0, off, 1);
    off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 4, opts, state);
    ok = ok && tetra_calls_peek(state, 0x3333)->phase == TETRA_CALL_CONNECTED
         && tetra_calls_peek(state, 0x3333)->timeout == 7;
    free_state(state);
    free(opts);
    return ok;
}

static int
test_release_notification_and_atomicity(void) {
    uint8_t bits[48] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    dispatch_setup(opts, state, 0x1234);
    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, TETRA_CMCE_D_RELEASE, 3, 5);
    pack_bits(bits, 0x1234, 8, 14);
    pack_bits(bits, 9, 22, 5);
    pack_bits(bits, 1, 27, 1); /* O-bit */
    pack_bits(bits, 1, 28, 1);
    pack_bits(bits, 41, 29, 6);
    pack_bits(bits, 0, 35, 1); /* M-bit */
    tetra_mle_dispatch(bits, 36, 2, opts, state);
    int ok = tetra_calls_peek(state, 0x1234) == NULL;

    /* A matching D-DISCONNECT whose notification is declared but truncated must not release the call. */
    DSD_MEMSET(bits, 0, sizeof bits);
    dispatch_setup(opts, state, 0x1234);
    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, TETRA_CMCE_D_DISCONNECT, 3, 5);
    pack_bits(bits, 0x1234, 8, 14);
    pack_bits(bits, 3, 22, 5);
    pack_bits(bits, 1, 27, 1); /* O-bit */
    pack_bits(bits, 1, 28, 1);
    pack_bits(bits, 2, 29, 2);
    tetra_mle_dispatch(bits, 31, 2, opts, state);
    ok = ok && tetra_calls_peek(state, 0x1234) != NULL;
    free_state(state);
    free(opts);
    return ok;
}

/* Floor control acts on the call it names: PDUs of call 78, which no record holds, leave call 77 alone. */
static int
test_floor_control_scoped_to_active_call(void) {
    uint8_t cmce[64] = {0}, mle[128] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    dispatch_setup(opts, state, 77);
    tetra_call_t* const c = tetra_calls_find(state, 77);
    c->phase = TETRA_CALL_TX_GRANTED;
    c->talker_ssi = 0x112233;
    c->seen_m = 1.0;
    int off = 0;

    pack_bits(cmce, TETRA_CMCE_D_TX_GRANTED, off, 5);
    off += 5;
    pack_bits(cmce, 78, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 1, opts, state);
    int ok = c->phase == TETRA_CALL_TX_GRANTED && c->talker_ssi == 0x112233 && tetra_calls_peek(state, 78) == NULL;

    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_CEASED, off, 5);
    off += 5;
    pack_bits(cmce, 78, off, 14);
    off += 14;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 1, opts, state);
    ok = ok && c->phase == TETRA_CALL_TX_GRANTED && c->talker_ssi == 0x112233;

    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, off, 5);
    off += 5;
    pack_bits(cmce, 78, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    char log[512];
    wrap_cmce(mle, cmce, off);
    tetra_test_mle_logged(mle, off + 3, 1, opts, state, log, sizeof log);
    ok = ok && c->seen_m < 2.0 && strstr(log, "[TETRA CMCE D-TX-WAIT] CC=1 call=78") != NULL;

    /* The same event for call 77 refreshes it. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, off, 5);
    off += 5;
    pack_bits(cmce, 77, off, 14);
    off += 14;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 1, opts, state);
    ok = ok && c->seen_m > 2.0 && c->phase == TETRA_CALL_TX_GRANTED;
    free_state(state);
    free(opts);
    return ok;
}

static int
test_floor_control_party_optionals_and_clear(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;
    dispatch_setup(opts, state, 77);
    const tetra_call_t* const c = tetra_calls_peek(state, 77);

    /* Table 14.18: notification followed by TPTI=2, SSI and extension. */
    pack_bits(cmce, TETRA_CMCE_D_TX_GRANTED, off, 5);
    off += 5;
    pack_bits(cmce, 77, off, 14);
    off += 14;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 23, off, 6);
    off += 6;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 2, off, 2);
    off += 2;
    pack_bits(cmce, 0x123456, off, 24);
    off += 24;
    pack_bits(cmce, 0x654321, off, 24);
    off += 24;
    pack_bits(cmce, 0, off, 1);
    off++; /* terminating M-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 1, opts, state);
    /* The transmitting SSI is read past the notification indicator. */
    int ok = c->phase == TETRA_CALL_TX_GRANTED && c->talker_ssi == 0x123456;

    /* A complete mandatory-only replacement clears absent optional identity. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_GRANTED, off, 5);
    off += 5;
    pack_bits(cmce, 77, off, 14);
    off += 14;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 1, opts, state);
    ok = ok && c->talker_ssi == 0u;

    /* Table 14.19 has the same transmitting-party optional chain: granted to another user, end-to-end encrypted. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_INTERRUPT, off, 5);
    off += 5;
    pack_bits(cmce, 77, off, 14);
    off += 14;
    pack_bits(cmce, 3, off, 2);
    off += 2;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 31, off, 6);
    off += 6;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 2, off, 2);
    off += 2;
    pack_bits(cmce, 0xABCDEF, off, 24);
    off += 24;
    pack_bits(cmce, 0x102030, off, 24);
    off += 24;
    pack_bits(cmce, 0, off, 1);
    off++; /* terminating M-bit */
    char log[512];
    wrap_cmce(mle, cmce, off);
    tetra_test_mle_logged(mle, off + 3, 1, opts, state, log, sizeof log);
    ok = ok && c->talker_ssi == 0xABCDEF && c->e2ee
         && strstr(log, "[TETRA CMCE D-TX-INTERRUPT] CC=1 call=77 request=1 notif=31 continue=0 grant=3 enc=1") != NULL;

    /* A claimed CPTI=2 extension that is truncated must preserve the event. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_INTERRUPT, off, 5);
    off += 5;
    pack_bits(cmce, 77, off, 14);
    off += 14;
    pack_bits(cmce, 2, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 0, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 2, off, 2);
    off += 2;
    pack_bits(cmce, 0x111111, off, 24);
    off += 24;
    pack_bits(cmce, 0xAA, off, 8);
    off += 8;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 1, opts, state);
    ok = ok && c->talker_ssi == 0xABCDEF && c->e2ee;

    free_state(state);
    free(opts);
    return ok;
}

static int
test_sds_transfer_timestamp_text(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 72, 9, 11);
    pack_bits(cmce, 130, 20, 8);
    pack_bits(cmce, 0xb7, 36, 8);
    pack_bits(cmce, 0x81, 44, 8); /* Timestamp present, Latin-1. */
    pack_bits(cmce, 0x192345, 52, 24);
    pack_bits(cmce, 'O', 76, 8);
    pack_bits(cmce, 'K', 84, 8);
    pack_bits(cmce, 0, 92, 1); /* O-bit */
    wrap_cmce(mle, cmce, 93);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(mle, 96, &pdu) && strcmp(pdu.data.text.text, "OK") == 0 && pdu.data.tl.reference == 0xb7;
    /* The outer length is complete; the timestamp itself lacks one bit. */
    pack_bits(cmce, 55, 9, 11);
    pack_bits(cmce, 0x11, 36, 8);
    pack_bits(cmce, 0, 75, 1); /* O-bit */
    wrap_cmce(mle, cmce, 76);
    ok = ok && !tetra_sds_decode(mle, 79, &pdu);
    return ok;
}

static int
test_sds_gsm_text(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 40, 9, 11);
    pack_bits(cmce, 2, 20, 8);
    /* Packed septets A, ESC, euro: 0x41,0x1b,0x65. */
    pack_bits(cmce, 0xc1, 36, 8);
    pack_bits(cmce, 0x4d, 44, 8);
    pack_bits(cmce, 0x19, 52, 8);
    pack_bits(cmce, 0, 60, 1); /* O-bit */
    wrap_cmce(mle, cmce, 61);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(mle, 64, &pdu) && pdu.data.text.len == 4
             && memcmp(pdu.data.text.text, "A\xe2\x82\xac", 5) == 0;
    /* One packed ESC without its extension character shows as U+FFFD. */
    pack_bits(cmce, 24, 9, 11);
    pack_bits(cmce, 27, 36, 8);
    pack_bits(cmce, 0, 44, 1);
    wrap_cmce(mle, cmce, 45);
    ok = ok && tetra_sds_decode(mle, 48, &pdu) && strcmp(pdu.data.text.text, "\xef\xbf\xbd") == 0;
    /* TETRA carries packed octets; a seven-bit payload is incomplete. */
    pack_bits(cmce, 23, 9, 11);
    pack_bits(cmce, 0, 43, 1);
    wrap_cmce(mle, cmce, 44);
    ok = ok && !tetra_sds_decode(mle, 47, &pdu);
    /* Undefined extension falls back to the default alphabet character. */
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 0x9b, 36, 8); /* ESC followed by A. */
    pack_bits(cmce, 0x20, 44, 8);
    pack_bits(cmce, 0, 52, 1);
    wrap_cmce(mle, cmce, 53);
    ok = ok && tetra_sds_decode(mle, 56, &pdu) && strcmp(pdu.data.text.text, "A") == 0;
    return ok;
}

static int
test_sds_utf16_display_bound(void) {
    uint8_t mle[2048] = {0};
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(mle, TETRA_CMCE_D_SDS_DATA, 3, 5);
    pack_bits(mle, 3, 10, 2);
    pack_bits(mle, 16 + 90 * 16, 12, 11);
    pack_bits(mle, 2, 23, 8);
    pack_bits(mle, 26, 31, 8);
    for (int i = 0; i < 90; i++) {
        pack_bits(mle, 0x4e2d, 39 + i * 16, 16);
    }
    pack_bits(mle, 0, 39 + 90 * 16, 1); /* O-bit */
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(mle, 40 + 90 * 16, &pdu) && pdu.data.text.len == 255 && pdu.data.text.text[255] == 0;
    for (size_t i = 0; ok && i < 85; i++) {
        ok = memcmp(pdu.data.text.text + (i * 3), "\xe4\xb8\xad", 3) == 0;
    }
    /* An unpaired surrogate past the displayed characters changes nothing that is shown. */
    pack_bits(mle, 0xd800, 39 + 89 * 16, 16);
    ok = ok && tetra_sds_decode(mle, 40 + 90 * 16, &pdu) && pdu.data.text.len == 255;
    return ok;
}

static int
test_sds_utf16_text(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 64, 9, 11);
    pack_bits(cmce, 2, 20, 8);
    pack_bits(cmce, 26, 28, 8);
    pack_bits(cmce, 0x4e2d, 36, 16);
    pack_bits(cmce, 0xd83d, 52, 16);
    pack_bits(cmce, 0xde00, 68, 16);
    pack_bits(cmce, 0, 84, 1);
    wrap_cmce(mle, cmce, 85);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(mle, 88, &pdu) && pdu.data.text.unicode && pdu.data.text.len == 7
             && memcmp(pdu.data.text.text, "\xe4\xb8\xad\xf0\x9f\x98\x80", 8) == 0;
    /* A high surrogate without its low half shows as U+FFFD; the rest of the message is kept. */
    pack_bits(cmce, 0x0041, 68, 16);
    wrap_cmce(mle, cmce, 85);
    ok = ok && tetra_sds_decode(mle, 88, &pdu) && pdu.data.text.len == 7
         && memcmp(pdu.data.text.text,
                   "\xe4\xb8\xad\xef\xbf\xbd"
                   "A",
                   8)
                == 0;
    return ok;
}

static int
test_sds_latin1_text(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 2, 20, 8);
    pack_bits(cmce, 1, 28, 8);
    pack_bits(cmce, 'A', 36, 8);
    pack_bits(cmce, 0xe9, 44, 8);
    pack_bits(cmce, 0, 52, 1);
    wrap_cmce(mle, cmce, 53);
    tetra_sds_pdu pdu;
    int ok =
        tetra_sds_decode(mle, 56, &pdu) && pdu.data.text.len == 3 && memcmp(pdu.data.text.text, "A\xc3\xa9", 4) == 0;
    pack_bits(cmce, 8, 9, 11); /* PID only: missing the coding-scheme octet. */
    pack_bits(cmce, 0, 28, 1);
    wrap_cmce(mle, cmce, 29);
    ok = ok && !tetra_sds_decode(mle, 32, &pdu);
    pack_bits(cmce, 31, 9, 11);
    pack_bits(cmce, 0, 51, 1);
    wrap_cmce(mle, cmce, 52);
    ok = ok && !tetra_sds_decode(mle, 55, &pdu);
    return ok;
}

static int
test_sds_transfer_reference(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 56, 9, 11);
    pack_bits(cmce, 131, 20, 8);
    pack_bits(cmce, 1, 35, 1);
    pack_bits(cmce, 0xa9, 36, 8);
    pack_bits(cmce, 17, 44, 5);
    pack_bits(cmce, 1, 49, 3); /* Forward SSI, 24 bits. */
    pack_bits(cmce, 0x123456, 52, 24);
    pack_bits(cmce, 0, 76, 1);
    wrap_cmce(mle, cmce, 77);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(mle, 80, &pdu) && pdu.data.tl.message_type == TETRA_SDS_TL_TRANSFER
             && pdu.data.tl.reference == 0xa9 && pdu.data.tl.storage_forward && pdu.data.tl.validity_period == 17
             && pdu.data.tl.forward_valid && pdu.data.tl.forward_type == 1 && pdu.data.tl.forward_ssi == 0x123456;
    pack_bits(cmce, 0x11, 36, 8);
    pack_bits(cmce, 55, 9, 11);
    pack_bits(cmce, 0, 75, 1);
    wrap_cmce(mle, cmce, 76);
    ok = ok && !tetra_sds_decode(mle, 79, &pdu);
    return ok;
}

static int
test_sds_report_transport(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 130, 20, 8);
    pack_bits(cmce, 1, 28, 4);
    pack_bits(cmce, 0x4a, 36, 8);
    pack_bits(cmce, 0xd3, 44, 8);
    int ok = 1;
    tetra_sds_pdu pdu;
    const int lengths[] = {48, 64, 88, 64, 0, 0, 0, 40};
    pack_bits(cmce, 1, 35, 1);
    for (unsigned address = 0; address < 8; address++) {
        DSD_MEMSET(cmce + 60, 0, sizeof(cmce) - 60);
        pack_bits(cmce, address, 57, 3);
        switch (address) {
            case 0: pack_bits(cmce, 0xa5, 60, 8); break;
            case 1: pack_bits(cmce, 0x123456, 60, 24); break;
            case 2:
                pack_bits(cmce, 0x234567, 60, 24);
                pack_bits(cmce, 0x345678, 84, 24);
                break;
            case 3:
                pack_bits(cmce, 3, 60, 8);
                pack_bits(cmce, 1, 68, 4);
                pack_bits(cmce, 10, 72, 4);
                pack_bits(cmce, 12, 76, 4);
                /* Bits 80..83 are the mandatory zero dummy digit. */
                break;
            default: break;
        }
        int length = lengths[address] ? lengths[address] : 40;
        pack_bits(cmce, length, 9, 11);
        pack_bits(cmce, 0, 20 + length, 1); /* O-bit */
        wrap_cmce(mle, cmce, 21 + length);
        ok = ok && tetra_sds_decode(mle, 24 + length, &pdu) == (lengths[address] != 0);
        if (lengths[address]) {
            const tetra_sds_tl* const tl = &pdu.data.tl;
            ok = ok && tl->message_type == TETRA_SDS_TL_REPORT && tl->status == 0x4a && tl->reference == 0xd3
                 && tl->storage_forward && tl->forward_valid && tl->forward_type == address;
            if (address == 0) {
                ok = ok && tl->forward_sna == 0xa5;
            }
            if (address == 1) {
                ok = ok && tl->forward_ssi == 0x123456;
            }
            if (address == 2) {
                ok = ok && tl->forward_ssi == 0x234567 && tl->forward_extension == 0x345678;
            }
            if (address == 3) {
                ok = ok && strcmp(tl->forward_external, "1*+") == 0;
            }
            pack_bits(cmce, length - 1, 9, 11);
            pack_bits(cmce, 0, 19 + length, 1); /* O-bit */
            wrap_cmce(mle, cmce, 20 + length);
            ok = ok && !tetra_sds_decode(mle, 23 + length, &pdu);
        }
    }
    /* Reserved external digits and a non-zero odd-digit pad are malformed. */
    pack_bits(cmce, 3, 57, 3);
    pack_bits(cmce, 3, 60, 8);
    pack_bits(cmce, 13, 68, 4);
    pack_bits(cmce, 0, 72, 8);
    pack_bits(cmce, 64, 9, 11);
    pack_bits(cmce, 0, 84, 1);
    wrap_cmce(mle, cmce, 85);
    ok = ok && !tetra_sds_decode(mle, 88, &pdu);
    pack_bits(cmce, 1, 68, 4);
    pack_bits(cmce, 2, 72, 4);
    pack_bits(cmce, 3, 76, 4);
    pack_bits(cmce, 1, 80, 4);
    wrap_cmce(mle, cmce, 85);
    ok = ok && !tetra_sds_decode(mle, 88, &pdu);
    /* Delivery status 2 (table 29.16: in the success range) without storage/forward control. */
    pack_bits(cmce, 0, 35, 1);
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 2, 36, 8);
    pack_bits(cmce, 0, 52, 1);
    wrap_cmce(mle, cmce, 53);
    ok = ok && tetra_sds_decode(mle, 56, &pdu) && pdu.data.tl.message_type == TETRA_SDS_TL_REPORT
         && pdu.data.tl.status == 2 && !pdu.data.tl.storage_forward && !pdu.data.tl.forward_valid;
    return ok;
}

static int
test_sds_ack_transport(void) {
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 130, 20, 8);
    pack_bits(cmce, 2, 28, 4);
    pack_bits(cmce, 3, 36, 8);
    pack_bits(cmce, 0xe7, 44, 8);
    pack_bits(cmce, 0, 52, 1);
    wrap_cmce(mle, cmce, 53);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(mle, 56, &pdu) && pdu.data.tl.message_type == TETRA_SDS_TL_ACK
             && pdu.data.tl.reference == 0xe7 && pdu.data.tl.status == 3;
    /* Complete CMCE payload, but truncated inner transport header. */
    pack_bits(cmce, 31, 9, 11);
    pack_bits(cmce, 0x22, 44, 8);
    pack_bits(cmce, 0, 51, 1);
    wrap_cmce(mle, cmce, 52);
    ok = ok && !tetra_sds_decode(mle, 55, &pdu);
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 2, 20, 8); /* Simple text PID: no transport ACK. */
    pack_bits(cmce, 0, 52, 1);
    wrap_cmce(mle, cmce, 53);
    ok = ok && tetra_sds_decode(mle, 56, &pdu) && pdu.data.tl.message_type == TETRA_SDS_TL_NONE;
    return ok;
}

static int
test_sds_published_trace(void) {
    uint8_t bits[88] = {0};

    /* Published tetra-multiframe-sds example trace:
     * https://github.com/itds-consulting/tetra-multiframe-sds#example-output
     *
     * Its independently decoded 64-bit D-SDS-DATA user field is
     * 82:00:20:01:41:68:6F:6A: PID 130, SDS-TRANSFER, message reference 32,
     * no timestamp, Latin-1 coding, text "Ahoj".  The frozen 88-bit value
     * below wraps that field in a minimal CMCE/MLE envelope (CPTI=0, SDTI=3,
     * 64-bit length, no optional tail) without using pack_bits().
     */
    unpack_hex_bits("4F30810400400282D0DED4", bits, 88);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(bits, 88, &pdu) && pdu.data.sdti == 3 && pdu.data.calling.ssi == 0 && pdu.data.pid == 130
             && pdu.data.tl.reference == 32 && pdu.data.text.len == 4 && !pdu.data.text.unicode
             && strcmp(pdu.data.text.text, "Ahoj") == 0;
    return ok;
}

static int
test_sds_short_report_status(void) {
    uint8_t cmce[48] = {0}, mle[128] = {0};
    int ok = 1;
    pack_bits(cmce, TETRA_CMCE_D_STATUS, 0, 5);
    pack_bits(cmce, 1, 5, 2);
    pack_bits(cmce, 0x123456, 7, 24);
    pack_bits(cmce, 0, 47, 1); /* O-bit */
    tetra_sds_pdu pdu;
    for (unsigned result = 0; result < 4; result++) {
        pack_bits(cmce, 0x7c00u | (result << 8) | 0xe5u, 31, 16);
        wrap_cmce(mle, cmce, 48);
        ok = ok && tetra_sds_decode(mle, 51, &pdu) && pdu.type == TETRA_CMCE_D_STATUS && pdu.status.short_report
             && pdu.status.short_report_type == result && pdu.status.short_report_msg_ref == 0xe5
             && pdu.status.calling.ssi == 0x123456;
    }
    /* A truncated status is incomplete. */
    pack_bits(cmce, 0x7c11, 31, 16);
    wrap_cmce(mle, cmce, 48);
    ok = ok && !tetra_sds_decode(mle, 50, &pdu);
    /* Adjacent prefixes are ordinary status, not short reports. */
    for (unsigned status = 0x7800; status <= 0x8000; status += 0x800) {
        pack_bits(cmce, status, 31, 16);
        wrap_cmce(mle, cmce, 48);
        ok = ok && tetra_sds_decode(mle, 51, &pdu) && !pdu.status.short_report;
    }
    return ok;
}

/* The calling party of a D-SDS-DATA (table 14.13): its type identifier (1 SSI, 2 SSI and address extension), SSI and,
 * for type 2, extension (the MCC and MNC of the sender's network). */
typedef struct {
    uint32_t type;
    uint32_t ssi;
    uint32_t extension;
} sds_sender;

/* A concatenation part (tables 29.62 and 29.63) of a message from @p from into @p mle; returns its bits. */
static int
build_concat_fragment_from(uint8_t* mle, sds_sender from, int with_transport, uint16_t reference, uint8_t total,
                           uint8_t sequence, int payload_pid, const uint8_t* data, int data_bits, uint8_t message_ref) {
    uint8_t cmce[128] = {0};
    int off = 0;
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, off, 5);
    off += 5;
    pack_bits(cmce, from.type, off, 2);
    off += 2;
    pack_bits(cmce, from.ssi, off, 24);
    off += 24;
    if (from.type == 2) {
        pack_bits(cmce, from.extension, off, 24);
        off += 24;
    }
    pack_bits(cmce, 3, off, 2);
    off += 2;
    int length_offset = off;
    off += 11;
    int payload_start = off;
    pack_bits(cmce, with_transport ? 140 : 12, off, 8);
    off += 8;
    if (with_transport) {
        pack_bits(cmce, 0, off, 4);
        off += 4; /* SDS-TRANSFER. */
        pack_bits(cmce, 0, off, 4);
        off += 4; /* Report request + no store/forward. */
        pack_bits(cmce, message_ref, off, 8);
        off += 8;
    }
    pack_bits(cmce, 0, off, 3);
    off += 3; /* Concatenation transfer. */
    pack_bits(cmce, reference > 15, off, 1);
    off++;
    if (reference > 15) {
        pack_bits(cmce, reference >> 4, off, 8);
        off += 8;
    }
    pack_bits(cmce, reference & 15u, off, 4);
    off += 4;
    pack_bits(cmce, total, off, 8);
    off += 8;
    pack_bits(cmce, sequence, off, 8);
    off += 8;
    if (sequence == 1) {
        pack_bits(cmce, (uint32_t)payload_pid, off, 8);
        off += 8;
    }
    DSD_MEMCPY(cmce + off, data, (size_t)data_bits);
    off += data_bits;
    pack_bits(cmce, (uint32_t)(off - payload_start), length_offset, 11);
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    return off + 3;
}

/* A concatenation part of a message from SSI @p src_ssi (calling party type 1). */
static int
build_concat_fragment(uint8_t* mle, uint32_t src_ssi, int with_transport, uint16_t reference, uint8_t total,
                      uint8_t sequence, int payload_pid, const uint8_t* data, int data_bits, uint8_t message_ref) {
    const sds_sender from = {1, src_ssi, 0};
    return build_concat_fragment_from(mle, from, with_transport, reference, total, sequence, payload_pid, data,
                                      data_bits, message_ref);
}

/* The part build_concat_fragment() builds, decoded with colour code 6; @p log receives the summary lines. */
static void
dispatch_concat_fragment(dsd_state* state, dsd_opts* opts, uint32_t src_ssi, int with_transport, uint16_t reference,
                         uint8_t total, uint8_t sequence, int payload_pid, const uint8_t* data, int data_bits,
                         uint8_t message_ref, char* log, size_t log_size) {
    uint8_t mle[128] = {0};
    const int nbits = build_concat_fragment(mle, src_ssi, with_transport, reference, total, sequence, payload_pid, data,
                                            data_bits, message_ref);
    tetra_test_mle_logged(mle, nbits, 6, opts, state, log, log_size);
}

/* Parts arrive out of order and repeated: the message completes once, from its three parts, and only then is noted. */
static int
test_sds_concat_out_of_order_and_duplicates(void) {
    dsd_state* state = alloc_notice_state();
    dsd_opts* opts = alloc_opts();
    char log[512];
    uint8_t first[24] = {0}, second[16] = {0}, third[8] = {0};
    pack_bits(first, 1, 0, 8); /* No timestamp, Latin-1. */
    pack_bits(first, 'H', 8, 8);
    pack_bits(first, 'E', 16, 8);
    pack_bits(second, 'L', 0, 8);
    pack_bits(second, 'L', 8, 8);
    pack_bits(third, 'O', 0, 8);
    const uint64_t before = notices(state);

    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 2, -1, second, 16, 0, log, sizeof log);
    int ok = strstr(log, "[TETRA SDS CONCAT] CC=6 src_SSI=1193046 ref=2748 part=2/3 received=1\n") != NULL
             && strstr(log, "D-SDS-DATA") == NULL && notices(state) == before;
    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 2, -1, second, 16, 0, log, sizeof log);
    ok = ok && strstr(log, "part=2/3 received=1 duplicate") != NULL;
    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 3, -1, third, 8, 0, log, sizeof log);
    ok = ok && strstr(log, "part=3/3 received=2\n") != NULL && notices(state) == before;
    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 1, 2, first, 24, 0, log, sizeof log);
    ok = ok && strstr(log, "part=1/3 received=3 complete") != NULL
         && strstr(log, "[TETRA CMCE D-SDS-DATA] CC=6 src_SSI=1193046") != NULL && notices(state) == before + 1U
         && notice(state, 1)->source_id == 0x123456 && strcmp(notice(state, 1)->text_message, "HELLO") == 0;
    free_state(state);
    free(opts);
    return ok;
}

/* Part @p sequence of the two-part simple text with reference 9 from @p from, decoded with colour code 6. */
static void
dispatch_part_from(dsd_state* state, dsd_opts* opts, sds_sender from, uint8_t sequence, const uint8_t* data,
                   int data_bits, char* log, size_t log_size) {
    uint8_t mle[160] = {0};
    const int nbits =
        build_concat_fragment_from(mle, from, 0, 9, 2, sequence, sequence == 1 ? 2 : -1, data, data_bits, 0);
    tetra_test_mle_logged(mle, nbits, 6, opts, state, log, log_size);
}

/* A concatenated message is its sender's, by its TETRA subscriber identity: the SSI with the network a calling party of
 * type 2 names in its extension (tables 14.43, 14.44) and one of type 1 leaves to the current network (EN 300 392-2
 * clause 14.2.3). Parts with the same SSI and reference from senders of two networks complete neither message; parts
 * of one sender complete its message whether they name its network or not. */
static int
test_sds_concat_keyed_by_calling_party(void) {
    dsd_state* state = alloc_notice_state();
    dsd_opts* opts = alloc_opts();
    char log[512];
    uint8_t first[16] = {0}, second[8] = {0};
    pack_bits(first, 1, 0, 8); /* No timestamp, Latin-1. */
    pack_bits(first, 'A', 8, 8);
    pack_bits(second, 'B', 0, 8);
    tetra_state_t* const ts = tetra_st(state);
    ts->net_known = 1;
    ts->mcc = 262;
    ts->mnc = 30;
    const sds_sender one = {2, 77, 0x111111};
    const sds_sender two = {2, 77, 0x222222};
    const sds_sender here = {1, 77, 0};
    const sds_sender here_named = {2, 77, (262u << 14) | 30u};
    const uint64_t before = notices(state);

    dispatch_part_from(state, opts, one, 1, first, 16, log, sizeof log);
    int ok = strstr(log, "part=1/2 received=1\n") != NULL;
    dispatch_part_from(state, opts, two, 2, second, 8, log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=1\n") != NULL && notices(state) == before;
    dispatch_part_from(state, opts, here, 1, first, 16, log, sizeof log);
    ok = ok && strstr(log, "part=1/2 received=1\n") != NULL && notices(state) == before;
    dispatch_part_from(state, opts, here_named, 2, second, 8, log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=2 complete") != NULL && notices(state) == before + 1U
         && strcmp(notice_text(state, 1), "AB") == 0;
    dispatch_part_from(state, opts, one, 2, second, 8, log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=2 complete") != NULL && notices(state) == before + 2U;
    free_state(state);
    free(opts);
    return ok;
}

/* With SDS-TL: a part that differs from the one held under its number discards the message; a message completes from
 * a new set of parts; a part count or number out of range is malformed. */
static int
test_sds_concat_transport_and_conflict(void) {
    dsd_state* state = alloc_notice_state();
    dsd_opts* opts = alloc_opts();
    char log[512];
    uint8_t first[16] = {0}, second[8] = {0}, conflict[8] = {0};
    pack_bits(first, 1, 0, 8);
    pack_bits(first, 'O', 8, 8);
    pack_bits(second, 'K', 0, 8);
    pack_bits(conflict, 'X', 0, 8);

    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 2, -1, second, 8, 0x44, log, sizeof log);
    int ok = strstr(log, "part=2/2 received=1\n") != NULL;
    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 2, -1, conflict, 8, 0x44, log, sizeof log);
    ok = ok && strstr(log, "SDS CONCAT") == NULL;
    /* The conflicting part discarded the entire old context. */
    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 1, 130, first, 16, 0x44, log, sizeof log);
    ok = ok && strstr(log, "part=1/2 received=1\n") != NULL;
    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 2, -1, second, 8, 0x44, log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=2 complete") != NULL && strcmp(notice(state, 1)->text_message, "OK") == 0;
    uint8_t mle[128] = {0};
    tetra_sds_pdu pdu;
    const int nbits = build_concat_fragment(mle, 77, 1, 9, 2, 2, -1, second, 8, 0x44);
    ok = ok && tetra_sds_decode(mle, nbits, &pdu) && pdu.data.concat_present && pdu.data.concat.with_transport
         && pdu.data.tl.message_type == TETRA_SDS_TL_TRANSFER && pdu.data.tl.reference == 0x44
         && pdu.data.concat.reference == 9 && pdu.data.concat.total == 2 && pdu.data.concat.sequence == 2
         && pdu.data.concat.payload_pid == -1 && pdu.data.text.len == 0;

    const uint64_t before = notices(state);
    dispatch_concat_fragment(state, opts, 88, 0, 3, 1, 1, 2, first, 16, 0, log, sizeof log);
    ok = ok && strstr(log, "SDS CONCAT") == NULL;
    dispatch_concat_fragment(state, opts, 88, 0, 3, 2, 3, -1, second, 8, 0, log, sizeof log);
    ok = ok && strstr(log, "SDS CONCAT") == NULL && notices(state) == before;
    free_state(state);
    free(opts);
    return ok;
}

static int
test_sds_annex_e_type34_tail(void) {
    uint8_t cmce[64] = {0}, mle[128] = {0};
    int off = 0;

    /* CPTI=0, SDTI=0, followed by one unknown length-delimited Type 3 IE. */
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, off, 5);
    off += 5;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0xBEEF, off, 16);
    off += 16;
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 1, off, 1);
    off++; /* M-bit */
    pack_bits(cmce, 4, off, 4);
    off += 4;
    pack_bits(cmce, 3, off, 11);
    off += 11;
    pack_bits(cmce, 5, off, 3);
    off += 3;
    pack_bits(cmce, 0, off, 1);
    off++; /* terminating M-bit */
    wrap_cmce(mle, cmce, off);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(mle, off + 3, &pdu) && pdu.data.sdti == 0 && pdu.data.short_data == 0xBEEF;

    /* An incomplete Type 3 value leaves the PDU incomplete. */
    DSD_MEMSET(cmce, 0, sizeof cmce);
    off = 0;
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, off, 5);
    off += 5;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0, off, 2);
    off += 2;
    pack_bits(cmce, 0x1234, off, 16);
    off += 16;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 1, off, 1);
    off++;
    pack_bits(cmce, 4, off, 4);
    off += 4;
    pack_bits(cmce, 4, off, 11);
    off += 11;
    pack_bits(cmce, 2, off, 2);
    off += 2;
    wrap_cmce(mle, cmce, off);
    ok = ok && !tetra_sds_decode(mle, off + 3, &pdu);
    return ok;
}

static int
test_function_not_supported_extract_and_o_bit(void) {
    uint8_t cmce[64] = {0}, mle[128] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    int off = 0;
    pack_bits(cmce, TETRA_CMCE_FUNCTION_NOT_SUPPORTED, off, 5);
    off += 5;
    pack_bits(cmce, TETRA_CMCE_D_INFO, off, 5);
    off += 5;
    pack_bits(cmce, 0, off, 1);
    off++; /* no call identifier */
    pack_bits(cmce, 3, off, 8);
    off += 8;
    pack_bits(cmce, 5, off, 8);
    off += 8;
    pack_bits(cmce, 0x15, off, 5);
    off += 5;
    pack_bits(cmce, 0, off, 1);
    off++; /* O-bit */
    char log[512];
    wrap_cmce(mle, cmce, off);
    tetra_test_mle_logged(mle, off + 3, 4, opts, state, log, sizeof log);
    int ok = strstr(log, "[TETRA CMCE FUNCTION-NOT-SUPPORTED] CC=4 rejected=5 call=0 pointer=3 extract=5") != NULL;

    /* The same body without its mandatory O-bit is not a complete PDU. */
    tetra_test_mle_logged(mle, off + 2, 5, opts, state, log, sizeof log);
    ok = ok && strstr(log, "FUNCTION-NOT-SUPPORTED] CC=5") == NULL;
    free_state(state);
    free(opts);
    return ok;
}

/* -------------------------------------------------------------------------
 * Regression tests for the bit-reader rewrite.
 * ------------------------------------------------------------------------- */

/* Appends fields MSB first to a one-bit-per-byte buffer. */
typedef struct {
    uint8_t* bits;
    int pos;
} bit_writer;

static void
put(bit_writer* w, uint32_t value, int width) {
    pack_bits(w->bits, value, w->pos, width);
    w->pos += width;
}

/* A CMCE TM-SDU: protocol discriminator, then the PDU type. */
static bit_writer
cmce_writer(uint8_t* bits, size_t size, uint32_t pdu_type) {
    bit_writer w = {bits, 0};
    DSD_MEMSET(bits, 0, size);
    put(&w, TETRA_MLE_PD_CMCE, 3);
    put(&w, pdu_type, 5);
    return w;
}

/* A D-SDS-DATA from @p ssi (CPTI 1) with short data type identifier 3; the caller writes @p data_bits of user data. */
static bit_writer
sds_writer(uint8_t* bits, size_t size, uint32_t ssi, int data_bits) {
    bit_writer w = cmce_writer(bits, size, TETRA_CMCE_D_SDS_DATA);
    put(&w, 1, 2);
    put(&w, ssi, 24);
    put(&w, 3, 2);
    put(&w, (uint32_t)data_bits, 11);
    return w;
}

/* Continuation bytes after a UTF-8 lead byte; -1 for a byte that cannot start a character. */
static int
utf8_extra_bytes(unsigned char lead) {
    if (lead < 0x80) {
        return 0;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 1;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 2;
    }
    return (lead & 0xF8) == 0xF0 ? 3 : -1;
}

/* 1 when @p text is well-formed UTF-8 that holds no C0 or C1 control. */
static int
text_is_safe_utf8(const char* text) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p != 0) {
        const int extra = utf8_extra_bytes(*p);
        if (extra < 0 || *p < 0x20 || *p == 0x7F || (*p == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F)) {
            return 0;
        }
        for (int i = 1; i <= extra; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return 0;
            }
        }
        p += 1 + extra;
    }
    return 1;
}

/* Table 14.16: the notification indicator is a 6-bit type-2 element. A D-TX CEASED carrying one (most significant bit
 * set) and a type-3 element after it must parse, so the reader has to step over the notification. */
static int
test_d_tx_ceased_notification_tail(void) {
    uint8_t bits[64];
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    bit_writer w = cmce_writer(bits, sizeof bits, TETRA_CMCE_D_TX_CEASED);
    put(&w, 42, 14);
    put(&w, 1, 1);  /* transmission request permission */
    put(&w, 1, 1);  /* O-bit */
    put(&w, 1, 1);  /* notification indicator present */
    put(&w, 33, 6); /* 100001 */
    put(&w, 1, 1);  /* M-bit: a type-3 element follows */
    put(&w, 3, 4);  /* element identifier */
    put(&w, 4, 11); /* length */
    put(&w, 9, 4);
    put(&w, 0, 1); /* last M-bit */
    dispatch_setup(opts, state, 42);
    tetra_calls_find(state, 42)->phase = TETRA_CALL_TX_GRANTED;
    char log[512];
    tetra_test_mle_logged(bits, w.pos, 1, opts, state, log, sizeof log);
    int ok = tetra_calls_peek(state, 42)->phase == TETRA_CALL_CEASED
             && strstr(log, "[TETRA CMCE D-TX-CEASED] CC=1 call=42 request=1 notification=33") != NULL;
    free_state(state);
    free(opts);
    return ok;
}

/* A D-TX GRANTED that ends at its O-bit (set) is truncated: the buffer is exactly the PDU, so a read past it would be
 * caught by the address sanitizer, and nothing may be published. */
static int
test_tx_granted_ending_at_o_bit(void) {
    enum { NBITS = 3 + 25 };

    uint8_t* bits = (uint8_t*)calloc(NBITS, 1);
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    if (!bits || !state || !opts) {
        free(bits);
        free(state);
        free(opts);
        return 0;
    }
    bit_writer w = {bits, 0};
    put(&w, TETRA_MLE_PD_CMCE, 3);
    put(&w, TETRA_CMCE_D_TX_GRANTED, 5);
    put(&w, 7, 14);
    put(&w, 0, 2);
    put(&w, 0, 3);
    put(&w, 1, 1); /* O-bit set, P-bits missing */
    dispatch_setup(opts, state, 7);
    tetra_calls_find(state, 7)->talker_ssi = 0x112233u;
    tetra_mle_dispatch(bits, NBITS, 0, opts, state);
    int ok =
        tetra_calls_peek(state, 7)->phase == TETRA_CALL_SETUP && tetra_calls_peek(state, 7)->talker_ssi == 0x112233u;
    free(bits);
    free_state(state);
    free(opts);
    return ok;
}

/* The SDS sender belongs to the SDS text: each notice names the sender of its own message, and neither a D-STATUS nor
 * an unfinished concatenation part from someone else re-attributes the last text. */
static int
test_sds_sender_stays_with_text(void) {
    uint8_t bits[160];
    dsd_state* state = alloc_notice_state();
    dsd_opts* opts = alloc_opts();

    bit_writer w = sds_writer(bits, sizeof bits, 1001, 32);
    put(&w, 2, 8); /* simple text messaging */
    put(&w, 1, 8); /* reserved bit, Latin-1 */
    put(&w, 'H', 8);
    put(&w, 'I', 8);
    put(&w, 0, 1); /* O-bit */
    tetra_mle_dispatch(bits, w.pos, 1, opts, state);
    int ok = notice(state, 1)->source_id == 1001 && strcmp(notice(state, 1)->text_message, "HI") == 0;

    w = cmce_writer(bits, sizeof bits, TETRA_CMCE_D_STATUS);
    put(&w, 1, 2);
    put(&w, 2002, 24);
    put(&w, 0x1234, 16);
    put(&w, 0, 1);
    tetra_mle_dispatch(bits, w.pos, 1, opts, state);
    ok = ok && notice(state, 1)->source_id == 2002 && notice(state, 1)->text_message[0] == '\0'
         && strstr(notice(state, 1)->event_string, "STATUS SRC: 2002; TGT: 0; STATUS: 0x1234;") != NULL
         && notice(state, 2)->source_id == 1001 && strcmp(notice(state, 2)->text_message, "HI") == 0;

    const uint64_t before = notices(state);
    w = sds_writer(bits, sizeof bits, 3003, 8 + 24 + 8);
    put(&w, 12, 8); /* concatenated SDS without SDS-TL */
    put(&w, 0, 3);
    put(&w, 0, 1);
    put(&w, 5, 4); /* reference */
    put(&w, 2, 8); /* parts */
    put(&w, 2, 8); /* this is part 2 */
    put(&w, 'X', 8);
    put(&w, 0, 1);
    tetra_mle_dispatch(bits, w.pos, 1, opts, state);
    ok = ok && notices(state) == before && notice(state, 2)->source_id == 1001;
    free_state(state);
    free(opts);
    return ok;
}

/* C0 and C1 controls in SDS text (U+009B is CSI to a UTF-8 terminal) become spaces in every text coding scheme. */
static int
test_sds_control_characters(void) {
    uint8_t bits[200];
    static const uint16_t k_units[] = {'A', 0x009B, 'B', 0x001B, 'C', 0x0085, 'D'};

    bit_writer w = sds_writer(bits, sizeof bits, 7, 16 + 7 * 16);
    put(&w, 2, 8);
    put(&w, 26, 8); /* UCS-2/UTF-16BE */
    for (size_t i = 0; i < sizeof k_units / sizeof k_units[0]; i++) {
        put(&w, k_units[i], 16);
    }
    put(&w, 0, 1);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(bits, w.pos, &pdu) && strcmp(pdu.data.text.text, "A B C D") == 0;

    w = sds_writer(bits, sizeof bits, 7, 16 + 6 * 8);
    put(&w, 2, 8);
    put(&w, 1, 8); /* Latin-1 */
    put(&w, 'E', 8);
    put(&w, 0x9B, 8);
    put(&w, 0x1B, 8);
    put(&w, 0x0A, 8);
    put(&w, 0xE9, 8);
    put(&w, 'F', 8);
    put(&w, 0, 1);
    ok = ok && tetra_sds_decode(bits, w.pos, &pdu)
         && strcmp(pdu.data.text.text, "E   \xc3\xa9"
                                       "F")
                == 0
         && text_is_safe_utf8(pdu.data.text.text);

    /* GSM 7-bit "A", LF, "B" packed into three octets (table 29.34): 0x41, 0x85, 0x10. */
    w = sds_writer(bits, sizeof bits, 7, 16 + 24);
    put(&w, 2, 8);
    put(&w, 0, 8); /* GSM 7-bit */
    put(&w, 0x41, 8);
    put(&w, 0x85, 8);
    put(&w, 0x10, 8);
    put(&w, 0, 1);
    ok = ok && tetra_sds_decode(bits, w.pos, &pdu) && strcmp(pdu.data.text.text, "A B") == 0;
    return ok;
}

/* A lone low surrogate, and a high surrogate at the end of the text, show as U+FFFD instead of dropping the SDS. */
static int
test_sds_unpaired_surrogates(void) {
    uint8_t bits[160];
    bit_writer w = sds_writer(bits, sizeof bits, 9, 16 + 3 * 16);
    put(&w, 2, 8);
    put(&w, 26, 8);
    put(&w, 0xDC00, 16);
    put(&w, 'A', 16);
    put(&w, 0xD83D, 16);
    put(&w, 0, 1);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(bits, w.pos, &pdu) && pdu.data.calling.ssi == 9 && pdu.data.text.unicode
             && strcmp(pdu.data.text.text, "\xef\xbf\xbd"
                                           "A\xef\xbf\xbd")
                    == 0;
    return ok;
}

/* Long Latin-1 text is cut at a character boundary, never inside a UTF-8 sequence. */
static int
test_sds_long_text_utf8_boundary(void) {
    uint8_t bits[2048];
    bit_writer w = sds_writer(bits, sizeof bits, 1234567, 16 + 150 * 8);
    put(&w, 2, 8);
    put(&w, 1, 8);
    for (int i = 0; i < 150; i++) {
        put(&w, 0xE9, 8); /* two UTF-8 bytes each */
    }
    put(&w, 0, 1);
    tetra_sds_pdu pdu;
    int ok = tetra_sds_decode(bits, w.pos, &pdu) && pdu.data.text.len == 254 && pdu.data.text.full
             && text_is_safe_utf8(pdu.data.text.text) && strncmp(pdu.data.text.text, "\xc3\xa9\xc3\xa9", 4) == 0;
    return ok;
}

/* Part @p sequence of a two-part simple text from SSI 55; @p log receives the summary lines. */
static void
dispatch_concat_part(dsd_state* state, dsd_opts* opts, uint8_t sequence, uint8_t octet, char* log, size_t log_size) {
    uint8_t bits[160];
    const int data_bits = 8 + 24 + (sequence == 1 ? 8 + 8 : 0) + 8;
    bit_writer w = sds_writer(bits, sizeof bits, 55, data_bits);
    put(&w, 12, 8);
    put(&w, 0, 3);
    put(&w, 0, 1);
    put(&w, 3, 4); /* reference */
    put(&w, 2, 8); /* parts */
    put(&w, sequence, 8);
    if (sequence == 1) {
        put(&w, 2, 8); /* payload PID: simple text */
        put(&w, 1, 8); /* reserved bit, Latin-1 */
    }
    put(&w, octet, 8);
    put(&w, 0, 1);
    tetra_test_mle_logged(bits, w.pos, 1, opts, state, log, log_size);
}

/* Reassembly lives in each session's TETRA state: parts given to one session never complete a message in another, and
 * resetting the TETRA state drops what was collected. */
static int
test_sds_concat_is_per_session(void) {
    dsd_state* a = alloc_notice_state();
    dsd_state* b = alloc_notice_state();
    dsd_opts* opts = alloc_opts();
    char log[512];

    dispatch_concat_part(a, opts, 1, 'O', log, sizeof log);
    dispatch_concat_part(b, opts, 2, 'K', log, sizeof log);
    int ok = strstr(log, "part=2/2 received=1\n") != NULL;
    dispatch_concat_part(a, opts, 2, 'K', log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=2 complete") != NULL && strcmp(notice(a, 1)->text_message, "OK") == 0;

    dispatch_concat_part(a, opts, 1, 'N', log, sizeof log);
    tetra_state_reset(a);
    dispatch_concat_part(a, opts, 2, 'O', log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=1\n") != NULL;

    free_state(a);
    free_state(b);
    free(opts);
    return ok;
}

/* The sender of a concatenated message is an SSI of the network: a BSCH naming another network drops the messages
 * being reassembled, so a part from the same SSI on the new network completes nothing of the last network's; a BSCH
 * of another cell of the same network keeps them. */
static int
test_sds_concat_dropped_with_the_network(void) {
    dsd_state* state = alloc_notice_state();
    dsd_opts* opts = alloc_opts();
    char log[512];
    tetra_state_t* const ts = tetra_st(state);
    ts->net_known = 1;
    ts->mcc = 262;
    ts->mnc = 30;
    ts->colour = 1;
    dispatch_concat_part(state, opts, 1, 'O', log, sizeof log);
    ttb_block cell = {0};
    ttb_sync_pdu(&cell, 262, 30, 2);
    int ok = tetra_bsch_parse(cell.bits, cell.n, opts, state) == 1;
    dispatch_concat_part(state, opts, 2, 'K', log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=2 complete") != NULL && strcmp(notice_text(state, 1), "OK") == 0;

    dispatch_concat_part(state, opts, 1, 'N', log, sizeof log);
    ttb_block other = {0};
    ttb_sync_pdu(&other, 262, 31, 2);
    ok = ok && tetra_bsch_parse(other.bits, other.n, opts, state) == 1;
    const uint64_t before = notices(state);
    dispatch_concat_part(state, opts, 2, 'O', log, sizeof log);
    ok = ok && strstr(log, "part=2/2 received=1\n") != NULL && notices(state) == before;
    free_state(state);
    free(opts);
    return ok;
}

/* A message whose parts stop arriving for 300 s on the decode clock is dropped. */
static int
test_sds_concat_timeout(void) {
    const uint64_t second_ns = 1000000000ULL;
    dsd_state* state = alloc_notice_state();
    dsd_opts* opts = alloc_opts();
    char log[512];
    dsd_decode_clock_use_test(1000 * second_ns);
    dispatch_concat_part(state, opts, 1, 'O', log, sizeof log);
    dsd_decode_clock_test_set_ns(1301 * second_ns);
    dispatch_concat_part(state, opts, 2, 'K', log, sizeof log);
    int ok = strstr(log, "part=2/2 received=1\n") != NULL;
    dispatch_concat_part(state, opts, 1, 'O', log, sizeof log);
    ok = ok && strstr(log, "part=1/2 received=2 complete") != NULL && strcmp(notice(state, 1)->text_message, "OK") == 0;
    dsd_decode_clock_use_system();
    free_state(state);
    free(opts);
    return ok;
}

/* A concatenation part too short for its table 29.60 control fields is malformed and publishes nothing. */
static int
test_sds_truncated_concat_control(void) {
    uint8_t bits[96];
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    bit_writer w = sds_writer(bits, sizeof bits, 77, 16);
    put(&w, 12, 8);
    put(&w, 0, 8);
    put(&w, 0, 1);
    tetra_sds_pdu pdu;
    int ok = !tetra_sds_decode(bits, w.pos, &pdu);
    char log[256];
    tetra_test_mle_logged(bits, w.pos, 6, opts, state, log, sizeof log);
    ok = ok && strstr(log, "SDS") == NULL;
    free_state(state);
    free(opts);
    return ok;
}

int
main(void) {
    int passed = 0, total = 0;

#define RUN(fn)                                                                                                        \
    do {                                                                                                               \
        total++;                                                                                                       \
        if (fn()) {                                                                                                    \
            DSD_FPRINTF(stderr, "PASS: " #fn "\n");                                                                    \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            DSD_FPRINTF(stderr, "FAIL: " #fn "\n");                                                                    \
        }                                                                                                              \
    } while (0)

    RUN(test_standard_setup_release_status);
    RUN(test_sds_short_report_status);
    RUN(test_sds_published_trace);
    RUN(test_sds_ack_transport);
    RUN(test_sds_report_transport);
    RUN(test_sds_transfer_reference);
    RUN(test_sds_latin1_text);
    RUN(test_sds_utf16_text);
    RUN(test_sds_utf16_display_bound);
    RUN(test_sds_gsm_text);
    RUN(test_sds_transfer_timestamp_text);
    RUN(test_sds_concat_out_of_order_and_duplicates);
    RUN(test_sds_concat_transport_and_conflict);
    RUN(test_sds_concat_keyed_by_calling_party);
    RUN(test_sds_annex_e_type34_tail);
    RUN(test_function_not_supported_extract_and_o_bit);
    RUN(test_standard_mle_envelopes);
    RUN(test_annex_e19_e24_mle_responses);
    RUN(test_alert_proceeding_optional_atomicity);
    RUN(test_call_restore_optionals_and_atomicity);
    RUN(test_d_info_optionals_and_atomicity);
    RUN(test_connect_optionals_and_atomicity);
    RUN(test_release_notification_and_atomicity);
    RUN(test_floor_control_scoped_to_active_call);
    RUN(test_floor_control_party_optionals_and_clear);
    RUN(test_d_tx_granted);
    RUN(test_d_tx_ceased);
    RUN(test_d_setup_truncated_optional_rejected);
    RUN(test_floor_control_truncated_optionals_rejected);
    RUN(test_release_is_scoped_to_active_call);
    RUN(test_d_tx_ceased_notification_tail);
    RUN(test_tx_granted_ending_at_o_bit);
    RUN(test_sds_sender_stays_with_text);
    RUN(test_sds_control_characters);
    RUN(test_sds_unpaired_surrogates);
    RUN(test_sds_long_text_utf8_boundary);
    RUN(test_sds_concat_is_per_session);
    RUN(test_sds_concat_timeout);
    RUN(test_sds_concat_dropped_with_the_network);
    RUN(test_sds_truncated_concat_control);
#undef RUN

    DSD_FPRINTF(stderr, "\n%d / %d passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
