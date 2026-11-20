// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for M17 sync constants and dispatch routing.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_m17(int synctype);
dsd_frame_verdict dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state);

static int lsf_calls;
static int pkt_calls;
static int skip_calls;
static int skipped_dibits;
static int str_calls;
static int brt_calls;
static int confirm_reset_calls;

static void
reset_calls(void) {
    lsf_calls = 0;
    pkt_calls = 0;
    skip_calls = 0;
    skipped_dibits = 0;
    str_calls = 0;
    brt_calls = 0;
    confirm_reset_calls = 0;
}

/* What the stubbed frame decoders report back: 1 = validated something, 0 = proved nothing. */
static int g_frame_validated = 1;
static int g_confirmed = 1;

void
m17_confirm_reset(dsd_state* state) {
    (void)state;
    confirm_reset_calls++;
    g_confirmed = 0;
}

int
m17_confirm_is_confirmed(const dsd_state* state) {
    (void)state;
    return g_confirmed;
}

/* What an EOT dispatch reads after the marker words the hunt took: the signs of the symbols on air, oldest first, as
   the hunt slices them; past the end of the script the air is negative ('3'). */
static const char* g_air = "";
static size_t g_air_pos;
static int g_air_reads;
static int g_resume_calls;
static int g_resume_symbols;
/* An input interruption the read at this symbol (1-based) meets: 0 none. */
static int g_air_interrupt_at;

static void
set_air(const char* signs) {
    g_air = signs;
    g_air_pos = 0U;
    g_air_reads = 0;
    g_resume_calls = 0;
    g_resume_symbols = 0;
    g_air_interrupt_at = 0;
}

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)out_analog_signal;
    g_air_reads++;
    g_air_pos++;
    if (g_air_interrupt_at != 0 && g_air_reads >= g_air_interrupt_at) {
        state->input_interrupted = 1;
    }
    return 1;
}

char
dsd_frame_sync_symbol_sign_back(const dsd_state* state, int back) {
    (void)state;
    const long index = (long)g_air_pos - 1L - (long)back;
    if (index < 0 || (size_t)index >= strlen(g_air)) {
        return '3';
    }
    return g_air[index];
}

void
dsd_frame_sync_hand_back_symbols(dsd_state* state, int symbols) {
    (void)state;
    g_resume_calls++;
    g_resume_symbols = symbols;
}

void
skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    skip_calls++;
    skipped_dibits += count;
}

int
processM17LSF(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    lsf_calls++;
    return g_frame_validated;
}

int
processM17PKT(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    pkt_calls++;
    return g_frame_validated;
}

int
processM17STR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    str_calls++;
    return g_frame_validated;
}

int
processM17BRT(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    brt_calls++;
    return g_frame_validated;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(M17_LSF) == 9U, "M17_LSF length");
    _Static_assert(sizeof(M17_STR) == 9U, "M17_STR length");
    _Static_assert(sizeof(M17_PRE) == 9U, "M17_PRE length");
    _Static_assert(sizeof(M17_PIV) == 9U, "M17_PIV length");
    _Static_assert(sizeof(M17_BRT) == 9U, "M17_BRT length");
    _Static_assert(sizeof(M17_PKT) == 9U, "M17_PKT length");
    _Static_assert(sizeof(M17_EOT) == 9U, "M17_EOT length");
    _Static_assert(sizeof(M17_EOT_INV) == 9U, "M17_EOT_INV length");
    _Static_assert(sizeof(M17_PRE_LSF) == 17U, "M17_PRE_LSF length");
    _Static_assert(sizeof(M17_PIV_LSF) == 17U, "M17_PIV_LSF length");
    assert(strcmp(M17_STR, "33331131") == 0);
    assert(strcmp(M17_PKT, "13113333") == 0);
}

