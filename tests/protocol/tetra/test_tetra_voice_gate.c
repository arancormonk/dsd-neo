// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The gates a TETRA speech frame passes: the followed traffic channel, the allocated timeslot (by the tracked TDMA
 * timeslot, TN1 in bit 3 of the allocation's bitmap), the encryption class and the talkgroup.
 */
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tetra_state.h"
#include "tetra_voice_gate.h"

static int g_fail = 0;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void
test_channel_gate(dsd_opts* opts) {
    expect(tetra_voice_channel_gate_passes(opts), "conventional monitoring passes speech");
    opts->trunk_enable = 1;
    expect(!tetra_voice_channel_gate_passes(opts), "the trunking control channel does not");
    opts->trunk_is_tuned = 1;
    expect(tetra_voice_channel_gate_passes(opts), "the acquired traffic channel does");
    expect(tetra_voice_channel_gate_passes(NULL), "no options pass");
}

static void
test_slot_gate(dsd_state* st) {
    tetra_state_t* ts = tetra_st(st);
    expect(tetra_voice_slot_gate_passes(st), "no allocation passes every timeslot");
    ts->vc_assignment_valid = 1;
    ts->tdma_valid = 0;
    ts->vc_timeslot_bitmap = 4;
    expect(tetra_voice_slot_gate_passes(st), "an allocation without TDMA timing passes");
    ts->tdma_valid = 1;
    for (unsigned tn = 1; tn <= 4; ++tn) {
        ts->tn = (uint8_t)tn;
        for (unsigned assigned = 1; assigned <= 4; ++assigned) {
            ts->vc_timeslot_bitmap = (uint8_t)(1u << (4u - assigned));
            char label[64];
            DSD_SNPRINTF(label, sizeof(label), "TN%u under an allocation of TN%u", tn, assigned);
            expect(tetra_voice_slot_gate_passes(st) == (tn == assigned), label);
        }
    }
    ts->vc_timeslot_bitmap = 0;
    expect(!tetra_voice_slot_gate_passes(st), "an allocation to the control channel (no timeslot) passes none");
    expect(tetra_voice_slot_gate_passes(NULL), "no state passes");
}

static void
test_crypto_class(dsd_state* st) {
    tetra_state_t* ts = tetra_st(st);
    ts->enc_mode = TETRA_ENC_MODE_NONE;
    expect(tetra_voice_crypto_class(st, 0) == DSD_VOICE_CRYPTO_CLEAR, "not encrypted is clear");
    expect(tetra_voice_crypto_class(st, 1) == DSD_VOICE_CRYPTO_CLEAR, "a stray keystream does not make it encrypted");
    for (uint8_t mode = TETRA_ENC_MODE_ON; mode <= TETRA_ENC_MODE_RSVD; mode++) {
        ts->enc_mode = mode;
        expect(tetra_voice_crypto_class(st, 1) == DSD_VOICE_CRYPTO_DECRYPTABLE, "encrypted and decrypted");
        expect(tetra_voice_crypto_class(st, 0) == DSD_VOICE_CRYPTO_UNDECRYPTABLE, "encrypted, no keystream");
    }
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    expect(tetra_voice_crypto_class(st, 0) == DSD_VOICE_CRYPTO_CLEAR, "clear direct mode, whatever trunked mode said");
    ts->dmo_crypt = TETRA_DMO_CRYPT_AIR;
    expect(tetra_voice_crypto_class(st, 1) == DSD_VOICE_CRYPTO_UNDECRYPTABLE,
           "direct mode air encryption is undecryptable");
    ts->dmo_crypt = TETRA_DMO_CRYPT_E2E;
    expect(tetra_voice_crypto_class(st, 1) == DSD_VOICE_CRYPTO_UNDECRYPTABLE,
           "direct mode end-to-end encryption is undecryptable");
    ts->dmo_crypt = TETRA_DMO_CRYPT_UNKNOWN;
    expect(tetra_voice_crypto_class(st, 0) == DSD_VOICE_CRYPTO_UNDECRYPTABLE,
           "direct mode speech before its security class is known is not taken as clear");
    expect(tetra_voice_crypto_class(NULL, 0) == DSD_VOICE_CRYPTO_CLEAR, "no state is clear");
}

static void
test_talkgroup(dsd_state* st) {
    tetra_state_t* ts = tetra_st(st);
    ts->dmo = 0;
    ts->gssi = 0x123456u;
    expect(tetra_voice_talkgroup(st) == 0x123456ul, "trunked speech is judged by the call's GSSI");
    ts->dmo = 1;
    expect(tetra_voice_talkgroup(st) == 0ul, "direct mode has no group address yet");
    expect(tetra_voice_talkgroup(NULL) == 0ul, "no state has none");
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!opts || !st) {
        free(opts);
        free(st);
        return 1;
    }
    test_channel_gate(opts);
    test_slot_gate(st);
    test_crypto_class(st);
    test_talkgroup(st);
    dsd_state_ext_free_all(st);
    free(st);
    free(opts);
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_voice_gate");
    return 0;
}
