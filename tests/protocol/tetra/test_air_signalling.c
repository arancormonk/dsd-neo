// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Air encrypted TETRA downlink signalling decrypted with a loaded key (tetra_air_sig.h, issue #750). Blocks are built
 * field by field from the ETSI tables (tetra_test_pdu.h) and encrypted as EN 300 392-7 clause 6.7.1.2 says: everything
 * after the channel allocation flag of a MAC-RESOURCE or MAC-END, a MAC-FRAG's fragment, the fill bits clear, under
 * the slot's key stream segment from KSS(0), or KSS(216) in a second half slot (clause 6.4.2.2, table 6.4), the address
 * an encrypted short identity (clause 4.2.6). The first case is encrypted with key streams computed by TETRA_crypto's
 * own TB5 and TEA2 outside this code base and frozen here, so the decoder and this test's encoder cannot share a
 * mapping mistake. The real MAC, MLE and call records decode them; the tuning backend is a pair of stand-in hooks;
 * time is the decode clock's TEST source.
 */

#include <assert.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tetra_air_sig.h"
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_pdu.h"

#define SECOND_NS     1000000000ULL
/* Downlink frequency of carrier @p n in band 4 with offset -6,25 kHz (EN 300 392-2 clause 21.4.4.1). */
#define CARRIER_HZ(n) (400000000L + ((long)(n) * 25000L) - 6250L)

enum { NET_MCC = 262, NET_MNC = 1001, NET_CC = 25, NET_LA = 77, NET_HN = 5, CC_CARRIER = 100 };

/* The network's cipher key and the identities of its parties. */
static const uint8_t k_sck[10] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x32};
static const uint8_t k_other_key[10] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23};
#define GSSI 0x0DBC68u /* 900200, the reporter's talkgroup */
/* TA61 of GSSI under k_sck, as TETRA_crypto's ta61() computes it. */
#define GESI 0xA0422Cu

/* TETRA_crypto's tb5() and tea2() of k_sck on carrier 100, location area 77, colour code 25 (ECK 00 17 5C 7E 8F ED 8C
 * 7E 80 56), at TN1 of frames 3 and 4 of multiframe 7, hyperframe 5, downlink: initial values 0xA38C and 0xA390. */
static const uint8_t k_ks_fn3[54] = {0x53, 0x3B, 0xC8, 0x58, 0xEF, 0x96, 0xAE, 0xBF, 0xC6, 0xFF, 0xB7, 0xCE, 0xFF, 0x64,
                                     0xD6, 0xB8, 0x64, 0xCD, 0x1A, 0xC1, 0x0D, 0x78, 0x20, 0x9B, 0xA3, 0x53, 0x13, 0x4E,
                                     0x7A, 0x5D, 0xEC, 0xCB, 0x10, 0xC9, 0x5D, 0xEC, 0x47, 0xDA, 0xD9, 0x2F, 0xAC, 0x18,
                                     0x39, 0x25, 0x95, 0x51, 0x19, 0x06, 0x11, 0x62, 0x69, 0xC1, 0xB5, 0x43};
static const uint8_t k_ks_fn4[54] = {0xFD, 0xFF, 0xFF, 0x88, 0x50, 0xD7, 0xB3, 0x46, 0x56, 0xB6, 0x7C, 0xE4, 0x07, 0x70,
                                     0xC0, 0xD7, 0x18, 0xB2, 0x60, 0xAF, 0xC7, 0x22, 0xCB, 0xCF, 0x87, 0x75, 0xFC, 0x79,
                                     0x51, 0xB6, 0x86, 0x03, 0x91, 0x41, 0xC9, 0x64, 0x59, 0x2E, 0xA3, 0x71, 0xAE, 0xC1,
                                     0x41, 0x52, 0x36, 0x2B, 0xAC, 0xCD, 0x9D, 0x99, 0x69, 0xFF, 0x2A, 0x7E};

static int g_tunes;
static int g_returns;

static dsd_trunk_tune_result
tune_hook(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    g_tunes++;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_returns++;
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
at_seconds(double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * (double)SECOND_NS));
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} session;

/* The control channel on carrier 100 of a class 2 cell of network 262/1001 (colour code 25, location area 77),
 * received, proved and placed by its TDMA clock at hyperframe 5, trunking every kind of call but encrypted ones the
 * listener cannot decrypt. */
static session
session_open(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = return_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_trunk_tuning_requests_reset();
    dsd_decode_clock_use_test(SECOND_NS * 1000ULL);
    at_seconds(1000.0);
    g_tunes = 0;
    g_returns = 0;
    session s;
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    s.opts->trunk_enable = 1;
    s.opts->trunk_hangtime = 2.0F;
    s.opts->trunk_tune_group_calls = 1;
    s.opts->trunk_tune_private_calls = 1;
    s.opts->trunk_tune_enc_calls = 0;
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(CC_CARRIER);
    tetra_state_t* const ts = tetra_st(s.state);
    assert(ts != NULL);
    ts->net_known = 1;
    ts->mcc = NET_MCC;
    ts->mnc = NET_MNC;
    ts->colour = NET_CC;
    ts->la = NET_LA;
    ts->sysinfo_known = 1;
    ts->freq_band = 4;
    ts->freq_offset = 2;
    ts->hyperframe = NET_HN;
    ts->hyperframe_valid = 1;
    ts->tdma_valid = 1;
    ts->tn = 1;
    ts->fn = 1;
    ts->mn = 7;
    s.state->trunk_cc_freq = CARRIER_HZ(CC_CARRIER);
    tetra_sm_on_cc_sync(s.opts, s.state);
    tetra_confirm_note_signalling(s.state);
    return s;
}

