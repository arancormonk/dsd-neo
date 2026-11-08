// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 scrambling code recovery (issue #804). A carrier's FACCH and SACCH bursts are scrambled with a sequence
 * that is linear in the 44-bit code WACN|SYSID|NAC, and their Reed-Solomon codes are linear over GF(2), so one burst's
 * 114 (FACCH) or 132 (SACCH) parity checks are equations in the code's 44 bits (ez_p25p2_seed_tables). With the burst's
 * superframe slot T known, one burst without errors fixes the code with 70 or 88 checks to spare; bursts with errors
 * are solved with their least reliable symbols erased.
 *
 * A code solved at a wrong T is a shifted code that decodes every burst mislabelled the same way, and a wrong code's
 * verdict is the same for every burst at one T. So a candidate (solved, or a code the Signal search session learned)
 * is only accepted once a burst of another window, located by its own I-ISCH, at another T, passes Reed-Solomon (the
 * punctured symbols erased, nothing more) and its MAC CRC under it.
 *
 * A burst is left to the normal path only when the code in force passes that same check, which is what proves a code
 * (p25p2_acch_verdict): one the normal path decodes with soft erasures proves nothing, so recovery still tries it.
 *
 * Nothing here allocates per window or per burst: the tables are built once per process (ez_p25p2_seed_tables_get())
 * and the context once per decoder state, at its first located window on an unproven carrier.
 */

#include "p25p2_seed_recover.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/fec/ez.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../p25_search_site.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25p2_frame_internal.h"

extern int p2bit[4320];
extern int16_t p2llr[1400];

enum {
    SR_TIMESLOTS = 4,
    SR_TIMESLOT_BITS = 360,
    SR_SEQUENCE_START = 20, /* a timeslot's burst bit 0 takes sequence bit 20 + 360 * T */
    SR_SLOTS = 12,
    SR_CODE_BITS = 44,
    SR_SYMBOL_BITS = 6,
    SR_MAX_SYMBOLS = EZ_P25P2_SEED_MAX_BITS / SR_SYMBOL_BITS,
    SR_MAX_CODES = 16,
    /* Checks left once the code and the erased symbols are fixed: a burst of noise satisfies them by chance with
       2^-16, so a solve ends with at most 9 (FACCH) or 12 (SACCH) symbols erased. */
    SR_MIN_SPARE_CHECKS = 16,
    SR_DUID_SACCH = 3,
    SR_DUID_FACCH = 9,
};

/* A row's syndrome bit, beside the code bits 0..43. */
#define SR_RHS (1ULL << 63)

/* A candidate waiting for its confirmation, and where it came from. */
typedef struct {
    int valid;
    uint64_t code;
    int source_t;           /* superframe slot of the burst that gave it */
    uint32_t source_window; /* p25p2_seed_recover_ctx::window_serial of its window */
    uint32_t carrier_seq;   /* dsd_state::carrier_seq it was found on */
    uint32_t alignment;     /* p25p2_seed_recover_ctx::alignment it was found under */
} p25p2_seed_pending;

typedef struct {
    p25p2_seed_pending pending;
    /* A committed code whose system the Signal search hears of once a burst proves it (p25p2_seed_recover_note_proven()),
       0 for none, and the dsd_state::carrier_seq it was committed on. */
    uint64_t owed_network;
    uint32_t owed_carrier_seq;
    uint32_t window_serial; /* windows recovery saw since the context was made */
    uint32_t alignment;     /* windows whose I-ISCH contradicted continuity */
} p25p2_seed_recover_ctx;

/* One scrambled FACCH or SACCH of the window, as received. */
typedef struct {
    int kind; /* EZ_P25P2_SEED_FACCH or EZ_P25P2_SEED_SACCH */
    p25p2_acch_kind acch;
    int t;                                   /* superframe slot */
    uint64_t bits[EZ_P25P2_SEED_WORDS];      /* codeword bits, packed as the check rows */
    uint16_t reliab[EZ_P25P2_SEED_MAX_BITS]; /* |LLR| of each */
} sr_burst;

/* What one burst did. */
typedef enum {
    SR_BURST_NOTHING = 0,
    SR_BURST_IN_FORCE,  /* the code in force passes: the burst proves it as it decodes */
    SR_BURST_HELD,      /* the pending candidate passes, but not where it can be confirmed */
    SR_BURST_CANDIDATE, /* a new candidate waits for confirmation */
    SR_BURST_CONFIRMED, /* the pending candidate is confirmed */
} sr_burst_result;

