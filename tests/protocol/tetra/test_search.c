// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What a Signal search visit's own TETRA bursts prove (issue #748, src/protocol/tetra/tetra_search.h): gate A (trunked
 * mode), gate D (direct mode), the site note and its withdrawal, and the visit's activity, as the decoder reports them
 * through the runtime Signal search hooks, captured here by stand-in hooks.
 *
 * Every burst goes through the decoder's burst entry (processTetraSBFrame(), processTetraFrame()) as real bursts from
 * the test transmit chain (tetra_test_burst.h), read from a symbol file, so the symbol clock counts them as the live
 * readers do. Network identities are made up: MCC 901 is the international shared code, not a country.
 */

#include <assert.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "tetra_state.h"
#include "tetra_test_burst.h"
#include "tetra_test_pdu.h"

/* Cells of a made-up network (MCC 901, MNC 1234): A and B with colour codes, Z with colour code 0. */
enum { MCC = 901, MNC = 1234, COL_A = 10, COL_B = 20, LA_A = 77, LA_B = 78, CARR_MAIN = 50 };

static int g_fail;

static void
check(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---- The Signal search hooks, captured --------------------------------------------------------------------------- */

#define MAX_NOTES 16

typedef struct {
    int running;
    int evidence;
    char protocol[24];
    char access[24];
    int access_null;
    int sync;
    int slot;
    int notes_at_evidence; /* the site notes sent before the latest evidence */
    int notes;
    dsd_signal_search_site_note note[MAX_NOTES];
    int activity;
} hook_log;

static hook_log g_log;

static int
running_hook(const dsd_state* state) {
    (void)state;
    return g_log.running;
}

static void
evidence_hook(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target, uint32_t source,
              const char* access) {
    (void)state;
    assert(target == 0U && source == 0U);
    g_log.evidence++;
    DSD_SNPRINTF(g_log.protocol, sizeof g_log.protocol, "%s", protocol);
    g_log.access_null = access == NULL;
    DSD_SNPRINTF(g_log.access, sizeof g_log.access, "%s", access ? access : "");
    g_log.sync = sync;
    g_log.slot = slot;
    g_log.notes_at_evidence = g_log.notes;
}

static void
site_hook(const dsd_state* state, const dsd_signal_search_site_note* note) {
    (void)state;
    if (g_log.notes < MAX_NOTES) {
        g_log.note[g_log.notes] = *note;
    }
    g_log.notes++;
}

static void
activity_hook(const dsd_state* state) {
    (void)state;
    g_log.activity++;
}

static void
log_clear(void) {
    const int running = g_log.running;
    DSD_MEMSET(&g_log, 0, sizeof g_log);
    g_log.running = running;
}

/* The last site note, or NULL. */
static const dsd_signal_search_site_note*
last_note(void) {
    return (g_log.notes > 0 && g_log.notes <= MAX_NOTES) ? &g_log.note[g_log.notes - 1] : NULL;
}

static int
withdrawals(void) {
    int n = 0;
    for (int i = 0; i < g_log.notes && i < MAX_NOTES; i++) {
        n += g_log.note[i].kind == DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW;
    }
    return n;
}

/* ---- A session and its bursts ----------------------------------------------------------------------------------- */

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    char path[512];
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
    s.opts->payload = 0;
    s.opts->frame_tetra = 1;
    s.opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    /* A symbol file replays each dibit at its nominal level (+1, +3, -1, -3): slice there. */
    s.state->max = 3;
    s.state->umid = 2;
    s.state->center = 0;
    s.state->lmid = -2;
    s.state->min = -3;
    assert(tetra_st(s.state) != NULL);
    g_log.running = 1;
    log_clear();
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

/* The symbols the burst decoder reads after the training sequence, as a symbol file (one dibit a byte). */
static void
feed(session* s, const uint8_t* dibits, int count) {
    if (s->opts->symbolfile) {
        (void)fclose(s->opts->symbolfile);
        (void)remove(s->path);
    }
    const int fd = dsd_test_mkstemp(s->path, sizeof s->path, "tetra_search");
    assert(fd >= 0);
    FILE* f = dsd_test_fdopen(fd, "w+b");
    assert(f != NULL);
    const size_t written = fwrite(dibits, 1U, (size_t)count, f);
    const int rewound = fseek(f, 0L, SEEK_SET);
    assert(written == (size_t)count && rewound == 0);
    s->opts->symbolfile = f;
    s->state->symbol_replay_header_checked = 0;
}

enum { SB_BB_DIBITS = 15, NDB_AACH2_DIBITS = 8, SB_TAIL = SB_BB_DIBITS + TTB_BLOCK_DIBITS };

static void
sb_captured(session* s, const uint8_t* bsch) {
    DSD_MEMCPY(s->state->tetra_sync.sb1_dibuf, bsch, TTB_BSCH_DIBITS);
    s->state->tetra_sync.sb1_valid = 1;
    s->state->tetra_sync.sb1_soft_valid = 0;
    s->state->tetra_sync.phase_rot = 0;
    s->state->tetra_sync.sb_polarity = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_SB_POS;
    s->state->synctype = DSD_SYNC_TETRA_SB_POS;
}

/* A trunked cell's SYNC: its network and colour code, then system code, sharing mode and frame number as a case
 * needs. */
typedef struct {
    uint32_t colour;
    uint32_t system_code;
    uint32_t sharing;
    uint32_t fn;
} sync_spec;

static const sync_spec k_a = {COL_A, 4u, 0u, 18u};
static const sync_spec k_b = {COL_B, 4u, 0u, 18u};
static const sync_spec k_z = {0u, 4u, 0u, 18u};

static uint32_t
seed_of(uint32_t colour) {
    return ttb_seed(MCC, MNC, colour);
}

/* A trunked SB: the SYNC of @p sync, captured before the training sequence, then the broadcast block and block 2,
 * @p block2 (124 type-1 bits) scrambled with @p seed, or noise for NULL. */
static void
sb(session* s, const sync_spec* sync, const ttb_block* block2, uint32_t seed) {
    ttb_block pdu = {0};
    ttb_sync_pdu(&pdu, MCC, MNC, sync->colour);
    ttb_set(&pdu, 0, sync->system_code, 4);
    ttb_set(&pdu, 12, sync->fn, 5);
    ttb_set(&pdu, 23, sync->sharing, 2);
    uint8_t bsch[TTB_BSCH_DIBITS];
    ttb_encode_bsch(pdu.bits, bsch);
    uint8_t tail[SB_TAIL] = {0};
    if (block2) {
        ttb_encode_sch_hd(block2->bits, seed, tail + SB_BB_DIBITS);
    }
    feed(s, tail, (int)sizeof tail);
    sb_captured(s, bsch);
    (void)processTetraSBFrame(s->opts, s->state);
}

/* A cell's SB with its own SYSINFO (main carrier 50 in band 4, offset 2; location area @p la) in block 2. */
static void
sb_sysinfo(session* s, const sync_spec* sync, uint32_t la) {
    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_MAIN, 4u, 2u, la);
    sb(s, sync, &si, seed_of(sync->colour));
}

