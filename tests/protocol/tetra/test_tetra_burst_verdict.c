// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Whole TETRA bursts through the dispatch handler (dsd_dispatch_handle_tetra()) and the burst decoders, with the
 * symbols after the training sequence read from a symbol file:
 *
 * - The verdict is PROFILE_PROVEN for a burst that decoded a block whose CRC passed and UNPRODUCTIVE otherwise.
 * - A synchronisation burst decodes its block 2, the BNCH, with the network's scrambling the BSCH in block 1 just set
 *   (EN 300 392-2 clauses 9.5.2 and 8.2.5.2), and both blocks count as evidence about the carrier.
 * - A signalling block that fails its CRC drops the partial TM-SDU of its timeslot (clause 23.4.3.1.1 ii).
 * - The MAC is told the network's colour code, not 0.
 *
 * The BSCH and SYSINFO vectors are those of test_air_chain_vector.c: MCC 460, MNC 4242, colour code 17.
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
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol_dispatch_impl.h"
#include "test_support.h"
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_pdu.h"
#include "tetra_voice_gate.h"

/* Trunking can follow a TETRA carrier (the Qt "Follow this system" control asks), and the negative syncs are the
 * inverted polarity. */
_Static_assert(DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_NDB_POS) && DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_NDB_NEG)
                   && DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_SB_POS) && DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_SB_NEG),
               "TETRA syncs are trunkable");
_Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_NDB_NEG) && DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_SB_NEG),
               "negative TETRA syncs are inverted");
_Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_NDB_POS) == 0 && DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_SB_POS) == 0,
               "positive TETRA syncs are not inverted");
/* The normal and synchronisation burst syncs of both polarities are four distinct TETRA synctypes. */
_Static_assert(DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_NDB_POS) && DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_NDB_NEG)
                   && DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_SB_POS) && DSD_SYNC_IS_TETRA(DSD_SYNC_TETRA_SB_NEG),
               "every TETRA sync is a TETRA synctype");
_Static_assert(DSD_SYNC_IS_TETRA(0) == 0 && DSD_SYNC_IS_TETRA(5) == 0, "other synctypes are not TETRA");
_Static_assert(DSD_SYNC_TETRA_SB_POS != DSD_SYNC_TETRA_NDB_POS && DSD_SYNC_TETRA_SB_NEG != DSD_SYNC_TETRA_NDB_NEG
                   && DSD_SYNC_TETRA_SB_POS != DSD_SYNC_TETRA_SB_NEG
                   && DSD_SYNC_TETRA_NDB_POS != DSD_SYNC_TETRA_NDB_NEG,
               "the four TETRA syncs are distinct");

enum {
    BSCH_DIBITS = 60,
    SB_BB_DIBITS = 15,
    BLOCK_DIBITS = 108,
    NDB_AACH2_DIBITS = 8,
};

static const char k_bsch_type5[] = "1011100110110010011101011001001111111010111101101001000011110011"
                                   "01111001010100010110000000111011010011110110110000010100";

static const char k_sysinfo_type5[] = "1000000110100010000111000011011000101011101001011000000111010101"
                                      "1100110110011011101100010111101101011101001010011000011011011110"
                                      "1011000100000101100001110001100101101101010100101001100100100100"
                                      "100001101111000101011111";

/* Type-5 bits, two to a dibit, first bit high: the decoder's own dibit order (tetra_expand_dibits()). */
static void
dibits_of(const char* text, uint8_t* dibits, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dibits[i] = (uint8_t)(((text[2U * i] - '0') << 1) | (text[(2U * i) + 1U] - '0'));
    }
}

/* Signalling transmit chain (EN 300 392-2 clauses 8.2.3, 8.2.4 and 8.2.5) for @p ntype1 type-1 bits: CRC-16 and four
 * tail bits, the rate-1/4 mother code punctured to 2/3 into @p ncoded bits, block interleaving with @p a and scrambling
 * with @p seed, as the dibits the decoder reads. SCH/HD (clause 8.3.1.4.1) is 124 bits to 216 with a = 101, SCH/HU
 * (clause 8.3.1.4.3) 92 bits to 168 with a = 13. */
static void
encode_signalling(const uint8_t* type1, int ntype1, int ncoded, int a, uint32_t seed, uint8_t* dibits) {
    enum { TYPE2_MAX = 144, CODED_MAX = 216 };

    const int ntype2 = ntype1 + 16 + 4;
    assert(ntype2 <= TYPE2_MAX && ncoded <= CODED_MAX);
    uint8_t type2[TYPE2_MAX] = {0};
    uint8_t mother[TYPE2_MAX * 4] = {0};
    uint8_t type3[CODED_MAX];
    uint8_t type4[CODED_MAX];
    uint16_t pn[CODED_MAX];
    DSD_MEMCPY(type2, type1, (size_t)ntype1);
    unsigned crc = 0;
    for (; crc < 65536U; crc++) {
        for (int i = 0; i < 16; i++) {
            type2[ntype1 + i] = (uint8_t)((crc >> (15 - i)) & 1U);
        }
        if (tetra_crc16_ccitt_bits(type2, ntype1 + 16) == TETRA_CRC16_OK) {
            break;
        }
    }
    assert(crc < 65536U);
    /* G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4, G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4 (8.2.3.1.1). */
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    uint8_t d3 = 0;
    uint8_t d4 = 0;
    for (size_t i = 0; i < (size_t)ntype2; i++) {
        const uint8_t b = type2[i];
        mother[(i * 4U) + 0U] = (uint8_t)(b ^ d1 ^ d4);
        mother[(i * 4U) + 1U] = (uint8_t)(b ^ d2 ^ d3 ^ d4);
        mother[(i * 4U) + 2U] = (uint8_t)(b ^ d1 ^ d2 ^ d4);
        mother[(i * 4U) + 3U] = (uint8_t)(b ^ d1 ^ d3 ^ d4);
        d4 = d3;
        d3 = d2;
        d2 = d1;
        d1 = b;
    }
    const int punctured = tetra_rcpc_puncture_by_id(TETRA_RCPC_PUNCT_2_3, mother, ntype2 * 4, type3, ncoded);
    assert(punctured == ncoded);
    tetra_block_interleave(type3, type4, ncoded, a);
    /* The scrambling sequence is what descrambling flips in an all-zero block. */
    const uint8_t zeros[CODED_MAX] = {0};
    tetra_hard_bits_to_soft(zeros, pn, ncoded);
    tetra_descramble_soft(pn, ncoded, seed);
    for (size_t i = 0; i < (size_t)ncoded / 2U; i++) {
        const uint8_t b0 = (uint8_t)(type4[2U * i] ^ (pn[2U * i] == 0xFFFFU ? 1U : 0U));
        const uint8_t b1 = (uint8_t)(type4[(2U * i) + 1U] ^ (pn[(2U * i) + 1U] == 0xFFFFU ? 1U : 0U));
        dibits[i] = (uint8_t)((b0 << 1) | b1);
    }
}