static void
session_close(session* s) {
    dsd_state_ext_free_all(s->state);
    free(s->state);
    free(s->opts);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* Key slot @p slot: an 80-bit key of the network naming TEA @p alg (0: none). */
static void
load_key(session* s, int slot, const uint8_t* key, uint8_t alg) {
    dsd_tetra_air_key* const k = &s->state->tetra_air_keys[slot];
    DSD_MEMSET(k, 0, sizeof(*k));
    k->loaded = 1;
    k->kind = DSD_TETRA_AIR_KEY_CK;
    k->alg = alg;
    k->mcc = NET_MCC;
    k->mnc = NET_MNC;
    DSD_MEMCPY(k->key, key, 10);
}

/* The burst of timeslot @p tn, frame @p fn of multiframe 7, as the TDMA clock places it (255 symbols a slot). */
static void
in_slot_of(session* s, uint8_t tn, uint8_t fn, uint8_t mn) {
    tetra_state_t* const ts = tetra_st(s->state);
    ts->tdma_valid = 1;
    ts->tn = tn;
    ts->fn = fn;
    ts->mn = mn;
    ts->tdma_slot_start =
        5000u + (((((uint32_t)mn - 1u) * 18u + (uint32_t)(fn - 1u)) * 4u + (uint32_t)(tn - 1u)) * 255u);
}

static void
in_slot(session* s, uint8_t tn, uint8_t fn) {
    in_slot_of(s, tn, fn, 7);
}

/* The session's TETRA state; every session has one once it has decoded a slot. */
static const tetra_state_t*
session_ts(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts) {
        DSD_FPRINTF(stderr, "no TETRA state\n");
        abort();
    }
    return ts;
}

/* Key stream bits @p offset.. of the slot under @p key (TB5 for the cell, TEA @p ksg) into @p ks. */
static void
slot_keystream(const session* s, const uint8_t* key, int ksg, uint16_t carrier, uint8_t* ks, int nbytes) {
    const tetra_state_t* const ts = session_ts(s->state);
    uint8_t eck[10];
    tetra_tea_derive_eck(carrier, NET_LA, NET_CC, key, eck);
    tetra_tea_keystream(ksg, eck, tetra_tea_build_iv(ts->tn, ts->fn, ts->mn, NET_HN, 0), ks, nbytes);
}

/* XOR bits [@p from, @p to) of @p b with key stream bits @p offset.. of @p ks. */
static void
encrypt_span(ttb_block* b, int from, int to, const uint8_t* ks, int offset) {
    for (int i = from; i < to; i++) {
        const int bit = offset + (i - from);
        b->bits[i] = (uint8_t)(b->bits[i] ^ ((ks[bit / 8] >> (7 - (bit % 8))) & 1u));
    }
}

/* A D-SETUP (table 14.15) of group call @p call_id, grant "granted to another user", in a BL-UDATA. */
static void
put_setup_sdu(ttb_block* b, uint16_t call_id) {
    ttb_block cmce = {0};
    ttb_d_setup(&cmce, call_id, TTB_SPEECH_GROUP);
    ttb_put(b, 2, 4); /* BL-UDATA */
    ttb_put_bits(b, cmce.bits, cmce.n);
}

/* The span a MAC-RESOURCE's encryption covers: [*body, *end). */
typedef struct {
    int body;
    int end;
} cipher_span;

/* An encrypted (mode @p enc) MAC-RESOURCE to address @p addr (type SSI + usage marker 42) carrying the D-SETUP of call
 * @p call_id with an allocation of timeslots @p slots on carrier @p carrier, alone in an SCH/F; its ciphertext span
 * not yet encrypted. */
static cipher_span
build_setup(ttb_block* b, uint32_t enc, uint32_t addr, uint16_t call_id, uint32_t slots, uint32_t carrier) {
    DSD_MEMSET(b, 0, sizeof(*b));
    cipher_span span;
    const int start = ttb_resource(b, enc, TETRA_MAC_ADDR_SSI_USAGE, addr);
    ttb_put(b, 42, 6);
    ttb_resource_flags(b, 1);
    span.body = b->n;
    ttb_allocation(b, slots, 1, carrier);
    put_setup_sdu(b, call_id);
    span.end = b->n;
    ttb_close_resource(b, start);
    ttb_finish(b, TTB_SCH_F_BITS, -1);
    return span;
}

/* The block @p b decoded as a CRC-passed SCH/F (or half slot block, @p ks_offset 216 for block 2) of the slot the
 * TDMA clock places. */
static void
decode(session* s, const ttb_block* b, int ks_offset) {
    tetra_mac_parse_block(b->bits, b->n, NET_CC, ks_offset, s->opts, s->state);
}

/* The D-SETUP of call @p call_id to the group sent encrypted under key @p key with TEA @p ksg in TN1 of frame @p fn. */
static void
send_setup_enc(session* s, const uint8_t* key, int ksg, uint8_t tn, uint8_t fn, uint16_t call_id, uint16_t carrier,
               uint32_t enc) {
    in_slot(s, tn, fn);
    ttb_block b;
    const cipher_span span = build_setup(&b, enc, GESI, call_id, 4, 120);
    uint8_t ks[54];
    slot_keystream(s, key, ksg, carrier, ks, (int)sizeof ks);
    encrypt_span(&b, span.body, span.end, ks, 0);
    decode(s, &b, 0);
}

/* send_setup_enc() under encryption mode 10, key version parity 0. */
static void
send_setup_on(session* s, const uint8_t* key, int ksg, uint8_t tn, uint8_t fn, uint16_t call_id, uint16_t carrier) {
    send_setup_enc(s, key, ksg, tn, fn, call_id, carrier, 2);
}

static void
send_setup(session* s, const uint8_t* key, int ksg, uint8_t fn, uint16_t call_id) {
    send_setup_on(s, key, ksg, 1, fn, call_id, CC_CARRIER);
}

static const tetra_call_t*
record(const session* s, uint16_t call_id) {
    return tetra_calls_peek(s->state, call_id);
}