/* A normal downlink burst: NTS2 blocks @p b1, @p b2 (108 dibits each), or NTS1 across both, no AACH captured. */
static void
ndb_raw(session* s, int nts2, const uint8_t* b1, const uint8_t* b2) {
    uint8_t tail[NDB_AACH2_DIBITS + TTB_BLOCK_DIBITS] = {0};
    DSD_MEMCPY(tail + NDB_AACH2_DIBITS, b2, TTB_BLOCK_DIBITS);
    DSD_MEMCPY(s->state->tetra_sync.b1_dibuf, b1, TTB_BLOCK_DIBITS);
    s->state->tetra_sync.b1_valid = 1;
    s->state->tetra_sync.b1_soft_valid = 0;
    s->state->tetra_sync.aach1_valid = 0;
    s->state->tetra_sync.nts2 = (uint8_t)(nts2 != 0);
    s->state->tetra_sync.cb = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    s->state->synctype = DSD_SYNC_TETRA_NDB_POS;
    feed(s, tail, (int)sizeof tail);
    (void)processTetraFrame(s->opts, s->state);
}

/* An NTS2 burst: half-slot blocks @p h1 and @p h2 scrambled with @p seed, either noise for NULL. */
static void
ndb(session* s, uint32_t seed, const ttb_block* h1, const ttb_block* h2) {
    uint8_t b1[TTB_BLOCK_DIBITS] = {0};
    uint8_t b2[TTB_BLOCK_DIBITS] = {0};
    if (h1) {
        ttb_encode_sch_hd(h1->bits, seed, b1);
    }
    if (h2) {
        ttb_encode_sch_hd(h2->bits, seed, b2);
    }
    ndb_raw(s, 1, b1, b2);
}

/* An NTS1 burst: an SCH/F of @p block scrambled with @p seed. */
static void
ndb_full(session* s, uint32_t seed, const ttb_block* block) {
    uint8_t slot[2 * TTB_BLOCK_DIBITS];
    ttb_encode_signalling(block->bits, TTB_SCH_F_BITS, 432, 103, seed, slot);
    ndb_raw(s, 0, slot, slot + TTB_BLOCK_DIBITS);
}

/* A half-slot block of one Null PDU and fill bits: plain MAC signalling. */
static ttb_block
null_block(void) {
    ttb_block b = {0};
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    return b;
}

static void
ndb_null(session* s, uint32_t seed) {
    const ttb_block b = null_block();
    ndb(s, seed, &b, NULL);
}

/* ---- Direct mode bursts ----------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t system_code;
    uint32_t pdu_type; /* 0 DMAC-SYNC, 1 DPRES-SYNC, 2 and 3 reserved */
    uint32_t comm_type;
    uint32_t ab_usage;
    uint32_t fn;
    uint32_t enc;       /* DMAC-SYNC air interface encryption state */
    uint32_t rep_modes; /* DPRES-SYNC from a DM-REP */
} dsb_spec;

static const dsb_spec k_direct = {0xD, 0, 0, 0, 3, 0, 0};

/* The SCH/S of @p d (EN 300 396-3 table 21; a presence signal EN 300 396-4 table 2). */
static ttb_block
sch_s_of(const dsb_spec* d) {
    ttb_block b = {0};
    ttb_put(&b, d->system_code, 4);
    ttb_put(&b, d->pdu_type, 2);
    ttb_put(&b, d->comm_type, 2);
    if (d->pdu_type == 1u) {
        ttb_put(&b, 0, 1);            /* not managed direct mode */
        ttb_put(&b, 0, 2);            /* reserved, or the gateway flags */
        ttb_put(&b, 0, 1);            /* one frequency */
        ttb_put(&b, d->rep_modes, 2); /* repeater operating modes */
        ttb_put(&b, 0, 6);            /* spacing of uplink */
        ttb_put(&b, 0, 1);            /* slave link */
        ttb_put(&b, d->ab_usage, 2);  /* channel usage */
        ttb_put(&b, 0, 2);            /* channel free */
        ttb_put(&b, 0, 2);            /* slot 1 */
        ttb_put(&b, d->fn, 5);
    } else {
        ttb_put(&b, 0, 2); /* link and gateway flags */
        ttb_put(&b, d->ab_usage, 2);
        ttb_put(&b, 0, 2); /* slot 1 */
        ttb_put(&b, d->fn, 5);
        ttb_put(&b, d->enc, 2);
        if (d->enc != 0u) {
            ttb_put(&b, 0x1234567u, 29); /* time variant parameter */
            ttb_put(&b, 0, 1);
            ttb_put(&b, 1, 4); /* KSG */
            ttb_put(&b, 7, 5); /* key number */
        }
    }
    while (b.n < TTB_BSCH_BITS) {
        ttb_put(&b, 0, 1);
    }
    return b;
}

/* The source address of every DMAC-SYNC here. */
#define DMO_SRC 2u

/* A DMAC-SYNC SCH/H (EN 300 396-3 table 22) carrying @p msg from source DMO_SRC to 1 with MNI @p mni, its leading 10 bits
 * the repeater or gateway address @p relay (reserved, 0, for direct operation), and the message elements of a call PDU
 * (tables 31, 35, 36). */