static void
encode_sch_hd(const uint8_t* type1, uint32_t seed, uint8_t* dibits) {
    encode_signalling(type1, TTB_SCH_HD_BITS, 216, 101, seed, dibits);
}

static int g_tunes;
static long g_tuned_hz;

static dsd_trunk_tune_result
tune_accepted(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)state;
    (void)ted_sps;
    (void)request_id;
    g_tunes++;
    g_tuned_hz = freq;
    opts->trunk_is_tuned = 1;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    char path[DSD_TEST_PATH_MAX];
} session;

static session
session_open(void) {
    session s;
    DSD_MEMSET(&s, 0, sizeof s);
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    initOpts(s.opts);
    initState(s.state);
    s.opts->frame_tetra = 1;
    s.opts->payload = 0;
    s.opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    /* A symbol file replays each dibit at its nominal level (+1, +3, -1, -3): slice there. */
    s.state->max = 3;
    s.state->umid = 2;
    s.state->center = 0;
    s.state->lmid = -2;
    s.state->min = -3;
    return s;
}

static void
session_close(session* s) {
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

/* The symbols the burst decoder reads after the training sequence, as a legacy symbol file (one dibit a byte). */
static void
feed(session* s, const uint8_t* dibits, int count) {
    if (s->opts->symbolfile) {
        (void)fclose(s->opts->symbolfile);
        (void)remove(s->path);
    }
    const int fd = dsd_test_mkstemp(s->path, sizeof s->path, "tetra_burst");
    assert(fd >= 0);
    FILE* f = dsd_test_fdopen(fd, "w+b");
    assert(f != NULL);
    const size_t written = fwrite(dibits, 1U, (size_t)count, f);
    const int rewound = fseek(f, 0L, SEEK_SET);
    assert(written == (size_t)count && rewound == 0);
    s->opts->symbolfile = f;
    s->state->symbol_replay_header_checked = 0;
}

/* A synchronisation burst whose BSCH the frame sync captured as @p bsch, followed by block 2 @p block2. */
static dsd_frame_verdict
sync_burst(session* s, const uint8_t* bsch, const uint8_t* block2) {
    uint8_t tail[SB_BB_DIBITS + BLOCK_DIBITS] = {0};
    DSD_MEMCPY(tail + SB_BB_DIBITS, block2, BLOCK_DIBITS);
    feed(s, tail, (int)sizeof tail);
    DSD_MEMCPY(s->state->tetra_sync.sb1_dibuf, bsch, BSCH_DIBITS);
    s->state->tetra_sync.sb1_valid = 1;
    s->state->tetra_sync.sb1_soft_valid = 0;
    s->state->tetra_sync.phase_rot = 0;
    s->state->tetra_sync.polarity = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_SB_POS;
    s->state->synctype = DSD_SYNC_TETRA_SB_POS;
    return dsd_dispatch_handle_tetra(s->opts, s->state);
}

/* A normal downlink burst with two half-slot blocks: block 1 as the frame sync captured it, then block 2. */
static dsd_frame_verdict
nts2_burst(session* s, const uint8_t* block1, const uint8_t* block2) {
    uint8_t tail[NDB_AACH2_DIBITS + BLOCK_DIBITS] = {0};
    DSD_MEMCPY(tail + NDB_AACH2_DIBITS, block2, BLOCK_DIBITS);
    feed(s, tail, (int)sizeof tail);
    DSD_MEMCPY(s->state->tetra_sync.b1_dibuf, block1, BLOCK_DIBITS);
    s->state->tetra_sync.b1_valid = 1;
    s->state->tetra_sync.b1_soft_valid = 0;
    s->state->tetra_sync.nts2 = 1;
    s->state->tetra_sync.cb = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    s->state->synctype = DSD_SYNC_TETRA_NDB_POS;
    return dsd_dispatch_handle_tetra(s->opts, s->state);
}

/* A normal downlink burst carrying one full-slot channel: block 1 as the frame sync captured it, then block 2. */
static dsd_frame_verdict
nts1_burst(session* s, const uint8_t* block1, const uint8_t* block2) {
    uint8_t tail[NDB_AACH2_DIBITS + BLOCK_DIBITS] = {0};
    DSD_MEMCPY(tail + NDB_AACH2_DIBITS, block2, BLOCK_DIBITS);
    feed(s, tail, (int)sizeof tail);
    DSD_MEMCPY(s->state->tetra_sync.b1_dibuf, block1, BLOCK_DIBITS);
    s->state->tetra_sync.b1_valid = 1;
    s->state->tetra_sync.b1_soft_valid = 0;
    s->state->tetra_sync.nts2 = 0;
    s->state->tetra_sync.cb = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    s->state->synctype = DSD_SYNC_TETRA_NDB_POS;
    return dsd_dispatch_handle_tetra(s->opts, s->state);
}

/* Noise: a synchronisation burst whose BSCH fails its CRC proves nothing, and with no network known its block 2 cannot
 * be descrambled. */
static void
test_noise_is_unproductive(void) {
    session s = session_open();
    const uint8_t zeros[BLOCK_DIBITS] = {0};
    uint8_t sysinfo[BLOCK_DIBITS];
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    const dsd_frame_verdict sync_verdict = sync_burst(&s, zeros, sysinfo);
    assert(sync_verdict == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    const tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts != NULL && !ts->net_known && ts->frames_sysinfo == 0U);
    assert(!tetra_confirm_carrier_proven(s.state));
    const dsd_frame_verdict normal_verdict = nts2_burst(&s, zeros, zeros);
    assert(normal_verdict == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    session_close(&s);
}

/* A synchronisation burst: the BSCH sets the network, and block 2 decodes as the BNCH with it. */
static void
test_sync_burst_decodes_bnch(void) {
    session s = session_open();
    uint8_t bsch[BSCH_DIBITS];
    uint8_t sysinfo[BLOCK_DIBITS];
    dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    const dsd_frame_verdict verdict = sync_burst(&s, bsch, sysinfo);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    const tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts->net_known && ts->mcc == 460U && ts->mnc == 4242U && ts->colour == 17U);
    assert(ts->frames_sysinfo == 1U && ts->sysinfo_main_carrier == 321U);
    assert(ts->confirm.signalling_blocks == 2U);
    session_close(&s);
}

/* A normal burst whose first half fails its CRC drops the partial TM-SDU of its timeslot; its second half reaches the
 * MAC with the network's colour code. */
static void
test_normal_burst_failure_and_colour(void) {
    session s = session_open();
    uint8_t bsch[BSCH_DIBITS];
    uint8_t sysinfo[BLOCK_DIBITS];
    const uint8_t zeros[BLOCK_DIBITS] = {0};
    dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    const dsd_frame_verdict sync_verdict = sync_burst(&s, bsch, sysinfo);
    assert(sync_verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts->tdma_valid && ts->tn == 3U);
    ts->mac_frag[3].active = 1;
    ts->mac_frag[3].nbits = 40;
    ts->mac_frag[2].active = 1;

    dsd_test_capture_stderr cap;
    char out[8192];
    assert(dsd_test_capture_stderr_begin(&cap, "tetra_burst_err") == 0);
    const dsd_frame_verdict verdict = nts2_burst(&s, zeros, sysinfo);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, out, sizeof out) == 0);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(!ts->mac_frag[3].active && ts->mac_frag[2].active);
    assert(strstr(out, "[TETRA SYSINFO] CC=17  carrier=321") != NULL);
    assert(ts->frames_sysinfo == 2U);
    session_close(&s);
}

