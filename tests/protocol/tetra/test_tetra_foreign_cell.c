// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A followed TETRA call returns to the control channel it left (discussion #748): the traffic carrier F that cell A
 * assigns can be a carrier of another cell B too, and what B sends there (its BSCH, its SYSINFO naming its own main
 * carrier) rewrites the session's control channel (state->trunk_cc_freq) and cell. After an assigned channel an MS
 * returns to the MCCH it used before the assignment (EN 300 392-2 clause 23.3.1.2.1.2), so the state machine keeps the
 * control channel the follow started from, its home, and every way back goes there (tetra_sm_return_cc_hz()).
 *
 * Once synchronised an MS takes on only the SYNC of its own colour code (EN 300 392-2 clause 23.6.2; ETR 300-1 clause
 * 5.8.1.1.1), and only the home cell's SYSINFO names the home's main carrier: while the state machine guards a follow,
 * another cell's SYNC, a direct mode burst and another cell's SYSINFO on the followed carrier are not taken on, so the
 * session's control channel, cell, call records and the carrier's evidence stay the home's.
 *
 * What is heard on the followed carrier goes through the decoder's burst entry (processTetraSBFrame(),
 * processTetraFrame()) as real bursts from the test transmit chain (tetra_test_burst.h): another cell's blocks are
 * scrambled with its own colour code and fail their CRC under the home's, as on air. The control channel side goes
 * through the real BSCH parser (tetra_bsch_parse()) and MAC (tetra_mac_parse_schd()) the way tetra.c decodes a block.
 * Both reach the real call records and state machine; the tuning backend is a pair of stand-in hooks that do to the
 * shared tuning state what the engine's do. Time is the decode clock's TEST source.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_burst.h"
#include "tetra_test_pdu.h"

#define SECOND_NS 1000000000ULL

/* Cell A (home) and cell B share network 262/1001; B has another colour code unless a case says otherwise. Carrier
 * numbers are band 4, offset 2 (tetra_carrier_to_dl_hz()). */
enum { MCC = 262, MNC = 1001, COL_A = 10, COL_B = 20, LA_A = 100, LA_B = 200 };

enum { CARR_A_CC = 50, CARR_F = 60, CARR_G = 70, CARR_H = 80 };

enum { CALL_A = 0x0BEE, CALL_B = 0x0CAF, GSSI_A = 0x00BEEF, GSSI_B = 0x00CAFE, GSSI_HELD = 0x00F00D };

static int g_tunes;
static int g_returns;
static long g_tuned_hz;
static long g_returned_hz;
static uint64_t g_request;
static dsd_trunk_tune_result g_tune_result;
static dsd_trunk_tune_result g_return_result;

static long
hz(uint32_t carrier) {
    return tetra_carrier_to_dl_hz(carrier, 4U, 2U);
}

/* What the engine's tune hook does when it accepts a voice-channel tune (dsd_engine_tune_rtl()), in flight or done. */
static dsd_trunk_tune_result
tune_hook(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    g_tunes++;
    g_tuned_hz = freq;
    g_request = request_id;
    if (dsd_trunk_tune_result_is_ok(g_tune_result)) {
        opts->rtlsdr_center_freq = (uint32_t)freq;
        opts->trunk_is_tuned = 1;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
        state->last_vc_sync_time = dsd_decode_time();
        state->last_vc_sync_time_m = dsd_decode_now_mono_s();
    }
    return g_tune_result;
}

/* And when it accepts a return (dsd_engine_return_to_cc_request()), which takes no target: the P25 alias when one is
 * set, else state->trunk_cc_freq (dsd_engine_resolve_cc_freq()). */
static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    g_returns++;
    g_request = request_id;
    g_returned_hz = state->p25_cc_freq != 0 ? state->p25_cc_freq : state->trunk_cc_freq;
    if (dsd_trunk_tune_result_is_ok(g_return_result)) {
        opts->rtlsdr_center_freq = (uint32_t)g_returned_hz;
        opts->trunk_is_tuned = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    }
    return g_return_result;
}

static int
tuner_free_hook(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return 1;
}

static int
same_time(double a, double b) {
    return fabs(a - b) < 1e-9;
}

static void
at(double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * (double)SECOND_NS));
}

static void
reset_backend(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = return_hook;
    hooks.tuner_free_for_protocol = tuner_free_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_trunk_tuning_requests_reset();
    g_tunes = 0;
    g_returns = 0;
    g_tuned_hz = 0;
    g_returned_hz = 0;
    g_request = 0U;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_decode_clock_use_test(SECOND_NS * 1000ULL);
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    double t; /* the decode clock, seconds */
    char path[DSD_TEST_PATH_MAX];
} session;

static void
advance(session* s, double seconds) {
    s->t += seconds;
    at(s->t);
}

/* A cell as its bursts show it: network and colour code, which its scrambling seed is made of. */
typedef struct {
    uint32_t mcc;
    uint32_t mnc;
    uint32_t colour;
} cell;

static const cell k_cell_a = {MCC, MNC, COL_A};

static uint32_t
seed_of(const cell* c) {
    return ttb_seed(c->mcc, c->mnc, c->colour);
}

/* One burst whose block is a MAC block, as tetra.c decodes it. */
static void
mac_burst(session* s, const ttb_block* b) {
    tetra_confirm_burst_begin(s->state);
    tetra_mac_parse_schd(b->bits, b->n, 0, s->opts, s->state);
    tetra_confirm_note_signalling(s->state);
    (void)tetra_confirm_burst_end(s->state);
    tetra_sm_after_burst(s->opts, s->state);
}

static void
bsch_burst_of(session* s, const cell* c) {
    ttb_block b = {0};
    ttb_sync_pdu(&b, c->mcc, c->mnc, c->colour);
    tetra_confirm_burst_begin(s->state);
    assert(tetra_bsch_parse(b.bits, b.n, s->opts, s->state) == TETRA_BSCH_TAKEN);
    tetra_confirm_note_signalling(s->state);
    (void)tetra_confirm_burst_end(s->state);
    tetra_sm_after_burst(s->opts, s->state);
}

static void
bsch_burst(session* s, uint32_t colour) {
    const cell c = {MCC, MNC, colour};
    bsch_burst_of(s, &c);
}

static void
sysinfo_burst(session* s, uint32_t main_carrier, uint32_t la) {
    ttb_block b = {0};
    ttb_sysinfo(&b, main_carrier, 4U, 2U, la);
    mac_burst(s, &b);
}

/* A downlink allocation of timeslot bitmap @p timeslots on @p carrier (table 21.87), with @p extended carrier numbering
 * naming the band and offset of the cell's SYSINFO, and cell change flag @p cell_change. */
static void
allocation(ttb_block* b, uint32_t timeslots, uint32_t carrier, int extended, uint32_t cell_change) {
    if (!extended) {
        ttb_allocation_cell(b, timeslots, 1, carrier, cell_change);
        return;
    }
    ttb_put(b, 0, 2);           /* allocation type: replace current channel */
    ttb_put(b, timeslots, 4);   /* timeslot assigned */
    ttb_put(b, 1, 2);           /* downlink only */
    ttb_put(b, 0, 1);           /* CLCH permission */
    ttb_put(b, cell_change, 1); /* cell change flag */
    ttb_put(b, carrier, 12);    /* carrier number */
    ttb_put(b, 1, 1);           /* extended carrier numbering */
    ttb_put(b, 4, 4);           /* frequency band */
    ttb_put(b, 2, 2);           /* offset */
    ttb_put(b, 0, 4);           /* duplex spacing, reverse operation */
    ttb_put(b, 1, 2);           /* monitoring pattern: one */
}

/* The MAC-RESOURCE address of a PDU to @p ssi: by type (table 21.55) the SSI alone, or with the usage marker (6 bits)
 * or event label (10 bits) @p assigned it assigns. */
typedef struct {
    uint32_t type;
    uint32_t ssi;
    uint32_t assigned;
} mac_addr;

static int
resource_to(ttb_block* b, const mac_addr* addr) {
    const int start = ttb_resource(b, 0, addr->type, addr->ssi);
    if (addr->type == TETRA_MAC_ADDR_SSI_USAGE) {
        ttb_put(b, addr->assigned, 6);
    } else if (addr->type == TETRA_MAC_ADDR_SSI_EVENT) {
        ttb_put(b, addr->assigned, 10);
    }
    return start;
}

/* A D-SETUP's block (setup_block_as()). */
typedef struct {
    uint32_t gssi;
    uint32_t call_id;
    uint32_t timeslots;
    uint32_t carrier;
    int extended;
    const mac_addr* addr;
    uint32_t cell_change;
} setup_spec;

/* A D-SETUP to group @p gssi for @p call_id with a downlink allocation of timeslot bitmap @p timeslots on @p carrier,
 * whose MAC-RESOURCE is addressed @p addr (the group's SSI alone for NULL), its allocation of another cell with
 * @p cell_change (allocation()): an SCH/HD block, or with @p extended carrier numbering, which an SCH/HD has no room
 * for, an SCH/F. */
static void
setup_block_as(ttb_block* b, const setup_spec* spec) {
    const mac_addr group = {TETRA_MAC_ADDR_SSI, spec->gssi, 0U};
    const int start = resource_to(b, spec->addr ? spec->addr : &group);
    ttb_resource_flags(b, 1);
    allocation(b, spec->timeslots, spec->carrier, spec->extended, spec->cell_change);
    ttb_put(b, 2, 4); /* BL-UDATA */
    ttb_d_setup(b, spec->call_id, TTB_SPEECH_GROUP);
    ttb_close_resource(b, start);
    ttb_finish(b, spec->extended ? TTB_SCH_F_BITS : TTB_SCH_HD_BITS, -1);
}