/* The Signal search session's codes, read once per window when a burst first needs them. */
typedef struct {
    int loaded;
    size_t count;
    uint64_t codes[SR_MAX_CODES];
} sr_learned;

/* One check row while symbols are erased: the codeword bits it still holds, and the code bits with its syndrome. */
typedef struct {
    uint64_t check[EZ_P25P2_SEED_WORDS];
    uint64_t seed;
} sr_row;

/* The rows that still constrain the code are rows[0..n_rows-1]; each erased bit takes one row with it. */
typedef struct {
    sr_row rows[EZ_P25P2_SEED_MAX_CHECKS];
    int n_rows;
} sr_system;

static p25p2_seed_recover_ctx*
sr_ctx_peek(const dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(p25p2_seed_recover_ctx, state, DSD_STATE_EXT_PROTO_P25P2_SEED_RECOVER);
}

/* The context, made at the first located window of an unproven carrier and kept for the decoder state's life: the
   module's only allocation besides the tables. */
static p25p2_seed_recover_ctx*
sr_ctx_get(dsd_state* state) {
    p25p2_seed_recover_ctx* ctx = sr_ctx_peek(state);
    if (ctx) {
        return ctx;
    }
    ctx = (p25p2_seed_recover_ctx*)calloc(1, sizeof(*ctx));
    if (ctx && dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_P25P2_SEED_RECOVER, ctx, free) != 0) {
        free(ctx);
        ctx = NULL;
    }
    return ctx;
}

/* A code whose every part is a usable value, as p25p2_duid_has_valid_site() requires of the seed in force: no zero or
   all-ones WACN, SYSID or NAC. */
static int
sr_code_valid(uint64_t code) {
    const uint64_t wacn = code >> 24;
    const uint64_t sysid = (code >> 12) & 0xFFFU;
    const uint64_t nac = code & 0xFFFU;
    return wacn != 0U && wacn < 0xFFFFFU && sysid != 0U && sysid != 0xFFFU && nac != 0U && nac != 0xFFFU;
}

/* The code in force, or 0 when it is no valid code. */
static uint64_t
sr_code_in_force(const dsd_state* state) {
    if (state->p2_wacn > 0xFFFFFU || state->p2_sysid > 0xFFFU || state->p2_cc > 0xFFFU) {
        return 0U;
    }
    const uint64_t code = ((uint64_t)state->p2_wacn << 24) | ((uint64_t)state->p2_sysid << 12) | (uint64_t)state->p2_cc;
    return sr_code_valid(code) ? code : 0U;
}

static int
sr_bit(const uint64_t* row, int bit) {
    return (int)((row[bit / 64] >> (bit % 64)) & 1U);
}

static int
sr_n_bits(const ez_p25p2_seed_tables* tables, int kind) {
    return tables->payload_bits[kind] + tables->checks[kind];
}

/* The scrambled FACCH or SACCH of window timeslot @p ts, if it carries one. */
static int
sr_load_burst(const ez_p25p2_seed_tables* tables, const dsd_state* state, int ts, sr_burst* burst) {
    const int duid = p25p2_window_duid(ts);
    if (duid == SR_DUID_FACCH) {
        burst->kind = EZ_P25P2_SEED_FACCH;
        burst->acch = P25P2_ACCH_FACCH;
    } else if (duid == SR_DUID_SACCH) {
        burst->kind = EZ_P25P2_SEED_SACCH;
        burst->acch = P25P2_ACCH_SACCH;
    } else {
        return 0;
    }
    burst->t = (((state->p2_scramble_offset + ts) % SR_SLOTS) + SR_SLOTS) % SR_SLOTS;
    DSD_MEMSET(burst->bits, 0, sizeof(burst->bits));
    const int n_bits = sr_n_bits(tables, burst->kind);
    for (int k = 0; k < n_bits; k++) {
        const int abs_bit = (ts * SR_TIMESLOT_BITS) + tables->burst_bit[burst->kind][k];
        if (p2bit[abs_bit] & 1) {
            burst->bits[k / 64] |= 1ULL << (k % 64);
        }
        const int llr = p2llr[abs_bit];
        burst->reliab[k] = (uint16_t)(llr < 0 ? -llr : llr);
    }
    return 1;
}

/* Whether the burst, descrambled with @p code, passes Reed-Solomon with only the punctured symbols erased and its MAC
   CRC: what proves a code on the normal path (p25p2_acch_verdict). Writes nothing but local buffers. */
