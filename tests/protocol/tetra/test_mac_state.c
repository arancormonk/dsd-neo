// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA downlink upper MAC and basic-link LLC decoding (EN 300 392-2 clauses 21.2, 21.4 and 23.4) and the state it
 * publishes. Every block is built field by field from the ETSI tables (tetra_test_pdu.h).
 */

#include <assert.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_mac_pdu.h"
#include "tetra_state.h"
#include "tetra_test_log.h"
#include "tetra_test_pdu.h"

static int tune_calls;
static int return_calls;
static long tuned_freq;

static dsd_trunk_tune_result
tune_stub(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    tune_calls++;
    tuned_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_stub(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return_calls++;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    assert(state != NULL);
    assert(tetra_st(state) != NULL);
    return state;
}

static dsd_opts*
new_opts(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    return opts;
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* The trunking state machine starts over on a control channel some CRC-passed block has already proved: these tests
 * hand the MAC blocks straight, without the bursts that would have proved it. */
static void
start_on_proven_control_channel(dsd_opts* opts, dsd_state* state) {
    tetra_sm_init(opts, state);
    tetra_confirm_note_signalling(state);
}

/* A control channel on band 4 with offset -6,25 kHz and trunking that follows every call type. */
static void
setup_trunking(dsd_opts* opts, dsd_state* state) {
    tetra_st(state)->freq_band = 4;
    tetra_st(state)->freq_offset = 2;
    state->trunk_cc_freq = 460000000L;
    state->samplesPerSymbol = 10;
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    start_on_proven_control_channel(opts, state);
    tune_calls = 0;
    return_calls = 0;
    tuned_freq = 0;
}

/* Whether any call record holds a traffic channel (tetra_calls_traffic()). */
static int
has_traffic(const dsd_state* state) {
    tetra_chan_alloc a;
    return tetra_calls_traffic(state, &a);
}

static int
check(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
    }
    return ok;
}

/* EN 300 392-2 annex C, step by step: complement the first 32 bits (all of them when the frame is shorter), multiply
 * by x^32, divide by G(x) of equation C.1 by long division and complement the 32-bit remainder. */
static uint32_t
annex_c_fcs(const uint8_t* frame, int n) {
    static const int k_exponents[] = {32, 26, 23, 22, 16, 12, 11, 10, 8, 7, 5, 4, 2, 1, 0};
    uint8_t generator[33] = {0};
    uint8_t dividend[256 + 32] = {0};
    assert(n >= 0 && n <= 256);
    for (size_t i = 0; i < sizeof k_exponents / sizeof k_exponents[0]; i++) {
        generator[32 - k_exponents[i]] = 1;
    }
    for (int i = 0; i < n; i++) {
        dividend[i] = (uint8_t)((frame[i] & 1u) ^ (i < 32 ? 1u : 0u));
    }
    for (int i = 0; i < n; i++) {
        if (dividend[i] != 0u) {
            for (int k = 0; k <= 32; k++) {
                dividend[i + k] ^= generator[k];
            }
        }
    }
    uint32_t remainder = 0;
    for (int k = 0; k < 32; k++) {
        remainder = (remainder << 1) | dividend[n + k];
    }
    return ~remainder;
}

static int
test_llc_fcs_annex_c(void) {
    static const uint8_t text[] = "123456789";
    uint8_t bits[256];
    for (int i = 0; i < 72; i++) {
        bits[i] = (uint8_t)((text[i / 8] >> (7 - (i % 8))) & 1u);
    }
    /* For frames of 32 bits or more annex C is CRC-32/BZIP2, whose catalogued check value is 0xFC891918. */
    int ok = check(tetra_llc_fcs32(bits, 72) == 0xFC891918u, "LLC FCS of '123456789' is the CRC-32/BZIP2 check value");
    ok &= check(annex_c_fcs(bits, 72) == 0xFC891918u, "annex C reference agrees with the CRC-32/BZIP2 check value");
    uint32_t lfsr = 0xACE1u;
    for (int i = 0; i < 256; i++) {
        lfsr = (lfsr >> 1) ^ ((lfsr & 1u) != 0u ? 0xB400u : 0u);
        bits[i] = (uint8_t)(lfsr & 1u);
    }
    static const int k_lengths[] = {0, 1, 7, 20, 31, 32, 33, 44, 100, 255};
    for (size_t i = 0; i < sizeof k_lengths / sizeof k_lengths[0]; i++) {
        ok &= check(tetra_llc_fcs32(bits, k_lengths[i]) == annex_c_fcs(bits, k_lengths[i]),
                    "LLC FCS follows annex C for short and long frames");
    }
    return ok;
}

/* SYSINFO of main carrier @p carrier in location area 987. */
static void
put_sysinfo(ttb_block* b, uint32_t carrier, uint32_t band, uint32_t offset) {
    ttb_sysinfo(b, carrier, band, offset, 987);
}

static int
test_sysinfo(void) {
    ttb_block b = {0};
    put_sysinfo(&b, 50, 4, 2);
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    tetra_mac_broadcast bc;
    int ok =
        check(tetra_mac_broadcast_decode(b.bits, b.n, &bc) && bc.type == TETRA_MAC_BC_SYSINFO && bc.sysinfo.mle_present
                  && bc.sysinfo.la == 987 && bc.sysinfo.subscriber_class == 0xBEEF && bc.sysinfo.bs_service == 0xA5C
                  && !bc.sysinfo.cck_flag && bc.sysinfo.hyperframe_or_cck == 0x1234,
              "SYSINFO decodes with its D-MLE-SYSINFO");
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
    const tetra_state_t* ts = tetra_st(state);
    ok &= check(ts->sysinfo_known && ts->la == 987, "SYSINFO keeps the location area of its D-MLE-SYSINFO");
    ok &= check(ts->hyperframe_valid && ts->hyperframe == 0x1234, "SYSINFO hyperframe number");
    ok &= check(state->trunk_cc_freq == 401243750L && ts->dl_carrier_hz == 401243750L && ts->freq_band == 4,
                "SYSINFO main carrier becomes the control channel");
    free_state(state);
    free(opts);
    return ok;
}

/* Table 21.65 note 3: main carrier 0 and band 0 mean the cell has no CA main carrier. Band 0 is never a carrier
 * frequency, so neither may replace the known control channel. */
static int
test_sysinfo_band_zero_keeps_control_channel(void) {
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    tetra_state_t* ts = tetra_st(state);
    ts->freq_band = 4;
    ts->freq_offset = 2;
    ts->dl_carrier_hz = 401243750L;
    state->trunk_cc_freq = 401243750L;
    int ok = 1;
    static const uint32_t k_carriers[] = {100, 0};
    for (int i = 0; i < 2; i++) {
        ttb_block b = {0};
        put_sysinfo(&b, k_carriers[i], 0, 0);
        tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
        ok &= check(state->trunk_cc_freq == 401243750L && ts->dl_carrier_hz == 401243750L && ts->freq_band == 4
                        && ts->freq_offset == 2,
                    "SYSINFO in band 0 leaves the control channel and band alone");
        ok &= check(ts->sysinfo_known && ts->la == 987, "SYSINFO in band 0 still publishes its cell data");
    }
    free_state(state);
    free(opts);
    return ok;
}

static int
test_sysinfo_tooshort(void) {
    uint8_t bits[10] = {1, 0};
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    tetra_mac_parse_schd(bits, 10, 0, opts, state);
    const int ok = check(!tetra_st(state)->sysinfo_known, "a truncated SYSINFO publishes nothing");
    free_state(state);
    free(opts);
    return ok;
}

static int
test_null_state(void) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 1);
    ttb_resource_flags(&b, 0);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    dsd_opts* opts = new_opts();
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, NULL);
    free(opts);
    return 1;
}