static void
setup_block(ttb_block* b, uint32_t gssi, uint32_t call_id, uint32_t timeslots, uint32_t carrier, int extended) {
    const setup_spec spec = {gssi, call_id, timeslots, carrier, extended, NULL, 0U};
    setup_block_as(b, &spec);
}

static void
setup_burst(session* s, uint32_t gssi, uint32_t call_id, uint32_t timeslots, uint32_t carrier) {
    ttb_block b = {0};
    setup_block(&b, gssi, call_id, timeslots, carrier, 0);
    mac_burst(s, &b);
}

/* A live RTL session with a 2 s hangtime, on the control channel of @p home (A's network and colour code unless a case
 * says otherwise) by its BSCH and SYSINFO, whose D-MLE-SYSINFO names A's location area unless @p no_la: then the
 * SYSINFO carries none (its MAC part only). */
static session
open_on(const cell* home, int no_la) {
    session s;
    DSD_MEMSET(&s, 0, sizeof s);
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    s.t = 1000.0;
    at(s.t);
    initOpts(s.opts);
    initState(s.state);
    s.opts->payload = 0;
    s.opts->trunk_enable = 1;
    s.opts->trunk_hangtime = 2.0F;
    s.opts->frame_tetra = 1;
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)hz(CARR_A_CC);
    s.opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    s.opts->trunk_tune_group_calls = 1;
    s.opts->trunk_tune_private_calls = 1;
    s.opts->trunk_tune_data_calls = 1;
    s.opts->trunk_tune_enc_calls = 1;
    s.state->samplesPerSymbol = 10;
    /* A symbol file replays each dibit at its nominal level (+1, +3, -1, -3): slice there. */
    s.state->max = 3;
    s.state->umid = 2;
    s.state->center = 0;
    s.state->lmid = -2;
    s.state->min = -3;
    assert(tetra_st(s.state) != NULL);
    tetra_sm_init(s.opts, s.state);
    bsch_burst_of(&s, home);
    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_A_CC, 4U, 2U, LA_A);
    if (no_la) {
        si.n = 82; /* through the optional field: no room for a D-MLE-SYSINFO */
    }
    mac_burst(&s, &si);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.state->trunk_cc_freq == hz(CARR_A_CC));
    assert(tetra_st_peek(s.state)->sysinfo_known == !no_la);
    assert(tetra_sm_return_cc_hz(s.state) == 0 && !tetra_sm_guarding(s.opts, s.state));
    return s;
}

static session
open_on_a_as(int no_la) {
    return open_on(&k_cell_a, no_la);
}

static session
open_on_a(void) {
    return open_on_a_as(0);
}

static void
close_session(session* s) {
    if (s->opts->symbolfile) {
        (void)fclose(s->opts->symbolfile);
        s->opts->symbolfile = NULL;
    }
    if (s->path[0] != '\0') {
        (void)remove(s->path);
    }
    freeState(s->state);
    free(s->state);
    free(s->opts);
}

/* A's speech on the followed timeslot 2. */
static void
speech(session* s) {
    tetra_confirm_note_signalling(s->state);
    tetra_sm_note_voice(s->state, 2U);
}

/* A's group call granted to timeslot 2 of @p carrier and followed there. */
static void
follow_a(session* s, uint32_t carrier) {
    setup_burst(s, GSSI_A, CALL_A, 0x4U, carrier);
    tetra_sm_grant g = {0};
    assert(tetra_sm_followed(s->state, &g) && g.freq_hz == hz(carrier) && g.call_id == CALL_A);
    advance(s, 0.1);
    speech(s);
    tetra_sm_tick(s->opts, s->state);
    assert(tetra_sm_get_state(s->state) == TETRA_SM_TUNED);
}

/* The follow holds A's control channel as its home, however the session's control channel reads now. */
static void
assert_home_is_a(const session* s) {
    const tetra_state_t* const ts = tetra_st_peek(s->state);
    assert(ts->sm.home.valid && ts->sm.home.cc_hz == hz(CARR_A_CC));
    assert(ts->sm.home.mcc == MCC && ts->sm.home.mnc == MNC && ts->sm.home.colour == COL_A && ts->sm.home.net_known);
    assert(ts->sm.home.la == LA_A && ts->sm.home.la_known);
    assert(tetra_sm_return_cc_hz(s->state) == hz(CARR_A_CC));
    assert(tetra_sm_guarding(s->opts, s->state) == (s->opts->trunk_enable == 1));
}

