// SPDX-License-Identifier: GPL-3.0-or-later
// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
/*
 * TETRA synchronisation bursts whose read a carrier boundary crossed or an input interruption cut (issue #890).
 *
 * A burst is locked on its training sequence, and the decoder then reads the rest of it: a DSB's SCH/H and the 15
 * dibits after it, a trunked SB's broadcast block and block 2. An I/Q replay that adopts a recorded retune inside that
 * read runs the carrier boundary there (dsd_engine_carrier_boundary(): tetra_forget_carrier() and a new carrier count),
 * and a Signal search's yield or a stalled live read interrupts the input. Either way the rest of the burst is not of the
 * carrier the receiver is on now, so nothing of it may land there: no direct mode, DM colour code, protection or call
 * from a DSB, no network, colour code or received identity from a BSCH, no polarity, and no proof of content.
 *
 * The bursts are built with the transmit chain of tetra_test_burst.h and decoded by the real decoder; GNU ld --wrap
 * seams around the three symbol readers serve their symbols and run the cut at a chosen symbol, after which the
 * burst's own symbols still follow (the worst case: symbols that pass every check of the burst).
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
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "protocol_dispatch_impl.h"
#include "tetra_state.h"
#include "tetra_test_burst.h"

int __wrap_getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol);
int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);
void __wrap_skipDibit(dsd_opts* opts, dsd_state* state, int count);

enum {
    SB_BB_DIBITS = 15,
    BLOCK_DIBITS = 108,
    TAIL_DIBITS = SB_BB_DIBITS + BLOCK_DIBITS,
    SCH_H_BITS = 124,
};

/* The symbols the burst decoder reads after the training sequence, the next one to serve, and the symbol whose read
   runs the cut (-1: none). */
static uint8_t g_tail[TAIL_DIBITS];
static int g_pos;
static int g_cut_at = -1;

typedef enum {
    CUT_BOUNDARY =
        0, /* a replay retune inside the read: the carrier boundary forgets the carrier and counts a new one */
    CUT_INTERRUPT = 1, /* an input interruption: the read is cut, the carrier the same */
} cut_kind;

static cut_kind g_cut_kind = CUT_BOUNDARY;
static int g_cuts;

static int
next_dibit(dsd_state* state) {
    assert(g_pos < TAIL_DIBITS);
    if (g_pos == g_cut_at) {
        g_cuts++;
        if (g_cut_kind == CUT_BOUNDARY) {
            /* As dsd_engine_carrier_boundary() runs it: the TETRA decoding forgotten, the received identity started
               over, and the carrier count moved. */
            tetra_forget_carrier(state);
            dsd_rxid_reset(state);
            state->carrier_seq++;
        } else {
            state->input_interrupted = 1;
        }
    }
    return g_tail[g_pos++] & 3;
}

/* The nominal level of dibit @p d (+1 00, +3 01, -1 10, -3 11), as a soft symbol. */
static float
level_of(int d) {
    static const float k_levels[4] = {1.0F, 3.0F, -1.0F, -3.0F};
    return k_levels[d & 3];
}

int
__wrap_getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol) {
    (void)opts;
    const int d = next_dibit(state);
    if (out_soft_symbol) {
        *out_soft_symbol = level_of(d);
    }
    return d;
}

int
__wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    const int d = next_dibit(state);
    if (out_analog_signal) {
        *out_analog_signal = (int)level_of(d);
    }
    return d;
}

void
__wrap_skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    for (int i = 0; i < count; i++) {
        (void)next_dibit(state);
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
    assert(s.opts != NULL && s.state != NULL);
    initOpts(s.opts);
    initState(s.state);
    s.opts->frame_tetra = 1;
    s.opts->payload = 0;
    s.state->max = 3;
    s.state->umid = 2;
    s.state->center = 0;
    s.state->lmid = -2;
    s.state->min = -3;
    return s;
}

static void
session_close(session* s) {
    freeState(s->state);
    free(s->state);
    free(s->opts);
}

static const char k_bsch_type5[] = "1011100110110010011101011001001111111010111101101001000011110011"
                                   "01111001010100010110000000111011010011110110110000010100";

static const char k_sysinfo_type5[] = "1000000110100010000111000011011000101011101001011000000111010101"
                                      "1100110110011011101100010111101101011101001010011000011011011110"
                                      "1011000100000101100001110001100101101101010100101001100100100100"
                                      "100001101111000101011111";

/* The SCH/S and SCH/H of a direct mode synchronisation burst (EN 300 396-3 tables 21 and 22): DMAC-SYNC, direct,
 * DM-1, carrying DM-OCCUPIED from source 100 to 0x800001 (the vector of TETRA_DMO). */