static ttb_block
sch_h_as(uint32_t msg, uint32_t relay, uint32_t mni) {
    ttb_block b = {0};
    ttb_put(&b, relay, 10); /* repeater or gateway address, or reserved */
    ttb_put(&b, 0, 1);      /* fill bit indication */
    ttb_put(&b, 0, 1);      /* not fragmented */
    ttb_put(&b, 0, 2);      /* frame countdown */
    ttb_put(&b, 0, 2);
    ttb_put(&b, 1, 24); /* destination */
    ttb_put(&b, 0, 2);
    ttb_put(&b, DMO_SRC, 24); /* source */
    ttb_put(&b, mni, 24);     /* MNI */
    ttb_put(&b, msg, 5);
    ttb_put(&b, 0, 3); /* timing, LCH in frame 3, pre-emption flags */
    ttb_put(&b, 4, 3); /* power class 4 */
    ttb_put(&b, 0, 1);
    ttb_put(&b, 0, 2);
    ttb_put(&b, 0, 2);
    ttb_put(&b, 0, 4); /* speech */
    ttb_put(&b, 0, 4);
    ttb_put(&b, 0, 2); /* priority */
    ttb_put(&b, 0, 1);
    ttb_put(&b, 1, 1); /* group call */
    ttb_put(&b, 0, 1);
    ttb_put(&b, 0, 2);
    while (b.n < TTB_SCH_HD_BITS) {
        ttb_put(&b, 0, 1);
    }
    return b;
}

static ttb_block
sch_h_of(uint32_t msg) {
    return sch_h_as(msg, 0u, 0u);
}

/* A presence signal's SCH/H (EN 300 396-4 table 3): repeater address 17, an all-ones MNI (not advertised). */
static ttb_block
pres_h(void) {
    ttb_block b = {0};
    ttb_put(&b, 17, 10);
    ttb_put(&b, 0xFFFFFFu, 24);
    ttb_put(&b, 3, 2);
    while (b.n < TTB_SCH_HD_BITS) {
        ttb_put(&b, 0, 1);
    }
    return b;
}

/* A DSB of @p d whose SCH/H is @p sch_h, received wrong in its first 40 dibits with @p corrupt_h. */
static void
dsb_with(session* s, const dsb_spec* d, const ttb_block* sch_h, int corrupt_h) {
    const ttb_block ss = sch_s_of(d);
    uint8_t sch_s_dibits[TTB_BSCH_DIBITS];
    ttb_encode_bsch(ss.bits, sch_s_dibits);
    uint8_t tail[SB_TAIL] = {0};
    ttb_encode_sch_hd(sch_h->bits, TTB_BSCH_SEED, tail);
    for (int i = 0; corrupt_h && i < 40; i++) {
        tail[i] ^= 3U;
    }
    feed(s, tail, (int)sizeof tail);
    sb_captured(s, sch_s_dibits);
    (void)processTetraSBFrame(s->opts, s->state);
}

/* A DSB of @p d carrying DM-OCCUPIED (a DMAC-SYNC), or its presence signal's SCH/H. */
static void
dsb(session* s, const dsb_spec* d) {
    const ttb_block h = d->pdu_type == 1u ? pres_h() : sch_h_of(TETRA_DMO_MSG_OCCUPIED);
    dsb_with(s, d, &h, 0);
}

/* A direct mode normal burst (EN 300 396-2 clause 9.4.3.2.1): an SCH/F scrambled with DM colour code seed @p seed,
 * block 1 against the training sequence and block 2 after it. */
static void
dnb_full_with(session* s, uint32_t seed) {
    const tetra_state_t* const ts = tetra_st_existing(s->state);
    assert(ts && ts->dmo && ts->dmo_dcc_valid);
    ttb_block b = {0};
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    uint8_t slot[2 * TTB_BLOCK_DIBITS];
    ttb_encode_signalling(b.bits, TTB_SCH_F_BITS, 432, 103, seed, slot);
    DSD_MEMCPY(s->state->tetra_sync.b1_dibuf, slot, TTB_BLOCK_DIBITS);
    s->state->tetra_sync.b1_valid = 1;
    s->state->tetra_sync.b1_soft_valid = 0;
    s->state->tetra_sync.nts2 = 0;
    s->state->tetra_sync.cb = 0;
    s->state->tetra_sync.b2_lead_count = 0;
    s->state->lastsynctype = DSD_SYNC_TETRA_NDB_POS;
    s->state->synctype = DSD_SYNC_TETRA_NDB_POS;
    feed(s, slot + TTB_BLOCK_DIBITS, TTB_BLOCK_DIBITS);
    (void)processTetraFrame(s->opts, s->state);
}

/* dnb_full_with() under the DM colour code the decoder learned. */
static void
dnb_full(session* s) {
    dnb_full_with(s, tetra_st_existing(s->state)->dmo_dcc_seed);
}

/* The DM colour code seed of 6 network bits @p net6 and the source DMO_SRC (EN 300 396-3 clause 8.2.4: e(1) to e(6)
 * the 6 bits, e(7) to e(30) the source address), in the register form of tetra_test_burst.h, written out here. */
static uint32_t
dcc_seed(uint32_t net6) {
    return ((((net6 & 0x3Fu) << 24) | DMO_SRC) << 2) | 3u;
}

/* ---- Carrier events --------------------------------------------------------------------------------------------- */

/* The hunt's no-carrier pass, as noCarrier() runs it: the decoder's carrier evidence goes, the visit stays. */
static void
no_carrier(session* s) {
    tetra_forget_carrier(s->state);
}

/* A carrier boundary inside the visit (a replay retune, a stream gap). */
static void
carrier_boundary(session* s) {
    tetra_forget_carrier(s->state);
    s->state->carrier_seq++;
}

/* A hunt read the Signal search interrupted between two bursts (a radio read that gave up on a stalled input), which the
 * engine loop then takes up before the next hunt finds a burst: no carrier count or stream epoch moved. */
static void
hunt_interrupted(session* s) {
    dsd_state_raise_input_interrupt(s->state, DSD_INTERRUPT_SEARCH);
    dsd_state_consume_input_interrupt(s->state);
}