/* Back on A's control channel after one return there, resting ON_CC on it, the follow and its home gone. */
static void
assert_back_home(const session* s, int returns) {
    assert(g_returns == returns && g_returned_hz == hz(CARR_A_CC));
    assert((long)s->opts->rtlsdr_center_freq == hz(CARR_A_CC) && s->opts->trunk_is_tuned == 0);
    assert(tetra_sm_get_state(s->state) == TETRA_SM_ON_CC && !tetra_sm_followed(s->state, NULL));
    assert(s->state->trunk_cc_freq == hz(CARR_A_CC));
    assert(tetra_sm_return_cc_hz(s->state) == 0 && !tetra_sm_guarding(s->opts, s->state));
    assert(!tetra_st_peek(s->state)->sm.home.valid && !tetra_st_peek(s->state)->sm.return_required);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Bursts on air, through the decoder's burst entry.
 * ------------------------------------------------------------------------------------------------------------- */

enum { SB_BB_DIBITS = 15, NDB_AACH2_DIBITS = 8 };

/* The symbols the burst decoder reads after the training sequence, as a symbol file (one dibit a byte). */
static void
feed(session* s, const uint8_t* dibits, int count) {
    if (s->opts->symbolfile) {
        (void)fclose(s->opts->symbolfile);
        (void)remove(s->path);
    }
    const int fd = dsd_test_mkstemp(s->path, sizeof s->path, "tetra_foreign");
    assert(fd >= 0);
    FILE* f = dsd_test_fdopen(fd, "w+b");
    assert(f != NULL);
    const size_t written = fwrite(dibits, 1U, (size_t)count, f);
    const int rewound = fseek(f, 0L, SEEK_SET);
    assert(written == (size_t)count && rewound == 0);
    s->opts->symbolfile = f;
    s->state->symbol_replay_header_checked = 0;
}

/* One burst through the burst entry, @p sync a synchronisation burst, whose symbols after the training sequence are
 * @p tail. The decoder reads them from a symbol file, so the input is one while the burst is read and the radio again
 * after it; while a call is followed nothing in the burst asks the radio's frequency (tetra_sm_receiver_freq() is the
 * followed channel's). 1 when the burst decoded a block whose CRC passed. */
static int
air_burst(session* s, int sync, const uint8_t* tail, int count) {
    feed(s, tail, count);
    s->opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    const int proven = sync ? processTetraSBFrame(s->opts, s->state) : processTetraFrame(s->opts, s->state);
    s->opts->audio_in_type = AUDIO_IN_RTL;
    return proven;
}

/* The training polarity of a synchronisation burst received @p inverted (every dibit's sign bit flipped), handed over
 * with its capture as the frame sync hands it; the carrier's polarity is the decoder's to set. */
static void
sync_captured(session* s, int inverted) {
    s->state->tetra_sync.sb1_valid = 1;
    s->state->tetra_sync.phase_rot = 0;
    s->state->tetra_sync.sb_polarity = (uint8_t)(inverted != 0);
    s->state->lastsynctype = inverted ? DSD_SYNC_TETRA_SB_NEG : DSD_SYNC_TETRA_SB_POS;
    s->state->synctype = s->state->lastsynctype;
}

/* A trunked synchronisation burst: BSCH @p sync_pdu (60 type-1 bits), captured before the training sequence, then the
 * broadcast block and block 2, @p block2 (124 type-1 bits) scrambled with @p seed, or noise for NULL. */
static int
sync_burst_on_air(session* s, const ttb_block* sync_pdu, const ttb_block* block2, uint32_t seed, int inverted) {
    const uint8_t flip = inverted ? 2U : 0U;
    uint8_t bsch[TTB_BSCH_DIBITS];
    uint8_t b2[TTB_BLOCK_DIBITS] = {0};
    ttb_encode_bsch(sync_pdu->bits, bsch);
    if (block2) {
        ttb_encode_sch_hd(block2->bits, seed, b2);
    }
    uint8_t tail[SB_BB_DIBITS + TTB_BLOCK_DIBITS];
    for (size_t i = 0; i < sizeof tail; i++) {
        tail[i] = (uint8_t)((i < SB_BB_DIBITS ? 0U : b2[i - SB_BB_DIBITS]) ^ flip);
    }
    for (size_t i = 0; i < TTB_BSCH_DIBITS; i++) {
        s->state->tetra_sync.sb1_dibuf[i] = (uint8_t)(bsch[i] ^ flip);
    }
    s->state->tetra_sync.sb1_soft_valid = 0;
    sync_captured(s, inverted);
    return air_burst(s, 1, tail, (int)sizeof tail);
}

/* Cell @p c's synchronisation burst: its SYNC (sharing mode @p sharing, table 21.76: 0 continuous transmission, 1
 * carrier sharing, 2 MCCH sharing) and block 2 @p block2 scrambled with its own seed. */
static int
cell_sync_burst(session* s, const cell* c, uint32_t sharing, const ttb_block* block2, int inverted) {
    ttb_block sync = {0};
    ttb_sync_pdu(&sync, c->mcc, c->mnc, c->colour);
    ttb_set(&sync, 23, sharing, 2);
    return sync_burst_on_air(s, &sync, block2, seed_of(c), inverted);
}

/* A normal downlink burst: block 1 @p b1 and AACH half 1 as the frame sync captured them, then AACH half 2 and block 2
 * @p b2 (108 dibits each); @p nts2 two half-slot blocks, else one full-slot channel across both. @p aach is the AACH's
 * 15 dibits (ttb_encode_aach()), or NULL for one the frame sync did not capture. */
static int
ndb_on_air_with(session* s, int nts2, const uint8_t* aach, const uint8_t* b1, const uint8_t* b2) {
    uint8_t tail[NDB_AACH2_DIBITS + TTB_BLOCK_DIBITS] = {0};
    if (aach) {
        DSD_MEMCPY(s->state->tetra_sync.aach1_dibuf, aach, TTB_AACH_DIBITS - NDB_AACH2_DIBITS);
        DSD_MEMCPY(tail, aach + (TTB_AACH_DIBITS - NDB_AACH2_DIBITS), NDB_AACH2_DIBITS);
    }
    DSD_MEMCPY(tail + NDB_AACH2_DIBITS, b2, TTB_BLOCK_DIBITS);
    DSD_MEMCPY(s->state->tetra_sync.b1_dibuf, b1, TTB_BLOCK_DIBITS);
    s->state->tetra_sync.b1_valid = 1;
    s->state->tetra_sync.b1_soft_valid = 0;
    s->state->tetra_sync.aach1_valid = (uint8_t)(aach != NULL);
    s->state->tetra_sync.nts2 = (uint8_t)(nts2 != 0);
    s->state->tetra_sync.cb = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    s->state->synctype = DSD_SYNC_TETRA_NDB_POS;
    return air_burst(s, 0, tail, (int)sizeof tail);
}

/* ndb_on_air_with(), its AACH not captured. */
static int
ndb_on_air(session* s, int nts2, const uint8_t* b1, const uint8_t* b2) {
    return ndb_on_air_with(s, nts2, NULL, b1, b2);
}

/* An NTS2 normal downlink burst of cell @p c: half-slot blocks @p block1 and @p block2 (124 type-1 bits each)
 * scrambled with its seed, either noise for NULL. */
static int
cell_ndb(session* s, const cell* c, const ttb_block* block1, const ttb_block* block2) {
    uint8_t b1[TTB_BLOCK_DIBITS] = {0};
    uint8_t b2[TTB_BLOCK_DIBITS] = {0};
    if (block1) {
        ttb_encode_sch_hd(block1->bits, seed_of(c), b1);
    }
    if (block2) {
        ttb_encode_sch_hd(block2->bits, seed_of(c), b2);
    }
    return ndb_on_air(s, 1, b1, b2);
}

/* An NTS1 normal downlink burst of cell @p c: an SCH/F of @p block (268 type-1 bits) scrambled with its seed. */
static int
cell_ndb_full(session* s, const cell* c, const ttb_block* block) {
    uint8_t slot[2 * TTB_BLOCK_DIBITS];
    ttb_encode_signalling(block->bits, TTB_SCH_F_BITS, 432, 103, seed_of(c), slot);
    return ndb_on_air(s, 0, slot, slot + TTB_BLOCK_DIBITS);
}

/* The nominal level of dibit @p d (+1 00, +3 01, -1 10, -3 11), as a soft symbol. */
static float
level_of(uint8_t d) {
    static const float k_levels[4] = {1.0F, 3.0F, -1.0F, -3.0F};
    return k_levels[d & 3U];
}

/* A direct mode synchronisation burst (EN 300 396-2 clause 9.4.3.3): a DMAC-SYNC SCH/S of a direct MS-MS call,
 * captured before the training sequence like a BSCH, and an all-zero SCH/H right after it, then 15 dibits, scrambled
 * with the all-zero colour code (EN 300 396-3 clause 8.2.4) and received @p inverted. With @p soft_only the captured
 * hard decisions of SCH/S are lost and only its soft values carry it, so it is a DSB only to the soft decision. */
static int
dsb_on_air(session* s, int inverted, int soft_only) {
    ttb_block sch_s = {0};
    ttb_put(&sch_s, 0xD, 4); /* system code: DMO */
    ttb_put(&sch_s, 0, 2);   /* SYNC PDU type: DMAC-SYNC */
    ttb_put(&sch_s, 0, 2);   /* communication type: direct MS-MS */
    ttb_put(&sch_s, 0, 4);   /* reserved; channel A, normal mode */
    ttb_put(&sch_s, 0, 2);   /* slot 1 */
    ttb_put(&sch_s, 3, 5);   /* frame 3 */
    ttb_put(&sch_s, 0, 2);   /* air interface encryption state: clear */
    while (sch_s.n < TTB_BSCH_BITS) {
        ttb_put(&sch_s, 0, 1);
    }
    const ttb_block sch_h = {{0}, TTB_SCH_HD_BITS};
    const uint8_t flip = inverted ? 2U : 0U;
    uint8_t sch_s_dibits[TTB_BSCH_DIBITS];
    uint8_t tail[TTB_BLOCK_DIBITS + SB_BB_DIBITS] = {0};
    ttb_encode_bsch(sch_s.bits, sch_s_dibits);
    ttb_encode_sch_hd(sch_h.bits, TTB_BSCH_SEED, tail);
    for (size_t i = 0; i < sizeof tail; i++) {
        tail[i] ^= flip;
    }
    for (size_t i = 0; i < TTB_BSCH_DIBITS; i++) {
        const uint8_t d = (uint8_t)(sch_s_dibits[i] ^ flip);
        s->state->tetra_sync.sb1_dibuf[i] = soft_only ? 0U : d;
        s->state->tetra_sync.sb1_soft[i] = level_of(d);
    }
    s->state->tetra_sync.sb1_soft_valid = (uint8_t)(soft_only != 0);
    sync_captured(s, inverted);
    return air_burst(s, 1, tail, (int)sizeof tail);
}

/* What the follow and the cell it started on hold, to compare across bursts that must change none of it. */
typedef struct {
    int tuned;
    tetra_sm_grant followed;
    int record;
    tetra_chan_alloc alloc;
    int proven;
    uint8_t rx_perm;
    uint8_t published;
    dsd_call_phase call_phase;
    int candidates;
    long trunk_cc_hz;
    long dl_carrier_hz;
    uint16_t la;
    uint16_t calls_la;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t colour;
    uint32_t seed;
    uint16_t access;
    uint8_t polarity;
} follow_view;

static follow_view
view_of(const session* s) {
    const tetra_state_t* const ts = tetra_st_peek(s->state);
    follow_view v;
    DSD_MEMSET(&v, 0, sizeof v);
    v.tuned = tetra_sm_followed(s->state, &v.followed);
    const tetra_call_t* const c = tetra_calls_peek(s->state, CALL_A);
    v.record = c != NULL;
    if (c) {
        v.alloc = c->alloc;
    }
    v.proven = tetra_confirm_followed_call_proven(s->state);
    v.rx_perm = ts->confirm.rx_perm;
    v.published = ts->calls.published;
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    v.call_phase = dsd_call_state_get(s->state, 0U, &call) == 1 ? call.phase : DSD_CALL_PHASE_IDLE;
    const dsd_trunk_cc_candidates* const cand = dsd_trunk_cc_candidates_peek(s->state);
    v.candidates = cand ? cand->count : 0;
    v.trunk_cc_hz = s->state->trunk_cc_freq;
    v.dl_carrier_hz = ts->dl_carrier_hz;
    v.la = ts->la;
    v.calls_la = ts->calls.la;
    v.mcc = ts->mcc;
    v.mnc = ts->mnc;
    v.colour = ts->colour;
    v.seed = ts->lfsr_seed;
    v.access = s->state->rx_id.access;
    v.polarity = s->state->tetra_sync.polarity;
    return v;
}

/* Nothing of the follow or of the home cell changed: still following A's call on its channel, its record, allocation,
 * proof, receive permission, publication and canonical call, the trunking candidates, the session's control channel
 * (A's), cell, location area, network, colour code, scrambling seed, received identity and the carrier's polarity. */
static void
assert_unchanged(const session* s, const follow_view* before) {
    const follow_view now = view_of(s);
    assert(now.tuned == before->tuned && now.followed.freq_hz == before->followed.freq_hz);
    assert(now.followed.call_id == before->followed.call_id && now.followed.timeslots == before->followed.timeslots);
    assert(now.record == before->record && now.alloc.carrier == before->alloc.carrier);
    assert(now.alloc.timeslots == before->alloc.timeslots && now.alloc.freq_hz == before->alloc.freq_hz);
    assert(now.proven == before->proven && now.rx_perm == before->rx_perm && now.published == before->published);
    assert(now.call_phase == before->call_phase && now.candidates == before->candidates);
    assert(now.trunk_cc_hz == hz(CARR_A_CC) && now.trunk_cc_hz == before->trunk_cc_hz);
    assert(now.dl_carrier_hz == before->dl_carrier_hz && now.la == before->la && now.calls_la == before->calls_la);
    assert(now.mcc == before->mcc && now.mnc == before->mnc && now.colour == before->colour);
    assert(now.seed == before->seed && now.access == before->access && now.polarity == before->polarity);
}

/* What is decoded on F after A's call landed there. */
typedef struct {
    const char* name;
    uint32_t colour; /* B's colour code */
    uint32_t main_carrier;
    uint32_t la;
    int sysinfo;     /* B's SYSINFO decoded, not only its BSCH */
    int a_bsch_then; /* A's BSCH decoded again after B */
} foreign_case;

static const foreign_case k_cases[] = {
    {"(a) other colour, other LA, B's SYSINFO names F", COL_B, CARR_F, LA_B, 1, 0},
    {"(a2) other colour, same LA", COL_B, CARR_F, LA_A, 1, 0},
    {"(a3) other colour, BSCH only", COL_B, CARR_F, LA_B, 0, 0},
    {"(b) same colour, other LA", COL_A, CARR_F, LA_B, 1, 0},
    {"(b2) same colour, same LA", COL_A, CARR_F, LA_A, 1, 0},
    {"(e) other colour, F is B's secondary carrier (SYSINFO names H)", COL_B, CARR_H, LA_B, 1, 0},
    {"(f) B heard, then A's BSCH again", COL_B, CARR_F, LA_B, 1, 1},
    {"(d, control) A's own BSCH and SYSINFO on F", COL_A, CARR_A_CC, LA_A, 1, 0},
};

static int
is_home_case(const foreign_case* c) {
    return c->colour == COL_A && c->main_carrier == CARR_A_CC && c->la == LA_A;
}

/* The case's cell (A itself for d) heard on F: its synchronisation burst, with its SYSINFO in block 2 when it has one,
 * then a normal burst carrying that SYSINFO again; for f, A's synchronisation burst after them. */
static void
hear_on_f(session* s, const foreign_case* c) {
    const cell b = {MCC, MNC, c->colour};
    ttb_block si = {0};
    ttb_sysinfo(&si, c->main_carrier, 4U, 2U, c->la);
    advance(s, 0.2);
    (void)cell_sync_burst(s, &b, 0U, c->sysinfo ? &si : NULL, 0);
    if (c->sysinfo) {
        (void)cell_ndb(s, &b, NULL, &si);
    }
    if (c->a_bsch_then) {
        (void)cell_sync_burst(s, &k_cell_a, 0U, NULL, 0);
    }
}

enum { END_HANGTIME, END_RELEASE, END_GRANT_TIMEOUT };

/* How the follow ends: the hangtime after speech, A's release of the call, or the grant timeout of a call that never
 * spoke. */
static void
end_follow(session* s, int how) {
    if (how == END_HANGTIME) {
        advance(s, 3.0);
        tetra_sm_tick(s->opts, s->state);
    } else if (how == END_RELEASE) {
        tetra_sm_on_release(s->opts, s->state, CALL_A);
    } else {
        advance(s, TETRA_SM_GRANT_TIMEOUT_S + 0.5);
        tetra_sm_tick(s->opts, s->state);
    }
}

/* Cell B on A's traffic carrier F takes nothing: B's SYNC of another colour code is not taken on, so its SYSINFO fails
 * its CRC under A's scrambling, and a SYSINFO of B's that A's scrambling decodes (same colour code) names another main
 * carrier or location area and is not applied. The session's control channel, cell, call records, the followed call's
 * proof and the trunking candidates stay A's (regression: B's BSCH cleared the control channel and forgot A's cell,
 * calls' allocations and proof; B's SYSINFO named F or H as the control channel, added it as a candidate, and with
 * another location area ended A's call records). The hangtime, a release and the grant timeout each return once to
 * A's control channel and rest ON_CC there (regression: main stayed on F as B's control channel, stranded IDLE with
 * none, or returned to H). Back there, A's BSCH and SYSINFO are taken on and A's next grant is followed with A's
 * control channel as its home again. A's own SYNC and SYSINFO on F (d) are taken on all along. */
static void
test_every_return_goes_home(void) {
    for (size_t k = 0; k < sizeof k_cases / sizeof k_cases[0]; k++) {
        for (int how = END_HANGTIME; how <= END_GRANT_TIMEOUT; how++) {
            printf("foreign cell %s, end %d\n", k_cases[k].name, how);
            reset_backend();
            session s = open_on_a();
            if (how == END_GRANT_TIMEOUT) {
                setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
            } else {
                follow_a(&s, CARR_F);
            }
            assert_home_is_a(&s);
            const follow_view before = view_of(&s);
            const uint32_t sysinfos = tetra_st_peek(s.state)->frames_sysinfo;
            hear_on_f(&s, &k_cases[k]);
            assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
            assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC) && tetra_sm_guarding(s.opts, s.state));
            assert_unchanged(&s, &before);
            assert(tetra_st_peek(s.state)->frames_sysinfo == sysinfos + (is_home_case(&k_cases[k]) ? 2U : 0U));
            assert_home_is_a(&s);
            end_follow(&s, how);
            assert_back_home(&s, 1);

            bsch_burst(&s, COL_A);
            sysinfo_burst(&s, CARR_A_CC, LA_A);
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && s.state->trunk_cc_freq == hz(CARR_A_CC));
            advance(&s, 1.0);
            setup_burst(&s, GSSI_A, CALL_A + 1U, 0x4U, CARR_G);
            assert(g_tuned_hz == hz(CARR_G) && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
            assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
            close_session(&s);
        }
    }
}