static int
sr_try_code(const ez_p25p2_seed_tables* tables, const sr_burst* burst, uint64_t code) {
    const int start = SR_SEQUENCE_START + (SR_TIMESLOT_BITS * burst->t);
    int payload[EZ_P25P2_SEED_MAX_BITS];
    int parity[EZ_P25P2_SEED_MAX_CHECKS];
    const int n_payload = tables->payload_bits[burst->kind];
    const int n_bits = sr_n_bits(tables, burst->kind);
    for (int k = 0; k < n_bits; k++) {
        const uint64_t mask = tables->sequence[start + tables->burst_bit[burst->kind][k]];
        const int bit = sr_bit(burst->bits, k) ^ (dsd_popcount64(mask & code) & 1);
        if (k < n_payload) {
            payload[k] = bit;
        } else {
            parity[k - n_payload] = bit;
        }
    }
    const int ec = burst->kind == EZ_P25P2_SEED_FACCH ? ez_rs28_facch_fixed(payload, parity)
                                                      : ez_rs28_sacch_fixed(payload, parity);
    return ec >= 0 && p25p2_acch_checksum_ok(burst->acch, payload);
}

/* The burst's equations: each check row with its syndrome over the received bits. */
static void
sr_system_init(sr_system* sys, const ez_p25p2_seed_tables* tables, const sr_burst* burst) {
    sys->n_rows = tables->checks[burst->kind];
    for (int i = 0; i < sys->n_rows; i++) {
        sr_row* row = &sys->rows[i];
        int ones = 0;
        for (int w = 0; w < EZ_P25P2_SEED_WORDS; w++) {
            row->check[w] = tables->check[burst->kind][i][w];
            ones += dsd_popcount64(row->check[w] & burst->bits[w]);
        }
        row->seed = tables->seed[burst->kind][burst->t][i] | ((ones & 1) ? SR_RHS : 0U);
    }
}

/* Makes codeword bit @p bit an unknown: the first row that holds it solves for it and stops constraining the code, and
   the others drop it. */
static void
sr_erase_bit(sr_system* sys, int bit) {
    const int word = bit / 64;
    const uint64_t mask = 1ULL << (bit % 64);
    int pivot = -1;
    for (int i = 0; i < sys->n_rows && pivot < 0; i++) {
        if (sys->rows[i].check[word] & mask) {
            pivot = i;
        }
    }
    if (pivot < 0) {
        return;
    }
    const sr_row taken = sys->rows[pivot];
    sys->n_rows--;
    sys->rows[pivot] = sys->rows[sys->n_rows];
    for (int i = 0; i < sys->n_rows; i++) {
        sr_row* row = &sys->rows[i];
        if (row->check[word] & mask) {
            for (int w = 0; w < EZ_P25P2_SEED_WORDS; w++) {
                row->check[w] ^= taken.check[w];
            }
            row->seed ^= taken.seed;
        }
    }
}

/* Gaussian elimination over the code bits. Returns 1 with @p code when the remaining rows fix all 44 bits and agree,
   0 when they disagree, -1 when they no longer fix them all. */
static int
sr_eliminate(const sr_system* sys, uint64_t* code) {
    uint64_t rows[EZ_P25P2_SEED_MAX_CHECKS];
    const int n = sys->n_rows;
    for (int i = 0; i < n; i++) {
        rows[i] = sys->rows[i].seed;
    }
    for (int c = 0; c < SR_CODE_BITS; c++) {
        const uint64_t mask = 1ULL << c;
        int pivot = -1;
        for (int i = c; i < n && pivot < 0; i++) {
            if (rows[i] & mask) {
                pivot = i;
            }
        }
        if (pivot < 0) {
            return -1;
        }
        const uint64_t taken = rows[pivot];
        rows[pivot] = rows[c];
        rows[c] = taken;
        for (int i = 0; i < n; i++) {
            if (i != c && (rows[i] & mask)) {
                rows[i] ^= taken;
            }
        }
    }
    for (int i = SR_CODE_BITS; i < n; i++) {
        if (rows[i] & SR_RHS) {
            return 0;
        }
    }
    uint64_t solved = 0;
    for (int c = 0; c < SR_CODE_BITS; c++) {
        if (rows[c] & SR_RHS) {
            solved |= 1ULL << c;
        }
    }
    *code = solved;
    return 1;
}

/* The burst's sent RS symbols, least reliable first by their weakest bit. */
static int
sr_symbol_order(const ez_p25p2_seed_tables* tables, const sr_burst* burst, int* order) {
    const int n_symbols = sr_n_bits(tables, burst->kind) / SR_SYMBOL_BITS;
    uint16_t weakest[SR_MAX_SYMBOLS];
    for (int s = 0; s < n_symbols; s++) {
        uint16_t low = UINT16_MAX;
        for (int b = 0; b < SR_SYMBOL_BITS; b++) {
            const uint16_t r = burst->reliab[(s * SR_SYMBOL_BITS) + b];
            low = r < low ? r : low;
        }
        weakest[s] = low;
        int at = s;
        while (at > 0 && weakest[order[at - 1]] > low) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = s;
    }
    return n_symbols;
}