/* ---- Expectations ----------------------------------------------------------------------------------------------- */

static int
tmo_evidence(uint32_t colour) {
    char access[16];
    DSD_SNPRINTF(access, sizeof access, "CC %u", (unsigned)colour);
    return g_log.evidence == 1 && strcmp(g_log.protocol, "TETRA") == 0 && strcmp(g_log.access, access) == 0
           && g_log.slot == -1 && DSD_SYNC_IS_TETRA(g_log.sync);
}

static int
dmo_evidence(void) {
    return g_log.evidence == 1 && strcmp(g_log.protocol, "TETRA DMO") == 0 && g_log.access_null && g_log.slot == -1
           && DSD_SYNC_IS_TETRA(g_log.sync);
}

static int
tmo_note_is(const dsd_signal_search_site_note* n, int la_valid, uint16_t la, uint8_t main_state, uint32_t main_hz) {
    return n && n->kind == DSD_SEARCH_SITE_NOTE_TETRA && n->tetra.mode == DSD_SEARCH_TETRA_TMO && n->tetra.mcc == MCC
           && n->tetra.mnc == MNC && n->tetra.la_valid == la_valid && (!la_valid || n->tetra.la == la)
           && n->tetra.main_state == main_state && n->tetra.main_hz == main_hz && n->tetra.route == 0u
           && n->tetra.presence_only == 0u;
}

static int
dmo_note_is(const dsd_signal_search_site_note* n, uint8_t route, uint8_t presence) {
    return n && n->kind == DSD_SEARCH_SITE_NOTE_TETRA && n->tetra.mode == DSD_SEARCH_TETRA_DMO
           && n->tetra.route == route && n->tetra.presence_only == presence && n->tetra.mcc == 0u && n->tetra.mnc == 0u
           && n->tetra.la_valid == 0u && n->tetra.main_state == DSD_SEARCH_TETRA_MAIN_NONE;
}

static uint32_t
main_hz(void) {
    return (uint32_t)tetra_carrier_to_dl_hz(CARR_MAIN, 4u, 2u);
}

/* ---- Gate A ----------------------------------------------------------------------------------------------------- */

/* A BSCH alone confirms nothing; the SYSINFO in its own burst's block 2, under its seed, opens gate A and supplies the
 * site: MCC, MNC, location area and the advertised main carrier. */
static void
test_gate_a_bsch_and_its_bnch(void) {
    session s = session_open();
    sb(&s, &k_a, NULL, 0);
    check(g_log.evidence == 0 && g_log.notes == 0, "gate A: a BSCH alone confirms nothing");
    sb_sysinfo(&s, &k_a, LA_A);
    check(tmo_evidence(COL_A), "gate A: the BNCH under the BSCH's seed confirms TETRA, CC 10");
    check(g_log.notes == 1 && tmo_note_is(last_note(), 1, LA_A, DSD_SEARCH_TETRA_MAIN_HZ, main_hz()),
          "gate A: the opening SYSINFO supplies the site");
    /* The same again changes nothing: no second evidence or note, the visit kept active. */
    const int activity = g_log.activity;
    sb_sysinfo(&s, &k_a, LA_A);
    ndb_null(&s, seed_of(COL_A));
    check(g_log.evidence == 1 && g_log.notes == 1, "gate A: repeated content publishes nothing");
    check(g_log.activity == activity + 2, "gate A: each burst of the gate identity refreshes the activity once");
    session_close(&s);
}

/* An SCH/HD and an SCH/F under the seed open the gate too; the site then has MCC and MNC until a half-slot SYSINFO comes
 * (a half-slot block always has room for its D-MLE part). An SCH/F that begins with SYSINFO bits is no site. */
static void
test_gate_a_by_other_blocks(void) {
    for (int full = 0; full <= 1; full++) {
        session s = session_open();
        sb(&s, &k_a, NULL, 0);
        if (full) {
            ttb_block f = {0};
            ttb_sysinfo(&f, CARR_MAIN, 4u, 2u, LA_A);
            ttb_finish(&f, TTB_SCH_F_BITS, -1);
            ndb_full(&s, seed_of(COL_A), &f);
        } else {
            ndb_null(&s, seed_of(COL_A));
        }
        check(tmo_evidence(COL_A), full ? "gate A: an SCH/F confirms" : "gate A: an SCH/HD confirms");
        check(g_log.notes == 1 && tmo_note_is(last_note(), 0, 0, DSD_SEARCH_TETRA_MAIN_NONE, 0U),
              full ? "gate A: an SCH/F's SYSINFO bits are no site" : "gate A: MCC and MNC before a SYSINFO");
        ttb_block si = {0};
        ttb_sysinfo(&si, CARR_MAIN, 4u, 2u, LA_A);
        ndb(&s, seed_of(COL_A), NULL, &si);
        check(g_log.notes == 2 && tmo_note_is(last_note(), 1, LA_A, DSD_SEARCH_TETRA_MAIN_HZ, main_hz()),
              "gate A: a later half-slot SYSINFO completes the site");
        check(g_log.evidence == 1 && withdrawals() == 0, "gate A: one evidence, nothing withdrawn");
        session_close(&s);
    }
}

/* What is no corroboration: a block under a --tetra-net preset's seed with no BSCH, or under another seed than the
 * observed BSCH's; a reserved (100y) system code; a reserved frame number; speech; a lone AACH. */