static int
test_mac_resource_ssi(void) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x123456u);
    ttb_resource_flags(&b, 0);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    tetra_mac_resource h;
    int ok = check(tetra_mac_resource_decode(b.bits, b.n, &h) && h.addr.type == TETRA_MAC_ADDR_SSI
                       && h.addr.ssi == 0x123456u && !h.addr.esi && h.enc_mode == 0 && h.length_ind == 6 && h.fill == 1
                       && h.pdu_bits == 48 && !h.alloc_present,
                   "an addressed header-only MAC-RESOURCE decodes with its SSI");
    char log[512];
    tetra_test_mac_logged(b.bits, b.n, 0, opts, state, log, sizeof log);
    const tetra_state_t* ts = tetra_st(state);
    ok &= check(ts->enc_mode == 0 && ts->frames_resource == 1
                    && strstr(log, "[TETRA MAC-RESOURCE] CC=0  enc=none  rand_acc=0  len_ind=6  addr=SSI(1193046)  "
                                   "sdu_bits=0\n")
                           != NULL,
                "an addressed header-only MAC-RESOURCE is acted on");
    free_state(state);
    free(opts);
    return ok;
}

/* Clause 21.4.3.1: nothing follows the address type of a Null PDU and its other header fields are ignored. Bits after
 * it that look like a channel allocation must not assign a channel, and the call and encryption state stay. The
 * length indication is that of the Null PDU (2 octets) or, on STCH, 111110 for a stolen second half slot. */
static int
test_null_pdu_leaves_state(void) {
    int ok = 1;
    static const uint32_t k_length_ind[] = {2, 62};
    for (int i = 0; i < 2; i++) {
        ttb_block b = {0};
        ttb_put(&b, 0, 2);               /* MAC-RESOURCE */
        ttb_put(&b, 1, 1);               /* fill bit indication */
        ttb_put(&b, 0, 4);               /* position of grant, encryption mode, random access flag */
        ttb_put(&b, k_length_ind[i], 6); /* length indication */
        ttb_put(&b, 0, 3);               /* address type: Null PDU */
        ttb_put(&b, 0, 2);               /* (not part of the PDU) power control and slot granting flags */
        ttb_put(&b, 1, 1);               /* (not part of the PDU) channel allocation flag */
        ttb_allocation(&b, 4, 1, 400);
        while (b.n < TTB_SCH_HD_BITS) {
            ttb_put(&b, 0, 1);
        }
        dsd_state* state = new_state();
        dsd_opts* opts = new_opts();
        setup_trunking(opts, state);
        tetra_state_t* ts = tetra_st(state);
        ts->enc_mode = TETRA_ENC_MODE_EVEN;
        tetra_mac_resource h;
        ok &= check(!tetra_mac_resource_decode(b.bits, b.n, &h) && h.addr.type == TETRA_MAC_ADDR_NULL,
                    "a Null PDU has no header to decode");
        tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
        ok &= check(!has_traffic(state) && tune_calls == 0, "a Null PDU carries no channel allocation");
        ok &= check(ts->enc_mode == TETRA_ENC_MODE_EVEN && ts->frames_resource == 0,
                    "a Null PDU leaves the encryption state");
        free_state(state);
        free(opts);
    }
    return ok;
}