/* A synchronisation burst whose BSCH fails its CRC still sits in the slot after the last burst: the TDMA clock runs to
 * it on the symbol count, as for a normal burst, before block 2 is decoded. After a normal burst on TN4, block 2 of a
 * synchronisation burst on TN1 that fails its CRC drops TN1's partial TM-SDU, not TN4's. The symbol count is set to
 * where each burst's slot starts, 255 symbols apart (EN 300 392-2 clause 9.4.1). */
static void
test_failed_bsch_runs_the_clock(void) {
    session s = session_open();
    uint8_t bsch[BSCH_DIBITS];
    uint8_t sysinfo[BLOCK_DIBITS];
    const uint8_t zeros[BLOCK_DIBITS] = {0};
    dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    const dsd_frame_verdict first = sync_burst(&s, bsch, sysinfo);
    assert(first == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts->tdma_valid && ts->tn == 3U);
    const uint32_t slot3 = ts->tdma_slot_start;

    /* The normal burst of the next slot is read from its training sequence, TETRA_TDMA_NDB_SYNC_SYMBOL into it. */
    s.state->symbolcnt = slot3 + TETRA_TDMA_SLOT_SYMBOLS + TETRA_TDMA_NDB_SYNC_SYMBOL;
    (void)nts2_burst(&s, zeros, zeros);
    assert(ts->tdma_valid && ts->tn == 4U && ts->tdma_slot_start == slot3 + TETRA_TDMA_SLOT_SYMBOLS);
    ts->mac_frag[4].active = 1;
    ts->mac_frag[4].nbits = 40;
    ts->mac_frag[1].active = 1;
    ts->mac_frag[1].nbits = 40;

    /* The synchronisation burst of the slot after: block 2 ends TETRA_TDMA_SB_PARSE_SYMBOL into it, and the burst
     * decoder reads the broadcast block and block 2 after the training sequence. */
    s.state->symbolcnt =
        slot3 + (2U * TETRA_TDMA_SLOT_SYMBOLS) + TETRA_TDMA_SB_PARSE_SYMBOL - SB_BB_DIBITS - BLOCK_DIBITS;
    const dsd_frame_verdict failed = sync_burst(&s, zeros, zeros);
    assert(failed == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    assert(ts->tdma_valid && ts->tn == 1U && ts->tdma_slot_start == slot3 + (2U * TETRA_TDMA_SLOT_SYMBOLS));
    assert(!ts->mac_frag[1].active && ts->mac_frag[4].active && ts->mac_frag[4].nbits == 40U);
    session_close(&s);
}

enum { CB_HALF_DIBITS = 42, SCH_HU_BITS = 92 };

/* A control uplink burst (--tetra-uplink, network 460/4242 colour code 17 preset) whose 84 coded dibits are @p coded,
 * locked as the frame sync locks it: block 1 captured, block 2 read after the training sequence, and the NDB synctype
 * of its polarity (dsd_frame_sync.c). */
static dsd_frame_verdict
control_burst(session* s, const uint8_t* coded, int inverted) {
    feed(s, coded + CB_HALF_DIBITS, CB_HALF_DIBITS);
    DSD_MEMCPY(s->state->tetra_sync.b1_dibuf, coded, CB_HALF_DIBITS);
    s->state->tetra_sync.b1_valid = 1;
    s->state->tetra_sync.b1_soft_valid = 0;
    s->state->tetra_sync.cb = 1;
    s->state->tetra_sync.phase_rot = 0;
    s->state->lastsynctype = inverted ? DSD_SYNC_TETRA_NDB_NEG : DSD_SYNC_TETRA_NDB_POS;
    s->state->synctype = s->state->lastsynctype;
    return dsd_dispatch_handle_tetra(s->opts, s->state);
}

/* The control burst's extended training sequence (EN 300 392-2 clause 9.4.4.3.3) tells its polarity, which normal
 * bursts cannot (NTS2 is NTS1 inverted) and which the uplink has no synchronisation burst to set: an inverted control
 * burst is decoded with its own polarity, and once its SCH/HU passed its CRC that polarity is the carrier's, for the
 * normal bursts after it. One whose SCH/HU fails -- a normal-polarity NTS2 burst can match the inverted training --
 * leaves the carrier's polarity as it was. */
static void
test_control_burst_tells_the_polarity(void) {
    session s = session_open();
    s.opts->tetra_uplink = 1;
    s.opts->tetra_net_preset = 1;
    s.opts->tetra_net_mcc = 460;
    s.opts->tetra_net_mnc = 4242;
    s.opts->tetra_net_colour = 17;
    const uint8_t mac[SCH_HU_BITS] = {0};
    uint8_t coded[2 * CB_HALF_DIBITS];
    encode_signalling(mac, SCH_HU_BITS, 168, 13, tetra_compute_scramb_seed(460, 4242, 17), coded);
    uint8_t inverted[2 * CB_HALF_DIBITS];
    for (size_t i = 0; i < sizeof inverted; i++) {
        inverted[i] = (uint8_t)(coded[i] ^ 2U);
    }
    s.state->tetra_sync.polarity = 0;
    dsd_frame_verdict verdict = control_burst(&s, coded, 0);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN && s.state->tetra_sync.polarity == 0);

    /* Its block 1 broken: the CRC fails and the polarity stays. */
    uint8_t broken[2 * CB_HALF_DIBITS];
    DSD_MEMCPY(broken, inverted, sizeof broken);
    for (int i = 0; i < CB_HALF_DIBITS; i += 2) {
        broken[i] ^= 1U;
    }
    verdict = control_burst(&s, broken, 1);
    assert(verdict == DSD_FRAME_VERDICT_UNPRODUCTIVE && s.state->tetra_sync.polarity == 0);

    verdict = control_burst(&s, inverted, 1);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN && s.state->tetra_sync.polarity == 1);
    verdict = control_burst(&s, coded, 0);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN && s.state->tetra_sync.polarity == 0);
    session_close(&s);
}