static void
test_gate_a_negatives(void) {
    session s = session_open();
    s.opts->tetra_net_preset = 1;
    s.opts->tetra_net_mcc = MCC;
    s.opts->tetra_net_mnc = MNC;
    s.opts->tetra_net_colour = COL_A;
    ndb_null(&s, seed_of(COL_A));
    check(tetra_st_existing(s.state)->decode_ok >= 1U, "gate A: the preset's block decoded");
    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_MAIN, 4u, 2u, LA_A);
    sb(&s, &k_b, &si, seed_of(COL_A));
    ndb_null(&s, seed_of(COL_A));
    check(g_log.evidence == 0 && g_log.notes == 0, "gate A: a preset's seed or another cell's is no corroboration");
    session_close(&s);

    static const sync_spec k_reserved = {COL_A, 8u, 0u, 18u};
    static const sync_spec k_bad_fn = {COL_A, 4u, 0u, 0u};
    const sync_spec* const k_unobserved[] = {&k_reserved, &k_bad_fn};
    for (size_t i = 0; i < 2; i++) {
        s = session_open();
        sb_sysinfo(&s, k_unobserved[i], LA_A);
        ndb_null(&s, seed_of(COL_A));
        check(g_log.evidence == 0 && g_log.notes == 0, i == 0 ? "gate A: a reserved system code is no observed BSCH"
                                                              : "gate A: a reserved frame number is no observed BSCH");
        session_close(&s);
    }

    /* Speech needs a burst outside frame 18 (signalling only): the BSCH names frame 1. */
    static const sync_spec k_frame1 = {COL_A, 4u, 0u, 1u};
    s = session_open();
    sb(&s, &k_frame1, NULL, 0);
    uint8_t speech[TTB_TCH_FS_BITS] = {0};
    for (int i = 0; i < TTB_TCH_FS_BITS; i++) {
        speech[i] = (uint8_t)((i * 7 + 3) % 5 == 0);
    }
    uint8_t slot[2 * TTB_BLOCK_DIBITS];
    ttb_encode_tch_fs(speech, seed_of(COL_A), slot);
    ndb_raw(&s, 0, slot, slot + TTB_BLOCK_DIBITS);
    check(tetra_st_existing(s.state)->decode_ok >= 2U, "gate A: the speech burst decoded");
    check(g_log.evidence == 0, "gate A: speech under the seed is no corroboration");
    session_close(&s);
}

/* Colour code 0: a block proves nothing of the network, so a second BSCH of the same identity from a later burst opens
 * the gate, and the site has MCC and MNC only; the same burst reported twice is one. */
static void
test_gate_a_colour_zero(void) {
    session s = session_open();
    const uint64_t first = s.state->symbol_clock;
    sb_sysinfo(&s, &k_z, LA_A);
    check(s.state->symbol_clock == first + SB_TAIL, "colour 0: the symbol clock counts the burst's symbols");
    ndb_null(&s, 3u);
    check(g_log.evidence == 0, "colour 0: one BSCH and blocks under the predefined seed confirm nothing");
    /* The same burst again (the symbol clock where it was). */
    const uint64_t later = s.state->symbol_clock;
    s.state->symbol_clock = first;
    sb_sysinfo(&s, &k_z, LA_A);
    s.state->symbol_clock = later;
    check(g_log.evidence == 0, "colour 0: one burst reported twice is one BSCH");
    sb_sysinfo(&s, &k_z, LA_A);
    check(tmo_evidence(0u), "colour 0: a second BSCH from a later burst confirms, CC 0");
    check(g_log.notes == 1 && tmo_note_is(last_note(), 0, 0, DSD_SEARCH_TETRA_MAIN_NONE, 0U),
          "colour 0: MCC and MNC only, never the SYSINFO");
    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_MAIN + 1u, 4u, 2u, LA_B);
    ndb(&s, 3u, &si, NULL);
    check(g_log.notes == 1 && withdrawals() == 0, "colour 0: another SYSINFO withdraws nothing");
    session_close(&s);
}

/* The observation outlives the hunt's no-carrier passes, but not a carrier boundary or a stream flush. */
static void
test_gate_a_across_no_carrier(void) {
    session s = session_open();
    sb(&s, &k_z, NULL, 0);
    no_carrier(&s);
    sb(&s, &k_z, NULL, 0);
    check(tmo_evidence(0u), "observation: a colour 0 BSCH survives a no-carrier pass");
    session_close(&s);

    s = session_open();
    sb(&s, &k_a, NULL, 0);
    no_carrier(&s);
    ndb_null(&s, seed_of(COL_A));
    check(tmo_evidence(COL_A), "observation: a block after a no-carrier pass confirms");
    session_close(&s);

    for (int flush = 0; flush <= 1; flush++) {
        s = session_open();
        sb(&s, &k_z, NULL, 0);
        if (flush) {
            s.state->symbol_clock_epoch++;
        } else {
            carrier_boundary(&s);
        }
        sb(&s, &k_z, NULL, 0);
        check(g_log.evidence == 0, flush ? "observation: a stream flush drops it" : "observation: a retune drops it");
        sb(&s, &k_z, NULL, 0);
        check(tmo_evidence(0u), "observation: the next carrier's own pair confirms");
        session_close(&s);
    }

    /* A stream flush inside the read of the corroborating burst (the input generation moves at its first symbol), with
     * no interruption raised: the block read across it is no corroboration, and the observation goes with it. */
    s = session_open();
    sb(&s, &k_a, NULL, 0);
    const uint32_t epoch = s.state->symbol_clock_epoch;
    s.opts->pcm_input_generation++;
    ndb_null(&s, seed_of(COL_A));
    check(s.state->symbol_clock_epoch == epoch + 1U, "observation: the burst's read crossed a stream flush");
    check(g_log.evidence == 0 && g_log.activity == 0, "observation: a block read across a flush confirms nothing");
    ndb_null(&s, seed_of(COL_A));
    check(g_log.evidence == 0, "observation: a flush inside a burst drops it");
    sb(&s, &k_a, NULL, 0);
    ndb_null(&s, seed_of(COL_A));
    check(tmo_evidence(COL_A), "observation: the new stream's own pair confirms");
    session_close(&s);

    /* An interruption the engine took up between two bursts is an input cut: the observation before it is dropped,
     * for colour code 0 and for a block under the seed alike, and the pair read after it confirms. */
    for (int colour = 0; colour <= 1; colour++) {
        s = session_open();
        sb(&s, colour ? &k_a : &k_z, NULL, 0);
        hunt_interrupted(&s);
        if (colour) {
            ndb_null(&s, seed_of(COL_A));
        } else {
            sb(&s, &k_z, NULL, 0);
        }
        check(g_log.evidence == 0, colour ? "observation: an interrupted hunt drops it before a block"
                                          : "observation: an interrupted hunt drops it before a colour 0 BSCH");
        if (colour) {
            ndb_null(&s, seed_of(COL_A));
            check(g_log.evidence == 0, "observation: none is left for a later block");
            sb(&s, &k_a, NULL, 0);
            ndb_null(&s, seed_of(COL_A));
        } else {
            sb(&s, &k_z, NULL, 0);
        }
        check(tmo_evidence(colour ? COL_A : 0u), "observation: the pair read after the interruption confirms");
        session_close(&s);
    }
}

