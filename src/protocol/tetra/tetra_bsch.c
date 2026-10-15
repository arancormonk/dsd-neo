// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA BSCH: the SYNC PDU (EN 300 392-2 clause 21.4.4.2) and the TDMA timebase it sets (clauses 7.3 and 7.4).
 *
 * The clock is kept as TN, FN and MN of one slot plus the symbol count at which that slot started. Moving it to a later
 * burst counts the symbols in between, so a burst the frame sync missed does not slip it.
 */

#include <dsd-neo/protocol/tetra/tetra_bsch.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>

#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_log.h"
#include "tetra_state.h"

#define TETRA_SYNC_PDU_BITS             60

/* EN 300 392-2 clause 9.3: 4 slots a frame, 18 frames a multiframe, 60 multiframes a hyperframe. */
#define TETRA_TDMA_SLOTS_PER_MULTIFRAME (4u * 18u)
#define TETRA_TDMA_SLOTS_PER_HYPERFRAME (TETRA_TDMA_SLOTS_PER_MULTIFRAME * 60u)

/* The longest the clock runs on without a burst: one multiframe, the BSCH period of every phase modulation carrier
 * (EN 300 392-2 table 9.27). Past that the symbol count may no longer measure air time, so the next BSCH sets the clock. */
#define TETRA_TDMA_MAX_GAP_SLOTS        TETRA_TDMA_SLOTS_PER_MULTIFRAME

typedef struct {
    uint8_t colour;
    uint8_t tn;
    uint8_t fn;
    uint8_t mn;
    uint16_t mcc;
    uint16_t mnc;
} tetra_sync_pdu;

/* EN 300 392-2 table 21.76 with the D-MLE-SYNC PDU of table 18.17. */
static int
tetra_sync_pdu_read(const uint8_t* bits, int len, tetra_sync_pdu* p) {
    tetra_bit_reader r = tetra_bits_reader(bits, len, 0);
    (void)tetra_bits_skip(&r, 4); /* system code */
    p->colour = (uint8_t)tetra_bits_read(&r, 6);
    p->tn = (uint8_t)(tetra_bits_read(&r, 2) + 1u);
    p->fn = (uint8_t)tetra_bits_read(&r, 5);
    p->mn = (uint8_t)tetra_bits_read(&r, 6);
    (void)tetra_bits_skip(&r, 8); /* sharing mode, TS reserved frames, U-plane DTX, frame 18 extension, reserved */
    p->mcc = (uint16_t)tetra_bits_read(&r, 10);
    p->mnc = (uint16_t)tetra_bits_read(&r, 14);
    (void)tetra_bits_skip(&r, 5); /* neighbour cell broadcast, cell load CA, late entry supported */
    return tetra_bits_ok(&r);
}

static int
tdma_numbers_ok(uint8_t tn, uint8_t fn, uint8_t mn) {
    return tn >= 1u && tn <= 4u && fn >= 1u && fn <= 18u && mn >= 1u && mn <= 60u;
}

/* The slot's place in its hyperframe, 0 .. TETRA_TDMA_SLOTS_PER_HYPERFRAME - 1. */
static uint32_t
tdma_slot_index(uint8_t tn, uint8_t fn, uint8_t mn) {
    return ((uint32_t)(mn - 1u) * 18u + (uint32_t)(fn - 1u)) * 4u + (uint32_t)(tn - 1u);
}

static void
tdma_stop(tetra_state_t* ts) {
    ts->tdma_valid = 0;
    /* Without the clock the hyperframe boundaries cannot be counted. */
    ts->hyperframe_valid = 0;
}

/* Whole slots from the clock's slot to the one that started at `slot_start`, rounded to the nearest; -1 when that slot
 * lies before the clock's or more than TETRA_TDMA_MAX_GAP_SLOTS after it. */
static int32_t
tdma_slots_to(const tetra_state_t* ts, uint32_t slot_start) {
    const uint32_t ahead = slot_start - ts->tdma_slot_start; /* both wrap with dsd_state::symbolcnt */
    const uint32_t behind = ts->tdma_slot_start - slot_start;
    if (behind < TETRA_TDMA_SLOT_SYMBOLS / 2u) {
        return 0;
    }
    if (ahead > TETRA_TDMA_MAX_GAP_SLOTS * TETRA_TDMA_SLOT_SYMBOLS + TETRA_TDMA_SLOT_SYMBOLS / 2u) {
        return -1;
    }
    return (int32_t)((ahead + TETRA_TDMA_SLOT_SYMBOLS / 2u) / TETRA_TDMA_SLOT_SYMBOLS);
}

