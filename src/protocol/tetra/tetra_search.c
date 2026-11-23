// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * What a Signal search visit's own TETRA bursts prove (issue #748): gate A (trunked mode), gate D (direct mode), the
 * site note, its withdrawal and the visit's activity. The rules are in tetra_search.h.
 */

#include "tetra_search.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "tetra_mac_pdu.h"
#include "tetra_state.h"

enum {
    /* The type-1 bits of a half-slot signalling block (SCH/HD, BNCH, STCH; EN 300 392-2 clause 8.3.1.4.1): the only
       channels a pi/4-DQPSK SYSINFO is sent on (clause 21.4.4.1). */
    SEARCH_HALF_SLOT_BITS = 124,
    /* The highest V+D system code (0xxx, table 21.76). */
    SEARCH_TMO_CODE_MAX = 7,
    /* The direct mode system codes a DSB confirms with (EN 300 396-3 clause 9.3.29): 1100 and 1101. */
    SEARCH_DMO_CODE_ETS = 0xC,
    SEARCH_DMO_CODE_EN = 0xD,
    /* A/B channel usage 11 is reserved (EN 300 396-3 clause 9.3.1), and so is a presence signal's channel usage 11
       (EN 300 396-4 clause 10.3.6, EN 300 396-5 clause 14.3.6). */
    SEARCH_AB_RESERVED = 3,
    /* Repeater operating modes 11 is reserved (EN 300 396-4 clause 10.3.12). */
    SEARCH_REP_MODES_RESERVED = 3,
    /* A presence signal's channel state 11: a gateway's registration phase (EN 300 396-5), reserved for a pure DM-REP
       (EN 300 396-4 clause 10.3.5). */
    SEARCH_CHANNEL_STATE_GW_REGISTRATION = 3,
};

/* The open burst's read is lost: a stream flush (the symbol clock's stream moved, which raises no interruption) or a
   carrier boundary inside it, or an input interruption that cut it (dsd_state_frame_read_lost()). What it read is not
   one transmission's, so it corroborates nothing. */
static int
search_burst_lost(const dsd_state* state, const tetra_search_t* sr) {
    return sr->burst_epoch != state->symbol_clock_epoch || dsd_state_frame_read_lost(state, sr->burst_carrier_seq);
}

/* The visit's search state, when the burst being decoded is a search visit's (tetra_search_burst_begin()) and its read
   is whole; a lost read drops the observation, whatever the burst carried. */
static tetra_search_t*
search_in_burst(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || !ts->search.burst_open) {
        return NULL;
    }
    tetra_search_t* const sr = &ts->search;
    if (search_burst_lost(state, sr)) {
        sr->obs_valid = 0;
        return NULL;
    }
    return sr;
}

/* The burst carried content of @p mode (DSD_SEARCH_TETRA_TMO or _DMO): it keeps the visit active when that mode named
   the detection. The other mode's content never does: its gate opening withdraws the identity instead, and a carrier
   that goes on in the other mode must not hold a detection whose own transmission ended. */
static void
search_mark_activity(tetra_search_t* sr, uint8_t mode) {
    if (sr->first_mode == mode) {
        sr->burst_activity = 1;
    }
}

void
tetra_search_reset(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        DSD_MEMSET(&ts->search, 0, sizeof(ts->search));
    }
}

void
tetra_search_burst_begin(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    tetra_search_t* const sr = &ts->search;
    sr->burst_open = (uint8_t)(dsd_signal_search_is_running(state) != 0);
    if (!sr->burst_open) {
        return;
    }
    sr->burst = state->symbol_clock;
    sr->burst_carrier_seq = state->carrier_seq;
    sr->burst_epoch = state->symbol_clock_epoch;
    sr->burst_interrupt_seq = state->input_interrupt_seq;
    sr->burst_sync = DSD_SYNC_IS_TETRA(state->lastsynctype) ? state->lastsynctype : -1;
    sr->burst_activity = 0;
    sr->burst_sysinfo.valid = 0;
    /* A retune, a stream flush or a gap since, or an input interruption standing or taken up since: the observation
       was of another carrier or stream, or the input no longer follows on from it. */
    if (sr->obs_valid
        && (sr->obs_carrier_seq != sr->burst_carrier_seq || sr->obs_epoch != sr->burst_epoch
            || sr->obs_interrupt_seq != sr->burst_interrupt_seq || state->input_interrupted)) {
        sr->obs_valid = 0;
    }
}