/* The first copy of an encrypted D-SETUP is not acted on; its repeat in another slot, which decrypts to the same
 * plaintext under the loaded SCK, is: the record of the group call, named by the GSSI TA61 gives back, its allocation
 * with its usage marker and air encryption, and the follow of the call, though encrypted calls the listener cannot
 * decrypt are not followed. The key streams are TETRA_crypto's own (k_ks_fn3, k_ks_fn4). */
static void
test_repeat_proves_the_setup(void) {
    session s = session_open();
    load_key(&s, 0, k_sck, 2);
    uint32_t esi = 0;
    tetra_ta61_encrypt(k_sck, GSSI, &esi);
    assert(esi == GESI);

    ttb_block b;
    cipher_span span = build_setup(&b, 2, GESI, 0x0A, 4, 120);
    encrypt_span(&b, span.body, span.end, k_ks_fn3, 0);
    in_slot(&s, 1, 3);
    decode(&s, &b, 0);
    assert(record(&s, 0x0A) == NULL && g_tunes == 0);

    span = build_setup(&b, 2, GESI, 0x0A, 4, 120);
    encrypt_span(&b, span.body, span.end, k_ks_fn4, 0);
    in_slot(&s, 1, 4);
    decode(&s, &b, 0);
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && c->called_ssi == GSSI && (c->flags & TETRA_CALL_F_SETUP_SEEN) != 0u);
    assert(c->alloc.carrier == 120u && c->alloc.timeslots == 4u && c->usage_marker == 42u);
    assert((c->flags & TETRA_CALL_F_USAGE_MARKER) != 0u);
    assert(c->air_enc == 2u && c->air_key.valid && c->air_key.ksg == 1u);
    assert(tetra_air_sig_key_current(s.state, &c->air_key));
    assert(g_tunes == 1 && g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    assert((tetra_air_ksg_seen(s.state, NET_MCC, NET_MNC) & 0x2u) != 0u);

    /* Its speech decrypts under the proving key: TEA2 on the traffic carrier, once its clock and hyperframe are set
     * there (the tune stopped them). */
    tetra_state_t* const ts = tetra_st(s.state);
    ts->hyperframe = NET_HN;
    ts->hyperframe_valid = 1;
    ts->mn = 7;
    ts->tdma_valid = 1;
    ts->tn = 2;
    ts->fn = 9;
    uint8_t bits[64];
    DSD_MEMSET(bits, 0, sizeof bits);
    assert(tetra_air_sig_decrypt_voice(s.opts, s.state, &c->air_key, bits, 64, 0));
    uint8_t ks[8];
    slot_keystream(&s, k_sck, 1, 120, ks, (int)sizeof ks);
    for (int i = 0; i < 64; i++) {
        assert(bits[i] == ((ks[i / 8] >> (7 - (i % 8))) & 1u));
    }
    session_close(&s);
}

/* A key that names no TEA is tried with each of TEA1..TEA3; the one a repeat proves is noted as the network's. A wrong
 * key, or the right key naming the wrong TEA, decrypts noise no repeat proves: no record, no tune, no evidence. */
static void
test_wrong_keys_act_on_nothing(void) {
    session s = session_open();
    load_key(&s, 0, k_sck, 0);
    send_setup(&s, k_sck, 1, 3, 0x0A);
    send_setup(&s, k_sck, 1, 4, 0x0A);
    const tetra_call_t* c = record(&s, 0x0A);
    assert(c != NULL && c->air_key.ksg == 1u && tetra_air_ksg_lookup(s.state, NET_MCC, NET_MNC) == 1);
    session_close(&s);

    for (int variant = 0; variant < 2; variant++) {
        s = session_open();
        load_key(&s, 0, variant == 0 ? k_other_key : k_sck, variant == 0 ? 2u : 1u);
        for (uint8_t fn = 3; fn <= 8; fn++) {
            send_setup(&s, k_sck, 1, fn, 0x0A);
        }
        assert(record(&s, 0x0A) == NULL && g_tunes == 0 && tetra_air_ksg_seen(s.state, NET_MCC, NET_MNC) == 0u);
        session_close(&s);
    }
}

/* A SwMI may run several TEAs (EN 300 392-7 clause 6.3.1): with one SCK, a group under TEA1 and one under TEA2 each
 * prove under their own. Traffic under a key the listener does not hold (a DCK, an MGCK) never proves, however often
 * it repeats. */
static void
test_each_addressee_its_own_key(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    load_key(&s, 0, k_sck, 0);
    send_setup(&s, k_sck, 0, 3, 0x0A);
    send_setup(&s, k_sck, 1, 4, 0x0B);
    send_setup(&s, k_sck, 0, 5, 0x0A);
    send_setup(&s, k_sck, 1, 6, 0x0B);
    assert(record(&s, 0x0A) != NULL && record(&s, 0x0A)->air_key.ksg == 0u);
    assert(record(&s, 0x0B) != NULL && record(&s, 0x0B)->air_key.ksg == 1u);
    for (uint8_t fn = 7; fn <= 12; fn++) {
        send_setup(&s, k_other_key, 1, fn, 0x0C);
    }
    assert(record(&s, 0x0C) == NULL);
    session_close(&s);
}

/* Two encrypted PDUs associated in one full slot each restart the slot's key stream at KSS(0) (EN 300 392-7 figure
 * 6.4), and a PDU in the second half slot starts at KSS(216) (figure 6.5, table 6.4). */
