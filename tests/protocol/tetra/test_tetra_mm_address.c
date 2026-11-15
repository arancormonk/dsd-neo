// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The MS a location update concerns, through the real MAC -> LLC -> MLE -> MM path (B25). D-LOCATION UPDATE ACCEPT and
 * REJECT carry no ISSI of their own (EN 300 392-2 clause 16.4.1 note 5, tables 16.11 and 16.13): the MS is the one the
 * MAC-RESOURCE addressed (table 21.55). Only a clear SSI or USSI names a radio; an SMI is a management identity
 * (EN 300 392-1 clauses 7.3.2, 7.3.3) and an encrypted short identity names nobody (EN 300 392-7 clause 4.2.6). The
 * accept type (table 16.68), location update type (table 16.67) and reject cause (table 16.81) are named, and the
 * D-DISABLE / D-ENABLE line says whether the command is the intent or the confirm stage (EN 300 392-7 table A.68).
 * Main gave MM no MAC address, printed bare numbers and printed the Intent/confirm bit as "intent=1" for a confirm.
 *
 * Identities and the network are made up.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_calls.h"
#include "tetra_mm_pdu.h"
#include "tetra_state.h"
#include "tetra_test_log.h"
#include "tetra_test_pdu.h"

#define TEST_MS_SSI       1234567u
#define TEST_ASSIGNED_SSI 7654321u
#define TEST_MCC          123u
#define TEST_MNC          456u

static int g_failures = 0;

static void
check(int cond, const char* what) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void
check_log(const char* log, const char* want, const char* what) {
    if (strstr(log, want) == NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s: no \"%s\" in:\n%s\n", what, want, log);
        g_failures++;
    }
}

static void
check_no_log(const char* log, const char* unwanted, const char* what) {
    if (strstr(log, unwanted) != NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s: \"%s\" in:\n%s\n", what, unwanted, log);
        g_failures++;
    }
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} session;

static session
session_open(void) {
    session s;
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    if (!s.opts || !s.state) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        exit(1);
    }
    tetra_state_t* const ts = tetra_st(s.state);
    if (!ts) {
        DSD_FPRINTF(stderr, "no TETRA state\n");
        exit(1);
    }
    ts->mcc = TEST_MCC;
    ts->mnc = TEST_MNC;
    ts->net_known = 1;
    return s;
}

static void
session_close(session* s) {
    dsd_state_ext_free_all(s->state);
    free(s->state);
    free(s->opts);
}

/* A MAC-RESOURCE with address type @p addr_type and @p ssi carrying the TM-SDU @p sdu in a BL-UDATA, decoded as one
 * SCH/F block; what the decoder printed lands in @p log. */
static void
send_sdu(session* s, uint32_t addr_type, uint32_t ssi, const ttb_block* sdu, char* log, size_t log_size) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, addr_type, ssi);
    if (addr_type == TETRA_MAC_ADDR_SSI_EVENT || addr_type == TETRA_MAC_ADDR_SMI_EVENT) {
        ttb_put(&b, 5, 10); /* event label */
    }
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 2, 4); /* BL-UDATA */
    ttb_put_bits(&b, sdu->bits, sdu->n);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_test_mac_logged(b.bits, b.n, 0, s->opts, s->state, log, log_size);
}