/* The frontends see which uplink burst was decoded: a control uplink burst publishes the control uplink mode
 * (dsd_tetra_publication), a normal uplink burst after it the uplink mode. */
static void
test_control_burst_publishes_its_mode(void) {
    session s = session_open();
    s.opts->tetra_uplink = 1;
    s.opts->tetra_net_preset = 1;
    s.opts->tetra_net_mcc = 460;
    s.opts->tetra_net_mnc = 4242;
    s.opts->tetra_net_colour = 17;
    const uint8_t mac[SCH_HU_BITS] = {0};
    uint8_t coded[2 * CB_HALF_DIBITS];
    encode_signalling(mac, SCH_HU_BITS, 168, 13, tetra_compute_scramb_seed(460, 4242, 17), coded);
    const dsd_frame_verdict verdict = control_burst(&s, coded, 0);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN && s.state->tetra_pub.mode == DSD_TETRA_MODE_TMO_CONTROL_UPLINK);
    const uint8_t zeros[BLOCK_DIBITS] = {0};
    (void)nts2_burst(&s, zeros, zeros);
    assert(s.state->tetra_pub.mode == DSD_TETRA_MODE_TMO_UPLINK);
    session_close(&s);
}

/* The SCH/S and SCH/H of a direct mode synchronisation burst (EN 300 396-3 tables 21 and 22): DMAC-SYNC, direct,
 * DM-1, carrying DM-OCCUPIED from source 100 to 0x800001 under MNI 753/2361 (the vector of TETRA_DMO). */
static const char k_dmo_sch_s[] = "110100000000000001100000000000000000000000000000000000000000";
static const char k_dmo_sch_h[] =
    "00000000001000001000000000000000000000010000000000000000000110010010111100010010010011100"
    "10110101110000000000000000"
    "001000100";

/* Bits of a 0/1 text, MSB first. */
static void
bits_of(const char* text, uint8_t* bits) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        bits[i] = (uint8_t)(text[i] - '0');
    }
}

/* The direct mode call a DSB with k_dmo_sch_s and k_dmo_sch_h sets up: the mode, its DM colour code and
 * protection, and the call on the channel, published. */
static void
enter_direct_mode(session* s) {
    uint8_t bits[TTB_SCH_HD_BITS] = {0};
    bits_of(k_dmo_sch_s, bits);
    tetra_dmo_sync_s_t sync_s;
    assert(tetra_dmo_parse_sync_s(bits, &sync_s) == 1);
    bits_of(k_dmo_sch_h, bits);
    tetra_dmo_sync_h_t sync_h;
    (void)tetra_dmo_parse_sync_h(bits, 0, &sync_h);
    tetra_dmo_apply_sync(s->state, &sync_s, &sync_h);
    tetra_calls_dmo_pdu(s->opts, s->state, sync_s.enc_state, &sync_h);
    const tetra_state_t* ts = tetra_st_existing(s->state);
    assert(ts->dmo && ts->dmo_dcc_valid && tetra_dmo_call_crypt(s->state) == TETRA_DMO_CRYPT_CLEAR);
    assert(ts->calls.dmo.active && ts->calls.published == TETRA_PUB_DMO);
}

/* A normal uplink burst (--tetra-uplink) with two half-slot blocks, block 1 right against its training sequence as the
 * frame sync captured it, block 2 read after it (EN 300 392-2 clause 9.4.4.2.4). */
static dsd_frame_verdict
uplink_nts2_burst(session* s, const uint8_t* block1, const uint8_t* block2) {
    feed(s, block2, BLOCK_DIBITS);
    DSD_MEMCPY(s->state->tetra_sync.b1_dibuf, block1, BLOCK_DIBITS);
    s->state->tetra_sync.b1_valid = 1;
    s->state->tetra_sync.b1_soft_valid = 0;
    s->state->tetra_sync.nts2 = 1;
    s->state->tetra_sync.cb = 0;
    s->state->tetra_sync.b2_lead_count = 0;
    s->state->tetra_sync.phase_rot = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    s->state->synctype = DSD_SYNC_TETRA_NDB_POS;
    return dsd_dispatch_handle_tetra(s->opts, s->state);
}

/* The uplink has no BSCH to end direct mode: a control uplink burst whose SCH/HU passed its CRC proves the carrier a
 * trunked mode uplink, so direct mode, which a DSB set before, ends with its DM colour code, protection and call, and
 * the normal bursts after it are NUBs again, decoded with the network's colour code. Before it a NUB is taken for a
 * DNB and does not decode. */