static void
test_key_stream_offsets(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    load_key(&s, 0, k_sck, 2);
    for (uint8_t fn = 3; fn <= 4; fn++) {
        in_slot(&s, 1, fn);
        uint8_t ks[54];
        slot_keystream(&s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
        ttb_block b = {0};
        for (uint16_t id = 0x0A; id <= 0x0B; id++) {
            const int start = ttb_resource(&b, 2, TETRA_MAC_ADDR_SSI, GESI);
            ttb_resource_flags(&b, 0);
            const int body = b.n;
            put_setup_sdu(&b, id);
            encrypt_span(&b, body, b.n, ks, 0);
            ttb_close_resource(&b, start);
        }
        ttb_finish(&b, TTB_SCH_F_BITS, -1);
        decode(&s, &b, 0);
    }
    assert(record(&s, 0x0A) != NULL && record(&s, 0x0B) != NULL);

    for (uint8_t fn = 5; fn <= 6; fn++) {
        in_slot(&s, 2, fn);
        uint8_t ks[54];
        slot_keystream(&s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
        ttb_block b = {0};
        const int start = ttb_resource(&b, 2, TETRA_MAC_ADDR_SSI, GESI);
        ttb_resource_flags(&b, 0);
        const int body = b.n;
        put_setup_sdu(&b, 0x0C);
        encrypt_span(&b, body, b.n, ks, TETRA_AIR_KSS_SECOND_HALF_BLOCK);
        ttb_close_resource(&b, start);
        ttb_finish(&b, TTB_SCH_HD_BITS, -1);
        decode(&s, &b, TETRA_AIR_KSS_SECOND_HALF_BLOCK);
    }
    assert(record(&s, 0x0C) != NULL);
    session_close(&s);
}

/* A TM-SDU sent in fragments (first fragment, MAC-FRAG, MAC-END with the channel allocation) is ciphertext piece by
 * piece, each under its own slot's key stream; the MAC-END's piece is the allocation then the last fragment (EN 300
 * 392-7 clause 6.7.1.2). Rebuilt and repeated, it proves like a whole one. */
static void
test_fragmented_tm_sdu(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    load_key(&s, 0, k_sck, 2);
    ttb_block sdu = {0};
    put_setup_sdu(&sdu, 0x0D);
    for (uint8_t base = 3; base <= 9; base = (uint8_t)(base + 6)) {
        uint8_t ks[54];
        /* The two copies are fragmented apart, the MAC-END and its allocation starting at another bit of the
         * TM-SDU: 20 + 16 bits before it, then 24 + 16. */
        const int first = base == 3 ? 20 : 24;
        const int second = 16;
        /* First fragment: the header and the first bits of the TM-SDU, then fill. */
        in_slot(&s, 1, base);
        slot_keystream(&s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
        ttb_block b = {0};
        const int start = ttb_resource(&b, 2, TETRA_MAC_ADDR_SSI_USAGE, GESI);
        ttb_put(&b, 42, 6);
        ttb_set(&b, start + 7, 63, 6);
        ttb_resource_flags(&b, 0);
        int body = b.n;
        ttb_put_bits(&b, sdu.bits, first);
        encrypt_span(&b, body, b.n, ks, 0);
        ttb_finish(&b, TTB_SCH_HD_BITS, start + 2);
        decode(&s, &b, 0);
        /* MAC-FRAG: the next ones. */
        in_slot(&s, 1, (uint8_t)(base + 1));
        slot_keystream(&s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
        DSD_MEMSET(&b, 0, sizeof b);
        const int fill = ttb_frag(&b);
        body = b.n;
        ttb_put_bits(&b, sdu.bits + first, second);
        encrypt_span(&b, body, b.n, ks, 0);
        ttb_finish(&b, TTB_SCH_HD_BITS, fill);
        decode(&s, &b, 0);
        /* MAC-END: the allocation, then the rest. */
        in_slot(&s, 1, (uint8_t)(base + 2));
        slot_keystream(&s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
        DSD_MEMSET(&b, 0, sizeof b);
        const int end_start = ttb_end(&b);
        ttb_put(&b, 1, 1); /* channel allocation flag */
        body = b.n;
        ttb_allocation(&b, 4, 1, 120);
        ttb_put_bits(&b, sdu.bits + first + second, sdu.n - first - second);
        encrypt_span(&b, body, b.n, ks, 0);
        ttb_close_end(&b, end_start);
        ttb_finish(&b, TTB_SCH_HD_BITS, -1);
        decode(&s, &b, 0);
        if (base == 3) {
            assert(record(&s, 0x0D) == NULL);
        }
    }
    const tetra_call_t* const c = record(&s, 0x0D);
    assert(c != NULL && c->called_ssi == GSSI && c->alloc.carrier == 120u && c->air_enc == 2u);
    assert((c->flags & TETRA_CALL_F_USAGE_MARKER) != 0u && c->usage_marker == 42u);
    session_close(&s);
}

/* A reduced key (the TEA1 register of one channel) decrypts that channel's signalling, but holds no 80-bit key for
 * TA61: the address stays an encrypted short identity, so the D-SETUP names no called party and the call is not
 * followed. */
static void
test_reduced_key_keeps_identities_encrypted(void) {
    session s = session_open();
    dsd_tetra_air_key* const k = &s.state->tetra_air_keys[0];
    k->loaded = 1;
    k->kind = DSD_TETRA_AIR_KEY_TEA1_REDUCED;
    k->mcc = NET_MCC;
    k->mnc = NET_MNC;
    k->la = NET_LA;
    k->cc = NET_CC;
    k->carrier = CC_CARRIER;
    const uint32_t reg = 0x1234ABCDu;
    k->key[0] = 0x12;
    k->key[1] = 0x34;
    k->key[2] = 0xAB;
    k->key[3] = 0xCD;
    for (uint8_t fn = 3; fn <= 4; fn++) {
        in_slot(&s, 1, fn);
        const tetra_state_t* const ts = session_ts(s.state);
        uint8_t ks[54];
        tetra_tea1_keystream_reduced(reg, tetra_tea_build_iv(ts->tn, ts->fn, ts->mn, NET_HN, 0), ks, (int)sizeof ks);
        ttb_block b;
        const cipher_span span = build_setup(&b, 2, GESI, 0x0A, 4, 120);
        encrypt_span(&b, span.body, span.end, ks, 0);
        decode(&s, &b, 0);
    }
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && c->called_ssi == 0u && c->air_key.valid && c->air_key.kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED);
    assert(g_tunes == 0);
    session_close(&s);
}

/* The key a call's signalling proved stops applying when the cell's key may have changed: another SCK-VN or CCK-id,
 * SCKN or GCK support announced, the encryption mode's parity settling on the other value (the address of those PDUs
 * an ESI under the new key the old one cannot resolve), the key edited or unloaded, another cell. */
static void
test_key_epoch(void) {
    for (int change = 0; change < 6; change++) {
        session s = session_open();
        s.opts->trunk_enable = 0;
        load_key(&s, 0, k_sck, 2);
        tetra_cell_security sec;
        DSD_MEMSET(&sec, 0, sizeof sec);
        sec.sckn = 3;
        tetra_air_sig_note_sysinfo(s.state, 1, 0x0102u, &sec);
        send_setup(&s, k_sck, 1, 3, 0x0A);
        send_setup(&s, k_sck, 1, 4, 0x0A);
        const tetra_call_t* const c = record(&s, 0x0A);
        assert(c != NULL && tetra_air_sig_key_current(s.state, &c->air_key) && c->air_key.parity == 0u);
        switch (change) {
            case 0: tetra_air_sig_note_sysinfo(s.state, 1, 0x0103u, NULL); break;
            case 1:
                sec.sckn = 4;
                tetra_air_sig_note_sysinfo(s.state, 0, 0u, &sec);
                break;
            case 2:
                sec.gck_supported = 1;
                tetra_air_sig_note_sysinfo(s.state, 0, 0u, &sec);
                break;
            case 3:
                for (uint8_t fn = 5; fn <= 7; fn++) {
                    in_slot(&s, 1, fn);
                    ttb_block b;
                    (void)build_setup(&b, 3, 0x777777u, 0x2B, 4, 121); /* odd parity, an ESI of the new key */
                    decode(&s, &b, 0);
                }
                break;
            case 4: s.state->tetra_air_keys[0].key[9] ^= 1u; break;
            default: s.state->tetra_air_keys[0].alg = 1; break;
        }
        assert(!tetra_air_sig_key_current(s.state, &c->air_key));
        session_close(&s);
    }
}

/* What a repeat must be to prove: at least TETRA_AIR_PROOF_MIN_BITS encrypted bits, another slot (initial value), and
 * within TETRA_AIR_EVIDENCE_S; a carrier boundary drops the evidence. Interleaved messages of one clear header each
 * find their own earlier copy, and a busy carrier does not push a first copy out of the evidence before its repeat. */
static void
test_repeat_rules(void) {
    session s = session_open();
    load_key(&s, 0, k_sck, 2);
    tetra_state_t* const ts = tetra_st(s.state);
    uint8_t ct[64];
    uint8_t pt[64];
    tetra_air_cand used;
    tetra_air_sig_header h;
    DSD_MEMSET(&h, 0, sizeof h);
    h.kind = TETRA_AIR_SIG_RESOURCE;
    h.addr_type = TETRA_MAC_ADDR_SSI;
    h.addr = GESI;
    h.enc_mode = 2;
    /* Encrypt @p n bits of plaintext pattern @p p in slot (1, fn) and open it. */
#define OPEN_AT(fn, n, p)                                                                                              \
    do {                                                                                                               \
        in_slot(&s, 1, (uint8_t)(fn));                                                                                 \
        tetra_mac_piece piece;                                                                                         \
        assert(tetra_air_sig_slot(s.opts, s.state, 0, &piece));                                                        \
        piece.start = 0;                                                                                               \
        piece.nbits = (uint16_t)(n);                                                                                   \
        uint8_t ks_[54];                                                                                               \
        slot_keystream(&s, k_sck, 1, CC_CARRIER, ks_, (int)sizeof ks_);                                                \
        for (int i_ = 0; i_ < (n); i_++) {                                                                             \
            ct[i_] = (uint8_t)((((p) >> (i_ % 16)) & 1u) ^ ((ks_[i_ / 8] >> (7 - (i_ % 8))) & 1u));                    \
        }                                                                                                              \
        const tetra_air_sig_input in_ = {ct, &piece, NULL, NULL, (n), 1, -1, 0.0};                                     \
        opened = tetra_air_sig_open(s.state, &h, &in_, pt, &used);                                                     \
    } while (0)
    int opened = 0;
    OPEN_AT(3, 40, 0x5A5Au);
    OPEN_AT(4, 40, 0x5A5Au);
    assert(!opened); /* 40 bits prove nothing */
    OPEN_AT(5, 48, 0x1234u);
    assert(!opened);
    OPEN_AT(5, 48, 0x1234u);
    assert(!opened); /* the same slot is the same key stream */
    OPEN_AT(6, 48, 0x1234u);
    assert(opened && used.ksg == 1u);
    for (int i = 0; i < 48; i++) {
        assert(pt[i] == ((0x1234u >> (i % 16)) & 1u));
    }
    OPEN_AT(7, 56, 0x0F0Fu);
    at_seconds(1000.0 + TETRA_AIR_EVIDENCE_S + 0.01);
    OPEN_AT(8, 56, 0x0F0Fu);
    assert(!opened); /* too late */
    OPEN_AT(9, 56, 0x0F0Fu);
    assert(opened);
    tetra_air_sig_forget_carrier(s.state);
    OPEN_AT(10, 56, 0x0F0Fu);
    assert(!opened);

    /* Sixteen messages of one clear header, interleaved twice: each repeat finds its first copy. */
    at_seconds(1010.0);
    for (int round = 0; round < 2; round++) {
        for (uint16_t m = 0; m < 16; m++) {
            OPEN_AT(1 + (round * 16 + m) % 17, 64, (uint16_t)(0x1111u * (m + 1u)));
            assert(opened == (round == 1));
        }
    }
    /* A busy carrier: TETRA_AIR_EVIDENCE_MAX others between a first copy and its repeat 3 frames later. */
    at_seconds(1020.0);
    tetra_air_sig_forget_carrier(s.state);
    int proved = 0;
    for (int i = 0; i < 600; i++) {
        h.addr = 0x100000u + (uint32_t)(i % 300);
        OPEN_AT(1 + (i % 17), 64, (uint16_t)(0xACE1u + (unsigned)(i % 300)));
        proved += opened;
        if (i % 300 == 299) {
            at_seconds(1020.0 + 0.9);
        }
    }
    assert(proved == 300);
#undef OPEN_AT
    DSD_SECURE_ZERO(&used, sizeof used);
    (void)ts;
    session_close(&s);
}

/* A key change the cell announced leaves a followed call's key stale; the call's next proved allocation, under the
 * current key, carries that key in before the call is judged, so it is not left as undecryptable (encrypted calls the
 * listener cannot decrypt not being followed). */
static void
test_proved_key_renews_the_call(void) {
    session s = session_open();
    load_key(&s, 0, k_sck, 2);
    send_setup(&s, k_sck, 1, 3, 0x0A);
    send_setup(&s, k_sck, 1, 4, 0x0A);
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && g_tunes == 1 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->hyperframe = NET_HN;
    ts->hyperframe_valid = 1;
    tetra_air_sig_note_sysinfo(s.state, 1, 0x0100u, NULL);
    tetra_air_sig_note_sysinfo(s.state, 1, 0x0102u, NULL); /* another key version, the same key still loaded */
    assert(!tetra_air_sig_key_current(s.state, &c->air_key));
    send_setup_on(&s, k_sck, 1, 2, 5, 0x0A, 120); /* on its traffic carrier */
    send_setup_on(&s, k_sck, 1, 2, 6, 0x0A, 120);
    assert(record(&s, 0x0A) == c && tetra_air_sig_key_current(s.state, &c->air_key));
    assert(g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    session_close(&s);
}

/* A reduced key decrypts its own channel only: an allocation it proved that moves the call to another carrier leaves
 * the call undecryptable there, so with encrypted calls the listener cannot decrypt not followed, it is not. */
static void
test_reduced_key_off_its_channel(void) {
    session s = session_open();
    ttb_block b = {0};
    int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, GSSI);
    ttb_resource_flags(&b, 0);
    put_setup_sdu(&b, 0x0A);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    in_slot(&s, 1, 2);
    decode(&s, &b, 0); /* the call's clear D-SETUP names its group */
    assert(record(&s, 0x0A) != NULL && record(&s, 0x0A)->called_ssi == GSSI && g_tunes == 0);
    dsd_tetra_air_key* const k = &s.state->tetra_air_keys[0];
    k->loaded = 1;
    k->kind = DSD_TETRA_AIR_KEY_TEA1_REDUCED;
    k->mcc = NET_MCC;
    k->mnc = NET_MNC;
    k->la = NET_LA;
    k->cc = NET_CC;
    k->carrier = CC_CARRIER;
    k->key[0] = 0x12;
    for (uint8_t fn = 3; fn <= 4; fn++) {
        in_slot(&s, 1, fn);
        const tetra_state_t* const ts = session_ts(s.state);
        uint8_t ks[54];
        tetra_tea1_keystream_reduced(0x12000000u, tetra_tea_build_iv(ts->tn, ts->fn, ts->mn, NET_HN, 0), ks,
                                     (int)sizeof ks);
        const cipher_span span = build_setup(&b, 2, GESI, 0x0A, 4, 120);
        encrypt_span(&b, span.body, span.end, ks, 0);
        decode(&s, &b, 0);
    }
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && c->alloc.carrier == 120u && c->air_key.valid && g_tunes == 0);
    session_close(&s);
}

/* With the network's 80-bit key and the reduced key of the same key stream both loaded, the 80-bit key proves the
 * D-SETUP whichever slot holds it, so TA61 names the called party and the call is followed. */
static void
test_full_key_proves_before_its_reduced_key(void) {
    session s = session_open();
    uint8_t eck[10];
    tetra_tea_derive_eck(CC_CARRIER, NET_LA, NET_CC, k_sck, eck);
    const uint32_t reg = tetra_tea1_reduce_key(eck);
    DSD_SECURE_ZERO(eck, sizeof eck);
    dsd_tetra_air_key* const k = &s.state->tetra_air_keys[0];
    k->loaded = 1;
    k->kind = DSD_TETRA_AIR_KEY_TEA1_REDUCED;
    k->mcc = NET_MCC;
    k->mnc = NET_MNC;
    k->la = NET_LA;
    k->cc = NET_CC;
    k->carrier = CC_CARRIER;
    k->key[0] = (uint8_t)(reg >> 24);
    k->key[1] = (uint8_t)(reg >> 16);
    k->key[2] = (uint8_t)(reg >> 8);
    k->key[3] = (uint8_t)reg;
    load_key(&s, 1, k_sck, 1); /* TEA1 */
    send_setup(&s, k_sck, 0, 3, 0x0A);
    send_setup(&s, k_sck, 0, 4, 0x0A);
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && c->called_ssi == GSSI && c->air_key.valid && c->air_key.kind == DSD_TETRA_AIR_KEY_CK);
    assert(g_tunes == 1);
    session_close(&s);
}

/* A reallocation of the followed call to the channel of a reduced key, proved under that key, is judged on that
 * channel: the call followed clear on carrier 120, its record moved to 121 by an allocation with no call PDU while the
 * receiver stayed, then its D-SETUP under carrier 120's reduced key moving it back; it stays followed. */
static void
test_reduced_key_reallocation_judged_on_its_channel(void) {
    session s = session_open();
    ttb_block b;
    (void)build_setup(&b, 0, GSSI, 0x0A, 4, 120);
    in_slot(&s, 1, 2);
    decode(&s, &b, 0);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_tunes == 1);
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(120);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->hyperframe = NET_HN; /* the traffic carrier's SYNC */
    ts->hyperframe_valid = 1;
    DSD_MEMSET(&b, 0, sizeof b);
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, GSSI);
    ttb_resource_flags(&b, 1);
    ttb_allocation(&b, 4, 1, 121);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    in_slot(&s, 2, 3);
    decode(&s, &b, 0);
    assert(record(&s, 0x0A)->alloc.carrier == 121u && g_tunes == 1);
    dsd_tetra_air_key* const k = &s.state->tetra_air_keys[0];
    k->loaded = 1;
    k->kind = DSD_TETRA_AIR_KEY_TEA1_REDUCED;
    k->mcc = NET_MCC;
    k->mnc = NET_MNC;
    k->la = NET_LA;
    k->cc = NET_CC;
    k->carrier = 120;
    k->key[0] = 0x12;
    for (uint8_t fn = 4; fn <= 5; fn++) {
        in_slot(&s, 2, fn);
        uint8_t ks[54];
        tetra_tea1_keystream_reduced(0x12000000u, tetra_tea_build_iv(ts->tn, ts->fn, ts->mn, NET_HN, 0), ks,
                                     (int)sizeof ks);
        const cipher_span span = build_setup(&b, 2, GESI, 0x0A, 4, 120);
        encrypt_span(&b, span.body, span.end, ks, 0);
        decode(&s, &b, 0);
    }
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && c->alloc.carrier == 120u && c->air_key.valid && tetra_calls_air_key_applies(s.state, c));
    assert(g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    session_close(&s);
}