void
tetra_search_burst_end(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || !ts->search.burst_open) {
        return;
    }
    tetra_search_t* const sr = &ts->search;
    sr->burst_open = 0;
    /* A flush, a boundary or an interruption cut the burst: what the hunt reads next does not follow on from the
       observation, and the burst keeps the visit no more active. */
    if (search_burst_lost(state, sr)) {
        sr->obs_valid = 0;
        return;
    }
    if (sr->burst_activity) {
        dsd_signal_search_note_activity(state);
    }
}

/* ---- Site notes ----------------------------------------------------------------------------------------------- */

static int
note_equal(const dsd_signal_search_tetra_note* a, const dsd_signal_search_tetra_note* b) {
    return a->mode == b->mode && a->mcc == b->mcc && a->mnc == b->mnc && a->la == b->la && a->la_valid == b->la_valid
           && a->main_state == b->main_state && a->main_hz == b->main_hz && a->route == b->route
           && a->presence_only == b->presence_only;
}

/* Send @p n unless the visit's identity was withdrawn or it is the note sent last. */
static void
send_note(const dsd_state* state, tetra_search_t* sr, const dsd_signal_search_tetra_note* n) {
    if (sr->withdrawn || (sr->note_sent && note_equal(&sr->note, n))) {
        return;
    }
    sr->note = *n;
    sr->note_sent = 1;
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_TETRA;
    note.tetra = *n;
    dsd_signal_search_note_site(state, &note);
}

/* The visit's identity turned ambiguous: its site and access go for the rest of the visit, the note sent once. */
static void
withdraw(const dsd_state* state, tetra_search_t* sr) {
    if (sr->withdrawn) {
        return;
    }
    sr->withdrawn = 1;
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW;
    dsd_signal_search_note_site(state, &note);
}

/* The trunked cell's note: its MCC and MNC and, for a colour code not 0, its SYSINFO once one came; none while its
 * carrier is shared. */
static void
tmo_site(const dsd_state* state, tetra_search_t* sr) {
    if (sr->shared) {
        return;
    }
    dsd_signal_search_tetra_note n;
    DSD_MEMSET(&n, 0, sizeof(n));
    n.mode = DSD_SEARCH_TETRA_TMO;
    n.mcc = sr->cell.mcc;
    n.mnc = sr->cell.mnc;
    n.main_state = DSD_SEARCH_TETRA_MAIN_NONE;
    if (sr->cell.colour != 0u && sr->sysinfo.valid) {
        n.la = sr->sysinfo.la;
        n.la_valid = 1;
        n.main_state = sr->sysinfo.main_state;
        n.main_hz = sr->sysinfo.main_hz;
    }
    send_note(state, sr, &n);
}

/* ---- Gate A (trunked mode) ------------------------------------------------------------------------------------ */

static int
cell_same(const tetra_search_cell* a, const tetra_search_cell* b) {
    return a->mcc == b->mcc && a->mnc == b->mnc && a->colour == b->colour;
}

/* Whether the visit's identity is ambiguous as gate A opens: gate D opened first, a second cell was observed in the
 * visit, or the identity was withdrawn already. */
static int
gate_a_ambiguous(const tetra_search_t* sr) {
    return sr->gate_d || sr->two_cells || sr->withdrawn;
}

/* Gate A opens on @p cell (copied: it may be the observation). The SYSINFO of the burst that opened it, under its seed,
 * supplies its site. Its evidence names it unless gate D opened first, which is a second identity of the channel. An
 * identity ambiguous already (a second cell observed in the visit) still confirms TETRA, with no access code and no
 * site: the withdrawal follows at once, but the hit is published, and may reach the journal, before it. */
static void
gate_a_open(const dsd_state* state, tetra_search_t* sr, tetra_search_cell cell) {
    sr->gate_a = 1;
    sr->cell = cell;
    if (cell.colour != 0u && sr->burst_sysinfo.valid) {
        sr->sysinfo = sr->burst_sysinfo;
    }
    const int ambiguous = gate_a_ambiguous(sr);
    if (sr->first_mode == 0u) {
        sr->first_mode = DSD_SEARCH_TETRA_TMO;
        char access[8] = "";
        if (!ambiguous) {
            DSD_SNPRINTF(access, sizeof(access), "CC %u", (unsigned)cell.colour);
        }
        dsd_signal_search_note_evidence_at(state, "TETRA", sr->burst_sync, -1, 0U, 0U, ambiguous ? NULL : access);
    }
    search_mark_activity(sr, DSD_SEARCH_TETRA_TMO);
    if (ambiguous) {
        withdraw(state, sr);
        return;
    }
    tmo_site(state, sr);
}