/* Move the clock `slots` on (EN 300 392-2 clause 7.3.2: TN 4 is followed by TN 1 of the next frame, FN 18 by FN 1 of
 * the next multiframe, MN 60 by MN 1), counting a hyperframe (clause 9.3.8) at each MN 60 to MN 1 when its number is
 * known. */
static void
tdma_advance(tetra_state_t* ts, uint32_t slots) {
    const uint32_t index = tdma_slot_index(ts->tn, ts->fn, ts->mn) + slots;
    const uint32_t slot = index % TETRA_TDMA_SLOTS_PER_HYPERFRAME;
    if (ts->hyperframe_valid) {
        ts->hyperframe = (uint16_t)(ts->hyperframe + index / TETRA_TDMA_SLOTS_PER_HYPERFRAME);
    }
    ts->tn = (uint8_t)(slot % 4u + 1u);
    ts->fn = (uint8_t)((slot / 4u) % 18u + 1u);
    ts->mn = (uint8_t)(slot / TETRA_TDMA_SLOTS_PER_MULTIFRAME + 1u);
}

/* Run the clock to the slot that started at `slot_start`; 0 when it had to stop instead. */
static int
tdma_run(tetra_state_t* ts, uint32_t slot_start) {
    if (!ts->tdma_valid || !tdma_numbers_ok(ts->tn, ts->fn, ts->mn)) {
        tdma_stop(ts);
        return 0;
    }
    const int32_t slots = tdma_slots_to(ts, slot_start);
    if (slots < 0) {
        tdma_stop(ts);
        return 0;
    }
    tdma_advance(ts, (uint32_t)slots);
    ts->tdma_slot_start = slot_start;
    return 1;
}

/* Set the clock from a SYNC PDU heard in the slot that started at `slot_start`. A running clock first carries the
 * hyperframe number to this slot; if the PDU then puts the slot on the other side of a hyperframe boundary than the
 * clock did, the number moves by one to match. */
static void
tdma_anchor(tetra_state_t* ts, const tetra_sync_pdu* p, uint32_t slot_start) {
    if (!tdma_numbers_ok(p->tn, p->fn, p->mn)) {
        /* A reserved frame or multiframe number sets no clock. */
        tdma_stop(ts);
        return;
    }
    if (ts->tdma_valid && tdma_run(ts, slot_start) && ts->hyperframe_valid) {
        const uint32_t clock = tdma_slot_index(ts->tn, ts->fn, ts->mn);
        const uint32_t heard = tdma_slot_index(p->tn, p->fn, p->mn);
        if (clock > heard && clock - heard > TETRA_TDMA_SLOTS_PER_HYPERFRAME / 2u) {
            ts->hyperframe++;
        } else if (heard > clock && heard - clock > TETRA_TDMA_SLOTS_PER_HYPERFRAME / 2u) {
            ts->hyperframe--;
        }
    }
    ts->tn = p->tn;
    ts->fn = p->fn;
    ts->mn = p->mn;
    ts->tdma_slot_start = slot_start;
    ts->tdma_valid = 1;
}

void
tetra_tdma_follow(dsd_state* state, uint32_t slot_start) {
    tetra_state_t* const ts = tetra_st(state);
    if (ts && ts->tdma_valid) {
        (void)tdma_run(ts, slot_start);
    }
}

void
tetra_tdma_stop(dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (ts) {
        tdma_stop(ts);
    }
}

/* A BSCH of another cell, or a SYSINFO of one that shares the colour code (tetra_bsch_sysinfo_cell()): its timebase,
 * hyperframe numbering and SYSINFO are its own. The last cell's main carrier, band, offset and location area go
 * (EN 300 392-2 clause 21.4.4.1), and the control channel its SYSINFO gave with them, so a channel allocation decoded
 * before this cell's SYSINFO resolves to no frequency (clause 21.5.2) rather than to one of the last cell's, and
 * nothing is followed until the new SYSINFO names its control channel. The partial TM-SDUs go too: a TM-SDU is rebuilt
 * on one cell's control channel (clause 23.4.3.1.1), and a fragment of another cell must not continue it, whatever its
 * colour code. So do the channels the last cell allocated to the calls and the event labels it assigned
 * (tetra_calls_forget_cell()), the records staying. And the receiver is on another carrier, whatever the engine saw (a
 * radio retuned outside the decoder): what the last carrier's speech left goes as at a carrier boundary -- its
 * timeslots' signalling encryption, the heard timeslot with its canonical call, the speech in progress and the followed
 * call's proof on the carrier -- and so does the encryption mode the last cell's signalling showed. */
