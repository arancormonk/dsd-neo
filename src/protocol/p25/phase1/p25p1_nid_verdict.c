// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/protocol/p25/p25p1_nid_verdict.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

/* Whether the established NAC still describes the stream: the carrier, the accepted tune and the sync histories it was
   named under are all still current. */
static int
p25p1_carrier_nac_live(const dsd_state* state) {
    const dsd_p25p1_carrier_nac* rec = &state->p25_p1_carrier_nac;
    return rec->valid != 0U && rec->carrier_seq == state->carrier_seq
           && rec->sync_history_drops == state->frame_sync_carry.drops
           && rec->tune_generation == dsd_trunk_tuning_generation();
}

int
p25p1_carrier_nac_established(const dsd_state* state) {
    if (state == NULL || !p25p1_carrier_nac_live(state)) {
        return -1;
    }
    return (int)state->p25_p1_carrier_nac.nac;
}

p25p1_nid_verdict
p25p1_nid_verdict_classify(const dsd_state* state, const struct p25p1_nid_result* decoded) {
    p25p1_nid_verdict verdict = {P25P1_NID_REJECTED, 0, 0U, 0, 0U, 0U, 0U, 0U};
    if (state == NULL || decoded == NULL || decoded->status <= 0) {
        return verdict;
    }
    verdict.disposition = P25P1_NID_ACCEPTED;
    verdict.nac = decoded->nac;
    verdict.duid = decoded->duid;
    verdict.corrections = decoded->error_count;
    verdict.parity_agreed = decoded->status == NID_OK ? 1U : 0U;
    verdict.nac_borrowed = decoded->nac_borrowed != 0 ? 1U : 0U;

    /* A borrowed NAC is the decoder's own, read off nothing. Nor is any NID before the carrier's NIDs established a NAC
       weighed against one. */
    const int established = p25p1_carrier_nac_established(state);
    if (verdict.nac_borrowed || established < 0 || decoded->nac == established) {
        return verdict;
    }
    /* 0x000 and 0xFFF never become the carrier's NAC, so they change nothing; but a costly NID naming either is as much
       a miscorrection as any other, and the likeliest one (the words a dead or stuck input decodes as), so it is
       weighed like one below. */
    verdict.nac_changed = p25p1_nid_nac_usable(decoded->nac) ? 1U : 0U;
    if (decoded->error_count <= P25P1_NID_NAC_CHANGE_MAX_CORRECTIONS) {
        return verdict;
    }
    /* A costly decode naming another NAC is as likely a miscorrected NID of the carrier's own as a real change (a
       control channel TSDU read as an LDU1 of another NAC). A second NID naming the same NAC settles it as a change. */
    const dsd_p25p1_carrier_nac* rec = &state->p25_p1_carrier_nac;
    if (rec->candidate_valid != 0U && (int)rec->candidate_nac == decoded->nac) {
        verdict.nac_confirmed = 1U;
        return verdict;
    }
    verdict.disposition = P25P1_NID_PROVISIONAL;
    return verdict;
}

void
p25p1_nid_verdict_commit(dsd_state* state, const p25p1_nid_verdict* verdict) {
    if (state == NULL || verdict == NULL || verdict->disposition == P25P1_NID_REJECTED || verdict->nac_borrowed) {
        return;
    }
    /* A NID a carrier boundary split names no carrier's NAC (issue #804). */
    if (state->frame_dispatch_active && dsd_state_frame_read_lost(state, state->frame_dispatch_carrier_seq)) {
        return;
    }
    dsd_p25p1_carrier_nac* rec = &state->p25_p1_carrier_nac;
    if (verdict->disposition == P25P1_NID_PROVISIONAL) {
        rec->candidate_nac = (uint16_t)verdict->nac;
        rec->candidate_valid = 1U;
        return;
    }
    /* An accepted NID settles the candidate; only a usable NAC becomes the carrier's. */
    rec->candidate_valid = 0U;
    if (!p25p1_nid_nac_usable(verdict->nac)) {
        return;
    }
    rec->nac = (uint16_t)verdict->nac;
    rec->carrier_seq = state->carrier_seq;
    rec->sync_history_drops = state->frame_sync_carry.drops;
    rec->tune_generation = dsd_trunk_tuning_generation();
    rec->valid = 1U;
}