/* The visit's first BSCH identity, and whether this one is another. */
static void
note_identity(tetra_search_t* sr, const tetra_search_cell* c) {
    if (!sr->first_valid) {
        sr->first = *c;
        sr->first_valid = 1;
    } else if (!cell_same(&sr->first, c)) {
        sr->two_cells = 1;
    }
}

/* A checked BSCH whose SYNC sharing mode is not 00: the carrier is shared, so the gate cell shows no site, and one sent
 * already goes. It holds for the rest of the visit, whatever becomes of the observation gate A waits on (a yield or an
 * interruption that drops it proves nothing of the cell): while the visit saw one BSCH identity it is that cell's
 * word, and a second identity makes the visit's identity ambiguous anyway. */
static void
note_sharing(const dsd_state* state, tetra_search_t* sr, uint8_t sharing) {
    if (sharing == 0u || sr->shared) {
        return;
    }
    sr->shared = 1;
    if (sr->gate_a && sr->note_sent) {
        withdraw(state, sr);
    }
}

/* A BSCH after gate A opened: the gate cell's keeps the visit active; another cell's withdraws the identity, and until
 * the gate cell's next BSCH a colour code 0 block (under the seed every such cell shares) is no activity. */
static void
bsch_after_gate(const dsd_state* state, tetra_search_t* sr, const tetra_search_cell* c) {
    sr->other_bsch = (uint8_t)!cell_same(&sr->cell, c);
    if (sr->other_bsch) {
        withdraw(state, sr);
        return;
    }
    search_mark_activity(sr, DSD_SEARCH_TETRA_TMO);
}

/* A BSCH before gate A opened: it becomes the observation, or, for a colour code 0 cell, a second one of the same
 * identity from a later burst opens the gate. One burst reported twice is one observation. */
static void
bsch_before_gate(const dsd_state* state, tetra_search_t* sr, const tetra_search_cell* c) {
    const int same = sr->obs_valid && cell_same(&sr->obs, c);
    if (same && sr->burst <= sr->obs_burst) {
        return;
    }
    if (same && c->colour == 0u) {
        gate_a_open(state, sr, *c);
        return;
    }
    sr->obs = *c;
    sr->obs_burst = sr->burst;
    sr->obs_carrier_seq = sr->burst_carrier_seq;
    sr->obs_epoch = sr->burst_epoch;
    sr->obs_interrupt_seq = sr->burst_interrupt_seq;
    sr->obs_valid = 1;
}

void
tetra_search_note_bsch(dsd_state* state, const uint8_t* bits, int len) {
    tetra_search_t* const sr = search_in_burst(state);
    tetra_bsch_sync p;
    if (!sr || !tetra_bsch_read_sync(bits, len, &p) || p.system_code > SEARCH_TMO_CODE_MAX
        || !tetra_bsch_tdma_numbers_ok(p.tn, p.fn, p.mn)) {
        return;
    }
    const tetra_search_cell c = {tetra_compute_scramb_seed(p.mcc, p.mnc, p.colour), p.mcc, p.mnc, p.colour};
    note_identity(sr, &c);
    note_sharing(state, sr, p.sharing_mode);
    if (sr->gate_a) {
        bsch_after_gate(state, sr, &c);
    } else {
        bsch_before_gate(state, sr, &c);
    }
}

/* The main carrier a SYSINFO advertises (EN 300 392-2 table 21.65): none (band 0 and carrier 0, note 3), none this
 * decoder can name (band 0 otherwise, or a band with no base frequency, TS 100 392-15 table 1), or its downlink
 * frequency. */
static void
sysinfo_main(const tetra_mac_sysinfo* si, tetra_search_sysinfo* out) {
    out->main_state = DSD_SEARCH_TETRA_MAIN_NONE;
    out->main_hz = 0U;
    out->main_carrier = si->main_carrier;
    out->band = si->band;
    out->offset = si->offset;
    if (si->band == 0u) {
        if (si->main_carrier == 0u) {
            out->main_state = DSD_SEARCH_TETRA_MAIN_NO_CA;
        }
        return;
    }
    const long hz = tetra_carrier_to_dl_hz(si->main_carrier, si->band, si->offset);
    if (hz > 0L) {
        out->main_state = DSD_SEARCH_TETRA_MAIN_HZ;
        out->main_hz = (uint32_t)hz;
    }
}