/* A reallocation of the followed call (TUNED to TUNED) keeps the home it started with, even once B's SYSINFO named
 * another control channel; the return from the reallocated channel still goes to A. */
static void
test_reallocation_keeps_the_home(void) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    sysinfo_burst(&s, CARR_F, LA_A); /* B, same colour and LA: not applied, the control channel stays A's */
    assert(s.state->trunk_cc_freq == hz(CARR_A_CC));
    const tetra_sm_grant moved = {hz(CARR_G), CARR_G, 0x4U, CALL_A};
    assert(tetra_sm_on_grant(s.opts, s.state, &moved) == 1 && g_tuned_hz == hz(CARR_G));
    assert_home_is_a(&s);
    /* And one on the carrier the receiver is on now. */
    tetra_confirm_note_signalling(s.state);
    const tetra_sm_grant here = {hz(CARR_G), CARR_G, 0x2U, CALL_A + 1U};
    assert(tetra_sm_on_grant(s.opts, s.state, &here) == 1 && g_tunes == 2);
    assert_home_is_a(&s);
    advance(&s, TETRA_SM_GRANT_TIMEOUT_S + 0.5);
    tetra_sm_tick(s.opts, s.state);
    assert_back_home(&s, 1);
    close_session(&s);
}

/* A grant from the control channel the backend takes in flight: the home is the follow's from the request on. A
 * completion keeps it; a failure puts the state machine back on the control channel with no home, as a refused or
 * deferred tune leaves it, having taken none. A reallocation in flight that fails keeps the home of the follow it
 * leaves in place. */
static void
test_pending_grant(void) {
    for (int fails = 0; fails <= 1; fails++) {
        reset_backend();
        session s = open_on_a();
        g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
        assert(g_tunes == 1 && g_request != 0U);
        assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC) && tetra_sm_guarding(s.opts, s.state));
        dsd_trunk_tuning_request_publish(g_request, fails ? DSD_TRUNK_TUNE_RESULT_FAILED : DSD_TRUNK_TUNE_RESULT_OK);
        tetra_sm_tick(s.opts, s.state);
        if (fails) {
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_st_peek(s.state)->sm.home.valid);
            assert(tetra_sm_return_cc_hz(s.state) == 0 && !tetra_sm_guarding(s.opts, s.state));
        } else {
            assert_home_is_a(&s);
        }
        close_session(&s);
    }

    const dsd_trunk_tune_result unaccepted[] = {DSD_TRUNK_TUNE_RESULT_FAILED, DSD_TRUNK_TUNE_RESULT_DEFERRED};
    for (size_t k = 0; k < 2U; k++) {
        reset_backend();
        session s = open_on_a();
        g_tune_result = unaccepted[k];
        setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
        assert(g_tunes == 1 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
        assert(!tetra_st_peek(s.state)->sm.home.valid && tetra_sm_return_cc_hz(s.state) == 0);
        close_session(&s);
    }

    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    sysinfo_burst(&s, CARR_F, LA_A);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    const tetra_sm_grant moved = {hz(CARR_G), CARR_G, 0x4U, CALL_A};
    assert(tetra_sm_on_grant(s.opts, s.state, &moved) == 1);
    assert_home_is_a(&s);
    dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    tetra_sm_grant g = {0};
    assert(tetra_sm_followed(s.state, &g) && g.freq_hz == hz(CARR_F));
    assert_home_is_a(&s);
    close_session(&s);
}

/* Following A on F, B heard there, and the session's control channel set to F since by another owner (as a frontend
 * may), the hangtime passed. */
static session
open_released_on_f(void) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    hear_on_f(&s, &k_cases[0]);
    assert(s.state->trunk_cc_freq == hz(CARR_A_CC));
    s.state->trunk_cc_freq = hz(CARR_F);
    advance(&s, 3.0);
    return s;
}

/* The return asked for at the hangtime goes home and the backend takes it in flight: the state machine still holds the
 * follow (its home and the session's control channel are A's) until the backend answers. A completion rests ON_CC on
 * A. A failure puts the follow back, its home and the session's control channel as they were (F) -- unless
 * something changed that since, which stays -- and the next tick returns home again. */
static void
test_pending_return(void) {
    for (int answer = 0; answer < 3; answer++) {
        session s = open_released_on_f();
        g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        tetra_sm_tick(s.opts, s.state);
        assert(g_returns == 1 && g_returned_hz == hz(CARR_A_CC) && s.state->trunk_cc_freq == hz(CARR_A_CC));
        assert(tetra_sm_owns_return(s.state) && !tetra_sm_followed(s.state, NULL));
        assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC) && tetra_sm_guarding(s.opts, s.state));
        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        if (answer == 0) {
            dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_OK);
            tetra_sm_tick(s.opts, s.state);
            assert_back_home(&s, 1);
            close_session(&s);
            continue;
        }
        const long newer_hz = hz(CARR_H);
        if (answer == 2) {
            s.state->trunk_cc_freq = newer_hz; /* another owner's since the request */
        }
        dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
        assert(tetra_sm_settle(s.opts, s.state) == 1);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_sm_followed(s.state, NULL));
        assert(s.state->trunk_cc_freq == (answer == 2 ? newer_hz : hz(CARR_F)));
        assert_home_is_a(&s);
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 2);
        close_session(&s);
    }
}

/* A return the backend refuses or defers keeps the call and the home, and puts the session's control channel back at
 * once; the next tick returns home. */