static const char k_dmo_sch_s[] = "110100000000000001100000000000000000000000000000000000000000";
static const char k_dmo_sch_h[] =
    "00000000001000001000000000000000000000010000000000000000000110010010111100010010010011100"
    "10110101110000000000000000"
    "001000100";

static void
dibits_of(const char* text, uint8_t* dibits, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dibits[i] = (uint8_t)(((text[2U * i] - '0') << 1) | (text[(2U * i) + 1U] - '0'));
    }
}

static void
bits_of(const char* text, uint8_t* bits) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        bits[i] = (uint8_t)(text[i] - '0');
    }
}

/* Serve @p tail (TAIL_DIBITS) received @p inverted, with the cut at symbol @p cut_at (-1: none). */
static void
serve(const uint8_t* tail, int inverted, int cut_at, cut_kind kind) {
    for (int i = 0; i < TAIL_DIBITS; i++) {
        g_tail[i] = (uint8_t)(tail[i] ^ (inverted ? 2U : 0U));
    }
    g_pos = 0;
    g_cut_at = cut_at;
    g_cut_kind = kind;
    g_cuts = 0;
}

/* The block before the training sequence as the frame sync captured it: @p block (60 dibits) received @p inverted,
   with only its soft values with @p soft_only (its hard decisions lost), then the synchronisation burst dispatched. */
static dsd_frame_verdict
dispatch_sync_burst(session* s, const uint8_t* block, int inverted, int soft_only) {
    const uint8_t flip = inverted ? 2U : 0U;
    for (size_t i = 0; i < TTB_BSCH_DIBITS; i++) {
        const uint8_t d = (uint8_t)(block[i] ^ flip);
        s->state->tetra_sync.sb1_dibuf[i] = soft_only ? 0U : d;
        s->state->tetra_sync.sb1_soft[i] = level_of(d);
    }
    s->state->tetra_sync.sb1_valid = 1;
    s->state->tetra_sync.sb1_soft_valid = (uint8_t)(soft_only != 0);
    s->state->tetra_sync.phase_rot = 0;
    s->state->tetra_sync.sb_polarity = (uint8_t)(inverted != 0);
    s->state->lastsynctype = inverted ? DSD_SYNC_TETRA_SB_NEG : DSD_SYNC_TETRA_SB_POS;
    s->state->synctype = s->state->lastsynctype;
    return dsd_dispatch_handle_tetra(s->opts, s->state);
}

/* A DSB with k_dmo_sch_s and k_dmo_sch_h: SCH/H right after the training sequence, then the 15 dibits it skips. */
static dsd_frame_verdict
dsb(session* s, int inverted, int soft_only, int cut_at, cut_kind kind) {
    uint8_t sch_s[TTB_BSCH_BITS] = {0};
    uint8_t sch_h[SCH_H_BITS] = {0};
    bits_of(k_dmo_sch_s, sch_s);
    bits_of(k_dmo_sch_h, sch_h);
    uint8_t sch_s_dibits[TTB_BSCH_DIBITS];
    ttb_encode_bsch(sch_s, sch_s_dibits);
    uint8_t tail[TAIL_DIBITS] = {0};
    ttb_encode_sch_hd(sch_h, TTB_BSCH_SEED, tail);
    serve(tail, inverted, cut_at, kind);
    return dispatch_sync_burst(s, sch_s_dibits, inverted, soft_only);
}

/* A trunked SB of the BSCH and SYSINFO vectors (MCC 460, MNC 4242, colour code 17): the broadcast block, then block 2. */
static dsd_frame_verdict
sb(session* s, int inverted, int cut_at, cut_kind kind) {
    uint8_t bsch[TTB_BSCH_DIBITS];
    uint8_t tail[TAIL_DIBITS] = {0};
    dibits_of(k_bsch_type5, bsch, TTB_BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, tail + SB_BB_DIBITS, BLOCK_DIBITS);
    serve(tail, inverted, cut_at, kind);
    return dispatch_sync_burst(s, bsch, inverted, 0);
}

/* What a lost DSB left on the carrier the receiver is on now: nothing of it. The carrier stays trunked until its own
   DSB, with no DM colour code, protection or call, the polarity is the one before the burst, no canonical call is
   active, the received identity is as the boundary left it (@p rxid_epoch: a direct mode note would start it over),
   and the burst proved nothing for a -Y row. */