/* ACCESS-DEFINE, table 21.79: 32 bits without an optional field. */
static void
put_access_define(ttb_block* b) {
    ttb_put(b, 2, 2);  /* broadcast */
    ttb_put(b, 1, 2);  /* ACCESS-DEFINE */
    ttb_put(b, 0, 1);  /* common control channel */
    ttb_put(b, 3, 2);  /* access code D */
    ttb_put(b, 7, 4);  /* IMM */
    ttb_put(b, 5, 4);  /* WT */
    ttb_put(b, 3, 4);  /* Nu */
    ttb_put(b, 1, 1);  /* frame-length factor */
    ttb_put(b, 15, 4); /* timeslot pointer: all four timeslots */
    ttb_put(b, 2, 3);  /* minimum PDU priority */
    ttb_put(b, 0, 2);  /* optional field flag: none */
    ttb_put(b, 4, 3);  /* filler bits 100 */
}

/* Clause 23.4.3.3: the length indication bounds the MAC-RESOURCE, and the next PDU starts right after it. */
static int
test_length_indication_separates_pdus(void) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x00ABCDu);
    ttb_resource_flags(&b, 0);
    ttb_close_resource(&b, start);
    const int access_define = b.n;
    put_access_define(&b);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    char log[512];
    tetra_test_mac_logged(b.bits, b.n, 0, opts, state, log, sizeof log);
    tetra_mac_broadcast bc;
    const int ok =
        check(strstr(log, "[TETRA MAC-RESOURCE] CC=0  enc=none  rand_acc=0  len_ind=6  addr=SSI(43981)") != NULL,
              "first associated PDU decoded")
        & check(strstr(log,
                       "[TETRA ACCESS-DEFINE] CC=0  common  access_code=D  imm=7  wait=5  num_ra=3  frame_len_f=1  "
                       "ts_ptr=15  min_prio=2\n")
                    != NULL,
                "ACCESS-DEFINE after it decoded with its access code")
        & check(tetra_mac_broadcast_decode(b.bits + access_define, b.n - access_define, &bc)
                    && bc.type == TETRA_MAC_BC_ACCESS_DEF && bc.access_define.access_code == 3
                    && bc.access_define.imm == 7 && bc.access_define.wait_time == 5 && bc.access_define.num_ra == 3
                    && bc.access_define.frame_length_factor == 1 && bc.access_define.timeslot_pointer == 15
                    && bc.access_define.min_priority == 2 && bc.access_define.assigned == 0
                    && bc.access_define.option_flag == 0 && bc.access_define.nbits == 32,
                "ACCESS-DEFINE fields");
    free_state(state);
    free(opts);
    return ok;
}

/* The optional elements belong to the PDU only when they fit in its indicated length: a 5-octet MAC-RESOURCE ends
 * with its SSI, so bits after it are never read as its flags or channel allocation. */
static int
test_optional_elements_bounded_by_pdu_length(void) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x00ABCDu);
    ttb_set(&b, start + 7, 5, 6); /* length indication: 5 octets = 40 bits, the header through the SSI */
    ttb_put(&b, 0, 2);
    ttb_put(&b, 1, 1);
    ttb_allocation(&b, 4, 1, 50);
    while (b.n < TTB_SCH_HD_BITS) {
        ttb_put(&b, 0, 1);
    }
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    setup_trunking(opts, state);
    tetra_mac_resource h;
    int ok = check(!tetra_mac_resource_decode(b.bits, b.n, &h) && h.pdu_bits == 40,
                   "a header longer than its length indication does not decode");
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
    const tetra_state_t* ts = tetra_st(state);
    ok &= check(!has_traffic(state) && tune_calls == 0 && ts->frames_resource == 0,
                "a header longer than its length indication is discarded");
    free_state(state);
    free(opts);
    return ok;
}

/* A MAC-RESOURCE (table 21.55) addressed to @p target carrying BL-UDATA (table 21.10) and a CMCE D-SETUP for call
 * @p call_id, with a channel allocation on carrier @p carrier when @p timeslots is not 0, closed but not finished.
 * Returns the position of the CMCE PDU type. */
static int
put_call_pdu(ttb_block* b, uint32_t target, uint32_t call_id, uint32_t basic_service, uint32_t timeslots,
             uint32_t carrier) {
    const int start = ttb_resource(b, 0, TETRA_MAC_ADDR_SSI, target);
    ttb_resource_flags(b, timeslots != 0u ? 1u : 0u);
    if (timeslots != 0u) {
        ttb_allocation(b, timeslots, 1, carrier);
    }
    ttb_put(b, 2, 4); /* LLC PDU type 0010: BL-UDATA without FCS */
    const int cmce_type = b->n + 3;
    ttb_d_setup(b, call_id, basic_service);
    ttb_close_resource(b, start);
    return cmce_type;
}

/* A call setup addressed to @p target in a block of its own: call 0x1234, timeslot 4 of carrier 60 when
 * @p with_allocation. Returns the position of the CMCE PDU type. */
static int
put_call_block(ttb_block* b, uint32_t target, uint32_t basic_service, int with_allocation) {
    const int cmce_type = put_call_pdu(b, target, 0x1234u, basic_service, with_allocation ? 1u : 0u, 60);
    ttb_finish(b, TTB_SCH_HD_BITS, -1);
    return cmce_type;
}

/* The MAC-RESOURCE address of a D-SETUP is its called party: a GSSI for a group call, an ISSI for an individual one. A
 * D-SETUP that reuses a call identifier for another kind of call starts a new call. */