static void
test_refused_and_deferred_return(void) {
    const dsd_trunk_tune_result unaccepted[] = {DSD_TRUNK_TUNE_RESULT_FAILED, DSD_TRUNK_TUNE_RESULT_DEFERRED};
    for (size_t k = 0; k < 2U; k++) {
        session s = open_released_on_f();
        g_return_result = unaccepted[k];
        tetra_sm_tick(s.opts, s.state);
        assert(g_returns == 1 && g_returned_hz == hz(CARR_A_CC));
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.state->trunk_cc_freq == hz(CARR_F));
        assert_home_is_a(&s);
        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 2);
        close_session(&s);
    }
}

/* The engine or a frontend took the receiver (an external return, a carrier boundary) or the state machine started
 * over (engine start, a decode-mode change that drops the TETRA state): the home goes with the follow. A grant or
 * return of its own in flight is let go of, and its late answer puts nothing back: no follow, no control channel. */
static void
test_letting_go_drops_the_home(void) {
    enum { BY_EXTERNAL, BY_ABANDON, BY_INIT, BY_DROP };

    for (int by = BY_EXTERNAL; by <= BY_DROP; by++) {
        for (int pending = 0; pending < 3; pending++) {
            reset_backend();
            session s = open_on_a();
            if (pending == 1) {
                g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
                setup_burst(&s, GSSI_A, CALL_A, 0x4U, CARR_F);
            } else {
                follow_a(&s, CARR_F);
                sysinfo_burst(&s, CARR_F, LA_A);
                if (pending == 2) {
                    g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
                    advance(&s, 3.0);
                    tetra_sm_tick(s.opts, s.state);
                    assert(g_returns == 1);
                }
            }
            const uint64_t request = g_request;
            assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
            if (by == BY_EXTERNAL) {
                tetra_sm_on_external_cc_return(s.opts, s.state);
            } else if (by == BY_ABANDON) {
                tetra_sm_abandon_carrier(s.opts, s.state);
            } else if (by == BY_INIT) {
                tetra_sm_init(s.opts, s.state);
            } else {
                tetra_state_reset(s.state);
            }
            assert(tetra_sm_return_cc_hz(s.state) == 0 && !tetra_sm_guarding(s.opts, s.state));
            assert(!tetra_st_peek(s.state)->sm.home.valid);
            if (pending != 0) {
                const long operator_hz = hz(CARR_H);
                s.state->trunk_cc_freq = operator_hz;
                dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
                tetra_sm_tick(s.opts, s.state);
                assert(s.state->trunk_cc_freq == operator_hz && !tetra_sm_followed(s.state, NULL));
                assert(tetra_sm_return_cc_hz(s.state) == 0);
            }
            close_session(&s);
        }
    }
}

/* What is taken on while trunking is off and the receiver stays on the followed channel. Nothing guards the follow
 * then, so all of it is taken on as ever: B's SYNC, after which B's SYSINFO in block 2 decodes with B's own scrambling
 * and names F, a SYSINFO naming another location area, a direct mode burst. Each sets the carrier's polarity. */
enum { OFF_HEARS_B, OFF_HEARS_OTHER_LA, OFF_HEARS_DMO };

static void
hear_while_untrunked(session* s, int what) {
    advance(s, 0.2);
    const tetra_state_t* const ts = tetra_st_peek(s->state);
    ttb_block si = {0};
    if (what == OFF_HEARS_B) {
        const cell b = {MCC, MNC, COL_B};
        ttb_sysinfo(&si, CARR_F, 4U, 2U, LA_B);
        assert(cell_sync_burst(s, &b, 0U, &si, 1) == 1);
        assert(ts->colour == COL_B && ts->lfsr_seed == seed_of(&b) && s->state->tetra_sync.polarity == 1);
        assert(ts->la == LA_B && s->state->trunk_cc_freq == hz(CARR_F));
    } else if (what == OFF_HEARS_OTHER_LA) {
        ttb_sysinfo(&si, CARR_A_CC, 4U, 2U, LA_B);
        assert(cell_ndb(s, &k_cell_a, NULL, &si) == 1);
        assert(ts->la == LA_B);
    } else {
        assert(dsb_on_air(s, 1, 0) == 1);
        assert(ts->dmo && s->state->tetra_sync.polarity == 1);
    }
}

/* Following A on F, trunking turned off (the machine keeps the channel and lets go of the call), something taken on
 * there, A's speech still going on. */
static session
open_untrunked_on_f(int what) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && !tetra_sm_guarding(s.opts, s.state));
    assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
    hear_while_untrunked(&s, what);
    advance(&s, 0.1);
    speech(&s);
    return s;
}

/* Trunking on again while TUNED ends the follow at once, speech or not: the call was let go when trunking went off and
 * what the carrier delivered since (another cell, another location area, direct mode) cannot be trusted. The
 * session's control channel is A's again as soon as trunking is seen on -- by the tick, or by the trunk command before
 * it -- and the return goes home (regression: main kept the channel until the hangtime and then stayed on F as B's
 * control channel). */
static void
test_trunking_back_on_returns_home(void) {
    for (int what = OFF_HEARS_B; what <= OFF_HEARS_DMO; what++) {
        for (int by_command = 0; by_command <= 1; by_command++) {
            session s = open_untrunked_on_f(what);
            if (by_command) {
                tetra_sm_on_trunking_on(s.state);
                assert(s.state->trunk_cc_freq == hz(CARR_A_CC) && tetra_st_peek(s.state)->sm.return_required);
            }
            s.opts->trunk_enable = 1;
            assert(tetra_sm_guarding(s.opts, s.state));
            tetra_sm_tick(s.opts, s.state);
            assert_back_home(&s, 1);
            close_session(&s);
        }
    }
}

/* Until that return completes nothing on the carrier keeps the follow: a grant is ignored, speech and channel
 * maintenance start no timer. A return refused or deferred is asked for again at the next tick, and so is one that
 * failed in flight, whose rollback puts the follow back still owing its return. */
static void
test_return_required_until_home(void) {
    const dsd_trunk_tune_result answers[] = {
        DSD_TRUNK_TUNE_RESULT_FAILED,
        DSD_TRUNK_TUNE_RESULT_DEFERRED,
        DSD_TRUNK_TUNE_RESULT_PENDING,
    };
    for (size_t k = 0; k < 3U; k++) {
        session s = open_untrunked_on_f(OFF_HEARS_B);
        s.opts->trunk_enable = 1;
        g_return_result = answers[k];
        tetra_sm_tick(s.opts, s.state);
        assert(g_returns == 1 && g_returned_hz == hz(CARR_A_CC));
        if (answers[k] == DSD_TRUNK_TUNE_RESULT_PENDING) {
            dsd_trunk_tuning_request_publish(g_request, DSD_TRUNK_TUNE_RESULT_FAILED);
            assert(tetra_sm_settle(s.opts, s.state) == 1);
        }
        const tetra_state_t* const ts = tetra_st_peek(s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && ts->sm.return_required);
        assert(tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));

        /* Nothing on the carrier counts for the follow meanwhile. */
        const double voice_m = ts->sm.t_voice_m;
        const double maint_m = ts->sm.t_maint_m;
        advance(&s, 0.1);
        speech(&s);
        tetra_sm_note_maintenance(s.state);
        assert(same_time(ts->sm.t_voice_m, voice_m) && same_time(ts->sm.t_maint_m, maint_m));
        tetra_confirm_note_signalling(s.state);
        const int tunes = g_tunes;
        const tetra_sm_grant other = {hz(CARR_G), CARR_G, 0x4U, CALL_B};
        assert(tetra_sm_on_grant(s.opts, s.state, &other) == 0 && g_tunes == tunes);

        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 2);
        close_session(&s);
    }
}

/* A follow on a traffic timeslot of A's control channel's own carrier stops there without a tune once trunking is on
 * again, and the carrier's cell is taken on again from scratch: what was decoded there while untrunked (B's SYSINFO,
 * its timebase and the carrier's evidence) goes, the session's control channel is A's, and A's next SYNC and SYSINFO
 * are taken on as on any control channel. */
static void
test_trunking_back_on_on_the_home_carrier(void) {
    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_A_CC);
    assert(g_tunes == 0 && tetra_sm_return_cc_hz(s.state) == hz(CARR_A_CC));
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    advance(&s, 0.2);
    bsch_burst(&s, COL_B);
    sysinfo_burst(&s, CARR_F, LA_B);
    assert(s.state->trunk_cc_freq == hz(CARR_F) && tetra_st_peek(s.state)->dl_carrier_hz == hz(CARR_F));

    s.opts->trunk_enable = 1;
    tetra_sm_tick(s.opts, s.state);
    const tetra_state_t* const ts = tetra_st_peek(s.state);
    assert(g_returns == 0 && g_tunes == 0 && s.opts->trunk_is_tuned == 0);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_followed(s.state, NULL));
    assert(s.state->trunk_cc_freq == hz(CARR_A_CC) && tetra_sm_return_cc_hz(s.state) == 0);
    assert(!ts->sm.home.valid && !ts->sm.return_required);
    assert(ts->dl_carrier_hz == 0 && !ts->sysinfo_known && ts->tdma_valid == 0);
    assert(!tetra_confirm_carrier_proven(s.state));

    bsch_burst(&s, COL_A);
    sysinfo_burst(&s, CARR_A_CC, LA_A);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && ts->dl_carrier_hz == hz(CARR_A_CC) && ts->la == LA_A);
    close_session(&s);
}

/* A synchronisation burst of another cell on the followed carrier -- another colour code, another MCC, another MNC --
 * received inverted, is not taken on. The cell, its scrambling seed, the received identity, the session's control
 * channel, the call records and the carrier's polarity stay A's; the burst is no evidence about the carrier (its BSCH
 * counts for the burst's verdict only); and its block 2 is not decoded, scrambled with its cell's own seed or, to show
 * it is skipped rather than failing, with A's (regression: the BSCH was taken on, forgot A's cell, its block 2 decoded
 * with its seed, and the burst set the carrier's polarity). A's own synchronisation burst there, in MCCH sharing mode,
 * is taken on: its polarity becomes the carrier's and its SYSINFO in block 2 is applied. */