static void
test_synctype_helpers(void) {
    static const int m17_synctypes[] = {
        DSD_SYNC_M17_STR_POS, DSD_SYNC_M17_STR_NEG, DSD_SYNC_M17_LSF_POS, DSD_SYNC_M17_LSF_NEG,
        DSD_SYNC_M17_BRT_POS, DSD_SYNC_M17_BRT_NEG, DSD_SYNC_M17_PKT_POS, DSD_SYNC_M17_PKT_NEG,
        DSD_SYNC_M17_PRE_POS, DSD_SYNC_M17_PRE_NEG, DSD_SYNC_M17_EOT_POS, DSD_SYNC_M17_EOT_NEG,
    };

    for (size_t i = 0; i < sizeof m17_synctypes / sizeof m17_synctypes[0]; i++) {
        assert(DSD_SYNC_IS_M17(m17_synctypes[i]));
        assert(dsd_dispatch_matches_m17(m17_synctypes[i]));
    }

    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_STR_NEG), "M17 STR negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_LSF_NEG), "M17 LSF negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_BRT_NEG), "M17 BRT negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_PKT_NEG), "M17 PKT negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_PRE_NEG), "M17 PRE negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_EOT_NEG), "M17 EOT negative is inverted");
    _Static_assert(!DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_STR_POS), "M17 STR positive is not inverted");
    _Static_assert(!DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_LSF_POS), "M17 LSF positive is not inverted");

    _Static_assert(!DSD_SYNC_IS_M17(DSD_SYNC_DSTAR_VOICE_POS), "D-STAR voice is not M17");
    _Static_assert(!DSD_SYNC_IS_M17(DSD_SYNC_YSF_POS), "YSF is not M17");
    assert(!dsd_dispatch_matches_m17(DSD_SYNC_DSTAR_VOICE_POS));
    assert(!dsd_dispatch_matches_m17(DSD_SYNC_YSF_POS));
}

static void
test_synctype_names(void) {
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_STR_POS), "M17") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_LSF_NEG), "M17") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_BRT_POS), "M17 BRT") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_PKT_NEG), "M17 PKT") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_PRE_POS), "M17 PRE") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_EOT_NEG), "M17 EOT") == 0);
}

/* Verdict of the most recent dispatch_one(), so cases can check it without threading a
 * second return value through every caller (#391). */
static dsd_frame_verdict last_verdict = DSD_FRAME_VERDICT_PRODUCTIVE;

static int
dispatch_one(int synctype) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    state.synctype = synctype;
    last_verdict = dsd_dispatch_handle_m17(&opts, &state);
    return state.lastsynctype;
}

static void
test_preamble_dispatch(void) {
    dispatch_one(DSD_SYNC_M17_PRE_POS);
    /* 8 dibits is below DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS, which #388 calibrated against
     * exactly this path, so the floor already refuses it credit and the verdict stays at
     * the default. */
    assert(last_verdict == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(skip_calls == 1);
    assert(skipped_dibits == 8);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_PRE_NEG);
    assert(skip_calls == 1);
    assert(skipped_dibits == 8);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
}

static void
test_payload_dispatch(void) {
    dispatch_one(DSD_SYNC_M17_LSF_POS);
    assert(skip_calls == 0);
    assert(lsf_calls == 1);
    assert(pkt_calls == 0);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_LSF_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 1);
    assert(pkt_calls == 0);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_PKT_POS);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 1);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_PKT_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 1);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_STR_POS);
    /* A stream frame is a decode, whatever the payload turns out to hold. */
    assert(last_verdict == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 1);

    dispatch_one(DSD_SYNC_M17_STR_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 1);
}

/* Five words of the marker, in phase. */
#define EOT5     M17_EOT M17_EOT M17_EOT M17_EOT M17_EOT
#define EOT5_INV M17_EOT_INV M17_EOT_INV M17_EOT_INV M17_EOT_INV M17_EOT_INV

