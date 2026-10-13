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
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol_dispatch_impl.h"
#include "test_support.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_pdu.h"

/* Trunking can follow a TETRA carrier (the Qt "Follow this system" control asks), and the negative syncs are the
 * inverted polarity. */
_Static_assert(DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_NDB_POS) && DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_NDB_NEG)
                   && DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_SB_POS) && DSD_SYNC_IS_TRUNKABLE(DSD_SYNC_TETRA_SB_NEG),
               "TETRA syncs are trunkable");
_Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_NDB_NEG) && DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_SB_NEG),
               "negative TETRA syncs are inverted");
_Static_assert(!DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_NDB_POS) && !DSD_SYNC_IS_INVERTED(DSD_SYNC_TETRA_SB_POS),
               "positive TETRA syncs are not inverted");

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

/* SCH/HD transmit chain (EN 300 392-2 clauses 8.2.3, 8.2.4 and 8.2.5, 8.3.1.4.1) for 124 type-1 bits: CRC-16 and
 * four tail bits, the rate-1/4 mother code punctured to 2/3, block interleaving with a = 101 and scrambling with
 * @p seed, as the dibits the decoder reads. */
static void
encode_sch_hd(const uint8_t* type1, uint32_t seed, uint8_t* dibits) {
    enum { TYPE1 = 124, TYPE2 = 144, MOTHER = TYPE2 * 4, CODED = 216 };

    uint8_t type2[TYPE2] = {0};
    uint8_t mother[MOTHER];
    uint8_t type3[CODED];
    uint8_t type4[CODED];
    uint16_t pn[CODED];
    DSD_MEMCPY(type2, type1, TYPE1);
    unsigned crc = 0;
    for (; crc < 65536U; crc++) {
        for (int i = 0; i < 16; i++) {
            type2[TYPE1 + i] = (uint8_t)((crc >> (15 - i)) & 1U);
        }
        if (tetra_crc16_ccitt_bits(type2, TYPE1 + 16) == TETRA_CRC16_OK) {
            break;
        }
    }
    assert(crc < 65536U);
    /* G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4, G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4 (8.2.3.1.1). */
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    uint8_t d3 = 0;
    uint8_t d4 = 0;
    for (size_t i = 0; i < TYPE2; i++) {
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
    const int punctured = tetra_rcpc_puncture_by_id(TETRA_RCPC_PUNCT_2_3, mother, MOTHER, type3, CODED);
    assert(punctured == CODED);
    tetra_block_interleave(type3, type4, CODED, 101);
    /* The scrambling sequence is what descrambling flips in an all-zero block. */
    const uint8_t zeros[CODED] = {0};
    tetra_hard_bits_to_soft(zeros, pn, CODED);
    tetra_descramble_soft(pn, CODED, seed);
    for (size_t i = 0; i < CODED / 2U; i++) {
        const uint8_t b0 = (uint8_t)(type4[2U * i] ^ (pn[2U * i] == 0xFFFFU ? 1U : 0U));
        const uint8_t b1 = (uint8_t)(type4[(2U * i) + 1U] ^ (pn[(2U * i) + 1U] == 0xFFFFU ? 1U : 0U));
        dibits[i] = (uint8_t)((b0 << 1) | b1);
    }
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

int
main(void) {
    test_noise_is_unproductive();
    test_sync_burst_decodes_bnch();
    test_normal_burst_failure_and_colour();
    test_retune_leaves_the_rest_of_the_burst();
    DSD_FPRINTF(stdout, "TETRA_BURST_VERDICT: OK\n");
    return 0;
}
