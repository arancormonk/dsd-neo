// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What a Signal search visit's own TETRA bursts prove (issue #748): the detection's evidence, its site note and
 * its activity, reported through the runtime Signal search hooks (<dsd-neo/runtime/signal_search_hooks.h>) and only
 * while a search runs (dsd_signal_search_is_running()).
 *
 * Trunked mode (gate A). An observed BSCH is one whose CRC-16 passed and whose SYNC PDU the parser read whole, with a
 * V+D system code (0xxx, EN 300 392-2 table 21.76; reserved 100y and the direct mode codes never count) and TN, FN and
 * MN in range, taken once per burst. Gate A opens on the first of:
 * - colour code not 0: a CRC-16 downlink SCH/HD, BNCH, STCH or SCH/F block descrambled with exactly that BSCH's
 *   extended colour code (its seed binds MCC, MNC and colour code), from the BSCH's burst or a later one, once the MAC
 *   acted on it; never an uplink block, direct mode, speech or the AACH, and never a seed only a --tetra-net preset or
 *   an earlier visit gave;
 * - colour code 0: a second observed BSCH of the same MCC, MNC and colour code from a later burst (every colour code 0
 *   cell scrambles with the predefined all-zero code, table 21.76 note 1, so a block proves nothing of the network).
 * The observation outlives the hunt's no-carrier passes (they reset the decoder's carrier evidence, not this), and is
 * dropped when the carrier count or the symbol clock's stream moves (a retune, a stream flush or gap: between two
 * bursts, or inside a burst's read, which then corroborates nothing and keeps the visit no more active), when an input
 * interruption stands, cut a burst or was taken up by the engine loop since (dsd_state::input_interrupt_seq: a hunt
 * read the search interrupted between two bursts is an input cut), and at tetra_scan_visit_reset(), which the engine
 * runs at every landing and departure of a search visit.
 *
 * Its site note (DSD_SEARCH_SITE_NOTE_TETRA, mode TMO) carries the gate BSCH's MCC and MNC and, for a colour code not 0,
 * a SYSINFO decoded under the gate seed on a half-slot channel (SCH/HD, BNCH, STCH: EN 300 392-2 clause 21.4.4.1) with
 * its D-MLE-SYSINFO (table 21.65): the location area and the advertised main carrier (band 0 and carrier 0: no CA main
 * carrier, note 3; band 0 otherwise or 10 to 15: none; 1 to 9: its downlink frequency). A SYSINFO in an SCH/F, or one
 * without the D-MLE part, sets and withdraws nothing (the decoder's MAC still applies it). A colour code 0 cell shows
 * MCC and MNC only. A gate cell whose SYNC sharing mode is not 00 shows no site (carrier, MCCH or traffic carrier
 * sharing put several cells' bursts on one carrier).
 *
 * Direct mode (gate D). One DSB whose SCH/S passed its CRC with system code 1100 or 1101 (the reserved direct mode
 * codes never count), SYNC PDU type 00 (DMAC-SYNC) or 01 (DPRES-SYNC) read by its parser, frame number 1 to 18 and an
 * A/B channel usage valid for its layout (not 11; a gateway's DMAC-SYNC 00, EN 300 396-5 clause 14.1.1; a type 1 DM-REP's
 * presence signal 00, EN 300 396-4 table 2), and whose SCH/H passed its CRC in the same burst. Its evidence is "TETRA
 * DMO" with no access text, its note the route of a checked DMAC-SYNC's communication type (the latest one), or a
 * presence-only confirmation; never an MNI, an address or the DM colour code.
 *
 * The first gate to open names the detection. A latched withdrawal (DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW, sent once)
 * takes the hit's site and access back for the rest of the visit when its identity turns ambiguous: a second BSCH
 * identity on the carrier in the visit, a SYSINFO under the gate seed naming another location area or main carrier, a
 * gate cell's sharing mode turning out not 00 after its site was sent, or the other mode's gate opening. After a gate
 * opened, every burst that carries the gate's content refreshes the visit's activity
 * (dsd_signal_search_note_activity()): a CRC-passed downlink block or BSCH of the gate identity; in direct mode a
 * checked normal burst block under the visit's DM colour code, checked direct mode speech, or a checked DMAC-SYNC the
 * call records take as a call or short data (never a presence signal, DM-2-C, a reservation, a registration or a timing
 * acknowledgement). Evidence and site notes are sent only when they change.
 *
 * Decoder thread only; the state lives in tetra_state_t::search.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_SEARCH_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_SEARCH_H_

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A trunked cell's identity as its BSCH names it, and the scrambling seed it gives. */
typedef struct {
    uint32_t seed;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t colour;
} tetra_search_cell;

/* A SYSINFO decoded under the gate seed: what its site note shows. */
typedef struct {
    uint32_t main_hz;
    uint16_t la;
    uint8_t main_state; /* DSD_SEARCH_TETRA_MAIN_* */
    uint8_t valid;
} tetra_search_sysinfo;

typedef struct {
    /* The burst being decoded: its place on the symbol clock (one burst reported twice is one burst), the carrier and
     * stream it was read on, its sync type, and what it carried for the visit's activity. */
    uint64_t burst;
    uint32_t burst_carrier_seq;
    uint32_t burst_epoch;
    uint32_t burst_interrupt_seq;
    int burst_sync;
    uint8_t burst_open;
    uint8_t burst_activity;
    tetra_search_sysinfo burst_sysinfo; /* the burst's SYSINFO under the gate seed, once one passed */

    /* The observed BSCH gate A waits on, while the carrier and stream it was read on last, with no input interruption
     * taken up since. */
    tetra_search_cell obs;
    uint64_t obs_burst;
    uint32_t obs_carrier_seq;
    uint32_t obs_epoch;
    uint32_t obs_interrupt_seq;
    uint8_t obs_valid;

    /* The visit's first observed BSCH identity, and whether another one was observed since. */
    tetra_search_cell first;
    uint8_t first_valid;
    uint8_t two_cells;

    /* Gate A: its cell, its SYNC sharing mode seen other than 00, and the first SYSINFO under its seed. */
    tetra_search_cell cell;
    tetra_search_sysinfo sysinfo;
    uint8_t gate_a;
    uint8_t shared;

    /* Gate D, the mode that opened first, the withdrawal and the last site note sent. */
    uint8_t gate_d;
    uint8_t first_mode;
    uint8_t withdrawn;
    uint8_t note_sent;
    dsd_signal_search_tetra_note note;
} tetra_search_t;

/* Forget everything the visit proved (tetra_scan_visit_reset()). */
void tetra_search_reset(dsd_state* state);

/* A burst opens (tetra_burst_enter()) and closes (tetra_frame_leave(), after its proof is in). */
void tetra_search_burst_begin(dsd_state* state);
void tetra_search_burst_end(dsd_state* state);

/* A BSCH whose CRC passed and whose SYNC PDU (@p bits, @p len type-1 bits) tetra_bsch_parse() read, after it acted. */
void tetra_search_note_bsch(dsd_state* state, const uint8_t* bits, int len);

/* A trunked downlink signalling block whose CRC-16 passed once descrambled with @p seed: @p bits, @p len type-1 bits
 * (124 for a half-slot channel, 268 for an SCH/F), after the MAC acted on it. */
void tetra_search_note_tmo_block(dsd_state* state, uint32_t seed, const uint8_t* bits, int len);

/* A direct mode normal burst's checked block or speech. */
void tetra_search_note_dmo_content(dsd_state* state);

/* A DSB the decoder took (its SCH/S @p sch_s passed its CRC), with its SCH/H @p sch_h when that passed too (else NULL),
 * after the decoder acted on it. */
void tetra_search_note_dsb(dsd_state* state, const uint8_t* sch_s, const uint8_t* sch_h);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_SEARCH_H_ */