static void
test_eot_dispatch(void) {
    /* The hunt took the end on three marker words; the rest of the marker (here five words) is drained word by word
     * up to the first word that is not the marker, and that word, the next transmission's sync word here, is handed
     * back to the hunt, not skipped: the old blind 184-symbol skip consumed the next frame when the marker's first
     * words were lost to a fade. */
    set_air(EOT5 M17_LSF "1313");
    int lastsynctype = dispatch_one(DSD_SYNC_M17_EOT_POS);
    /* #391: the EOT marker is positive evidence, and this branch acts on it -- the same
     * DSD_CALL_END_TERMINATOR the call state is given. The SPS hunt is told the same. */
    assert(last_verdict == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(skip_calls == 0);
    assert(g_air_reads == 48);
    assert(g_resume_calls == 1);
    assert(g_resume_symbols == 8);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
    assert(lastsynctype == DSD_SYNC_NONE);
    /* ...and then forgets the transmission's proof, so the next carrier has to earn its own (#399). */
    assert(confirm_reset_calls == 1);

    set_air(EOT5_INV M17_STR);
    lastsynctype = dispatch_one(DSD_SYNC_M17_EOT_NEG);
    assert(skip_calls == 0);
    assert(g_air_reads == 48);
    assert(g_resume_calls == 1);
    assert(g_resume_symbols == 8);
    assert(lastsynctype == DSD_SYNC_NONE);
    assert(confirm_reset_calls == 1);

    /* A marker word read with one symbol off is still the marker, at either polarity. */
    set_air(M17_EOT "11311131" M17_EOT "33333333");
    (void)dispatch_one(DSD_SYNC_M17_EOT_POS);
    assert(g_air_reads == 32);
    assert(g_resume_symbols == 8);
    set_air(M17_EOT_INV "33333311"
                        "11111111");
    (void)dispatch_one(DSD_SYNC_M17_EOT_NEG);
    assert(g_air_reads == 24);
    assert(g_resume_symbols == 8);

    /* The rest of a whole marker is at most the 21 words the hunt did not take: an endless marker is not read past it,
     * and the symbol after it is the hunt's own. */
    set_air(EOT5 EOT5 EOT5 EOT5 EOT5);
    (void)dispatch_one(DSD_SYNC_M17_EOT_POS);
    assert(g_air_reads == 168);
    assert(g_resume_calls == 0);

    /* An input interruption ends the drain with nothing handed back: the next hunt starts afresh. */
    set_air(EOT5);
    g_air_interrupt_at = 12;
    (void)dispatch_one(DSD_SYNC_M17_EOT_POS);
    assert(g_air_reads == 16);
    assert(g_resume_calls == 0);
}

/* An end of transmission ends the slot's M17 call, never another protocol's call open on the slot. */
static void
test_eot_ends_only_m17_call(void) {
    static dsd_opts opts;
    static dsd_state state;

    const struct {
        int protocol;
        int ends;
    } cases[] = {
        {DSD_SYNC_M17_STR_POS, 1},
        {DSD_SYNC_M17_LSF_NEG, 1},
        {DSD_SYNC_DMR_BS_VOICE_POS, 0},
        {DSD_SYNC_YSF_POS, 0},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();
        const dsd_call_observation observation = {
            .protocol = cases[i].protocol,
            .slot = 0U,
            .kind = DSD_CALL_KIND_VOICE,
            .ota_target_id = 1U,
            .ota_source_id = 2U,
        };
        assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
        set_air("");
        state.synctype = DSD_SYNC_M17_EOT_POS;
        (void)dsd_dispatch_handle_m17(&opts, &state);
        dsd_call_snapshot call;
        assert(dsd_call_state_get(&state, 0U, &call) > 0);
        if (cases[i].ends) {
            assert(call.phase != DSD_CALL_PHASE_ACTIVE);
            assert(call.end_reason == DSD_CALL_END_TERMINATOR);
        } else {
            assert(call.phase == DSD_CALL_PHASE_ACTIVE);
            assert(call.protocol == cases[i].protocol);
        }
    }
}

static void
test_bert_dispatch(void) {
    dispatch_one(DSD_SYNC_M17_BRT_POS);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
    assert(brt_calls == 1);

    dispatch_one(DSD_SYNC_M17_BRT_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
    assert(brt_calls == 1);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_synctype_names();
    test_preamble_dispatch();
    test_payload_dispatch();
    test_eot_dispatch();
    test_eot_ends_only_m17_call();
    test_bert_dispatch();
    printf("M17_SYNC_DISPATCH: OK\n");
    return 0;
}