/* What withdraws the identity, latched and sent once: another cell's BSCH, a SYSINFO under the gate seed naming another
 * location area or main carrier, two cells before the gate opened (which confirms TETRA with no access code and no site
 * at all: the hit is published, and may reach the journal, before the withdrawal). A shared carrier shows no site. */
static void
test_gate_a_withdrawals(void) {
    session s = session_open();
    sb_sysinfo(&s, &k_a, LA_A);
    sb(&s, &k_b, NULL, 0);
    check(withdrawals() == 1 && last_note()->kind == DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW,
          "withdraw: another cell's BSCH");
    sb_sysinfo(&s, &k_a, LA_A);
    sb(&s, &k_b, NULL, 0);
    ndb_null(&s, seed_of(COL_A));
    check(withdrawals() == 1 && g_log.evidence == 1, "withdraw: A, B, A stays withdrawn, sent once");
    session_close(&s);

    for (int what = 0; what <= 1; what++) {
        s = session_open();
        sb_sysinfo(&s, &k_a, LA_A);
        ttb_block si = {0};
        ttb_sysinfo(&si, what ? CARR_MAIN + 1u : CARR_MAIN, 4u, 2u, what ? LA_A : LA_B);
        ndb(&s, seed_of(COL_A), NULL, &si);
        check(withdrawals() == 1, what ? "withdraw: another main carrier" : "withdraw: another location area");
        session_close(&s);
    }

    s = session_open();
    sb(&s, &k_b, NULL, 0);
    sb_sysinfo(&s, &k_a, LA_A);
    check(g_log.evidence == 1 && strcmp(g_log.protocol, "TETRA") == 0 && g_log.access_null
              && g_log.notes_at_evidence == 0,
          "withdraw: two cells before the gate opened confirm TETRA with no access code or site");
    check(g_log.notes == 1 && withdrawals() == 1, "withdraw: two cells before the gate opened, then withdrawn");
    sb_sysinfo(&s, &k_a, LA_A);
    ndb_null(&s, seed_of(COL_A));
    check(g_log.evidence == 1 && g_log.notes == 1, "withdraw: no later site or access for the ambiguous visit");
    session_close(&s);

    static const sync_spec k_shared = {COL_A, 4u, 2u, 18u};
    s = session_open();
    sb_sysinfo(&s, &k_shared, LA_A);
    check(tmo_evidence(COL_A) && g_log.notes == 0, "shared: MCCH sharing confirms with no site");
    session_close(&s);

    s = session_open();
    sb_sysinfo(&s, &k_a, LA_A);
    sb(&s, &k_shared, NULL, 0);
    check(g_log.notes == 2 && withdrawals() == 1, "shared: sharing seen after the site withdraws it");
    session_close(&s);
}

/* What gate A does not take: a reserved SYNC after confirmation changes nothing; a burst an input interruption cut is
 * no corroboration, and the observation before it is dropped; the next visit starts cold, its seed gone with the last
 * one's network. */
static void
test_gate_a_lost_and_reserved(void) {
    session s = session_open();
    sb_sysinfo(&s, &k_a, LA_A);
    static const sync_spec k_reserved_b = {COL_B, 9u, 0u, 18u};
    sb(&s, &k_reserved_b, NULL, 0);
    check(tmo_evidence(COL_A) && withdrawals() == 0 && g_log.notes == 1,
          "gate A: a reserved SYNC of another identity after confirmation changes nothing");
    tetra_scan_visit_reset(s.opts, s.state);
    log_clear();
    ndb_null(&s, seed_of(COL_A));
    check(g_log.evidence == 0 && g_log.activity == 0, "gate A: the next visit inherits no gate and no seed");
    session_close(&s);

    s = session_open();
    sb(&s, &k_a, NULL, 0);
    s.state->input_interrupted = 1;
    ndb_null(&s, seed_of(COL_A));
    s.state->input_interrupted = 0;
    check(g_log.evidence == 0, "gate A: a block of a cut burst is no corroboration");
    s.state->input_interrupted = 1;
    dsb(&s, &k_direct);
    s.state->input_interrupted = 0;
    check(g_log.evidence == 0, "gate D: a cut DSB confirms nothing");
    ndb_null(&s, seed_of(COL_A));
    check(g_log.evidence == 0, "gate A: an input interruption drops the observation");
    sb(&s, &k_a, NULL, 0);
    ndb_null(&s, seed_of(COL_A));
    check(tmo_evidence(COL_A), "gate A: a BSCH and a block read whole after it confirm");
    session_close(&s);
}

/* ---- Gate D ----------------------------------------------------------------------------------------------------- */

/* One DSB with both CRCs and a valid layout confirms TETRA DMO, no access text, with its route; a duplicate report is
 * one hit. */
static void
test_gate_d_opens(void) {
    session s = session_open();
    dsb(&s, &k_direct);
    check(dmo_evidence(), "gate D: a whole DSB confirms TETRA DMO");
    check(g_log.notes == 1 && dmo_note_is(last_note(), DSD_SEARCH_TETRA_ROUTE_DIRECT, 0), "gate D: direct route");
    dsb(&s, &k_direct);
    check(g_log.evidence == 1 && g_log.notes == 1, "gate D: a repeated DSB publishes nothing");
    static const dsb_spec k_rep = {0xD, 0, 1, 0, 3, 0, 0};
    dsb(&s, &k_rep);
    check(g_log.notes == 2 && dmo_note_is(last_note(), DSD_SEARCH_TETRA_ROUTE_REPEATER, 0),
          "gate D: the route follows the latest DMAC-SYNC");
    static const dsb_spec k_pres = {0xD, 1, 2, 0, 3, 0, 0};
    dsb(&s, &k_pres);
    check(g_log.notes == 2, "gate D: a presence signal is no route");
    no_carrier(&s);
    dsb(&s, &k_direct);
    check(g_log.evidence == 1 && g_log.notes == 3 && dmo_note_is(last_note(), DSD_SEARCH_TETRA_ROUTE_DIRECT, 0),
          "gate D: kept across a no-carrier pass; a direct DMAC-SYNC after a repeater one");
    tetra_scan_visit_reset(s.opts, s.state);
    log_clear();
    dsb(&s, &k_direct);
    check(dmo_evidence(), "gate D: the next visit confirms on its own DSB");
    session_close(&s);
}