static void
test_foreign_sync_burst_is_not_taken(void) {
    static const cell k_foreign[] = {{MCC, MNC, COL_B}, {MCC + 1U, MNC, COL_A}, {MCC, MNC + 1U, COL_A}};
    for (size_t k = 0; k < sizeof k_foreign / sizeof k_foreign[0]; k++) {
        for (int own_seed = 0; own_seed <= 1; own_seed++) {
            reset_backend();
            session s = open_on_a();
            follow_a(&s, CARR_F);
            const tetra_state_t* const ts = tetra_st_peek(s.state);
            const follow_view before = view_of(&s);
            const uint32_t sysinfos = ts->frames_sysinfo;
            const uint32_t decoded = ts->decode_ok;
            const uint32_t signalling = ts->confirm.signalling_blocks;
            const uint32_t carrier = ts->confirm.carrier_blocks;
            ttb_block sync = {0};
            ttb_sync_pdu(&sync, k_foreign[k].mcc, k_foreign[k].mnc, k_foreign[k].colour);
            ttb_block si = {0};
            ttb_sysinfo(&si, CARR_F, 4U, 2U, LA_B);
            const uint32_t seed = own_seed ? seed_of(&k_foreign[k]) : seed_of(&k_cell_a);
            advance(&s, 0.2);
            assert(sync_burst_on_air(&s, &sync, &si, seed, 1) == 1);
            assert_unchanged(&s, &before);
            assert(ts->frames_sysinfo == sysinfos && ts->decode_ok == decoded + 1U);
            assert(ts->confirm.signalling_blocks == signalling && ts->confirm.carrier_blocks == carrier);
            assert_home_is_a(&s);
            close_session(&s);
        }
    }

    reset_backend();
    session s = open_on_a();
    follow_a(&s, CARR_F);
    const tetra_state_t* const ts = tetra_st_peek(s.state);
    const uint32_t sysinfos = ts->frames_sysinfo;
    const uint32_t signalling = ts->confirm.signalling_blocks;
    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_A_CC, 4U, 2U, LA_A);
    advance(&s, 0.2);
    assert(cell_sync_burst(&s, &k_cell_a, 2U, &si, 1) == 1);
    assert(s.state->tetra_sync.polarity == 1 && ts->frames_sysinfo == sysinfos + 1U);
    assert(ts->confirm.signalling_blocks == signalling + 2U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && s.state->trunk_cc_freq == hz(CARR_A_CC));
    assert_home_is_a(&s);
    close_session(&s);
}

/* Cell B, of another colour code, either allocates the followed timeslot of F to a call of its own, or grants a call of
 * the group the operator holds on G, each in a session of its own. B's SYNC is not taken on, so its D-SETUP fails its
 * CRC under A's scrambling, as at an MS, and nothing of it is acted on: no tune, no record of B's call, and the follow,
 * A's record, proof, receive permission and publication unchanged (regression: B's SYNC set B's scrambling and its
 * SYSINFO named its control channel; B's D-SETUP took the followed timeslot over for B's call, or the held group's
 * grant tuned to G). */
static void
test_other_colour_signalling_is_not_acted_on(void) {
    for (int elsewhere = 0; elsewhere <= 1; elsewhere++) {
        reset_backend();
        session s = open_on_a();
        follow_a(&s, CARR_F);
        s.state->tg_hold = GSSI_HELD;
        const cell b = {MCC, MNC, COL_B};
        const follow_view before = view_of(&s);
        const int tunes = g_tunes;
        ttb_block si = {0};
        ttb_sysinfo(&si, CARR_H, 4U, 2U, LA_A);
        advance(&s, 0.2);
        (void)cell_sync_burst(&s, &b, 0U, &si, 0);
        ttb_block setup = {0};
        if (elsewhere) {
            setup_block(&setup, GSSI_HELD, CALL_B, 0x4U, CARR_G, 0);
        } else {
            setup_block(&setup, GSSI_B, CALL_B, 0x4U, CARR_F, 0);
        }
        assert(cell_ndb(&s, &b, &setup, NULL) == 0);
        assert(g_tunes == tunes && g_returns == 0);
        assert(tetra_calls_peek(s.state, CALL_B) == NULL);
        assert_unchanged(&s, &before);
        assert_home_is_a(&s);
        close_session(&s);
    }
}

/* A direct mode synchronisation burst on the followed carrier, received inverted and read from its hard decisions or
 * only by the soft decision, is consumed and nothing more: the carrier does not go direct mode, and the followed
 * call's proof, the call records, the session's control channel and the carrier's polarity stay (regression: the
 * carrier went direct mode, which forgot the followed call's proof and the trunked carrier's state, and the burst set
 * the polarity). The follow then ends on its hangtime and returns home. */
static void
test_guarded_direct_mode_is_not_taken(void) {
    for (int soft_only = 0; soft_only <= 1; soft_only++) {
        reset_backend();
        session s = open_on_a();
        follow_a(&s, CARR_F);
        const tetra_state_t* const ts = tetra_st_peek(s.state);
        const follow_view before = view_of(&s);
        advance(&s, 0.2);
        assert(dsb_on_air(&s, 1, soft_only) == 1);
        assert(!ts->dmo && !ts->calls.dmo.active);
        assert_unchanged(&s, &before);
        assert_home_is_a(&s);
        end_follow(&s, END_HANGTIME);
        assert_back_home(&s, 1);
        close_session(&s);
    }
}

/* A follow that started before a D-MLE-SYSINFO named the home cell's location area. A SYSINFO naming the home's main
 * carrier on the followed carrier is the home cell's: one without a D-MLE-SYSINFO is applied as ever, and one with it
 * gives the home its location area. A SYSINFO naming the home's main carrier with another location area after that is
 * another cell's, and is not applied: the session's cell and location area, A's records and the follow stay
 * (regression: it was taken as a change of location area, which forgot A's cell and ended A's call records, the
 * followed call's with them). */
static void
test_home_location_area_learned_on_the_carrier(void) {
    reset_backend();
    session s = open_on_a_as(1);
    follow_a(&s, CARR_F);
    const tetra_state_t* const ts = tetra_st_peek(s.state);
    assert(ts->sm.home.valid && ts->sm.home.cc_hz == hz(CARR_A_CC) && !ts->sm.home.la_known);
    uint32_t sysinfos = ts->frames_sysinfo;

    ttb_block bare = {0};
    ttb_sysinfo(&bare, CARR_A_CC, 4U, 2U, LA_A);
    bare.n = 82;
    advance(&s, 0.1);
    mac_burst(&s, &bare);
    sysinfos++;
    assert(ts->frames_sysinfo == sysinfos && !ts->sysinfo_known && !ts->sm.home.la_known);

    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_A_CC, 4U, 2U, LA_A);
    assert(cell_ndb(&s, &k_cell_a, NULL, &si) == 1);
    sysinfos++;
    assert(ts->frames_sysinfo == sysinfos && ts->sysinfo_known && ts->la == LA_A);
    assert_home_is_a(&s);

    const follow_view before = view_of(&s);
    assert(before.call_phase == DSD_CALL_PHASE_ACTIVE);
    ttb_block other = {0};
    ttb_sysinfo(&other, CARR_A_CC, 4U, 2U, LA_B);
    advance(&s, 0.1);
    (void)cell_ndb(&s, &k_cell_a, NULL, &other);
    assert(ts->frames_sysinfo == sysinfos);
    assert_unchanged(&s, &before);
    assert_home_is_a(&s);
    end_follow(&s, END_HANGTIME);
    assert_back_home(&s, 1);
    close_session(&s);
}

/* The home cell's signalling on the followed carrier goes on being acted on: a reallocation of the followed call to
 * another carrier, in a D-SETUP scrambled with A's colour code, is followed there, with the allocation naming its
 * carrier by the cell's band and offset or by extended carrier numbering, and the home stays the way back. */