static int
test_mac_resource_binds_group_target(void) {
    ttb_block b = {0};
    put_call_block(&b, 0x234567u, TTB_SPEECH_GROUP, 0);
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
    const tetra_call_t* c = tetra_calls_peek(state, 0x1234u);
    int ok = check(c && c->called_ssi == 0x234567u && c->basic_service == TTB_SPEECH_GROUP
                       && c->phase == TETRA_CALL_SETUP && (c->flags & TETRA_CALL_F_SETUP_SEEN) != 0u,
                   "MAC SSI bound to the CMCE group call");
    b = (ttb_block){0};
    put_call_block(&b, 0x345678u, TTB_SPEECH_P2P, 0);
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
    c = tetra_calls_peek(state, 0x1234u);
    ok &= check(c && c->called_ssi == 0x345678u && c->basic_service == TTB_SPEECH_P2P,
                "a point-to-point setup under the same identifier is a new individual call");
    free_state(state);
    free(opts);
    return ok;
}

static int
run_call_block(dsd_opts* opts, dsd_state* state, uint32_t basic_service, uint32_t cmce_type) {
    ttb_block b = {0};
    const int type_pos = put_call_block(&b, 0x345678u, basic_service, 1);
    ttb_set(&b, type_pos, cmce_type, 5);
    start_on_proven_control_channel(opts, state);
    tune_calls = 0;
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
    return tune_calls;
}

static int
test_call_allocation_obeys_group_policy(void) {
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    setup_trunking(opts, state);
    opts->trunk_tune_group_calls = 0;
    const tetra_call_t* c = NULL;
    int ok = check(run_call_block(opts, state, TTB_SPEECH_GROUP, TETRA_CMCE_D_SETUP) == 0
                       && (c = tetra_calls_peek(state, 0x1234u)) != NULL && c->called_ssi == 0x345678u
                       && c->alloc.carrier == 60u && c->alloc.timeslots == 1u,
                   "group calls off: no tune, the call keeps its channel");
    opts->trunk_tune_group_calls = 1;
    state->tg_hold = 0x345679u;
    ok &= check(run_call_block(opts, state, TTB_SPEECH_GROUP, TETRA_CMCE_D_SETUP) == 0, "held other group: no tune");
    state->tg_hold = 0x345678u;
    ok &= check(run_call_block(opts, state, TTB_SPEECH_GROUP, TETRA_CMCE_D_SETUP) == 1 && tuned_freq == 401493750L,
                "held group: tuned to carrier 60");
    state->tg_hold = 0;
    opts->trunk_tune_private_calls = 0;
    ok &= check(run_call_block(opts, state, TTB_SPEECH_P2P, TETRA_CMCE_D_SETUP) == 0, "private calls off: no tune");
    opts->trunk_tune_private_calls = 1;
    ok &= check(run_call_block(opts, state, TTB_SPEECH_P2P, TETRA_CMCE_D_SETUP) == 1, "private call tuned");
    opts->trunk_tune_enc_calls = 0;
    ok &= check(run_call_block(opts, state, 0x14u, TETRA_CMCE_D_SETUP) == 0
                    && dsd_enc_lockout_entry_active(state, 0x345678u, 1),
                "end-to-end encrypted calls off: no tune, and the group is locked out");
    opts->trunk_tune_enc_calls = 1;
    opts->trunk_tune_data_calls = 0;
    ok &= check(run_call_block(opts, state, 0x20u, TETRA_CMCE_D_SETUP) == 0, "data calls off: no tune");
    opts->trunk_tune_data_calls = 1;
    /* A TM-SDU that names no call must not let its allocation tune. */
    ok &= check(run_call_block(opts, state, TTB_SPEECH_GROUP, 31u) == 0, "unassigned CMCE PDU: no tune");
    free_state(state);
    free(opts);
    return ok;
}

/* Fill bits (clause 23.4.3.2) are removed before the TM-SDU: the LLC finds its FCS (table 21.11) at the end of the
 * real TL-SDU only then. */
static int
test_fill_bits_removed_before_tm_sdu(void) {
    int ok = 1;
    for (int corrupt = 0; corrupt < 2; corrupt++) {
        ttb_block b = {0};
        const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x345678u);
        ttb_resource_flags(&b, 0);
        ttb_put(&b, 6, 4); /* LLC PDU type 0110: BL-UDATA with FCS */
        const int sdu = b.n;
        ttb_d_setup(&b, 0x0ABCu, TTB_SPEECH_GROUP);
        ttb_put(&b, annex_c_fcs(b.bits + sdu, b.n - sdu) ^ (uint32_t)corrupt, 32);
        ttb_close_resource(&b, start);
        ttb_finish(&b, TTB_SCH_F_BITS, -1);
        assert(b.bits[start + 2] == 1u); /* this PDU carries fill bits */
        dsd_state* state = new_state();
        dsd_opts* opts = new_opts();
        tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
        const tetra_state_t* ts = tetra_st(state);
        const tetra_call_t* const c = tetra_calls_peek(state, 0x0ABCu);
        (void)ts;
        if (corrupt) {
            ok &= check(c == NULL, "an FCS mismatch discards the TL-SDU");
        } else {
            ok &= check(c && c->called_ssi == 0x345678u, "TL-SDU with FCS delivered once fill bits are removed");
        }
        free_state(state);
        free(opts);
    }
    return ok;
}

/* Encryption mode other than 00: the TM-SDU and the channel allocation are ciphertext and the SSI is an encrypted
 * short identity (EN 300 392-7 clauses 4.2.6 and 6.7.1.2). None of it may be read as plaintext. */