/* What gate D refuses: an SCH/H that failed, a reserved direct mode code, a reserved SYNC PDU type, a reserved frame
 * number, A/B usage 11, a gateway DMAC-SYNC off channel usage 00, a type 1 DM-REP presence signal off 00. */
static void
test_gate_d_negatives(void) {
    static const struct {
        dsb_spec d;
        const char* what;
    } k_cases[] = {
        {{0xA, 0, 0, 0, 3, 0, 0}, "gate D: a reserved direct mode system code"},
        {{0xD, 2, 0, 0, 3, 0, 0}, "gate D: SYNC PDU type 10"},
        {{0xD, 3, 0, 0, 3, 0, 0}, "gate D: SYNC PDU type 11"},
        {{0xD, 0, 0, 0, 0, 0, 0}, "gate D: frame number 0"},
        {{0xD, 0, 0, 0, 19, 0, 0}, "gate D: frame number 19"},
        {{0xD, 0, 0, 3, 3, 0, 0}, "gate D: A/B channel usage 11"},
        {{0xD, 0, 2, 1, 3, 0, 0}, "gate D: a gateway DMAC-SYNC on channel usage 01"},
        {{0xD, 1, 1, 1, 3, 0, 0}, "gate D: a type 1 DM-REP presence signal on channel usage 01"},
        {{0xD, 1, 1, 2, 3, 0, 0}, "gate D: a type 1 DM-REP presence signal on channel B"},
    };

    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        session s = session_open();
        dsb(&s, &k_cases[i].d);
        dsb(&s, &k_cases[i].d); /* reported twice */
        check(g_log.evidence == 0 && g_log.notes == 0, k_cases[i].what);
        session_close(&s);
    }
    session s = session_open();
    const ttb_block h = sch_h_of(TETRA_DMO_MSG_OCCUPIED);
    dsb_with(&s, &k_direct, &h, 1);
    dsb_with(&s, &k_direct, &h, 1);
    check(g_log.evidence == 0 && g_log.notes == 0, "gate D: SCH/S-only DSBs, reported twice, confirm nothing");
    session_close(&s);

    /* A type 2 DM-REP's presence signal on channel B confirms, presence only. */
    s = session_open();
    static const dsb_spec k_type2 = {0xD, 1, 1, 2, 3, 0, 1};
    dsb(&s, &k_type2);
    check(dmo_evidence() && dmo_note_is(last_note(), DSD_SEARCH_TETRA_ROUTE_NONE, 1),
          "gate D: a type 2 DM-REP's channel B presence signal confirms as presence");
    dsb(&s, &k_direct);
    check(dmo_note_is(last_note(), DSD_SEARCH_TETRA_ROUTE_DIRECT, 0), "gate D: a DMAC-SYNC then gives the route");
    session_close(&s);

    /* Trunked mode bursts never open gate D. */
    s = session_open();
    sb_sysinfo(&s, &k_a, LA_A);
    sb(&s, &k_z, NULL, 0);
    sb(&s, &k_z, NULL, 0);
    check(g_log.evidence == 1 && strcmp(g_log.protocol, "TETRA") == 0, "gate D: trunked bursts are no DSB");
    session_close(&s);
}

/* Mixed visits, both orders: the first gate names the detection, the other gate's opening withdraws its site and
 * access; a lone opposite-mode CRC changes nothing. */
static void
test_mixed_visits(void) {
    session s = session_open();
    sb_sysinfo(&s, &k_a, LA_A);
    const ttb_block h = sch_h_of(TETRA_DMO_MSG_OCCUPIED);
    dsb_with(&s, &k_direct, &h, 1);
    check(withdrawals() == 0, "mixed: an SCH/S-only DSB after gate A changes nothing");
    dsb(&s, &k_direct);
    check(tmo_evidence(COL_A) && withdrawals() == 1, "mixed: gate D after gate A withdraws");
    session_close(&s);

    s = session_open();
    dsb(&s, &k_direct);
    sb(&s, &k_a, NULL, 0);
    check(withdrawals() == 0, "mixed: a lone BSCH after gate D changes nothing");
    sb_sysinfo(&s, &k_a, LA_A);
    check(dmo_evidence() && withdrawals() == 1, "mixed: gate A after gate D withdraws");
    session_close(&s);
}

/* A mixed visit's activity comes from the mode that named the detection only: once the direct mode call it named ends,
 * a trunked control channel on the carrier (gate A opening, its BSCHs, blocks and SYSINFO) keeps the visit active no
 * more, so the visit goes quiet at its time in the tick and the in-frame yield check alike, which read only these
 * notes. The other way round, direct mode content keeps no trunked detection active. */
static void
test_mixed_activity(void) {
    session s = session_open();
    dsb(&s, &k_direct);
    dnb_full(&s);
    check(dmo_evidence() && g_log.activity == 2, "mixed activity: the direct mode call keeps its visit active");
    /* The call ends; the trunked control channel goes on. */
    sb(&s, &k_a, NULL, 0);
    sb_sysinfo(&s, &k_a, LA_A);
    ndb_null(&s, seed_of(COL_A));
    sb(&s, &k_a, NULL, 0);
    ttb_block si = {0};
    ttb_sysinfo(&si, CARR_MAIN, 4u, 2u, LA_A);
    ndb(&s, seed_of(COL_A), &si, NULL);
    check(withdrawals() == 1 && g_log.activity == 2, "mixed activity: trunked bursts keep a TETRA DMO visit no more");
    /* A direct mode call again (the trunked BSCHs took the decoder out of direct mode). */
    dsb(&s, &k_direct);
    dnb_full(&s);
    check(g_log.activity == 4, "mixed activity: direct mode content still keeps it");
    session_close(&s);

    s = session_open();
    sb_sysinfo(&s, &k_a, LA_A);
    check(tmo_evidence(COL_A) && g_log.activity == 1, "mixed activity: gate A's burst keeps its visit active");
    dsb(&s, &k_direct);
    dnb_full(&s);
    check(withdrawals() == 1 && g_log.activity == 1, "mixed activity: direct mode content keeps a TETRA visit no more");
    /* Trunked bursts again (the DSB took the decoder into direct mode): the BSCH, then a block. */
    sb(&s, &k_a, NULL, 0);
    ndb_null(&s, seed_of(COL_A));
    check(g_log.activity == 3, "mixed activity: trunked content still keeps it");
    session_close(&s);
}