static void
test_control_burst_leaves_direct_mode(void) {
    session s = session_open();
    s.opts->tetra_uplink = 1;
    s.opts->tetra_net_preset = 1;
    s.opts->tetra_net_mcc = 460;
    s.opts->tetra_net_mnc = 4242;
    s.opts->tetra_net_colour = 17;
    const uint32_t seed = tetra_compute_scramb_seed(460, 4242, 17);
    const uint8_t mac[SCH_HU_BITS] = {0};
    uint8_t coded[2 * CB_HALF_DIBITS];
    encode_signalling(mac, SCH_HU_BITS, 168, 13, seed, coded);
    ttb_block b = {0};
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    uint8_t block2[BLOCK_DIBITS];
    encode_sch_hd(b.bits, seed, block2);
    const uint8_t zeros[BLOCK_DIBITS] = {0};

    /* A first control burst gives the session its network identity, then a DSB puts it in direct mode. */
    dsd_frame_verdict verdict = control_burst(&s, coded, 0);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    enter_direct_mode(&s);
    verdict = uplink_nts2_burst(&s, zeros, block2);
    assert(verdict == DSD_FRAME_VERDICT_UNPRODUCTIVE);

    verdict = control_burst(&s, coded, 0);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    const tetra_state_t* ts = tetra_st_existing(s.state);
    assert(!ts->dmo && !ts->dmo_dcc_valid && ts->dmo_dcc_seed == 0U && ts->dmo_crypt == TETRA_DMO_CRYPT_UNKNOWN);
    assert(!ts->calls.dmo.active && ts->calls.dmo.src == 0U && ts->calls.published == TETRA_PUB_NONE);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    assert(dsd_call_state_get(s.state, 0U, &call) > 0 && call.phase != DSD_CALL_PHASE_ACTIVE);
    verdict = uplink_nts2_burst(&s, zeros, block2);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN && s.state->tetra_pub.mode == DSD_TETRA_MODE_TMO_UPLINK);
    session_close(&s);
}

/* An uplink carrier (--tetra-uplink) has no BSCH, but its control burst's SCH/HU and its normal bursts' MAC blocks are
 * CRC-16 signalling of the carrier like a downlink block (EN 300 392-2 clause 8.2.3.3): once one decoded, the carrier is
 * proven for publication, so the speech after it publishes its anonymous call, with its media, in a session that
 * follows nothing. It is no evidence of a downlink control channel, whose grants the uplink never carries: a grant is
 * not admitted on it. */
static void
test_uplink_signalling_proves_the_carrier(void) {
    for (int nub = 0; nub < 2; nub++) {
        session s = session_open();
        s.opts->tetra_uplink = 1;
        s.opts->tetra_net_preset = 1;
        s.opts->tetra_net_mcc = 460;
        s.opts->tetra_net_mnc = 4242;
        s.opts->tetra_net_colour = 17;
        const uint32_t seed = tetra_compute_scramb_seed(460, 4242, 17);
        const uint8_t mac[SCH_HU_BITS] = {0};
        uint8_t coded[2 * CB_HALF_DIBITS];
        encode_signalling(mac, SCH_HU_BITS, 168, 13, seed, coded);
        ttb_block b = {0};
        ttb_finish(&b, TTB_SCH_HD_BITS, -1);
        uint8_t block2[BLOCK_DIBITS];
        encode_sch_hd(b.bits, seed, block2);
        const uint8_t zeros[BLOCK_DIBITS] = {0};
        const dsd_frame_verdict verdict = nub ? uplink_nts2_burst(&s, zeros, block2) : control_burst(&s, coded, 0);
        assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN && tetra_confirm_carrier_proven(s.state));

        const uint8_t speech[TETRA_SPEECH_FULL_BITS] = {0};
        tetra_voice_full_slot(s.opts, s.state, speech, 1);
        dsd_call_snapshot call;
        DSD_MEMSET(&call, 0, sizeof call);
        assert(dsd_call_state_get(s.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               && call.kind == DSD_CALL_KIND_VOICE && call.media_active);
        assert(tetra_st_existing(s.state)->calls.published == TETRA_PUB_ANON);

        s.opts->trunk_enable = 1;
        s.state->trunk_cc_freq = 400012500L;
        dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = tune_accepted});
        dsd_trunk_tuning_requests_reset();
        g_tunes = 0;
        const tetra_sm_grant grant = {401012500L, 40U, 4U, 0x123U};
        assert(tetra_sm_on_grant(s.opts, s.state, &grant) == 0 && g_tunes == 0);
        dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
        session_close(&s);
    }
}

/* Direct mode is the carrier's: a carrier boundary leaves it, and the next carrier is trunked until its own DSB says
 * otherwise. An uplink session, whose carriers have no BSCH, decodes the next carrier's NUBs at once; a downlink one
 * the next carrier's NDBs, with the network's colour code, and the frame sync looks for the first one's block 1 behind
 * AACH half 1 again. */
static void
test_carrier_boundary_leaves_direct_mode(void) {
    for (int uplink = 0; uplink < 2; uplink++) {
        session s = session_open();
        uint8_t bsch[BSCH_DIBITS];
        uint8_t sysinfo[BLOCK_DIBITS];
        dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
        dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
        dsd_frame_verdict verdict = sync_burst(&s, bsch, sysinfo);
        assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
        const uint32_t seed = tetra_st_existing(s.state)->lfsr_seed;
        s.opts->tetra_uplink = uplink;
        ttb_block b = {0};
        ttb_finish(&b, TTB_SCH_HD_BITS, -1);
        uint8_t block2[BLOCK_DIBITS];
        encode_sch_hd(b.bits, seed, block2);
        const uint8_t zeros[BLOCK_DIBITS] = {0};

        enter_direct_mode(&s);
        s.state->tetra_sync.block1_follows_training = 1; /* as the burst decoder leaves it after a DSB */
        tetra_forget_carrier(s.state);
        assert(s.state->tetra_sync.block1_follows_training == 0);
        verdict = uplink ? uplink_nts2_burst(&s, zeros, block2) : nts2_burst(&s, zeros, block2);
        assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
        assert(s.state->tetra_pub.mode == (uplink ? DSD_TETRA_MODE_TMO_UPLINK : DSD_TETRA_MODE_TMO_DOWNLINK));
        const tetra_state_t* ts = tetra_st_existing(s.state);
        assert(!ts->dmo && !ts->dmo_dcc_valid);
        session_close(&s);
    }
}

/* A direct mode synchronisation burst (EN 300 396-2 clause 9.4.3.3) as the frame sync locks it: SCH/S, captured before
 * the training sequence like a BSCH, and SCH/H right after it, then 15 dibits, all scrambled with the all-zero colour
 * code (EN 300 396-3 clause 8.2.4). @p sch_h is the 124 type-1 bits of SCH/H, or NULL for a block that fails its
 * CRC. */