static void
expect_nothing_of_the_dsb(const session* s, uint32_t rxid_epoch, const char* what) {
    const tetra_state_t* ts = tetra_st_existing(s->state);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    const int active = dsd_call_state_get(s->state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
    const int ok = ts && !ts->dmo && !ts->dmo_dcc_valid && ts->dmo_dcc_seed == 0U && ts->dmo_tx_seed == 0U
                   && tetra_dmo_call_crypt(s->state) == TETRA_DMO_CRYPT_UNKNOWN && !ts->calls.dmo.active
                   && ts->calls.published == TETRA_PUB_NONE && !active && s->state->tetra_sync.polarity == 0
                   && s->state->rx_id.epoch == rxid_epoch && tetra_burst_proof(s->state) == DSD_TETRA_BURST_PROOF_NONE
                   && s->state->scan_y_content_seq == 0U;
    if (!ok) {
        DSD_FPRINTF(stderr,
                    "FAIL %s: dmo=%d dcc=%d seed=%u tx_seed=%u crypt=%d call=%d published=%d active=%d polarity=%d "
                    "rxid epoch %u (was %u) proof=%d content=%u\n",
                    what, ts ? ts->dmo : -1, ts ? ts->dmo_dcc_valid : -1, ts ? (unsigned)ts->dmo_dcc_seed : 0U,
                    ts ? (unsigned)ts->dmo_tx_seed : 0U, (int)tetra_dmo_call_crypt(s->state),
                    ts ? ts->calls.dmo.active : -1, ts ? ts->calls.published : -1, active,
                    s->state->tetra_sync.polarity, (unsigned)s->state->rx_id.epoch, (unsigned)rxid_epoch,
                    (int)tetra_burst_proof(s->state), (unsigned)s->state->scan_y_content_seq);
    }
    assert(ok);
}

/* A DSB received inverted whose SCH/H decoded whole, with the carrier boundary (or the interruption) in the 15 dibits
   after it: the read is lost, so the DM-OCCUPIED it carries is neither applied, published nor its polarity taken. The
   same DSB uncut is direct mode, its call and its polarity. */
static void
test_dsb_tail_after_a_good_sch_h(cut_kind kind) {
    session s = session_open();
    /* Some TETRA state already, as on a carrier that decoded before. */
    assert(sb(&s, 0, -1, CUT_BOUNDARY) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(tetra_burst_proof(s.state) == DSD_TETRA_BURST_PROOF_TMO && s.state->scan_y_content_seq == 1U);
    tetra_forget_carrier(s.state);
    dsd_rxid_reset(s.state);
    s.state->scan_y_content_seq = 0U;
    s.state->tetra_sync.polarity = 0;
    const uint32_t epoch = s.state->rx_id.epoch;

    (void)dsb(&s, 1, 0, BLOCK_DIBITS, kind);
    assert(g_cuts == 1 && g_pos == TAIL_DIBITS);
    /* The boundary started the received identity over once; the lost DSB names nothing after it. */
    expect_nothing_of_the_dsb(&s, epoch + (kind == CUT_BOUNDARY ? 1U : 0U),
                              kind == CUT_BOUNDARY ? "DSB tail, carrier boundary" : "DSB tail, interrupted");
    session_close(&s);

    /* Uncut, the same burst is taken on. */
    s = session_open();
    assert(dsb(&s, 1, 0, -1, CUT_BOUNDARY) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    const tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts->dmo && ts->dmo_dcc_valid && ts->calls.dmo.active && ts->calls.published == TETRA_PUB_DMO);
    assert(s.state->tetra_sync.polarity == 1);
    assert(tetra_burst_proof(s.state) == DSD_TETRA_BURST_PROOF_DMO_SERVICE && s.state->scan_y_content_seq == 1U);
    session_close(&s);
}

/* A cut inside the SCH/H itself is the same: lost. */
static void
test_dsb_cut_inside_sch_h(void) {
    session s = session_open();
    const uint32_t epoch = s.state->rx_id.epoch;
    (void)dsb(&s, 1, 0, 40, CUT_BOUNDARY);
    assert(g_cuts == 1);
    expect_nothing_of_the_dsb(&s, epoch + 1U, "DSB, carrier boundary inside SCH/H");
    session_close(&s);
}

/* A DSB that only the soft decision decodes is taken in the BSCH's place once the SB's tail is read: a cut there loses
   it too, so the carrier is not put in direct mode and its polarity stays. */
static void
test_soft_only_dsb_tail(void) {
    session s = session_open();
    const uint32_t epoch = s.state->rx_id.epoch;
    (void)dsb(&s, 1, 1, SB_BB_DIBITS + 10, CUT_BOUNDARY);
    assert(g_cuts == 1);
    expect_nothing_of_the_dsb(&s, epoch + 1U, "soft-only DSB, carrier boundary in its tail");
    session_close(&s);

    s = session_open();
    assert(dsb(&s, 1, 1, -1, CUT_BOUNDARY) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(tetra_st_existing(s.state)->dmo && s.state->tetra_sync.polarity == 1);
    /* Its SCH/H went by unread: the burst proved the profile only, no content. */
    assert(tetra_burst_proof(s.state) == DSD_TETRA_BURST_PROOF_PROFILE && s.state->scan_y_content_seq == 0U);
    session_close(&s);
}

/* A trunked SB whose BSCH passed, received inverted, with the cut in its block 2 (or its broadcast block): the BSCH is
   parsed once the tail is read, and a lost read leaves its network, colour code, received identity and polarity off
   the carrier the receiver is on now. */
static void
test_sb_tail(int cut_at, cut_kind kind) {
    session s = session_open();
    (void)sb(&s, 1, cut_at, kind);
    assert(g_cuts == 1 && g_pos == TAIL_DIBITS);
    const tetra_state_t* ts = tetra_st_existing(s.state);
    const int ok = ts && !ts->net_known && ts->mcc == 0U && ts->mnc == 0U && ts->frames_sysinfo == 0U
                   && s.state->tetra_sync.polarity == 0 && s.state->rx_id.valid == 0U
                   && tetra_burst_proof(s.state) == DSD_TETRA_BURST_PROOF_NONE && s.state->scan_y_content_seq == 0U;
    if (!ok) {
        DSD_FPRINTF(stderr,
                    "FAIL SB cut at %d (kind %d): net_known=%d mcc=%u sysinfo=%u polarity=%d rxid=0x%X proof=%d "
                    "content=%u\n",
                    cut_at, (int)kind, ts ? ts->net_known : -1, ts ? (unsigned)ts->mcc : 0U,
                    ts ? (unsigned)ts->frames_sysinfo : 0U, s.state->tetra_sync.polarity,
                    (unsigned)s.state->rx_id.valid, (int)tetra_burst_proof(s.state),
                    (unsigned)s.state->scan_y_content_seq);
    }
    assert(ok);
    session_close(&s);

    /* Uncut, the same burst is the cell's. */
    s = session_open();
    assert(sb(&s, 1, -1, kind) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    ts = tetra_st_existing(s.state);
    assert(ts->net_known && ts->mcc == 460U && ts->mnc == 4242U && ts->frames_sysinfo == 1U);
    assert(s.state->tetra_sync.polarity == 1 && s.state->rx_id.valid != 0U);
    assert(tetra_burst_proof(s.state) == DSD_TETRA_BURST_PROOF_TMO && s.state->scan_y_content_seq == 1U);
    session_close(&s);
}

/* A lost burst publishes nothing: after a direct mode carrier, the trunked SB a carrier boundary cut leaves the
   publication as the boundary left it, never relabelled as the trunked mode the decoder starts the next carrier in
   until a burst of that carrier says so. */
static void
test_lost_burst_publishes_nothing(void) {
    session s = session_open();
    assert(dsb(&s, 0, 0, -1, CUT_BOUNDARY) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(s.state->tetra_pub.mode == DSD_TETRA_MODE_DMO_DIRECT);
    (void)sb(&s, 0, SB_BB_DIBITS + 1, CUT_BOUNDARY);
    assert(g_cuts == 1);
    if (s.state->tetra_pub.mode != DSD_TETRA_MODE_DMO_DIRECT || s.state->tetra_pub.net_known != 0U) {
        DSD_FPRINTF(stderr, "FAIL lost SB published mode %u net_known %u\n", (unsigned)s.state->tetra_pub.mode,
                    (unsigned)s.state->tetra_pub.net_known);
    }
    assert(s.state->tetra_pub.mode == DSD_TETRA_MODE_DMO_DIRECT && s.state->tetra_pub.net_known == 0U);
    /* The next carrier's own burst does. */
    assert(sb(&s, 0, -1, CUT_BOUNDARY) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(s.state->tetra_pub.mode == DSD_TETRA_MODE_TMO_DOWNLINK && s.state->tetra_pub.net_known == 1U);
    session_close(&s);
}

int
main(void) {
    test_lost_burst_publishes_nothing();
    test_dsb_tail_after_a_good_sch_h(CUT_BOUNDARY);
    test_dsb_tail_after_a_good_sch_h(CUT_INTERRUPT);
    test_dsb_cut_inside_sch_h();
    test_soft_only_dsb_tail();
    test_sb_tail(SB_BB_DIBITS, CUT_BOUNDARY);
    test_sb_tail(SB_BB_DIBITS + 60, CUT_BOUNDARY);
    test_sb_tail(3, CUT_BOUNDARY);
    test_sb_tail(SB_BB_DIBITS + 60, CUT_INTERRUPT);
    DSD_FPRINTF(stdout, "TETRA_BURST_CUT: OK\n");
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