static int
test_encrypted_pdus_not_parsed(void) {
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    setup_trunking(opts, state);
    tetra_state_t* ts = tetra_st(state);
    char log[512];

    ttb_block b = {0};
    int start = ttb_resource(&b, TETRA_ENC_MODE_EVEN, TETRA_MAC_ADDR_SSI, 0x345678u);
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 2, 4);
    ttb_d_setup(&b, 0x0ABCu, TTB_SPEECH_GROUP);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    tetra_test_mac_logged(b.bits, b.n, 0, opts, state, log, sizeof log);
    int ok = check(tetra_calls_peek(state, 0x0ABCu) == NULL && strstr(log, "encrypted; not decoded") != NULL
                       && strstr(log, "D-SETUP") == NULL,
                   "an encrypted TM-SDU is not handed to the MLE");
    tetra_mac_resource h;
    ok &= check(ts->enc_mode == TETRA_ENC_MODE_EVEN && tetra_mac_resource_decode(b.bits, b.n, &h) && h.addr.esi
                    && strstr(log, "addr=ESI(3430008)") != NULL,
                "an encrypted short identity is not taken for an SSI");

    b = (ttb_block){0};
    start = ttb_resource(&b, TETRA_ENC_MODE_ODD, TETRA_MAC_ADDR_SSI, 0x345678u);
    ttb_resource_flags(&b, 1);
    ttb_allocation(&b, 4, 1, 50);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    tetra_test_mac_logged(b.bits, b.n, 0, opts, state, log, sizeof log);
    ok &= check(!has_traffic(state) && tune_calls == 0
                    && strstr(log, "channel allocation and TM-SDU encrypted; not decoded") != NULL
                    && strstr(log, "CHANNEL-ALLOCATION") == NULL,
                "an encrypted channel allocation is not applied");

    /* The encryption mode of a first fragment applies to every fragment of its TM-SDU, rebuilt on its timeslot. */
    ts->tdma_valid = 1;
    ts->tn = 1;
    b = (ttb_block){0};
    start = ttb_resource(&b, TETRA_ENC_MODE_EVEN, TETRA_MAC_ADDR_SSI, 0x345678u);
    ttb_set(&b, start + 7, 63, 6);
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 2, 4);
    ttb_finish(&b, TTB_SCH_HD_BITS, start + 2);
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
    b = (ttb_block){0};
    start = ttb_end(&b);
    ttb_put(&b, 0, 1);
    ttb_d_setup(&b, 0x0ABCu, TTB_SPEECH_GROUP);
    ttb_close_end(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    tetra_test_mac_logged(b.bits, b.n, 0, opts, state, log, sizeof log);
    ok &= check(tetra_calls_peek(state, 0x0ABCu) == NULL && !ts->mac_frag[1].active && strstr(log, "D-SETUP") == NULL,
                "the last fragment of an encrypted TM-SDU is not handed to the MLE");
    free_state(state);
    free(opts);
    return ok;
}

/* LLC PDU types (table 21.1): basic-link headers have different lengths, and advanced-link PDUs carry no TL-SDU for
 * the MLE. */
static int
test_llc_pdu_types(void) {
    static const struct {
        uint32_t header;
        int header_bits;
        int delivered;
        const char* what;
    } k_cases[] = {
        {0x0u, 6, 1, "BL-ADATA: type, N(R) and N(S) precede the TL-SDU"},
        {0x2u, 5, 1, "BL-DATA: type and N(S) precede the TL-SDU"},
        {0x9u, 4, 0, "AL-DATA is not handed to the MLE"},
        {0xAu, 4, 0, "AL-UDATA is not handed to the MLE"},
        {0xEu, 4, 0, "layer 2 signalling is not handed to the MLE"},
    };

    int ok = 1;
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        ttb_block b = {0};
        const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x345678u);
        ttb_resource_flags(&b, 0);
        ttb_put(&b, k_cases[i].header, k_cases[i].header_bits);
        ttb_d_setup(&b, 0x0ABCu, TTB_SPEECH_GROUP);
        ttb_close_resource(&b, start);
        ttb_finish(&b, TTB_SCH_HD_BITS, -1);
        dsd_state* state = new_state();
        dsd_opts* opts = new_opts();
        tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
        ok &= check((tetra_calls_peek(state, 0x0ABCu) != NULL) == (k_cases[i].delivered != 0), k_cases[i].what);
        free_state(state);
        free(opts);
    }
    return ok;
}

/* MAC-D-BLCK (table 21.61) on SCH/F: its TM-SDU starts after the 18-bit header. On STCH the same MAC PDU type is
 * MAC-U-SIGNAL (table 21.80), U-plane signalling that is not handed to the MLE. */
static int
test_mac_d_blck_payload_offset(void) {
    int ok = 1;
    for (int full_slot = 1; full_slot >= 0; full_slot--) {
        ttb_block b = {0};
        ttb_put(&b, 3, 2);      /* supplementary MAC PDU */
        ttb_put(&b, 0, 1);      /* MAC-D-BLCK */
        ttb_put(&b, 1, 1);      /* fill bit indication */
        ttb_put(&b, 0, 2);      /* not encrypted */
        ttb_put(&b, 0x155, 10); /* event label */
        ttb_put(&b, 0, 1);      /* immediate napping permission */
        ttb_put(&b, 0, 1);      /* no slot granting */
        ttb_put(&b, 2, 4);      /* BL-UDATA */
        ttb_d_setup(&b, 0x0777u, TTB_SPEECH_GROUP);
        ttb_put(&b, 1, 1); /* fill bits to the implicit length */
        const int block_bits = full_slot ? TTB_SCH_F_BITS : TTB_SCH_HD_BITS;
        while (b.n < block_bits) {
            ttb_put(&b, 0, 1);
        }
        dsd_state* state = new_state();
        dsd_opts* opts = new_opts();
        tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
        const tetra_call_t* const c = tetra_calls_peek(state, 0x0777u);
        if (full_slot) {
            /* MAC-D-BLCK addresses its party by event label: the call is known, its called party is not. */
            ok &= check(c && c->called_ssi == 0u, "MAC-D-BLCK TM-SDU decoded after its header");
        } else {
            ok &= check(c == NULL, "MAC-U-SIGNAL is not C-plane signalling");
        }
        free_state(state);
        free(opts);
    }
    return ok;
}