static void
test_home_reallocation_still_followed(void) {
    for (int extended = 0; extended <= 1; extended++) {
        reset_backend();
        session s = open_on_a();
        follow_a(&s, CARR_F);
        const int tunes = g_tunes;
        ttb_block b = {0};
        setup_block(&b, GSSI_A, CALL_A, 0x4U, CARR_G, extended);
        advance(&s, 0.2);
        assert((extended ? cell_ndb_full(&s, &k_cell_a, &b) : cell_ndb(&s, &k_cell_a, &b, NULL)) == 1);
        tetra_sm_grant g = {0};
        assert(g_tunes == tunes + 1 && g_tuned_hz == hz(CARR_G));
        assert(tetra_sm_followed(s.state, &g) && g.freq_hz == hz(CARR_G) && g.call_id == CALL_A);
        assert(s.state->trunk_cc_freq == hz(CARR_A_CC));
        assert_home_is_a(&s);
        close_session(&s);
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Allocations of another cell, and the AACH beside the home cell's CRC evidence.
 * ------------------------------------------------------------------------------------------------------------- */

enum { UM_OTHER = 33, LABEL_OTHER = 0x155 };

/* What A's call record holds that a PDU's address assigns, besides its allocation (follow_view). */
typedef struct {
    uint8_t flags;
    uint8_t usage_marker;
    uint16_t event_label;
} assignments;

static assignments
assignments_of(const session* s) {
    const tetra_call_t* const c = tetra_calls_peek(s->state, CALL_A);
    assert(c != NULL);
    const assignments a = {c->flags, c->usage_marker, c->event_label};
    return a;
}

static void
assert_assignments_unchanged(const session* s, const assignments* before) {
    const assignments now = assignments_of(s);
    assert(now.flags == before->flags && now.usage_marker == before->usage_marker);
    assert(now.event_label == before->event_label);
}

/* A MAC-RESOURCE to @p addr with no TM-SDU, only a channel allocation of another cell (cell change flag 1) of
 * timeslots @p timeslots on @p carrier: an SCH/HD block. */
static void
cell_change_header_block(ttb_block* b, const mac_addr* addr, uint32_t timeslots, uint32_t carrier) {
    const int start = resource_to(b, addr);
    ttb_resource_flags(b, 1);
    allocation(b, timeslots, carrier, 0, 1U);
    ttb_close_resource(b, start);
    ttb_finish(b, TTB_SCH_HD_BITS, -1);
}

/* One of A's own PDUs on the followed carrier whose allocation is another cell's. */
typedef struct {
    const char* name;
    int header_only; /* a MAC-RESOURCE with the allocation alone, else a D-SETUP of @c call_id with it */
    mac_addr addr;
    uint32_t call_id;
    uint32_t timeslots;
    uint32_t carrier;
} cell_change_case;

static const cell_change_case k_cell_change[] = {
    {"held group's new call on G", 0, {TETRA_MAC_ADDR_SSI_USAGE, GSSI_HELD, UM_OTHER}, CALL_B, 0x4U, CARR_G},
    {"A's call moved to G", 0, {TETRA_MAC_ADDR_SSI, GSSI_A, 0U}, CALL_A, 0x4U, CARR_G},
    {"A's call on its channel, a marker", 0, {TETRA_MAC_ADDR_SSI_USAGE, GSSI_A, UM_OTHER}, CALL_A, 0x4U, CARR_F},
    {"event label to A's group, G", 1, {TETRA_MAC_ADDR_SSI_EVENT, GSSI_A, LABEL_OTHER}, 0U, 0x4U, CARR_G},
    {"A's group to the MCCH", 1, {TETRA_MAC_ADDR_SSI, GSSI_A, 0U}, 0U, 0x0U, CARR_F},
};

/* The case's PDU on F, scrambled with A's colour code (its CRC passes), the log it printed in @p log. */
static void
cell_change_on_air(session* s, const cell_change_case* c, char* log, size_t log_size) {
    ttb_block b = {0};
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "tetra_cell_change") == 0);
    if (c->header_only) {
        cell_change_header_block(&b, &c->addr, c->timeslots, c->carrier);
        assert(cell_ndb(s, &k_cell_a, &b, NULL) == 1);
    } else {
        /* The address with a usage marker leaves an SCH/HD no room: an SCH/F with extended carrier numbering. */
        const int full = c->addr.type == TETRA_MAC_ADDR_SSI_USAGE;
        const setup_spec spec = {c->addr.ssi, c->call_id, c->timeslots, c->carrier, full, &c->addr, 1U};
        setup_block_as(&b, &spec);
        assert((full ? cell_ndb_full(s, &k_cell_a, &b) : cell_ndb(s, &k_cell_a, &b, NULL)) == 1);
    }
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, log, log_size) == 0);
}

/* A channel allocation with its cell change flag set is for an MS being moved to another cell (EN 300 392-2 clause
 * 23.5.4.1), and the decoder follows no cell change. A's own PDU carrying one on the followed carrier -- a D-SETUP of a
 * held group's new call, a D-SETUP moving A's call to G, a D-SETUP naming A's own channel with a usage marker, a
 * header-only allocation assigning an event label, one sending A's group to the MCCH -- is printed with the allocation
 * logged as another cell's and acted on in nothing: no tune or return, no record of the new call, A's record, its
 * allocation, usage marker and event label, the follow, its proof, receive permission, publication and canonical call
 * unchanged (regression: main read past the flag and took each as an allocation of this cell: it tuned to G, made the
 * new call's record, took the usage marker or event label, moved A's record to G, or ended A's follow). The follow then
 * ends on its hangtime and returns home. */
static void
test_cell_change_allocation_logged_only(void) {
    for (size_t k = 0; k < sizeof k_cell_change / sizeof k_cell_change[0]; k++) {
        printf("cell change: %s\n", k_cell_change[k].name);
        reset_backend();
        session s = open_on_a();
        follow_a(&s, CARR_F);
        s.state->tg_hold = GSSI_HELD;
        const follow_view before = view_of(&s);
        const assignments held = assignments_of(&s);
        const int tunes = g_tunes;
        char log[16384];
        advance(&s, 0.2);
        cell_change_on_air(&s, &k_cell_change[k], log, sizeof log);
        assert(strstr(log, "cell change: for another cell; logged only") != NULL);
        assert(g_tunes == tunes && g_returns == 0 && tetra_calls_peek(s.state, CALL_B) == NULL);
        assert_unchanged(&s, &before);
        assert_assignments_unchanged(&s, &held);
        assert_home_is_a(&s);
        end_follow(&s, END_HANGTIME);
        assert_back_home(&s, 1);
        close_session(&s);
    }

    /* On the control channel, following nothing: a new call's D-SETUP with another cell's channel is not followed and
     * makes no record (regression: main followed it to G). */
    reset_backend();
    session s = open_on_a();
    ttb_block b = {0};
    const setup_spec spec = {GSSI_B, CALL_B, 0x4U, CARR_G, 0, NULL, 1U};
    setup_block_as(&b, &spec);
    mac_burst(&s, &b);
    assert(g_tunes == 0 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
    assert(tetra_calls_peek(s.state, CALL_B) == NULL);
    close_session(&s);
}

/* The next burst is read in the next slot of timeslot @p tn in frames 1 to 17 after the TDMA clock's (255 symbols a
 * slot, EN 300 392-2 clause 9.4.1). */
static void
in_next_slot(session* s, uint8_t tn) {
    const tetra_state_t* const ts = tetra_st_peek(s->state);
    assert(ts->tdma_valid);
    uint32_t k = 1U;
    for (; k <= 4U * 19U; k++) {
        const uint32_t pos = ((uint32_t)ts->tn - 1U) + k;
        const uint32_t fn = ((((uint32_t)ts->fn - 1U) + (pos / 4U)) % 18U) + 1U;
        if ((pos % 4U) + 1U == tn && fn != 18U) {
            break;
        }
    }
    s->state->symbolcnt = ts->tdma_slot_start + (k * TETRA_TDMA_SLOT_SYMBOLS) + TETRA_TDMA_NDB_SYNC_SYMBOL;
}

/* A first fragment of a D-SETUP of A's call to its group, whose MAC-END carries an allocation of timeslot 2 of G with
 * cell change flag @p cell_change: the BL-UDATA header and the D-SETUP's first 20 bits, then the rest of it. */
static void
fragmented_setup(ttb_block* first, ttb_block* end, uint32_t cell_change) {
    ttb_block sdu = {0};
    ttb_put(&sdu, 2, 4); /* BL-UDATA */
    ttb_d_setup(&sdu, CALL_A, TTB_SPEECH_GROUP);

    enum { FIRST_BITS = 24 };

    int start = ttb_resource(first, 0, TETRA_MAC_ADDR_SSI, GSSI_A);
    ttb_set(first, start + 7, 63, 6); /* length indication: first fragment */
    ttb_resource_flags(first, 0);
    ttb_put_bits(first, sdu.bits, FIRST_BITS);
    ttb_finish(first, TTB_SCH_HD_BITS, start + 2);
    start = ttb_end(end);
    ttb_put(end, 1, 1); /* channel allocation flag */
    allocation(end, 0x4U, CARR_G, 0, cell_change);
    ttb_put_bits(end, sdu.bits + FIRST_BITS, sdu.n - FIRST_BITS);
    ttb_close_end(end, start);
    ttb_finish(end, TTB_SCH_HD_BITS, -1);
}

/* A fragmented D-SETUP of A's call on the followed carrier, its allocation in the MAC-END (EN 300 392-2 clause
 * 23.4.2.1.1): its first fragment's address is acted on as ever, before its allocation is known (a documented limit),
 * and with another cell's allocation in the MAC-END the rebuilt D-SETUP and the allocation are printed only: no tune,
 * the follow, record and assignments unchanged (regression: main followed it to G). With the allocation of this cell
 * the D-SETUP is followed to G (control). The first fragment is block 2 of one burst of timeslot 2, the MAC-END block 1
 * of the next. */
static void
test_cell_change_in_a_mac_end(void) {
    for (uint32_t cell_change = 0U; cell_change <= 1U; cell_change++) {
        reset_backend();
        session s = open_on_a();
        follow_a(&s, CARR_F);
        advance(&s, 0.1);
        assert(cell_sync_burst(&s, &k_cell_a, 0U, NULL, 0) == 1); /* A's SYNC on F sets its TDMA clock */
        ttb_block first = {0};
        ttb_block end = {0};
        fragmented_setup(&first, &end, cell_change);
        in_next_slot(&s, 2U);
        assert(cell_ndb(&s, &k_cell_a, NULL, &first) == 1);
        assert(tetra_st_peek(s.state)->mac_frag[2].active);
        const follow_view before = view_of(&s);
        const assignments held = assignments_of(&s);
        in_next_slot(&s, 2U);
        assert(cell_ndb(&s, &k_cell_a, &end, NULL) == 1);
        if (cell_change) {
            assert(g_tunes == 1 && g_returns == 0);
            assert_unchanged(&s, &before);
            assert_assignments_unchanged(&s, &held);
            assert_home_is_a(&s);
        } else {
            tetra_sm_grant g = {0};
            assert(g_tunes == 2 && g_tuned_hz == hz(CARR_G));
            assert(tetra_sm_followed(s.state, &g) && g.freq_hz == hz(CARR_G) && g.call_id == CALL_A);
        }
        close_session(&s);
    }
}

/* The r2 review's counterexample (EN 300 392-2 clauses 8.2.5.2, 8.3.1.1): network 234/1234, home colour code 1, another
 * cell of colour code 8 on the followed carrier. That cell's ACCESS-ASSIGN 0x0000 corrects under the home's scrambling
 * into 0x2CFF: header 10, field 1 51 (a traffic usage marker), field 2 63. */
enum { VEC_MCC = 234, VEC_MNC = 1234, VEC_UM = 51 };

static const cell k_vec_home = {VEC_MCC, VEC_MNC, 1U};
static const cell k_vec_other = {VEC_MCC, VEC_MNC, 8U};

#define VEC_OTHER_PDU  0x0000U
#define VEC_HOME_READS 0x2CFFU

/* A's group call on timeslot 2 of F with usage marker 51, granted on the home's control channel and followed there (the
 * tune completes at once), the home's SYNC on F setting its TDMA clock: the home cell's last CRC evidence there. */
static void
follow_marked(session* s) {
    const mac_addr marked = {TETRA_MAC_ADDR_SSI_USAGE, GSSI_A, VEC_UM};
    const setup_spec spec = {GSSI_A, CALL_A, 0x4U, CARR_F, 1, &marked, 0U};
    ttb_block b = {0};
    setup_block_as(&b, &spec);
    mac_burst(s, &b);
    assert(tetra_sm_get_state(s->state) == TETRA_SM_TUNED && g_tuned_hz == hz(CARR_F));
    const tetra_call_t* const c = tetra_calls_peek(s->state, CALL_A);
    assert(c != NULL && (c->flags & TETRA_CALL_F_USAGE_MARKER) != 0U && c->usage_marker == VEC_UM);
    assert(cell_sync_burst(s, &k_vec_home, 0U, NULL, 0) == 1);
}

/* An NTS1 normal downlink burst in the next slot of the followed timeslot 2: its AACH ACCESS-ASSIGN @p pdu scrambled
 * with @p seed through the test transmit chain, its slot unscrambled zeros, which no seed decodes into signalling. */
static void
aach_on_air(session* s, uint16_t pdu, uint32_t seed) {
    uint8_t aach[TTB_AACH_DIBITS];
    const uint8_t slot[2 * TTB_BLOCK_DIBITS] = {0};
    ttb_encode_aach(pdu, 0U, seed, aach);
    in_next_slot(s, 2U);
    (void)ndb_on_air_with(s, 0, aach, slot, slot + TTB_BLOCK_DIBITS);
}

/* What the other cell sends on F, between the home's last SYNC and the deadline: its SYNC (not taken on, G1) and a
 * SYSINFO of the home's colour code naming another main carrier (not applied, G2). Neither is the home cell's
 * evidence. */
static void
other_cells_on_f(session* s) {
    assert(cell_sync_burst(s, &k_vec_other, 0U, NULL, 0) == 1);
    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_H, 4U, 2U, LA_B);
    assert(cell_ndb(s, &k_vec_home, NULL, &si) == 1);
}