/* A complete SYSINFO in a half-slot block (with its D-MLE-SYSINFO): the burst's, for the gate's site. An SCH/F that
 * begins with SYSINFO bits, or a SYSINFO without its D-MLE part, is no site. */
static void
note_block_sysinfo(tetra_search_t* sr, const uint8_t* bits, int len) {
    tetra_mac_broadcast bc;
    if (len != SEARCH_HALF_SLOT_BITS || !tetra_mac_broadcast_decode(bits, len, &bc) || bc.type != TETRA_MAC_BC_SYSINFO
        || !bc.sysinfo.mle_present) {
        return;
    }
    sr->burst_sysinfo.la = bc.sysinfo.la;
    sysinfo_main(&bc.sysinfo, &sr->burst_sysinfo);
    sr->burst_sysinfo.valid = 1;
}

/* Whether two SYSINFOs under the gate seed name other cells: another main carrier, or a change between none, one this
 * decoder cannot name and one it can; or, beside a CA main carrier, another location area. Two main carriers this
 * decoder cannot name both show none, so their band, carrier number and offset as sent tell them apart. With no CA main
 * carrier (band 0, carrier 0) the location area is the associated CA cell's, of which there is none (EN 300 392-2
 * clause 23.6.9), and the site shows none (site_tetra_format()): its bits tell no cell apart. */
static int
sysinfo_other_cell(const tetra_search_sysinfo* a, const tetra_search_sysinfo* b) {
    if (a->main_state != b->main_state || a->main_hz != b->main_hz) {
        return 1;
    }
    if (a->main_state == DSD_SEARCH_TETRA_MAIN_NONE
        && (a->main_carrier != b->main_carrier || a->band != b->band || a->offset != b->offset)) {
        return 1;
    }
    return a->main_state != DSD_SEARCH_TETRA_MAIN_NO_CA && a->la != b->la;
}

/* A SYSINFO under the open gate's seed: the first supplies the site, one naming another cell (sysinfo_other_cell())
 * withdraws it. A colour code 0 cell's SYSINFO is no one cell's. */
static void
gate_sysinfo(const dsd_state* state, tetra_search_t* sr) {
    const tetra_search_sysinfo* const si = &sr->burst_sysinfo;
    if (!si->valid || sr->cell.colour == 0u) {
        return;
    }
    if (!sr->sysinfo.valid) {
        sr->sysinfo = *si;
        tmo_site(state, sr);
        return;
    }
    if (sysinfo_other_cell(si, &sr->sysinfo)) {
        withdraw(state, sr);
    }
}

void
tetra_search_note_tmo_block(dsd_state* state, uint32_t seed, const uint8_t* bits, int len) {
    tetra_search_t* const sr = search_in_burst(state);
    if (!sr) {
        return;
    }
    const tetra_search_cell* const c = sr->gate_a ? &sr->cell : (sr->obs_valid ? &sr->obs : NULL);
    if (!c || c->seed != seed) {
        return;
    }
    note_block_sysinfo(sr, bits, len);
    if (sr->gate_a) {
        /* A colour code 0 seed binds no cell: after another cell's BSCH the block may be that cell's. */
        if (c->colour != 0u || !sr->other_bsch) {
            search_mark_activity(sr, DSD_SEARCH_TETRA_TMO);
        }
        gate_sysinfo(state, sr);
    } else if (c->colour != 0u) {
        gate_a_open(state, sr, *c);
    }
}

/* ---- Gate D (direct mode) ------------------------------------------------------------------------------------- */

/* A DMAC-SYNC's layout checks: frame number in range, A/B channel usage not reserved, 00 through a gateway. */
static int
dmac_sync_checked(const tetra_dmo_sync_s_t* s) {
    if (!s->parsed || s->frame == 0u || s->ab_usage == SEARCH_AB_RESERVED) {
        return 0;
    }
    return s->comm_type < 2u || s->ab_usage == 0u;
}

/* A DPRES-SYNC's: read by its parser with its SCH/H, frame number in range, and a channel usage and channel state its
 * equipment may send. The repeater operating modes, sent by a DM-REP and by a gateway offering the DM-REP function
 * (EN 300 396-4 table 2, EN 300 396-5 table 5), are not the reserved 11. Channel usage 01 (channel A, frequency
 * efficient mode) or 10 (channel B) comes only from a type 2 repeater function: a type 1 DM-REP sends 00 (EN 300 396-4
 * table 2), and so does a gateway, unless it serves a type 2 call of its DM-REP function (EN 300 396-5 clause
 * 13.4.7.12). Channel state 11, a gateway's registration phase, never comes from a pure DM-REP. */