/* MLE PDUs reach the MS through MAC-RESOURCE and the LLC (table 21.64 has no broadcast type for them): a
 * D-NWRK-BROADCAST (table 18.2) is decoded from a BL-UDATA, and broadcast type 11 is reserved. */
static int
test_nwrk_broadcast_comes_through_llc(void) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0xFFFFFFu);
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 2, 4); /* BL-UDATA */
    const int mle = b.n;
    ttb_put(&b, TETRA_MLE_PD_MLE, 3);
    ttb_put(&b, TETRA_MLE_D_NWRK_BROADCAST, 3);
    ttb_put(&b, 0xA55Au, 16); /* cell re-select parameters */
    ttb_put(&b, 2, 2);        /* cell load CA */
    ttb_put(&b, 0, 1);        /* O-bit */
    const int mle_end = b.n;
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    char log[1024];
    tetra_test_mac_logged(b.bits, b.n, 0, opts, state, log, sizeof log);
    int ok = check(strstr(log, "[TETRA MLE D-NWRK-BROADCAST] CC=0 reselect=0xA55A load=2 neighbours=0") != NULL,
                   "D-NWRK-BROADCAST decoded from a MAC-RESOURCE TM-SDU");
    free_state(state);

    ttb_block reserved = {0};
    ttb_put(&reserved, 2, 2); /* broadcast */
    ttb_put(&reserved, 3, 2); /* reserved broadcast type */
    ttb_put_bits(&reserved, b.bits + mle, mle_end - mle);
    while (reserved.n < TTB_SCH_HD_BITS) {
        ttb_put(&reserved, 0, 1);
    }
    state = new_state();
    tetra_test_mac_logged(reserved.bits, reserved.n, 0, opts, state, log, sizeof log);
    ok &= check(strstr(log, "D-NWRK-BROADCAST") == NULL, "a reserved broadcast type is not handed to the MLE");
    free_state(state);
    free(opts);
    return ok;
}

/* A header-only MAC-RESOURCE addressed to @p ssi with a basic channel allocation. */
static void
put_allocation_block(ttb_block* b, uint32_t addr_type, uint32_t address, uint32_t timeslots, uint32_t carrier) {
    const int start = ttb_resource(b, 0, addr_type, address);
    if (addr_type == TETRA_MAC_ADDR_EVENT_LABEL) {
        ttb_put(b, address, 10);
    }
    ttb_resource_flags(b, 1);
    ttb_allocation(b, timeslots, 1, carrier);
    ttb_close_resource(b, start);
    ttb_finish(b, TTB_SCH_HD_BITS, -1);
}

/* A D-SETUP to group 0x00BEEF for call 0x0BEE with a channel allocation on timeslot 2 of carrier 50. */
static void
put_followed_setup(ttb_block* b) {
    put_call_pdu(b, 0x00BEEFu, 0x0BEEu, TTB_SPEECH_GROUP, 4, 50);
    ttb_finish(b, TTB_SCH_HD_BITS, -1);
}

/* A channel allocation is followed only with the call PDU that came in its MAC PDU: a header-only one never tunes. Its
 * fields go to the call; timeslot assigned 0000 to the followed call's party ends it. */
static int
test_mac_resource_channel_allocation(void) {
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    setup_trunking(opts, state);
    ttb_block b = {0};
    put_allocation_block(&b, TETRA_MAC_ADDR_SSI, 0x00BEEFu, 4, 50);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    int ok = check(tune_calls == 0 && !has_traffic(state), "a header-only allocation to no call: nothing tunes");

    b = (ttb_block){0};
    put_followed_setup(&b);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    const tetra_call_t* c = tetra_calls_peek(state, 0x0BEEu);
    ok &= check(c && c->alloc.type == 0 && c->alloc.timeslots == 4 && c->alloc.up_down == 1 && c->alloc.carrier == 50
                    && c->alloc.freq_hz == 401243750L && c->air_enc == TETRA_ENC_MODE_NONE,
                "channel allocation fields go to the call");
    tetra_sm_grant g;
    ok &= check(tune_calls == 1 && tuned_freq == 401243750L && tetra_sm_followed(state, &g) && g.call_id == 0x0BEEu
                    && g.timeslots == 4,
                "the allocation of a D-SETUP tunes, for its call");

    /* Timeslot assigned 0000 for the followed call's party sends it back to the control channel. */
    b = (ttb_block){0};
    put_allocation_block(&b, TETRA_MAC_ADDR_SSI, 0x00BEEFu, 0, 50);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    c = tetra_calls_peek(state, 0x0BEEu);
    ok &= check(c && c->alloc.timeslots == 0 && return_calls == 1 && tetra_sm_get_state(state) == TETRA_SM_ON_CC,
                "release to the followed call's party ends it");
    free_state(state);
    free(opts);
    return ok;
}

/* A channel allocation the trunking state machine tunes to ends its block: the PDUs after it were sent on the carrier
 * the receiver left, so the second call setup here is neither recorded nor followed. */
