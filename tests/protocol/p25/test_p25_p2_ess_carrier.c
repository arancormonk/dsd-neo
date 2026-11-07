// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 ESS across a carrier boundary, through the real RS(63,35) decoder (issue #575).
 *
 * The ESS_B the 4V bursts collect sits in dsd_state until a 2V burst decodes it with that burst's ESS_A. After a
 * carrier boundary, a 2V burst that comes before any 4V burst would decode the carrier left's ESS_B: with weak ESS_A
 * symbols the decoder erases all 28 parity symbols, which always succeeds, recovers the old ALG/KID/MI and applies it to
 * the new carrier's call. And a decode that leaned on soft erasures to that depth checks nothing, so it cannot prove the
 * descrambling seed.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25p2_frame_internal.h"

extern int ess_a[2][168];
extern int16_t ess_a_llr[2][168];

/* The vocoder and GPS objects the Phase 2 frame pulls in reach the NXDN scrambler and the DMR text decoder, which this
   case never runs. */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

/* Access notes to the signal search (issue #802), and the Phase 2 NAC a call read when the latest one came. */
static int g_access_notes;
static long long g_access_read;

static void
capture_access(const dsd_state* state) {
    ++g_access_notes;
    uint16_t value = 0U;
    g_access_read = dsd_access_code_current(state, DSD_SYNC_P25P2_POS, 0U, 0U, NULL, &value) ? (long long)value : -1;
}

static int
expect_int(const char* tag, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* A site whose descrambling seed is in force, and a buffer descrambled with it. */
static void
seed_site(dsd_opts* opts, dsd_state* state, unsigned long long nac) {
    p25_p2_frame_reset();
    state->p2_wacn = 1;
    state->p2_sysid = 1;
    state->p2_cc = nac;
    p25p2_process_frame_scramble(opts, state);
}

/* Slot 0's ESS_B, received well: every 6-bit symbol 0x21, so no hard decode can snap it to the all-zero codeword. */
static void
receive_ess_b(dsd_state* state) {
    for (int i = 0; i < 96; i++) {
        state->ess_b[0][i] = ((i % 6) == 0 || (i % 6) == 5) ? 1 : 0;
        state->ess_b_llr[0][i] = 1000;
    }
}

/* Slot 0's ESS_A from a 2V burst received weakly: every parity symbol is an erasure candidate. */
static void
receive_weak_ess_a(void) {
    for (int i = 0; i < 168; i++) {
        ess_a[0][i] = 0;
        ess_a_llr[0][i] = 1;
    }
}

static void
reset_session(dsd_opts* opts, dsd_state* state) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->currentslot = 0;
    dsd_state_forget_p2_proof(state);
    g_access_notes = 0;
    g_access_read = -1;
}

/* Slot 0's ESS received well: an all-zero ESS_B and ESS_A, which a hard decode checks against all 28 parity symbols. */
static void
receive_strong_ess(dsd_state* state) {
    for (int i = 0; i < 96; i++) {
        state->ess_b[0][i] = 0;
        state->ess_b_llr[0][i] = 1000;
    }
    for (int i = 0; i < 168; i++) {
        ess_a[0][i] = 0;
        ess_a_llr[0][i] = 1000;
    }
}

/* Issue #802 B7: the ESS that proves the seed may be a transmission's last evidence (a final 2V burst): its proof tells
   the signal search at once, so the detection reads the NAC it proved. A repeat, or a decode that proves nothing, tells
   it nothing. And the latest proof wins: a seed a burst proves after a network status named another NAC is the NAC the
   carrier reads (issue #802 B8). */