/* Solves the burst's equations for the code: with no symbol erased, then with the least reliable symbols erased one
   more at a time while the code stays fixed with SR_MIN_SPARE_CHECKS to spare. The first consistent solution is the
   only one any larger erasure set can give, so it is the burst's one candidate: kept when it is a valid code under
   which the burst passes its checks. */
static int
sr_solve(const ez_p25p2_seed_tables* tables, const sr_burst* burst, uint64_t* code_out) {
    sr_system sys;
    int order[SR_MAX_SYMBOLS];
    sr_system_init(&sys, tables, burst);
    const int n_symbols = sr_symbol_order(tables, burst, order);
    for (int erased = 0; erased <= n_symbols; erased++) {
        if (erased > 0) {
            for (int b = 0; b < SR_SYMBOL_BITS; b++) {
                sr_erase_bit(&sys, (order[erased - 1] * SR_SYMBOL_BITS) + b);
            }
        }
        if (sys.n_rows - SR_CODE_BITS < SR_MIN_SPARE_CHECKS) {
            return 0;
        }
        uint64_t code = 0;
        const int solved = sr_eliminate(&sys, &code);
        if (solved != 0) {
            if (solved > 0 && sr_code_valid(code) && sr_try_code(tables, burst, code)) {
                *code_out = code;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

/* The first of the session's learned codes the burst passes under, skipping the two already tried. */
static int
sr_try_learned(const dsd_state* state, sr_learned* learned, const ez_p25p2_seed_tables* tables, const sr_burst* burst,
               const uint64_t* tried, uint64_t* code_out) {
    if (!learned->loaded) {
        learned->count = dsd_signal_search_p25p2_codes(state, learned->codes, SR_MAX_CODES);
        learned->loaded = 1;
    }
    for (size_t i = 0; i < learned->count; i++) {
        const uint64_t code = learned->codes[i];
        if (sr_code_valid(code) && code != tried[0] && code != tried[1] && sr_try_code(tables, burst, code)) {
            *code_out = code;
            return 1;
        }
    }
    return 0;
}

/* The candidate the burst passes under, other than the two already tried: a learned code, else a solved one. */
static int
sr_find_candidate(const dsd_state* state, sr_learned* learned, const ez_p25p2_seed_tables* tables,
                  const sr_burst* burst, const uint64_t* tried, uint64_t* code) {
    return sr_try_learned(state, learned, tables, burst, tried, code) || sr_solve(tables, burst, code);
}

/* One burst, in order: the code in force, the pending candidate, the learned codes, a solve. Only the context
   changes: a burst the code in force passes drops the candidate, which that code's proof makes moot. */
static sr_burst_result
sr_process_burst(const dsd_state* state, p25p2_seed_recover_ctx* ctx, sr_learned* learned,
                 const ez_p25p2_seed_tables* tables, const sr_burst* burst) {
    const uint64_t in_force = sr_code_in_force(state);
    if (in_force != 0U && sr_try_code(tables, burst, in_force)) {
        DSD_MEMSET(&ctx->pending, 0, sizeof(ctx->pending));
        return SR_BURST_IN_FORCE;
    }
    const int has_pending = ctx->pending.valid;
    if (has_pending && ctx->pending.code != in_force && sr_try_code(tables, burst, ctx->pending.code)) {
        if (ctx->pending.source_window != ctx->window_serial && ctx->pending.source_t != burst->t) {
            return SR_BURST_CONFIRMED;
        }
        return SR_BURST_HELD;
    }
    const uint64_t tried[2] = {in_force, has_pending ? ctx->pending.code : 0U};
    uint64_t code = 0;
    if (!sr_find_candidate(state, learned, tables, burst, tried, &code)) {
        return SR_BURST_NOTHING;
    }
    ctx->pending.valid = 1;
    ctx->pending.code = code;
    ctx->pending.source_t = burst->t;
    ctx->pending.source_window = ctx->window_serial;
    ctx->pending.carrier_seq = state->carrier_seq;
    ctx->pending.alignment = ctx->alignment;
    return SR_BURST_CANDIDATE;
}

/* The confirmed code becomes the seed in force, as a network status broadcast that passed its CRC applies one
   (p25p2_vpdu_apply_nsb_identity()): WACN and SYSID through p25_update_system_identity(), which resets what belonged
   to another system, and the NAC. The Signal search hears of the code now. The burst that confirmed it then proves it
   as this window decodes, and only then does the search hear of the system its WACN and SYSID name: the proof records
   the carrier's proven NAC first, which the search judges a site note by (issue #802 B11). */
static void
sr_commit(dsd_state* state, p25p2_seed_recover_ctx* ctx) {
    const uint64_t code = ctx->pending.code;
    const unsigned long long wacn = (unsigned long long)(code >> 24);
    const unsigned long long sysid = (unsigned long long)((code >> 12) & 0xFFFU);
    const unsigned long long nac = (unsigned long long)(code & 0xFFFU);
    DSD_MEMSET(&ctx->pending, 0, sizeof(ctx->pending));
    (void)p25_update_system_identity(state, wacn, sysid);
    if (state->p2_cc != nac) {
        dsd_state_forget_p2_proof(state);
    }
    state->p2_cc = nac;
    DSD_FPRINTF(stderr, " P25p2 scrambling code recovered: WACN %05llX SYSID %03llX NAC %03llX", wacn, sysid, nac);
    dsd_signal_search_note_p25p2_code(state, code, DSD_P25P2_CODE_RECOVERED);
    ctx->owed_network = code;
    ctx->owed_carrier_seq = state->carrier_seq;
}

/* A new window for the context, if there is one: a contradicted position, or another carrier, drops the candidate. */
static void
sr_ctx_next_window(p25p2_seed_recover_ctx* ctx, const dsd_state* state, int position_contradicted) {
    if (!ctx) {
        return;
    }
    ctx->window_serial++;
    if (position_contradicted) {
        ctx->alignment++;
    }
    if (ctx->pending.carrier_seq != state->carrier_seq || ctx->pending.alignment != ctx->alignment) {
        DSD_MEMSET(&ctx->pending, 0, sizeof(ctx->pending));
    }
}

void
p25p2_seed_recover_window(const dsd_opts* opts, dsd_state* state, int isch_located, int position_contradicted) {
    if (!state) {
        return;
    }
    if (state->p2_cc_verified != 0U) {
        // A proven code leaves nothing to recover: a candidate from before the proof does not outlive it.
        p25p2_seed_recover_forget(state);
        return;
    }
    if (!opts || state->p2_hardset != 0 || opts->trunk_enable != 0) {
        return;
    }
    p25p2_seed_recover_ctx* ctx = sr_ctx_peek(state);
    sr_ctx_next_window(ctx, state, position_contradicted);
    const ez_p25p2_seed_tables* tables = (position_contradicted || !isch_located) ? NULL : ez_p25p2_seed_tables_get();
    ctx = tables ? sr_ctx_get(state) : NULL;
    if (!ctx) {
        return;
    }
    sr_learned learned = {0};
    for (int ts = 0; ts < SR_TIMESLOTS; ts++) {
        sr_burst burst;
        if (!sr_load_burst(tables, state, ts, &burst)) {
            continue;
        }
        const sr_burst_result result = sr_process_burst(state, ctx, &learned, tables, &burst);
        if (result == SR_BURST_CONFIRMED) {
            sr_commit(state, ctx);
            return;
        }
        if (result == SR_BURST_IN_FORCE) {
            return;
        }
    }
}

void
p25p2_seed_recover_forget(const dsd_state* state) {
    p25p2_seed_recover_ctx* ctx = state ? sr_ctx_peek(state) : NULL;
    if (ctx) {
        DSD_MEMSET(&ctx->pending, 0, sizeof(ctx->pending));
        ctx->owed_network = 0U;
    }
}

void
p25p2_seed_recover_note_proven(const dsd_state* state, uint64_t code) {
    p25p2_seed_recover_ctx* ctx = state ? sr_ctx_peek(state) : NULL;
    if (!ctx || ctx->owed_network == 0U) {
        return;
    }
    const uint64_t owed = ctx->owed_network;
    const int same_carrier = ctx->owed_carrier_seq == state->carrier_seq;
    ctx->owed_network = 0U;
    if (owed == code && same_carrier) {
        p25_search_note_recovered_network(state, (uint32_t)(code >> 24), (uint16_t)((code >> 12) & 0xFFFU));
    }
}

int
p25p2_seed_recover_pending(const dsd_state* state, uint64_t* code) {
    const p25p2_seed_recover_ctx* ctx = state ? sr_ctx_peek(state) : NULL;
    if (!ctx || !ctx->pending.valid) {
        return 0;
    }
    if (code) {
        *code = ctx->pending.code;
    }
    return 1;
}