static void
bsch_forget_cell(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    tdma_stop(ts);
    tetra_mac_forget_fragments(state);
    tetra_calls_forget_trunked_carrier(opts, state);
    tetra_calls_forget_cell(state);
    tetra_voice_reset(state);
    tetra_confirm_forget_followed_call(state);
    ts->enc_mode = TETRA_ENC_MODE_NONE;
    ts->frames_resource = 0;
    if (ts->dl_carrier_hz > 0L && state->trunk_cc_freq == ts->dl_carrier_hz) {
        state->trunk_cc_freq = 0;
    }
    ts->dl_carrier_hz = 0;
    ts->frames_sysinfo = 0;
    ts->sysinfo_main_carrier = 0;
    ts->la = 0;
    ts->freq_band = 0;
    ts->freq_offset = 0;
    ts->sysinfo_known = 0;
}

/* What a BSCH naming another cell or another network leaves behind, decided in one place. Another cell (another colour
 * code, or another network) forgets the cell (bsch_forget_cell()). Another network (another MCC or MNC) forgets as well
 * what is keyed by the identities its SwMI gives out: a downlink SSI is a party of the current network (EN 300 392-2
 * clause 14.2.3), and its call identifiers are its own, so the call records end with the state machine's follow of
 * their identifiers (tetra_calls_forget_calls()), the concatenated short data being reassembled from its senders goes
 * (tetra_mle_forget_reassembly()), and so does the D-MM STATUS it sent. Another cell of the same network keeps them: its calls go on across cells, and
 * the call records end only when a SYSINFO names another location area (tetra_calls_note_location_area()). Runs before
 * the new identity is taken, so the calls that end carry the network they were decoded on. Nothing here tunes. */
static void
bsch_identity_change(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_sync_pdu* p) {
    if (!ts->net_known) {
        return;
    }
    const int other_network = ts->mcc != p->mcc || ts->mnc != p->mnc;
    if (other_network || ts->colour != p->colour) {
        bsch_forget_cell(opts, state, ts);
    }
    if (other_network) {
        tetra_calls_forget_calls(opts, state);
        tetra_mle_forget_reassembly(state);
        /* The D-MM STATUS the frontends show was the last network's to its MSs (EN 300 392-2 clause 16.9.2.5). */
        ts->mm_status_valid = 0;
        ts->mm_status_code = 0;
    }
}

void
tetra_bsch_sysinfo_cell(dsd_opts* opts, dsd_state* state, long main_carrier_hz, int la_present, uint16_t la) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    const int other_main = ts->dl_carrier_hz > 0L && main_carrier_hz > 0L && main_carrier_hz != ts->dl_carrier_hz;
    const int other_area = ts->sysinfo_known && la_present && la != ts->la;
    if (other_main || other_area) {
        TETRA_LOG_VERBOSE(ts, 1,
                          "[TETRA SYSINFO] main carrier %ld Hz, location area %u: another cell than %ld Hz, %u\n",
                          main_carrier_hz, (unsigned)la, ts->dl_carrier_hz, (unsigned)ts->la);
        bsch_forget_cell(opts, state, ts);
    }
}

int
tetra_bsch_parse(const uint8_t* bits, int len, dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    tetra_sync_pdu p;
    if (!bits || !ts) {
        return 0;
    }
    DSD_MEMSET(&p, 0, sizeof(p));
    if (!tetra_sync_pdu_read(bits, len, &p)) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA BSCH] short PDU len=%d, need %d\n", len, TETRA_SYNC_PDU_BITS);
        return 0;
    }
    bsch_identity_change(opts, state, ts, &p);
    ts->mcc = p.mcc;
    ts->mnc = p.mnc;
    ts->colour = p.colour;
    ts->lfsr_seed = tetra_compute_scramb_seed(p.mcc, p.mnc, p.colour);
    ts->net_known = 1;
    tdma_anchor(ts, &p, state->symbolcnt - TETRA_TDMA_SB_PARSE_SYMBOL);
    /* What the burst's block 2 signals reads the cell's colour code from the publication (dsd_access_code_current()):
     * it describes this cell from here, not the burst before. */
    tetra_publish(state);

    DSD_FPRINTF(stderr, "[TETRA BSCH] MCC=%u MNC=%u CC=0x%02x TN=%u FN=%u MN=%u\n", (unsigned)p.mcc, (unsigned)p.mnc,
                (unsigned)p.colour, (unsigned)p.tn, (unsigned)p.fn, (unsigned)p.mn);
    TETRA_LOG_PAYLOAD(ts, "[TETRA BSCH] scrambling seed=0x%08x\n", (unsigned)ts->lfsr_seed);
    return 1;
}