static int
dpres_sync_checked(const uint8_t* sch_s, const uint8_t* sch_h, tetra_dmo_sync_s_t* s) {
    tetra_dmo_pres_t p;
    if (!tetra_dmo_parse_presence(sch_s, sch_h, s, &p) || !p.parsed || s->frame == 0u
        || p.channel_usage == SEARCH_AB_RESERVED
        || (p.comm_type == 1u && p.channel_state == SEARCH_CHANNEL_STATE_GW_REGISTRATION)) {
        return 0;
    }
    const int repeater = p.comm_type == 1u || p.dmrep != 0u;
    if (repeater && p.rep_modes == SEARCH_REP_MODES_RESERVED) {
        return 0;
    }
    return p.channel_usage == 0u || (repeater && p.rep_modes != 0u);
}

/* Whether a DSB meets gate D, and what it shows: the route of a DMAC-SYNC (@p route), or a presence signal. */
static int
dsb_checked(const uint8_t* sch_s, const uint8_t* sch_h, uint8_t* route, uint8_t* presence) {
    tetra_dmo_sync_s_t s;
    if (!sch_h || !tetra_dmo_parse_sync_s(sch_s, &s)
        || (s.system_code != SEARCH_DMO_CODE_ETS && s.system_code != SEARCH_DMO_CODE_EN)) {
        return 0;
    }
    if (s.sync_pdu_type == TETRA_DMO_SYNC_DMAC) {
        *route = (uint8_t)(DSD_SEARCH_TETRA_ROUTE_DIRECT + (s.comm_type & 3u));
        *presence = 0;
        return dmac_sync_checked(&s);
    }
    *route = DSD_SEARCH_TETRA_ROUTE_NONE;
    *presence = 1;
    return s.sync_pdu_type == TETRA_DMO_SYNC_PRES && dpres_sync_checked(sch_s, sch_h, &s);
}

static void
dmo_site(const dsd_state* state, tetra_search_t* sr, uint8_t route, uint8_t presence) {
    dsd_signal_search_tetra_note n;
    DSD_MEMSET(&n, 0, sizeof(n));
    n.mode = DSD_SEARCH_TETRA_DMO;
    n.route = route;
    n.presence_only = presence;
    send_note(state, sr, &n);
}

/* Gate D opens: its evidence and note name the detection unless gate A opened first, which withdraws the identity. */
static void
gate_d_open(const dsd_state* state, tetra_search_t* sr, uint8_t route, uint8_t presence) {
    sr->gate_d = 1;
    if (sr->first_mode != 0u) {
        withdraw(state, sr);
        return;
    }
    sr->first_mode = DSD_SEARCH_TETRA_DMO;
    dsd_signal_search_note_evidence_at(state, "TETRA DMO", sr->burst_sync, -1, 0U, 0U, NULL);
    dmo_site(state, sr, route, presence);
}

void
tetra_search_note_dsb(dsd_state* state, const uint8_t* sch_s, const uint8_t* sch_h) {
    tetra_search_t* const sr = search_in_burst(state);
    uint8_t route = DSD_SEARCH_TETRA_ROUTE_NONE;
    uint8_t presence = 0;
    if (!sr || !dsb_checked(sch_s, sch_h, &route, &presence)) {
        return;
    }
    if (!sr->gate_d) {
        gate_d_open(state, sr, route, presence);
    } else if (!presence) {
        /* The route follows the latest checked DMAC-SYNC; a presence signal is no route. */
        dmo_site(state, sr, route, 0);
    }
    /* A DMAC-SYNC the call records took as a call or short data (DMO_SERVICE): never DM-2-C, a reservation, a
       registration, a timing acknowledgement or a presence signal. */
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!presence && ts->confirm.burst_proof == (uint8_t)DSD_TETRA_BURST_PROOF_DMO_SERVICE) {
        search_mark_activity(sr, DSD_SEARCH_TETRA_DMO);
    }
}

void
tetra_search_note_dmo_content(dsd_state* state) {
    tetra_search_t* const sr = search_in_burst(state);
    if (sr && sr->gate_d) {
        search_mark_activity(sr, DSD_SEARCH_TETRA_DMO);
    }
}