/* The other cell's AACH corrects into the followed call's own traffic marker under the home's scrambling (read through
 * the real AACH decode: "2/51/63"), which maintains the channel, yet the AACH has no CRC: while the follow is guarded
 * it keeps the channel no longer than TETRA_SM_GRANT_TIMEOUT_S after the home cell's last CRC-passed block there, its
 * SYNC at the tune or 1.5 s later. The other cell's SYNC and a SYSINFO naming another main carrier in between are no
 * such block. The grant timeout returns home at that deadline and not before (regression: main took every such AACH
 * as maintenance, so the follow stayed on the other cell's carrier as long as it sent). */
static void
test_aach_cannot_outlive_home_evidence(void) {
    static const double k_last_sync[] = {0.0, 1.5};
    for (size_t k = 0; k < sizeof k_last_sync / sizeof k_last_sync[0]; k++) {
        reset_backend();
        session s = open_on(&k_vec_home, 0);
        follow_marked(&s);
        const double t0 = s.t;
        const double deadline = t0 + k_last_sync[k] + TETRA_SM_GRANT_TIMEOUT_S;
        const tetra_state_t* const ts = tetra_st_peek(s.state);

        char log[16384];
        dsd_test_capture_stderr cap;
        s.opts->payload = 1;
        assert(dsd_test_capture_stderr_begin(&cap, "tetra_vector") == 0);
        aach_on_air(&s, VEC_OTHER_PDU, seed_of(&k_vec_other));
        assert(dsd_test_capture_stderr_end(&cap) == 0);
        assert(dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0);
        s.opts->payload = 0;
        assert(strstr(log, "AACH 2/51/63") != NULL);

        for (int step = 1; s.t + 0.25 < deadline; step++) {
            advance(&s, 0.25);
            if (k_last_sync[k] > 0.0 && step == 6) {
                assert(cell_sync_burst(&s, &k_vec_home, 0U, NULL, 0) == 1); /* the home's SYNC at t0 + 1.5 */
            }
            if (step == 10) {
                other_cells_on_f(&s);
            }
            aach_on_air(&s, VEC_OTHER_PDU, seed_of(&k_vec_other));
            tetra_sm_tick(s.opts, s.state);
            assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
        }
        assert(ts->sm.t_voice_m <= 0.0);
        s.t = deadline - 0.01;
        at(s.t);
        tetra_sm_tick(s.opts, s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
        s.t = deadline;
        at(s.t);
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 1);
        close_session(&s);
    }
}

/* Three AACHs of the other cell in successive slots of the followed channel, reading as the call's own marker. */
static void
three_other_aachs(session* s) {
    for (int i = 0; i < 3; i++) {
        aach_on_air(s, VEC_OTHER_PDU, seed_of(&k_vec_other));
    }
}

/* The other cell's AACHs reading as the call's marker count toward its receive permission (N.213, EN 300 392-2 clause
 * 23.8.2.3.2) only within TETRA_SM_GRANT_TIMEOUT_S of the home cell's last CRC-passed block on the carrier. A speaking
 * call (the hangtime holds it) whose home SYNC was 4.5 s ago gets no permission from three of them (regression: main
 * gave it, and with it the call's proof on the carrier); once the home's SYNC is heard again, three of them do. */
static void
test_aach_run_needs_home_evidence(void) {
    reset_backend();
    session s = open_on(&k_vec_home, 0);
    s.opts->trunk_hangtime = 10.0F;
    follow_marked(&s);
    const tetra_state_t* const ts = tetra_st_peek(s.state);
    advance(&s, 0.1);
    speech(&s);
    advance(&s, 4.4);
    three_other_aachs(&s);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && !ts->confirm.rx_perm);

    advance(&s, 0.1);
    assert(cell_sync_burst(&s, &k_vec_home, 0U, NULL, 0) == 1);
    three_other_aachs(&s);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && ts->confirm.rx_perm);
    close_session(&s);
}

/* A live home carrier: the call's channel sends its marker in the AACH of every slot and its SYNC once a second. A call
 * set up and silent keeps its channel as long as that goes on, 10 s here, and the grant timeout returns home
 * TETRA_SM_GRANT_TIMEOUT_S after the last SYNC (control: as on main). */
static void
test_live_home_carrier_keeps_a_silent_call(void) {
    reset_backend();
    session s = open_on(&k_vec_home, 0);
    follow_marked(&s);
    for (int i = 1; i <= 40; i++) {
        advance(&s, 0.25);
        if (i % 4 == 0) {
            assert(cell_sync_burst(&s, &k_vec_home, 0U, NULL, 0) == 1);
        }
        aach_on_air(&s, VEC_HOME_READS, seed_of(&k_vec_home));
        tetra_sm_tick(s.opts, s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    }
    advance(&s, TETRA_SM_GRANT_TIMEOUT_S - 0.01);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    advance(&s, 0.01);
    tetra_sm_tick(s.opts, s.state);
    assert_back_home(&s, 1);
    close_session(&s);
}

/* Adverse AACH evidence is the standard's own rule and stays as on main: three ACCESS-ASSIGN PDUs saying the channel
 * went back to common control (header 00, N.208) end a speaking call and return home, the home cell's last CRC evidence
 * 5 s old. So do three of the other cell's that read that way under the home's scrambling (a documented limit: as
 * noise can on main). The other cell's word is the wanted one XORed with what its zero word reads as (the code is
 * linear). */
static void
test_adverse_aach_ends_the_call(void) {
    static const uint16_t k_common_control = 0x0249U; /* 00, 9, 9 */
    for (int other = 0; other <= 1; other++) {
        reset_backend();
        session s = open_on(&k_vec_home, 0);
        s.opts->trunk_hangtime = 10.0F;
        follow_marked(&s);
        advance(&s, 0.1);
        speech(&s);
        advance(&s, 4.9);
        for (int i = 0; i < 3; i++) {
            if (other) {
                aach_on_air(&s, (uint16_t)(k_common_control ^ VEC_HOME_READS), seed_of(&k_vec_other));
            } else {
                aach_on_air(&s, k_common_control, seed_of(&k_vec_home));
            }
        }
        tetra_sm_tick(s.opts, s.state);
        assert_back_home(&s, 1);
        close_session(&s);
    }
}

int
main(void) {
    test_every_return_goes_home();
    test_reallocation_keeps_the_home();
    test_pending_grant();
    test_pending_return();
    test_refused_and_deferred_return();
    test_letting_go_drops_the_home();
    test_trunking_back_on_returns_home();
    test_return_required_until_home();
    test_trunking_back_on_on_the_home_carrier();
    test_foreign_sync_burst_is_not_taken();
    test_other_colour_signalling_is_not_acted_on();
    test_guarded_direct_mode_is_not_taken();
    test_home_location_area_learned_on_the_carrier();
    test_home_reallocation_still_followed();
    test_cell_change_allocation_logged_only();
    test_cell_change_in_a_mac_end();
    test_aach_cannot_outlive_home_evidence();
    test_aach_run_needs_home_evidence();
    test_live_home_carrier_keeps_a_silent_call();
    test_adverse_aach_ends_the_call();
    printf("TETRA foreign cell: OK\n");
    return 0;
}