static int
test_retune_ends_the_block(void) {
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    setup_trunking(opts, state);
    ttb_block b = {0};
    put_call_pdu(&b, 0x00BEEFu, 0x0BEEu, TTB_SPEECH_GROUP, 4, 50);
    put_call_pdu(&b, 0x00F00Du, 0x0F00u, TTB_SPEECH_GROUP, 2, 61);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_confirm_burst_begin(state);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    (void)tetra_confirm_burst_end(state);
    const int ok = check(tune_calls == 1 && tuned_freq == 401243750L && tetra_calls_peek(state, 0x0BEEu) != NULL
                             && tetra_calls_peek(state, 0x0F00u) == NULL,
                         "the PDU after a followed allocation is the left carrier's");
    free_state(state);
    free(opts);
    return ok;
}

/* Timeslot assigned 0000 sends only the addressed party back to the control channel (table 21.87). */
static int
test_zero_timeslots_for_another_party(void) {
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    setup_trunking(opts, state);
    ttb_block b = {0};

    put_allocation_block(&b, TETRA_MAC_ADDR_SSI, 0x00F00Du, 0, 50);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    int ok = check(!has_traffic(state) && return_calls == 0, "a release for no call does nothing");

    b = (ttb_block){0};
    put_followed_setup(&b);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    b = (ttb_block){0};
    put_allocation_block(&b, TETRA_MAC_ADDR_SSI, 0x00F00Du, 0, 50);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    const tetra_call_t* c = tetra_calls_peek(state, 0x0BEEu);
    ok &= check(c && c->alloc.timeslots == 4 && return_calls == 0 && tetra_sm_get_state(state) == TETRA_SM_TUNED,
                "a release for another SSI keeps the followed call");

    /* SSI + event label assignment (address type 101) to the called group, then a release by that event label. */
    b = (ttb_block){0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI_EVENT, 0x00BEEFu);
    ttb_put(&b, 0x2A5u, 10);
    ttb_resource_flags(&b, 0);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    ok &= check(c && (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x2A5u,
                "an event label assigned to the called group binds to its call");
    b = (ttb_block){0};
    put_allocation_block(&b, TETRA_MAC_ADDR_EVENT_LABEL, 0x2A4u, 0, 50);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    ok &= check(c && c->alloc.timeslots == 4 && return_calls == 0, "a release for another event label is ignored");
    b = (ttb_block){0};
    put_allocation_block(&b, TETRA_MAC_ADDR_EVENT_LABEL, 0x2A5u, 0, 50);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    ok &= check(c && c->alloc.timeslots == 0 && return_calls == 1, "a release by the call's event label ends it");
    free_state(state);
    free(opts);
    return ok;
}

/* A call setup can arrive before its separate, header-only channel allocation: the allocation goes to the call of the
 * addressed party, which shows it, and is not followed. */
static int
test_allocation_after_call_setup(void) {
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    setup_trunking(opts, state);
    ttb_block b = {0};
    put_call_pdu(&b, 0x00BEEFu, 0x0BEEu, TTB_SPEECH_GROUP, 0, 0);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    b = (ttb_block){0};
    put_allocation_block(&b, TETRA_MAC_ADDR_SSI, 0x00BEEFu, 4, 50);
    tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
    const tetra_call_t* c = tetra_calls_peek(state, 0x0BEEu);
    tetra_chan_alloc shown;
    const int ok = check(tune_calls == 0 && c && c->alloc.carrier == 50 && c->alloc.timeslots == 4
                             && tetra_calls_traffic(state, &shown) && shown.carrier == 50,
                         "a header-only allocation after a call setup attaches to the call and does not tune");
    free_state(state);
    free(opts);
    return ok;
}

/* Table 21.87 notes 5 and 15: an augmented allocation with up/downlink assigned 00 or the further augmentation flag
 * set is discarded and leaves the accepted assignment alone. */
static int
test_rejected_augmented_allocations(void) {
    int ok = 1;
    for (uint32_t further = 0; further < 2; further++) {
        ttb_block b = {0};
        const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x00BEEFu);
        ttb_resource_flags(&b, 1);
        ttb_put(&b, 0, 2);                     /* allocation type */
        ttb_put(&b, 3, 4);                     /* timeslots */
        ttb_put(&b, 0, 2);                     /* augmented channel allocation */
        ttb_put(&b, 0, 2);                     /* CLCH permission, cell change */
        ttb_put(&b, 99, 12);                   /* carrier */
        ttb_put(&b, 0, 1);                     /* no extended carrier numbering */
        ttb_put(&b, 1, 2);                     /* monitoring pattern */
        ttb_put(&b, further, 2);               /* augmented up/downlink: 00 is reserved */
        ttb_put(&b, 0, 3 + 3 + 3 + 3 + 4 + 5); /* bandwidth .. BS transmit power */
        ttb_put(&b, 0, 2);                     /* napping status */
        ttb_put(&b, 0, 4);                     /* reserved */
        ttb_put(&b, 0, 2);                     /* conditional elements A and B absent */
        ttb_put(&b, further, 1);               /* further augmentation */
        ttb_close_resource(&b, start);
        ttb_finish(&b, TTB_SCH_HD_BITS, -1);
        dsd_state* state = new_state();
        dsd_opts* opts = new_opts();
        /* The call to 0x00BEEF holds timeslot 1 of carrier 77, set up while not trunking. */
        ttb_block setup = {0};
        put_call_pdu(&setup, 0x00BEEFu, 0x0BEEu, TTB_SPEECH_GROUP, 8, 77);
        ttb_finish(&setup, TTB_SCH_HD_BITS, -1);
        tetra_st(state)->freq_band = 4;
        tetra_st(state)->freq_offset = 2;
        tetra_mac_parse_schd(setup.bits, setup.n, 2, opts, state);
        setup_trunking(opts, state);
        tetra_mac_parse_schd(b.bits, b.n, 2, opts, state);
        const tetra_call_t* c = tetra_calls_peek(state, 0x0BEEu);
        ok &= check(c && c->alloc.timeslots == 8 && c->alloc.carrier == 77 && c->alloc.freq_hz == 401918750L
                        && tune_calls == 0,
                    further ? "further augmentation discarded" : "reserved augmented up/downlink discarded");
        free_state(state);
        free(opts);
    }
    return ok;
}