static dsd_frame_verdict
dmo_sync_burst(session* s, const uint8_t* sch_s, const uint8_t* sch_h) {
    uint8_t sch_s_dibits[BSCH_DIBITS];
    encode_signalling(sch_s, 60, 120, 11, 3u, sch_s_dibits);
    uint8_t tail[BLOCK_DIBITS + SB_BB_DIBITS] = {0};
    if (sch_h) {
        encode_sch_hd(sch_h, 3u, tail);
    }
    feed(s, tail, (int)sizeof tail);
    DSD_MEMCPY(s->state->tetra_sync.sb1_dibuf, sch_s_dibits, BSCH_DIBITS);
    s->state->tetra_sync.sb1_valid = 1;
    s->state->tetra_sync.sb1_soft_valid = 0;
    s->state->tetra_sync.phase_rot = 0;
    s->state->tetra_sync.polarity = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_SB_POS;
    s->state->synctype = DSD_SYNC_TETRA_SB_POS;
    return dsd_dispatch_handle_tetra(s->opts, s->state);
}

/* A DMAC-SYNC SCH/S (EN 300 396-3 table 21), direct, channel A, slot 1, frame 3, under security class @p enc. */
static void
dmo_sch_s(uint32_t enc, uint8_t* bits) {
    ttb_block b = {0};
    ttb_put(&b, 0xD, 4); /* system code: DMO */
    ttb_put(&b, 0, 2);   /* SYNC PDU type: DMAC-SYNC */
    ttb_put(&b, 0, 2);   /* communication type: direct MS-MS */
    ttb_put(&b, 0, 2);   /* reserved */
    ttb_put(&b, 0, 2);   /* channel A, normal mode */
    ttb_put(&b, 0, 2);   /* slot 1 */
    ttb_put(&b, 3, 5);   /* frame 3 */
    ttb_put(&b, enc, 2); /* air interface encryption state */
    if (enc != 0u) {
        ttb_put(&b, 0x1234567u, 29); /* time variant parameter */
        ttb_put(&b, 0, 1);
        ttb_put(&b, 1, 4); /* KSG number */
        ttb_put(&b, 7, 5); /* encryption key number */
    }
    while (b.n < 60) {
        ttb_put(&b, 0, 1);
    }
    DSD_MEMCPY(bits, b.bits, 60U);
}

/* A DMAC-SYNC whose SCH/H fails its CRC leaves its message unknown, and it may be the DM-SETUP or DM-OCCUPIED that sets
 * the protection of the traffic after it (EN 300 396-3 clause 8.5.3): after a clear DM-1 call, a DSB whose SCH/S says
 * DM-2-A seals the channel's speech, which no direct mode key stream here decrypts, so encrypted audio stays muted, and
 * the canonical call shows it encrypted at once. */
