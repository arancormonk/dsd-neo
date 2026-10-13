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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol_dispatch_impl.h"
#include "test_support.h"
#include "tetra_confirm.h"
#include "tetra_state.h"

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

int
main(void) {
    test_noise_is_unproductive();
    test_sync_burst_decodes_bnch();
    test_normal_burst_failure_and_colour();
    DSD_FPRINTF(stdout, "TETRA_BURST_VERDICT: OK\n");
    return 0;
}