/* Fragmented call setup (clause 23.4.2.1.1): the channel allocation comes in the MAC-END and is the only one that
 * applies. */
static int
test_fragmented_call_uses_mac_end_allocation(void) {
    ttb_block sdu = {0};
    ttb_put(&sdu, 6, 4); /* BL-UDATA with FCS */
    ttb_d_setup(&sdu, 0x2345u, TTB_SPEECH_GROUP);
    ttb_put(&sdu, annex_c_fcs(sdu.bits + 4, sdu.n - 4), 32);

    enum { FIRST_PART = 28 };

    int ok = 1;
    for (int pass = 0; pass < 3; pass++) {
        ttb_block first = {0};
        int start = ttb_resource(&first, 0, TETRA_MAC_ADDR_SSI, 0x456789u);
        ttb_set(&first, start + 7, 63, 6);
        ttb_resource_flags(&first, 0);
        ttb_put_bits(&first, sdu.bits, FIRST_PART);
        ttb_finish(&first, TTB_SCH_HD_BITS, start + 2);

        ttb_block end = {0};
        start = ttb_end(&end);
        ttb_put(&end, 1, 1);
        ttb_allocation(&end, 2, 1, 61);
        ttb_put_bits(&end, sdu.bits + FIRST_PART, sdu.n - FIRST_PART);
        ttb_close_end(&end, start);
        ttb_finish(&end, TTB_SCH_HD_BITS, -1);
        if (pass == 2) {
            end.bits[start + 13 + 25 + (sdu.n - FIRST_PART) - 1] ^= 1u; /* last FCS bit */
        }

        dsd_state* state = new_state();
        dsd_opts* opts = new_opts();
        setup_trunking(opts, state);
        opts->trunk_tune_group_calls = pass != 0;
        tetra_state_t* ts = tetra_st(state);
        ts->tdma_valid = 1; /* the TM-SDU is rebuilt on timeslot 1 */
        ts->tn = 1;
        tetra_mac_parse_schd(first.bits, first.n, 1, opts, state);
        ok &= check(tune_calls == 0 && ts->mac_frag[1].active && tetra_calls_peek(state, 0x2345u) == NULL,
                    "first fragment buffered");
        tetra_mac_parse_schd(end.bits, end.n, 1, opts, state);
        const tetra_call_t* const c = tetra_calls_peek(state, 0x2345u);
        if (pass == 0) {
            ok &= check(tune_calls == 0 && c && c->called_ssi == 0x456789u && c->alloc.carrier == 61
                            && !ts->mac_frag[1].active,
                        "fragmented setup decoded; policy blocks the tune");
        } else if (pass == 1) {
            ok &= check(tune_calls == 1 && tuned_freq == 401518750L && c && c->alloc.carrier == 61,
                        "MAC-END allocation tunes to its own carrier");
        } else {
            ok &= check(c == NULL && tune_calls == 0, "LLC FCS failure publishes no call and tunes nowhere");
        }
        free_state(state);
        free(opts);
    }
    return ok;
}

/* Blocks received on an uplink carry uplink PDU layouts (clause 21.4.2) and are not decoded as downlink PDUs. */
static int
test_uplink_blocks_not_decoded(void) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, 0x123456u);
    ttb_resource_flags(&b, 0);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    dsd_state* state = new_state();
    dsd_opts* opts = new_opts();
    tetra_st(state)->uplink = 1;
    tetra_mac_parse_schd(b.bits, b.n, 0, opts, state);
    const int ok = check(tetra_st(state)->frames_resource == 0, "uplink block not decoded with downlink layouts");
    free_state(state);
    free(opts);
    return ok;
}

int
main(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_stub;
    hooks.return_to_cc_request = return_stub;
    dsd_trunk_tuning_hooks_set(hooks);

    int ok = 1;
    ok &= test_llc_fcs_annex_c();
    ok &= test_sysinfo();
    ok &= test_sysinfo_band_zero_keeps_control_channel();
    ok &= test_sysinfo_tooshort();
    ok &= test_null_state();
    ok &= test_mac_resource_ssi();
    ok &= test_null_pdu_leaves_state();
    ok &= test_length_indication_separates_pdus();
    ok &= test_optional_elements_bounded_by_pdu_length();
    ok &= test_mac_resource_binds_group_target();
    ok &= test_call_allocation_obeys_group_policy();
    ok &= test_fill_bits_removed_before_tm_sdu();
    ok &= test_encrypted_pdus_not_parsed();
    ok &= test_llc_pdu_types();
    ok &= test_mac_d_blck_payload_offset();
    ok &= test_nwrk_broadcast_comes_through_llc();
    ok &= test_mac_resource_channel_allocation();
    ok &= test_retune_ends_the_block();
    ok &= test_zero_timeslots_for_another_party();
    ok &= test_allocation_after_call_setup();
    ok &= test_rejected_augmented_allocations();
    ok &= test_fragmented_call_uses_mac_end_allocation();
    ok &= test_uplink_blocks_not_decoded();

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    if (!ok) {
        DSD_FPRINTF(stderr, "TETRA MAC tests FAILED\n");
        return 1;
    }
    DSD_FPRINTF(stderr, "All TETRA MAC tests passed\n");
    return 0;
}