static void
test_dmo_unread_sync_seals_the_traffic(void) {
    session s = session_open();
    s.opts->dmr_mute_encL = 1;
    uint8_t sch_s[60];
    uint8_t sch_h[TTB_SCH_HD_BITS] = {0};
    bits_of(k_dmo_sch_s, sch_s);
    bits_of(k_dmo_sch_h, sch_h);
    dsd_frame_verdict verdict = dmo_sync_burst(&s, sch_s, sch_h);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    const tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts->dmo && ts->dmo_dcc_valid && tetra_dmo_call_crypt(s.state) == TETRA_DMO_CRYPT_CLEAR);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    assert(dsd_call_state_get(s.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
           && call.crypto == DSD_CALL_CRYPTO_CLEAR && call.ota_source_id == 100U);
    const uint64_t epoch = call.epoch;
    tetra_speech sp;
    assert(tetra_voice_gate(s.opts, s.state, &sp) && !sp.sealed && tetra_voice_audible(s.opts, s.state, &sp, 0));

    dmo_sch_s(TETRA_DMO_AIE_DM2A, sch_s);
    verdict = dmo_sync_burst(&s, sch_s, NULL);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(tetra_dmo_call_crypt(s.state) == TETRA_DMO_CRYPT_AIR && ts->dmo_dcc_valid);
    assert(tetra_voice_gate(s.opts, s.state, &sp) && sp.sealed && !tetra_voice_audible(s.opts, s.state, &sp, 0));
    assert(dsd_call_state_get(s.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch
           && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING && !call.audio_permitted);
    session_close(&s);
}

/* A DSB puts a trunked carrier in direct mode: what the trunked carrier left goes as at a BSCH of another cell, its
 * partial TM-SDUs, its timeslots' signalling encryption and the heard timeslot, and the anonymous call its speech
 * published ends, even when the DSB's SCH/H fails and no direct mode call takes canonical slot 0 from it. */
static void
test_dsb_leaves_trunked_mode(void) {
    session s = session_open();
    uint8_t bsch[BSCH_DIBITS];
    uint8_t sysinfo[BLOCK_DIBITS];
    dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    dsd_frame_verdict verdict = sync_burst(&s, bsch, sysinfo);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    tetra_state_t* ts = tetra_st_existing(s.state);
    ts->tn = 2;
    ts->calls.slot_enc[2] = 1u + 0u;
    ts->mac_frag[2].active = 1;
    ts->mac_frag[2].nbits = 40;
    const uint8_t speech[TETRA_SPEECH_FULL_BITS] = {0};
    tetra_voice_full_slot(s.opts, s.state, speech, 1);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    assert(dsd_call_state_get(s.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(ts->calls.published == TETRA_PUB_ANON && ts->calls.heard && ts->calls.heard_tn == 2);

    uint8_t sch_s[60];
    bits_of(k_dmo_sch_s, sch_s);
    verdict = dmo_sync_burst(&s, sch_s, NULL);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN && ts->dmo && !ts->tdma_valid);
    assert(dsd_call_state_get(s.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    assert(ts->calls.published == TETRA_PUB_NONE && !ts->calls.heard && ts->calls.slot_enc[2] == 0u);
    assert(!ts->mac_frag[2].active);
    session_close(&s);
}

/* A DSB whose layout is not read is direct mode all the same, and nothing more: a presence signal of managed direct
 * mode (communication type 00, EN 300 396-10) stops the TMO clock as any DSB does, and one of a reserved SYNC PDU type
 * (EN 300 396-3 clause 9.3.28) does too, without its SCH/H being taken for a DMAC-SYNC that publishes a call. */
static void
test_dsb_of_unread_layout(void) {
    for (uint32_t pdu_type = 1; pdu_type <= 2; pdu_type++) {
        session s = session_open();
        uint8_t bsch[BSCH_DIBITS];
        uint8_t sysinfo[BLOCK_DIBITS];
        dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
        dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
        dsd_frame_verdict verdict = sync_burst(&s, bsch, sysinfo);
        assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
        const tetra_state_t* ts = tetra_st_existing(s.state);
        assert(ts->tdma_valid && !ts->dmo);
        ttb_block sch_s = {0};
        ttb_put(&sch_s, 0xD, 4);      /* system code: DMO */
        ttb_put(&sch_s, pdu_type, 2); /* DPRES-SYNC, or reserved */
        ttb_put(&sch_s, 0, 2);        /* communication type 00 */
        while (sch_s.n < 60) {
            ttb_put(&sch_s, 0, 1);
        }
        uint8_t sch_h[TTB_SCH_HD_BITS] = {0};
        bits_of(k_dmo_sch_h, sch_h);
        verdict = dmo_sync_burst(&s, sch_s.bits, sch_h);
        assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
        assert(ts->dmo && !ts->tdma_valid);
        assert(!ts->calls.dmo.active && ts->calls.published == TETRA_PUB_NONE && !ts->dmo_dcc_valid);
        session_close(&s);
    }
}

/* A normal burst whose signalling failed its CRC is tried as speech, full-slot on NTS1 and half-slot on NTS2, but not
 * in frame 18, which carries signalling only (EN 300 392-2 clause 19.4.3.1, the mapping of clause 9.5.1): the decoder
 * tries no speech frame of a burst the TDMA clock places there (no TCH line under -Z), so none plays and no speech CRC
 * that passes by chance makes the burst a proven one. */
static void
test_frame_18_tries_no_speech(void) {
    session s = session_open();
    s.opts->payload = 1;
    uint8_t bsch[BSCH_DIBITS];
    uint8_t sysinfo[BLOCK_DIBITS];
    const uint8_t zeros[BLOCK_DIBITS] = {0};
    dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    const dsd_frame_verdict sync_verdict = sync_burst(&s, bsch, sysinfo);
    assert(sync_verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts->tdma_valid);
    for (uint8_t fn = 17; fn <= 18; fn++) {
        for (int nts2 = 0; nts2 < 2; nts2++) {
            /* The burst is read in the slot the clock is at (TETRA_TDMA_NDB_SYNC_SYMBOL into it), frame @p fn. */
            ts->fn = fn;
            s.state->symbolcnt = ts->tdma_slot_start + TETRA_TDMA_NDB_SYNC_SYMBOL;
            dsd_test_capture_stderr cap;
            char out[8192];
            assert(dsd_test_capture_stderr_begin(&cap, "tetra_frame18_err") == 0);
            const dsd_frame_verdict verdict = nts2 ? nts2_burst(&s, zeros, zeros) : nts1_burst(&s, zeros, zeros);
            assert(dsd_test_capture_stderr_end(&cap) == 0);
            assert(dsd_test_capture_stderr_read(&cap, out, sizeof out) == 0);
            assert(verdict == DSD_FRAME_VERDICT_UNPRODUCTIVE && ts->fn == fn);
            const int tried = strstr(out, nts2 ? "[TETRA TCH/H]" : "[TETRA TCH/FS]") != NULL;
            assert(tried == (fn != 18U));
        }
    }
    session_close(&s);
}

/* A channel allocation in the first half of a normal burst moves the receiver: the second half was sent on the carrier
 * left, so it is not decoded, and nothing in it counts for the traffic carrier. */
static void
test_retune_leaves_the_rest_of_the_burst(void) {
    session s = session_open();
    s.opts->trunk_enable = 1;
    s.opts->trunk_tune_group_calls = 1;
    s.opts->trunk_tune_private_calls = 1;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = tune_accepted});
    dsd_trunk_tuning_requests_reset();
    g_tunes = 0;
    uint8_t bsch[BSCH_DIBITS];
    uint8_t sysinfo[BLOCK_DIBITS];
    dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    const dsd_frame_verdict sync_verdict = sync_burst(&s, bsch, sysinfo);
    assert(sync_verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && tetra_confirm_carrier_proven(s.state));

    /* A D-SETUP to group 0xBEEF in a MAC-RESOURCE with its channel allocation: carrier 50, timeslot 2. */
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x00BEEFu);
    ttb_resource_flags(&b, 1);
    ttb_allocation(&b, 4, 1, 50);
    ttb_put(&b, 2, 4); /* BL-UDATA */
    ttb_d_setup(&b, 0x0BEEu, TTB_SPEECH_GROUP);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    uint8_t grant[BLOCK_DIBITS];
    encode_sch_hd(b.bits, tetra_st_existing(s.state)->lfsr_seed, grant);

    const dsd_frame_verdict verdict = nts2_burst(&s, grant, sysinfo);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(g_tunes == 1 && g_tuned_hz > 0 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    const tetra_state_t* ts = tetra_st_existing(s.state);
    assert(ts->frames_sysinfo == 1U);
    assert(!tetra_confirm_carrier_proven(s.state));
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    session_close(&s);
}

static dsd_trunk_tune_result
tune_in_flight(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    g_tunes++;
    g_tuned_hz = freq;
    return DSD_TRUNK_TUNE_RESULT_PENDING;
}

/* The trunking state machine settles once per burst, at its entry and before any of its blocks (processTetraFrame(),
 * processTetraSBFrame()), never inside it. A tune to a traffic carrier the backend answered after the burst that
 * granted it: completed, it commits at the next burst's entry, and that burst, captured on the carrier the receiver
 * has just left, is dropped whole, its blocks not decoded -- a normal or synchronisation burst on the downlink, and an
 * uplink control burst; failed, it puts the control channel back, and the burst is decoded as the control channel's. */
static void
test_burst_entry_settles(void) {
    for (int kind = 0; kind < 3; kind++) {
        for (int completed = 0; completed < 2; completed++) {
            session s = session_open();
            s.opts->trunk_enable = 1;
            s.opts->trunk_tune_group_calls = 1;
            dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = tune_in_flight});
            dsd_trunk_tuning_requests_reset();
            g_tunes = 0;
            uint8_t bsch[BSCH_DIBITS];
            uint8_t sysinfo[BLOCK_DIBITS];
            dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
            dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
            uint8_t coded[2 * CB_HALF_DIBITS];
            if (kind == 2) {
                s.opts->tetra_uplink = 1;
                s.opts->tetra_net_preset = 1;
                s.opts->tetra_net_mcc = 460;
                s.opts->tetra_net_mnc = 4242;
                s.opts->tetra_net_colour = 17;
                const uint8_t mac[SCH_HU_BITS] = {0};
                encode_signalling(mac, SCH_HU_BITS, 168, 13, tetra_compute_scramb_seed(460, 4242, 17), coded);
                assert(control_burst(&s, coded, 0) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
                /* The uplink carries no grant: the state machine is handed one, on a control channel a downlink
                 * block proved, as the downlink's MAC would. */
                s.state->trunk_cc_freq = 400000000L;
                tetra_sm_on_cc_sync(s.opts, s.state);
                tetra_confirm_note_signalling(s.state);
                const tetra_sm_grant grant = {400250000L, 10, 4, 0x0BEE};
                assert(tetra_sm_on_grant(s.opts, s.state, &grant) == 1);
            } else {
                assert(sync_burst(&s, bsch, sysinfo) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
                assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
                ttb_block b = {0};
                const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x00BEEFu);
                ttb_resource_flags(&b, 1);
                ttb_allocation(&b, 4, 1, 50);
                ttb_put(&b, 2, 4); /* BL-UDATA */
                ttb_d_setup(&b, 0x0BEEu, TTB_SPEECH_GROUP);
                ttb_close_resource(&b, start);
                ttb_finish(&b, TTB_SCH_HD_BITS, -1);
                uint8_t grant[BLOCK_DIBITS];
                encode_sch_hd(b.bits, tetra_st_existing(s.state)->lfsr_seed, grant);
                (void)nts2_burst(&s, grant, sysinfo);
            }
            const uint64_t request = dsd_trunk_tuning_pending_request();
            assert(g_tunes == 1 && request != 0U && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
            dsd_trunk_tuning_request_publish(request,
                                             completed ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED);
            const tetra_state_t* const ts = tetra_st_existing(s.state);
            const uint32_t sysinfos = ts->frames_sysinfo;
            dsd_frame_verdict verdict = DSD_FRAME_VERDICT_UNPRODUCTIVE;
            if (kind == 0) {
                verdict = nts2_burst(&s, sysinfo, sysinfo);
            } else if (kind == 1) {
                verdict = sync_burst(&s, bsch, sysinfo);
            } else {
                verdict = control_burst(&s, coded, 0);
            }
            assert(dsd_trunk_tuning_pending_request() == 0U);
            if (completed) {
                assert(verdict == DSD_FRAME_VERDICT_UNPRODUCTIVE && ts->frames_sysinfo == sysinfos);
                assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
            } else {
                assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
                assert(kind == 2 || ts->frames_sysinfo == sysinfos + (kind == 0 ? 2U : 1U));
                assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
            }
            dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
            session_close(&s);
        }
    }
}

/* An uplink normal burst (--tetra-uplink) locks two symbols after its training sequence, which the frame sync hands
 * over as the first two dibits of block 2 (dsd_tetra_sync_capture::b2_lead_count): block 2 is those and the 106 dibits
 * after them, so its SCH/HD block decodes. Read from the input alone it would start two dibits late. */
static void
test_uplink_normal_burst_takes_the_lead(void) {
    session s = session_open();
    uint8_t bsch[BSCH_DIBITS];
    uint8_t sysinfo[BLOCK_DIBITS];
    const uint8_t zeros[BLOCK_DIBITS] = {0};
    dibits_of(k_bsch_type5, bsch, BSCH_DIBITS);
    dibits_of(k_sysinfo_type5, sysinfo, BLOCK_DIBITS);
    const dsd_frame_verdict sync_verdict = sync_burst(&s, bsch, sysinfo);
    assert(sync_verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    s.opts->tetra_uplink = 1;

    ttb_block b = {0};
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    uint8_t block2[BLOCK_DIBITS];
    encode_sch_hd(b.bits, tetra_st_existing(s.state)->lfsr_seed, block2);
    uint8_t tail[BLOCK_DIBITS] = {0};
    DSD_MEMCPY(tail, block2 + 2, BLOCK_DIBITS - 2);
    feed(&s, tail, (int)sizeof tail);
    DSD_MEMCPY(s.state->tetra_sync.b1_dibuf, zeros, BLOCK_DIBITS);
    s.state->tetra_sync.b1_valid = 1;
    s.state->tetra_sync.b1_soft_valid = 0;
    s.state->tetra_sync.nts2 = 1;
    s.state->tetra_sync.cb = 0;
    s.state->tetra_sync.b2_lead_dibits[0] = block2[0];
    s.state->tetra_sync.b2_lead_dibits[1] = block2[1];
    s.state->tetra_sync.b2_lead_count = 2;
    s.state->tetra_sync.b2_lead_soft_valid = 0;
    s.state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    s.state->synctype = DSD_SYNC_TETRA_NDB_POS;
    const dsd_frame_verdict verdict = dsd_dispatch_handle_tetra(s.opts, s.state);
    assert(verdict == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(s.state->tetra_sync.b2_lead_count == 0);
    session_close(&s);
}

int
main(void) {
    test_noise_is_unproductive();
    test_uplink_normal_burst_takes_the_lead();
    test_sync_burst_decodes_bnch();
    test_normal_burst_failure_and_colour();
    test_failed_bsch_runs_the_clock();
    test_control_burst_tells_the_polarity();
    test_control_burst_publishes_its_mode();
    test_control_burst_leaves_direct_mode();
    test_uplink_signalling_proves_the_carrier();
    test_carrier_boundary_leaves_direct_mode();
    test_dmo_unread_sync_seals_the_traffic();
    test_dsb_leaves_trunked_mode();
    test_dsb_of_unread_layout();
    test_retune_leaves_the_rest_of_the_burst();
    test_burst_entry_settles();
    test_frame_18_tries_no_speech();
    DSD_FPRINTF(stdout, "TETRA_BURST_VERDICT: OK\n");
    return 0;
}