/* D-LOCATION UPDATE ACCEPT (table 16.11) of @p accept_type, with the type 2 SSI element when @p assigned is not 0. */
static void
lu_accept(ttb_block* b, uint32_t accept_type, uint32_t assigned) {
    ttb_put(b, TETRA_MLE_PD_MM, 3);
    ttb_put(b, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    ttb_put(b, accept_type, 3);
    if (assigned == 0u) {
        ttb_put(b, 0, 1); /* O-bit */
        return;
    }
    ttb_put(b, 1, 1); /* O-bit */
    ttb_put(b, 1, 1); /* P-bit: SSI */
    ttb_put(b, assigned, 24);
    ttb_put(b, 0, 4); /* no address extension, subscriber class, energy saving, SCCH information */
    ttb_put(b, 0, 1); /* M-bit: no type 3/4 element */
}

/* D-LOCATION UPDATE REJECT (table 16.13) answering @p update_type with @p cause, no cipher control. */
static void
lu_reject(ttb_block* b, uint32_t update_type, uint32_t cause) {
    ttb_put(b, TETRA_MLE_PD_MM, 3);
    ttb_put(b, TETRA_MM_D_LOCATION_UPDATING_REJECT, 4);
    ttb_put(b, update_type, 3);
    ttb_put(b, cause, 5);
    ttb_put(b, 0, 1); /* cipher control */
    ttb_put(b, 0, 1); /* O-bit */
}

/* D-DISABLE (EN 300 392-7 table A.31) at Intent/confirm @p confirm, disabling a subscription temporarily. */
static void
d_disable(ttb_block* b, uint32_t confirm) {
    ttb_put(b, TETRA_MLE_PD_MM, 3);
    ttb_put(b, TETRA_MM_D_DISABLE, 4);
    ttb_put(b, confirm, 1);
    ttb_put(b, 0, 1); /* disabling type: temporary */
    ttb_put(b, 0, 1); /* equipment disable */
    ttb_put(b, 1, 1); /* subscription disable */
    ttb_put(b, TEST_MCC, 10);
    ttb_put(b, TEST_MNC, 14);
    ttb_put(b, TEST_MS_SSI, 24);
    ttb_put(b, 0, 1); /* O-bit */
}

/* LU ACCEPT names the MAC-addressed MS and its accept type; the SSI element is the identity assigned to it. */
static void
test_accept_names_addressed_ms(void) {
    session s = session_open();
    char log[2048];
    ttb_block sdu = {0};
    lu_accept(&sdu, 3, TEST_ASSIGNED_SSI);
    send_sdu(&s, TETRA_MAC_ADDR_SSI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=3 (ITSI attach) ms=SSI(1234567) assigned=SSI(7654321)",
              "ITSI attach names the addressed MS and the assigned SSI");

    sdu = (ttb_block){0};
    lu_accept(&sdu, 2, 0);
    send_sdu(&s, TETRA_MAC_ADDR_SSI_EVENT, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=2 (periodic) ms=SSI(1234567)\n",
              "an SSI that assigns an event label still names the MS");

    sdu = (ttb_block){0};
    lu_accept(&sdu, 5, 0);
    send_sdu(&s, TETRA_MAC_ADDR_USSI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=5 (migrating or service restoration migrating) ms=USSI(1234567)",
              "a migrating MS is named by its USSI");
    session_close(&s);
}

/* An SMI is a management identity: never the radio of a location update, whatever its number. */
static void
test_smi_never_a_radio(void) {
    session s = session_open();
    char log[2048];
    ttb_block sdu = {0};
    lu_accept(&sdu, 0, 0);
    send_sdu(&s, TETRA_MAC_ADDR_SMI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=0 (roaming) addr=SMI(1234567) (management identity)",
              "an SMI-addressed accept shows the SMI as such");
    check_no_log(log, "ms=", "an SMI-addressed accept names no radio");

    sdu = (ttb_block){0};
    lu_reject(&sdu, 0, 3);
    send_sdu(&s, TETRA_MAC_ADDR_SMI_EVENT, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "addr=SMI(1234567) (management identity)", "an SMI with an event label is an SMI");
    check_no_log(log, "ms=", "an SMI-addressed reject names no radio");
    session_close(&s);
}

/* REJECT names the update it answers and the cause, and the MS. */
static void
test_reject_cause_named(void) {
    session s = session_open();
    char log[2048];
    ttb_block sdu = {0};
    lu_reject(&sdu, 3, 19);
    send_sdu(&s, TETRA_MAC_ADDR_SSI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log,
              "[TETRA MM D-LU-REJECT] CC=0 type=3 cause=19 cipher=0 (ITSI attach rejected: authentication failure) "
              "ms=SSI(1234567)",
              "reject cause 19 is an authentication failure");

    sdu = (ttb_block){0};
    lu_reject(&sdu, 2, 6);
    send_sdu(&s, TETRA_MAC_ADDR_SSI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "(periodic rejected: congestion)", "reject cause 6 is congestion");

    static const uint32_t reserved[] = {0u, 17u, 22u, 31u};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        sdu = (ttb_block){0};
        lu_reject(&sdu, 0, reserved[i]);
        send_sdu(&s, TETRA_MAC_ADDR_SSI, TEST_MS_SSI, &sdu, log, sizeof log);
        check_log(log, "(roaming rejected: reserved cause)", "reserved reject causes are named reserved");
    }
    session_close(&s);
}