static int
test_ess_proof_notes_the_search(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    reset_session(opts, state);
    seed_site(opts, state, 0x456ULL);
    receive_strong_ess(state);
    p25p2_process_ess(opts, state, 0);
    rc |= expect_int("ess proof: one note", g_access_notes, 1);
    rc |= expect_int("ess proof: the note reads the seed's NAC", g_access_read, 0x456);
    receive_strong_ess(state);
    p25p2_process_ess(opts, state, 0);
    rc |= expect_int("ess proof: a repeated proof notes nothing", g_access_notes, 1);

    /* A decode that leaned on every parity symbol proves nothing and notes nothing. */
    reset_session(opts, state);
    seed_site(opts, state, 0x123ULL);
    receive_ess_b(state);
    receive_weak_ess_a();
    p25p2_process_ess(opts, state, 0);
    rc |= expect_int("full-erasure ess: no note", g_access_notes, 0);
    rc |= expect_int("full-erasure ess: no proven NAC", state->p2_proven_nac, -1);

    /* A network status named 0x293 on this carrier first; then a burst descrambled with the seed 0x456 passes. */
    reset_session(opts, state);
    seed_site(opts, state, 0x456ULL);
    (void)dsd_state_note_p2_proven_nac(state, 0x293ULL);
    receive_strong_ess(state);
    p25p2_process_ess(opts, state, 0);
    rc |= expect_int("latest proof: the seed's NAC", state->p2_proven_nac, 0x456);
    rc |= expect_int("latest proof: one note", g_access_notes, 1);
    rc |= expect_int("latest proof: the note reads it", g_access_read, 0x456);
    return rc;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    /* Control: on one carrier, the weak 2V burst's ESS decodes only by erasing all 28 parity symbols. It still decodes
       (the old behaviour), but a decode that checked nothing proves no seed. */
    reset_session(&opts, &state);
    seed_site(&opts, &state, 0x123ULL);
    receive_ess_b(&state);
    receive_weak_ess_a();
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("one carrier: the full-erasure decode still yields the ESS", state.payload_algid, 0x86);
    rc |= expect_int("one carrier: a full-erasure decode proves no seed", state.p2_cc_verified, 0);
    rc |= expect_int("one carrier: a full-erasure decode notes no identity", state.rx_id.valid, 0);

    /* A carrier boundary, then the new carrier's first voice burst is a 2V: the ESS_B left is not decoded. */
    reset_session(&opts, &state);
    seed_site(&opts, &state, 0x123ULL);
    receive_ess_b(&state);
    p25p2_frame_forget_carrier(&state);
    seed_site(&opts, &state, 0x456ULL);
    receive_weak_ess_a();
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("2V first after the boundary: no ALG of the carrier left", state.payload_algid, 0);
    rc |= expect_int("2V first after the boundary: no KID of the carrier left", state.payload_keyid, 0);
    rc |= expect_int("2V first after the boundary: no MI of the carrier left", (long long)state.payload_miP, 0);
    rc |= expect_int("2V first after the boundary: the new carrier's seed is not proven", state.p2_cc_verified, 0);

    /* A hard decode, which checks all 28 parity symbols, still proves the seed: a well-received all-zero ESS. */
    reset_session(&opts, &state);
    seed_site(&opts, &state, 0x456ULL);
    for (int i = 0; i < 96; i++) {
        state.ess_b[0][i] = 0;
        state.ess_b_llr[0][i] = 1000;
    }
    for (int i = 0; i < 168; i++) {
        ess_a[0][i] = 0;
        ess_a_llr[0][i] = 1000;
    }
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("a hard-decoded ESS proves the seed", state.p2_cc_verified, 1);
    rc |= expect_int("a hard-decoded ESS proves the seed's NAC", state.p2_proven_nac, 0x456);
    /* The proven seed is what the carrier sends: its WACN, SYSID and NAC are the received identity. */
    rc |= expect_int("the proven seed notes its parts", state.rx_id.valid,
                     DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_ACCESS);
    rc |= expect_int("the proven seed's network", (long long)state.rx_id.p25_wacn * 0x1000 + state.rx_id.p25_sysid,
                     0x1001);
    rc |= expect_int("the proven seed's NAC", state.rx_id.access, 0x456);

    /* Another system shared the carrier before (another NAC): the proven seed's NAC drops it, its network stays. */
    reset_session(&opts, &state);
    seed_site(&opts, &state, 0x456ULL);
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x111U);
    dsd_rxid_note_p25_network(&state, 0xAAAAAU, 0x222U);
    for (int i = 0; i < 96; i++) {
        state.ess_b[0][i] = 0;
        state.ess_b_llr[0][i] = 1000;
    }
    for (int i = 0; i < 168; i++) {
        ess_a[0][i] = 0;
        ess_a_llr[0][i] = 1000;
    }
    p25p2_process_ess(&opts, &state, 0);
    rc |= expect_int("co-channel: the proven seed's parts", state.rx_id.valid,
                     DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_ACCESS);
    rc |= expect_int("co-channel: the proven seed's network",
                     (long long)state.rx_id.p25_wacn * 0x1000 + state.rx_id.p25_sysid, 0x1001);
    rc |= expect_int("co-channel: the proven seed's NAC", state.rx_id.access, 0x456);

    /* A burst whose slot is out of range has no ESS_B, ESS_A or counters to read: the ESS path checks the slot once and
       decodes nothing, rather than indexing the slots' arrays with it. */
    static const int bad_slots[] = {-1, 2};
    for (size_t k = 0; k < sizeof bad_slots / sizeof bad_slots[0]; k++) {
        char tag[96];
        reset_session(&opts, &state);
        seed_site(&opts, &state, 0x456ULL);
        state.currentslot = bad_slots[k];
        p25p2_process_ess(&opts, &state, 0);
        DSD_SNPRINTF(tag, sizeof tag, "slot %d: no ESS decode is counted", bad_slots[k]);
        rc |= expect_int(tag, (long long)state.p25_p2_rs_ess_ok + (long long)state.p25_p2_rs_ess_err, 0);
        DSD_SNPRINTF(tag, sizeof tag, "slot %d: the seed is not proven", bad_slots[k]);
        rc |= expect_int(tag, state.p2_cc_verified, 0);
    }

    dsd_signal_search_access_set(capture_access);
    rc |= test_ess_proof_notes_the_search(&opts, &state);
    dsd_signal_search_access_set(NULL);

    dsd_state_ext_free_all(&state);
    if (rc == 0) {
        printf("P25_P2_ESS_CARRIER: OK\n");
    }
    return rc;
}