/* TN1 of frame @p fn of multiframe @p mn, the decode clock at its time (a TDMA frame is 56.67 ms) from multiframe 1. */
static void
in_frame(session* s, uint8_t fn, uint8_t mn) {
    in_slot_of(s, 1, fn, mn);
    at_seconds(1000.0 + (double)(((uint32_t)mn - 1u) * 18u + (uint32_t)(fn - 1u)) * 0.05667);
}

static void
next_frame(uint8_t* fn, uint8_t* mn) {
    if (++*fn > 18u) {
        *fn = 1;
        ++*mn;
    }
}

/* One copy of the encrypted TM-SDU @p sdu (@p sdu_bits) to GESI on the control channel, fragmented from TN1 of frame
 * @p fn of multiframe @p mn on, one fragment a frame as the clock runs: a MAC-RESOURCE filling its block, MAC-FRAGs of
 * at most @p frag_bits, and a MAC-END with a channel allocation of TN3 on carrier 121. @p fn and @p mn are left at the
 * frame after the MAC-END. Returns the fragments sent, MAC-RESOURCE and MAC-END included. */
static int
send_fragmented(session* s, const uint8_t* sdu, int sdu_bits, int frag_bits, uint8_t* fn, uint8_t* mn) {
    ttb_block b = {0};
    uint8_t ks[54];
    int sent = 0;
    int pieces = 2;
    in_frame(s, *fn, *mn);
    slot_keystream(s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
    const int start = ttb_resource(&b, 2, TETRA_MAC_ADDR_SSI, GESI);
    ttb_set(&b, start + 7, 63, 6);
    ttb_resource_flags(&b, 0);
    int body = b.n;
    const int head = TTB_SCH_F_BITS - b.n - 1;
    ttb_put_bits(&b, sdu, head);
    sent += head;
    encrypt_span(&b, body, b.n, ks, 0);
    ttb_finish(&b, TTB_SCH_F_BITS, start + 2);
    decode(s, &b, 0);
    while (sdu_bits - sent > 200) {
        next_frame(fn, mn);
        in_frame(s, *fn, *mn);
        slot_keystream(s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
        DSD_MEMSET(&b, 0, sizeof b);
        const int fill = ttb_frag(&b);
        body = b.n;
        int n = TTB_SCH_F_BITS - b.n - 1;
        n = n < frag_bits ? n : frag_bits;
        ttb_put_bits(&b, sdu + sent, n);
        sent += n;
        pieces++;
        encrypt_span(&b, body, b.n, ks, 0);
        ttb_finish(&b, TTB_SCH_F_BITS, fill);
        decode(s, &b, 0);
    }
    next_frame(fn, mn);
    in_frame(s, *fn, *mn);
    slot_keystream(s, k_sck, 1, CC_CARRIER, ks, (int)sizeof ks);
    DSD_MEMSET(&b, 0, sizeof b);
    const int end_start = ttb_end(&b);
    ttb_put(&b, 1, 1);
    body = b.n;
    ttb_allocation(&b, 2, 1, 121);
    ttb_put_bits(&b, sdu + sent, sdu_bits - sent);
    encrypt_span(&b, body, b.n, ks, 0);
    ttb_close_end(&b, end_start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    decode(s, &b, 0);
    next_frame(fn, mn);
    return pieces;
}

/* The longest TM-SDU (N.202, 2632 bits) sent encrypted in fragments with a channel allocation in its MAC-END: the
 * ciphertext pieces hold the allocation too until the plaintext is rebuilt, and the allocation attaches to the call of
 * the party the TM-SDU addresses. Fragments cut short (associated PDUs ahead of each MAC-FRAG leave it less of its
 * block) are as many pieces: 64-bit ones take more than 32, and more than two seconds to arrive, so the first copy is
 * kept for its repeat as much longer. */
static void
test_longest_fragmented_tm_sdu(void) {
    static uint8_t sdu[TETRA_MAC_TM_SDU_MAX_BITS];
    DSD_MEMSET(sdu, 0, sizeof sdu);
    sdu[0] = 1; /* LLC PDU type 1000: an advanced link PDU, not decoded */
    for (int frag_bits = 64; frag_bits <= TTB_SCH_F_BITS; frag_bits += TTB_SCH_F_BITS - 64) {
        session s = session_open();
        s.opts->trunk_enable = 0;
        ttb_block b = {0};
        const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, GSSI);
        ttb_resource_flags(&b, 0);
        put_setup_sdu(&b, 0x0A);
        ttb_close_resource(&b, start);
        ttb_finish(&b, TTB_SCH_F_BITS, -1);
        in_slot(&s, 1, 2);
        decode(&s, &b, 0);
        load_key(&s, 0, k_sck, 2);
        uint8_t fn = 1;
        uint8_t mn = 8;
        const double first_m = 1000.0 + 7.0 * 18.0 * 0.05667;
        const int pieces = send_fragmented(&s, sdu, TETRA_MAC_TM_SDU_MAX_BITS, frag_bits, &fn, &mn);
        assert(frag_bits == TTB_SCH_F_BITS || (pieces > 32 && dsd_decode_now_mono_s() - first_m > 2.0));
        assert(record(&s, 0x0A)->alloc.carrier != 121u);
        (void)send_fragmented(&s, sdu, TETRA_MAC_TM_SDU_MAX_BITS, frag_bits, &fn, &mn);
        const tetra_call_t* const c = record(&s, 0x0A);
        assert(c != NULL && c->alloc.carrier == 121u && c->alloc.timeslots == 2u && c->air_enc == 2u);
        session_close(&s);
    }
}

/* A clear reallocation of a followed call whose key a key change left stale is clear: the call is judged clear from
 * it, not by the stale key, so it is not left as undecryptable. */
static void
test_clear_reallocation_after_a_key_change(void) {
    session s = session_open();
    load_key(&s, 0, k_sck, 2);
    send_setup(&s, k_sck, 1, 3, 0x0A);
    send_setup(&s, k_sck, 1, 4, 0x0A);
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    tetra_air_sig_note_sysinfo(s.state, 1, 0x0100u, NULL);
    tetra_air_sig_note_sysinfo(s.state, 1, 0x0102u, NULL);
    ttb_block b;
    (void)build_setup(&b, 0, GSSI, 0x0A, 4, 120); /* clear */
    in_slot(&s, 2, 7);
    decode(&s, &b, 0);
    assert(c->air_enc == 0u && !c->air_key.valid && g_returns == 0 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    session_close(&s);
}

/* A call's proved key applies to the key version its allocation named: an encrypted allocation of the other parity,
 * which no key decrypted, leaves the key, of the old version, applying to nothing. And a key whose fingerprint would be
 * the "decrypted nothing" mark still proves. */
static void
test_key_version_of_the_call(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    load_key(&s, 0, k_sck, 2);
    send_setup(&s, k_sck, 1, 3, 0x0A);
    send_setup(&s, k_sck, 1, 4, 0x0A);
    const tetra_call_t* const c = record(&s, 0x0A);
    assert(c != NULL && tetra_calls_air_key_applies(s.state, c));
    tetra_calls_on_addressed(s.opts, s.state, &(tetra_mac_address){.type = TETRA_MAC_ADDR_SSI, .ssi = GSSI}, 3, 1, 1);
    assert(c->air_enc == 3u && !tetra_calls_air_key_applies(s.state, c));
    session_close(&s);

    /* A key version change: the cell's settled parity 0, then a D-SETUP under parity 1 proved by its repeat before a
     * run of parity 1 headers settles it. The proof settles it: the key is current, and the headers after it leave it
     * so. */
    s = session_open();
    s.opts->trunk_enable = 0;
    load_key(&s, 0, k_sck, 2);
    for (int i = 0; i < 3; i++) {
        tetra_air_sig_note_parity(s.state, 2);
    }
    send_setup_enc(&s, k_sck, 1, 1, 3, 0x0A, CC_CARRIER, 3);
    send_setup_enc(&s, k_sck, 1, 1, 4, 0x0A, CC_CARRIER, 3);
    const tetra_call_t* const changed = record(&s, 0x0A);
    assert(changed != NULL && changed->air_key.valid && tetra_calls_air_key_applies(s.state, changed));
    tetra_air_sig_note_parity(s.state, 3);
    tetra_air_sig_note_parity(s.state, 3);
    assert(tetra_calls_air_key_applies(s.state, changed));
    tetra_air_sig_note_parity(s.state, 2);
    assert(tetra_calls_air_key_applies(s.state, changed));
    session_close(&s);

    static const uint8_t k_zero_fingerprint[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98, 0x50, 0x82, 0x02};
    s = session_open();
    s.opts->trunk_enable = 0;
    load_key(&s, 0, k_zero_fingerprint, 2);
    send_setup(&s, k_zero_fingerprint, 1, 3, 0x0A);
    send_setup(&s, k_zero_fingerprint, 1, 4, 0x0A);
    assert(record(&s, 0x0A) != NULL);
    session_close(&s);
}

int
main(void) {
    test_repeat_proves_the_setup();
    test_wrong_keys_act_on_nothing();
    test_each_addressee_its_own_key();
    test_key_stream_offsets();
    test_fragmented_tm_sdu();
    test_reduced_key_keeps_identities_encrypted();
    test_key_epoch();
    test_repeat_rules();
    test_proved_key_renews_the_call();
    test_reduced_key_off_its_channel();
    test_reduced_key_reallocation_judged_on_its_channel();
    test_full_key_proves_before_its_reduced_key();
    test_longest_fragmented_tm_sdu();
    test_clear_reallocation_after_a_key_change();
    test_key_version_of_the_call();
    DSD_FPRINTF(stdout, "TETRA air signalling: all checks passed\n");
    return 0;
}