/* Direct mode activity: a DSB the call records take as a call, a DNB block under the DM colour code; never a
 * reservation, DM-2-C or a presence signal. */
static void
test_dmo_activity(void) {
    session s = session_open();
    dsb(&s, &k_direct);
    check(g_log.activity == 1, "DMO activity: a DM-OCCUPIED DSB");
    dnb_full(&s);
    check(g_log.activity == 2, "DMO activity: a checked DNB block under the DM colour code");
    const ttb_block reserved = sch_h_of(TETRA_DMO_MSG_RESERVED);
    dsb_with(&s, &k_direct, &reserved, 0);
    check(g_log.activity == 2, "DMO activity: DM-RESERVED is none");
    static const dsb_spec k_pres = {0xD, 1, 1, 0, 3, 0, 0};
    dsb(&s, &k_pres);
    check(g_log.activity == 2 && g_log.evidence == 1, "DMO activity: a presence signal is none");
    static const dsb_spec k_dm2c = {0xD, 0, 0, 0, 3, 1, 0};
    dsb(&s, &k_dm2c);
    check(g_log.activity == 2, "DMO activity: a DM-2-C DSB is none");
    session_close(&s);

    /* A visit confirmed by a reservation alone is identified, never kept active. */
    s = session_open();
    dsb_with(&s, &k_direct, &reserved, 0);
    dsb_with(&s, &k_direct, &reserved, 0);
    check(dmo_evidence() && g_log.activity == 0, "DMO activity: a reservation-only channel is identified only");
    session_close(&s);
}

/* The DM colour code of a call through a DM-REP is the low six bits of the repeater address with the source address, on
 * both links (EN 300 396-4 clause 8.2.4, which replaces EN 300 396-3 clause 8.2.4 there), and of a call through a gateway
 * or a DM-REP/GATE those of the gateway address (EN 300 396-5 clause 8.2.4); a direct call keeps the MNI's. Here the MNI
 * and the repeater address differ in their low six bits (0x12 against 0x15), so a normal burst encoded independently
 * with the clause's colour code decodes only under the right one, and keeps the confirmed visit active. */
static void
test_relayed_colour_code(void) {
    static const uint32_t k_mni = ((uint32_t)MCC << 14) | MNC; /* low six bits 0x12 */
    static const uint32_t k_relay = 0x155u;                    /* low six bits 0x15 */

    static const struct {
        uint32_t comm_type;
        uint32_t net6;
        uint8_t route;
        const char* what;
    } k_cases[] = {
        {0, k_mni & 0x3Fu, DSD_SEARCH_TETRA_ROUTE_DIRECT, "relayed colour code: a direct call keeps the MNI's"},
        {1, k_relay & 0x3Fu, DSD_SEARCH_TETRA_ROUTE_REPEATER,
         "relayed colour code: via a DM-REP, the repeater address"},
        {2, k_relay & 0x3Fu, DSD_SEARCH_TETRA_ROUTE_GATEWAY, "relayed colour code: via a DM-GATE, the gateway address"},
        {3, k_relay & 0x3Fu, DSD_SEARCH_TETRA_ROUTE_REPEATER_GATEWAY,
         "relayed colour code: via a DM-REP/GATE, the gateway address"},
    };

    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        session s = session_open();
        const dsb_spec d = {0xD, 0, k_cases[i].comm_type, 0, 3, 0, 0};
        const ttb_block h = sch_h_as(TETRA_DMO_MSG_OCCUPIED, k_cases[i].comm_type ? k_relay : 0u, k_mni);
        dsb_with(&s, &d, &h, 0);
        check(dmo_evidence() && dmo_note_is(last_note(), k_cases[i].route, 0), "relayed colour code: the DSB confirms");
        const uint32_t decoded = tetra_st_existing(s.state)->decode_ok;
        const int activity = g_log.activity;
        dnb_full_with(&s, dcc_seed(k_cases[i].net6));
        check(tetra_st_existing(s.state)->decode_ok == decoded + 1U && g_log.activity == activity + 1, k_cases[i].what);
        session_close(&s);
    }
}

/* Outside a Signal search nothing is reported. */
static void
test_not_searching(void) {
    session s = session_open();
    g_log.running = 0;
    sb_sysinfo(&s, &k_a, LA_A);
    dsb(&s, &k_direct);
    check(g_log.evidence == 0 && g_log.notes == 0 && g_log.activity == 0, "no search: nothing reported");
    g_log.running = 1;
    session_close(&s);
}

int
main(void) {
    dsd_signal_search_active_set(running_hook);
    dsd_signal_search_evidence_set(evidence_hook);
    dsd_signal_search_site_set(site_hook);
    dsd_signal_search_activity_set(activity_hook);
    test_gate_a_bsch_and_its_bnch();
    test_gate_a_by_other_blocks();
    test_gate_a_negatives();
    test_gate_a_colour_zero();
    test_gate_a_across_no_carrier();
    test_gate_a_withdrawals();
    test_gate_a_lost_and_reserved();
    test_gate_d_opens();
    test_gate_d_negatives();
    test_mixed_visits();
    test_mixed_activity();
    test_dmo_activity();
    test_relayed_colour_code();
    test_not_searching();
    if (g_fail) {
        DSD_FPRINTF(stderr, "TETRA_SEARCH: %d failed\n", g_fail);
        return 1;
    }
    DSD_FPRINTF(stdout, "TETRA_SEARCH: OK\n");
    return 0;
}