/* The MM PDU inside a D-NEW-CELL (EN 300 392-2 table 18.6) is named by the same MAC address. */
static void
test_new_cell_mm_sdu_named(void) {
    session s = session_open();
    char log[2048];
    ttb_block sdu = {0};
    ttb_put(&sdu, TETRA_MLE_PD_MLE, 3);
    ttb_put(&sdu, TETRA_MLE_D_NEW_CELL, 3);
    ttb_put(&sdu, 1, 2); /* channel command valid */
    ttb_put(&sdu, 0, 1); /* O-bit */
    ttb_put(&sdu, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    ttb_put(&sdu, 0, 3); /* roaming */
    ttb_put(&sdu, 0, 1); /* O-bit */
    send_sdu(&s, TETRA_MAC_ADDR_SSI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=0 (roaming) ms=SSI(1234567)", "D-NEW-CELL's accept names the MS");
    session_close(&s);
}

/* An encrypted short identity no key decrypted names nobody, and a TM-SDU without its MAC PDU names no MS. */
static void
test_encrypted_identity_skipped(void) {
    session s = session_open();
    char log[2048];
    ttb_block sdu = {0};
    lu_accept(&sdu, 3, 0);
    tetra_mac_sdu ctx = {0};
    ctx.addr.type = TETRA_MAC_ADDR_SSI;
    ctx.addr.ssi = TEST_MS_SSI;
    ctx.addr.esi = 1;
    ctx.has_addr = 1;
    dsd_test_capture_stderr cap;
    const int captured = dsd_test_capture_stderr_begin(&cap, "tetra_mm_esi") == 0;
    tetra_mle_dispatch_sdu(sdu.bits, sdu.n, 0, &ctx, s.opts, s.state);
    log[0] = '\0';
    if (captured) {
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, sizeof log);
    }
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=3 (ITSI attach) addr=ESI (encrypted identity)",
              "an encrypted identity is shown as such");
    check_no_log(log, "1234567", "an encrypted identity's number is not shown as an SSI");

    tetra_test_mle_logged(sdu.bits, sdu.n, 0, s.opts, s.state, log, sizeof log);
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=3 (ITSI attach) addr=none", "no MAC PDU, no MS");

    /* A MAC PDU without an address of its own is no address either: not an event label, and never the SSI its
     * address field happens to hold. */
    tetra_mac_sdu bare = {0};
    bare.addr.type = TETRA_MAC_ADDR_SSI;
    bare.addr.ssi = TEST_MS_SSI;
    bare.has_addr = 0;
    const int captured_bare = dsd_test_capture_stderr_begin(&cap, "tetra_mm_bare") == 0;
    tetra_mle_dispatch_sdu(sdu.bits, sdu.n, 0, &bare, s.opts, s.state);
    log[0] = '\0';
    if (captured_bare) {
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, sizeof log);
    }
    check_log(log, "[TETRA MM D-LU-ACCEPT] CC=0 type=3 (ITSI attach) addr=none",
              "a MAC PDU without an address names no MS and no event label");
    check_no_log(log, "event label", "a MAC PDU without an address is not an event label");
    check_no_log(log, "1234567", "a MAC PDU without an address shows no SSI");
    session_close(&s);
}

/* The Intent/confirm element is printed as the stage it names: 0 intent, 1 confirm. */
static void
test_enable_disable_stage(void) {
    session s = session_open();
    char log[2048];
    ttb_block sdu = {0};
    d_disable(&sdu, 0);
    send_sdu(&s, TETRA_MAC_ADDR_SSI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "[TETRA MM D-DISABLE] CC=0 stage=intent equipment=0 subscription=1", "Intent/confirm 0 is intent");

    sdu = (ttb_block){0};
    d_disable(&sdu, 1);
    send_sdu(&s, TETRA_MAC_ADDR_SSI, TEST_MS_SSI, &sdu, log, sizeof log);
    check_log(log, "[TETRA MM D-DISABLE] CC=0 stage=confirm equipment=0 subscription=1", "Intent/confirm 1 is confirm");
    check_no_log(log, "intent=", "no field reads the confirm stage as intent");
    session_close(&s);
}

/* tetra_mm_addr_radio_ssi(): the one rule for whether an MM address is a radio. */
static void
test_radio_ssi_rule(void) {
    uint32_t ssi = 0;
    tetra_mm_addr a = {TEST_MS_SSI, TETRA_MM_ADDR_SSI, 0, TEST_MCC, TEST_MNC, 1};
    check(tetra_mm_addr_radio_ssi(&a, &ssi) == 1 && ssi == TEST_MS_SSI, "a clear SSI is a radio");
    a.type = TETRA_MM_ADDR_USSI;
    check(tetra_mm_addr_radio_ssi(&a, &ssi) == 1, "a clear USSI is a radio");
    a.type = TETRA_MM_ADDR_SMI;
    check(tetra_mm_addr_radio_ssi(&a, &ssi) == 0, "an SMI is never a radio");
    a.type = TETRA_MM_ADDR_EVENT_LABEL;
    check(tetra_mm_addr_radio_ssi(&a, &ssi) == 0, "an event label is no radio");
    a.type = TETRA_MM_ADDR_UNKNOWN;
    check(tetra_mm_addr_radio_ssi(&a, &ssi) == 0, "an unknown address is no radio");
    a.type = TETRA_MM_ADDR_SSI;
    a.encrypted = 1;
    check(tetra_mm_addr_radio_ssi(&a, &ssi) == 0, "an encrypted identity is no radio");
    a.encrypted = 0;
    a.valid = 0;
    check(tetra_mm_addr_radio_ssi(&a, &ssi) == 0, "no address is no radio");
    check(tetra_mm_addr_radio_ssi(NULL, &ssi) == 0, "NULL is no radio");
}

int
main(void) {
    test_accept_names_addressed_ms();
    test_smi_never_a_radio();
    test_reject_cause_named();
    test_new_cell_mm_sdu_named();
    test_encrypted_identity_skipped();
    test_enable_disable_stage();
    test_radio_ssi_rule();
    if (g_failures != 0) {
        printf("FAIL  tetra_mm_address: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS  tetra_mm_address: all checks passed\n");
    return 0;
}
