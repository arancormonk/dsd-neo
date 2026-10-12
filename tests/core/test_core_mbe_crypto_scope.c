// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #656: each protocol's voice gets only its own protocol's crypto transforms, and every keystream read stays
 * inside the keystream generated for it. Drives the real vocoder and reads back the bits it hands to the synthesizer
 * from the frame log (the same bits -Z prints), comparing keyed runs with an unkeyed baseline and with independently
 * generated reference keystreams. Every "untouched" check has a positive control that shows the same setting does
 * transform the protocol it belongs to.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/des.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <mbelib-neo/mbelib.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#define TRACE_MAX_FRAMES 512
#define FIXTURE_FRAMES   24

typedef struct {
    int frames;
    int exhausted;
    char data[TRACE_MAX_FRAMES][24];
} frame_trace;

static dsd_opts g_opts;
static dsd_state g_state;
static mbe_parms g_cur, g_prev, g_prev_enh, g_cur2, g_prev2, g_prev_enh2;
static char g_trace_path[DSD_TEST_PATH_MAX];
static frame_trace g_trace;
static frame_trace g_base;

/* AMBE+2 frames that decode and that the DMR stream ciphers do not skip as silence, and the 49 bits each decodes to. */
static char g_ambe_fr[FIXTURE_FRAMES][4][24];
static char g_ambe_d[FIXTURE_FRAMES][49];
/* IMBE 7200 frames and the 88 bits each decodes to. */
static char g_imbe_fr[FIXTURE_FRAMES][8][23];
static char g_imbe_d[FIXTURE_FRAMES][88];

static int
expect_int(const char* tag, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
reset_state(int synctype, int slot) {
    dsd_state_ext_free_all(&g_state);
    free(g_state.rc2_context);
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    mbe_initMbeParms(&g_cur, &g_prev, &g_prev_enh);
    mbe_initMbeParms(&g_cur2, &g_prev2, &g_prev_enh2);
    g_state.cur_mp = &g_cur;
    g_state.prev_mp = &g_prev;
    g_state.prev_mp_enhanced = &g_prev_enh;
    g_state.cur_mp2 = &g_cur2;
    g_state.prev_mp2 = &g_prev2;
    g_state.prev_mp_enhanced2 = &g_prev_enh2;
    g_state.synctype = synctype;
    g_state.currentslot = slot;
    g_state.p25_crypto_state[0] = DSD_P25_CRYPTO_DECRYPTABLE;
    g_state.p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    g_opts.floating_point = 1;
    g_opts.audio_gain = 1.0f;
    g_opts.audio_gainR = 1.0f;
    // Both output formats run the same voice chain and short staging (issues #652/#653).
    static short out_l[100 + (160 * TRACE_MAX_FRAMES)];
    static short out_r[100 + (160 * TRACE_MAX_FRAMES)];
    g_state.audio_out_buf = out_l;
    g_state.audio_out_buf_p = out_l + 100;
    g_state.audio_out_bufR = out_r;
    g_state.audio_out_buf_pR = out_r + 100;
    g_state.aout_max_buf_p = g_state.aout_max_buf;
    g_state.aout_max_buf_pR = g_state.aout_max_bufR;
    g_state.aout_gain = 1.0f;
    g_state.aout_gainR = 1.0f;
}

static int
trace_begin(void) {
    int fd = dsd_test_mkstemp(g_trace_path, sizeof(g_trace_path), "dsdneo_crypto_scope");
    if (fd < 0) {
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(g_opts.frame_log_file, sizeof(g_opts.frame_log_file), "%s", g_trace_path);
    return 0;
}

/* The bits of every AMBE/IMBE frame the vocoder synthesized, in order, and the frames it muted for keystream. */
static int
trace_end(frame_trace* trace) {
    dsd_frame_log_close(&g_opts);
    g_opts.frame_log_file[0] = '\0';
    DSD_MEMSET(trace, 0, sizeof(*trace));
    FILE* fp = dsd_fopen_existing_regular_file(g_trace_path, "r");
    if (fp == NULL) {
        (void)remove(g_trace_path);
        return 1;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "type=KEYSTREAM_EXHAUSTED") != NULL) {
            trace->exhausted++;
            continue;
        }
        if (strstr(line, "FRAME AMBE ") == NULL && strstr(line, "FRAME IMBE ") == NULL) {
            continue;
        }
        const char* data = strstr(line, "data=");
        if (data == NULL || trace->frames >= TRACE_MAX_FRAMES) {
            continue;
        }
        data += 5;
        size_t n = strcspn(data, " \n");
        if (n >= sizeof(trace->data[0])) {
            n = sizeof(trace->data[0]) - 1U;
        }
        DSD_MEMCPY(trace->data[trace->frames], data, n);
        trace->data[trace->frames][n] = '\0';
        trace->frames++;
    }
    (void)fclose(fp);
    (void)remove(g_trace_path);
    return 0;
}

static void
ambe_hex(const char bits[49], char out[24]) {
    unsigned long long v = 0ULL;
    for (int i = 0; i < 49; i++) {
        v = (v << 1) | (unsigned long long)(bits[i] & 1);
    }
    DSD_SNPRINTF(out, 24, "%014llX", v << 7);
}

static void
imbe_hex(const char bits[88], char out[24]) {
    size_t off = 0;
    for (int i = 0; i < 11; i++) {
        unsigned v = 0;
        for (int b = 0; b < 8; b++) {
            v = (v << 1) | (unsigned)(bits[(i * 8) + b] & 1);
        }
        DSD_SNPRINTF(out + off, 24 - off, "%02X", v);
        off += 2;
    }
}

static int
build_fixtures(void) {
    int have = 0;
    for (int seed = 1; seed < 4000 && have < FIXTURE_FRAMES; seed++) {
        char fr[4][24];
        char d[49];
        DSD_MEMSET(fr, 0, sizeof(fr));
        for (int k = 0; k < 6; k++) {
            fr[(seed + k) % 4][(seed * (5 + (2 * k)) + (3 * k) + 1) % 24] = 1;
        }
        mbe_process_result result;
        // Few corrections: DMR RC4 passes a frame with 3 or more C0 errors through undecrypted.
        if (mbe_decodeAmbe3600x2450Frame((const char (*)[24])fr, d, &result) < 0 || result.total_errors > 2
            || dmr_ambe49_should_skip_crypto(d) == 1) {
            continue;
        }
        DSD_MEMCPY(g_ambe_fr[have], fr, sizeof(fr));
        DSD_MEMCPY(g_ambe_d[have], d, sizeof(d));
        have++;
    }
    if (have != FIXTURE_FRAMES) {
        DSD_FPRINTF(stderr, "fixture: only %d AMBE frames\n", have);
        return 1;
    }
    for (int f = 0; f < FIXTURE_FRAMES; f++) {
        DSD_MEMSET(g_imbe_fr[f], 0, sizeof(g_imbe_fr[f]));
        g_imbe_fr[f][0][(f * 3 + 1) % 23] = 1;
        g_imbe_fr[f][2][(f * 5 + 7) % 23] = 1;
        g_imbe_fr[f][5][(f * 7 + 2) % 23] = 1;
        g_imbe_fr[f][7][f % 7] = 1;
        mbe_process_result result;
        if (mbe_decodeImbe7200x4400Frame((const char (*)[23])g_imbe_fr[f], g_imbe_d[f], &result) < 0) {
            DSD_FPRINTF(stderr, "fixture: IMBE frame %d does not decode\n", f);
            return 1;
        }
    }
    return 0;
}

static void
run_ambe(dsd_voice_protocol owner, int frames) {
    for (int f = 0; f < frames; f++) {
        processMbeFrame(&g_opts, &g_state, owner, NULL, g_ambe_fr[f % FIXTURE_FRAMES], NULL);
    }
}

static int
traces_equal(const frame_trace* a, const frame_trace* b) {
    if (a->frames != b->frames || a->frames == 0) {
        return 0;
    }
    for (int i = 0; i < a->frames; i++) {
        if (strcmp(a->data[i], b->data[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

static int
traces_all_differ(const frame_trace* a, const frame_trace* b) {
    if (a->frames != b->frames || a->frames == 0) {
        return 0;
    }
    for (int i = 0; i < a->frames; i++) {
        if (strcmp(a->data[i], b->data[i]) == 0) {
            return 0;
        }
    }
    return 1;
}

typedef void (*key_setup_fn)(dsd_state* state);

/* Baseline: the same frames through the same owner, with no key material. */
static int
capture_ambe(int synctype, int slot, dsd_voice_protocol owner, key_setup_fn setup, int frames, frame_trace* out) {
    reset_state(synctype, slot);
    if (setup != NULL) {
        setup(&g_state);
    }
    if (trace_begin() != 0) {
        return 1;
    }
    run_ambe(owner, frames);
    return trace_end(out);
}

/* ---- key settings that belong to one protocol ---- */

static void
key_forced_scrambler(dsd_state* s) {
    s->M = 1;
    s->R = 0x1234ULL;
}

static void
key_nxdn_cipher1(dsd_state* s) {
    s->nxdn_cipher_type = 0x01;
    s->R = 0x1234ULL;
}

static void
key_nxdn_cipher2(dsd_state* s) {
    s->nxdn_cipher_type = 0x02;
    s->nxdn_new_iv = 1;
    s->payload_miN = 0x0102030405060708ULL;
    s->R = 0x0123456789ABCDULL;
}

static void
key_nxdn_cipher3(dsd_state* s) {
    s->nxdn_cipher_type = 0x03;
    s->nxdn_new_iv = 1;
    s->aes_key_loaded[0] = 1;
    for (int i = 0; i < 32; i++) {
        s->aes_key[i] = (uint8_t)(0x11U * (unsigned)(i + 1));
    }
    for (int i = 0; i < 16; i++) {
        s->aes_iv[i] = (uint8_t)(0xA0U + (unsigned)i);
    }
}

static void
key_forced_basic_privacy(dsd_state* s) {
    s->M = 1;
    s->K = 1ULL;
}

static void
key_forced_hytera(dsd_state* s) {
    s->M = 1;
    s->K1 = s->H = 0x0123456789ULL;
    s->hytera_key_segments = 1U;
}

static void
key_tg_autoload(dsd_state* s) {
    // No call target: the autoload hashes target 0. Service options say "encrypted, no key id, Motorola".
    s->rkey_array[0] = 0x55ULL;
    s->dmr_so = s->dmr_soR = 0x40;
    s->dmr_fid = s->dmr_fidR = 0x10;
}

static void
key_retevis(dsd_state* s) {
    retevis_rc2_keystream_creation(s, "00112233445566778899AABBCCDDEEFF", 0);
}

static void
key_tyt_ap(dsd_state* s) {
    tyt_ap_pc4_keystream_creation(s, "00112233445566778899AABBCCDDEEFF", 0);
}

static void
key_baofeng(dsd_state* s) {
    (void)baofeng_ap_pc5_keystream_creation(s, "00112233445566778899AABBCCDDEEFF", 0);
}

static void
key_tyt_ep(dsd_state* s) {
    tyt_ep_aes_keystream_creation(s, "0123456789ABCDEF 0011223344556677", 0);
}

static void
key_kenwood(dsd_state* s) {
    char input[] = "1234";
    ken_dmr_scrambler_keystream_creation(s, input, 0);
}

static void
key_anytone(dsd_state* s) {
    char input[] = "1234";
    anytone_bp_keystream_creation(s, input, 0);
}

static void
load_aes_slot(dsd_state* s, int slot) {
    s->A1[slot] = 0x0011223344556677ULL;
    s->A2[slot] = 0x8899AABBCCDDEEFFULL;
    s->A3[slot] = 0x1021324354657687ULL;
    s->A4[slot] = 0x98A9BACBDCEDFE0FULL;
    s->aes_key_loaded[slot] = 1;
    s->aes_key_segments[slot] = 4U;
    uint8_t* iv = (slot == 0) ? s->aes_iv : s->aes_ivR;
    for (int i = 0; i < 16; i++) {
        iv[i] = (uint8_t)(0x30U + (unsigned)(i * 7));
    }
}

static void
key_dmr_rc4(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0x21;
    s->R = s->RR = 0x0102030405ULL;
    s->payload_mi = s->payload_miR = 0x11223344ULL;
}

static void
key_dmr_des(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0x22;
    s->R = s->RR = 0x0123456789ABCDULL;
    s->payload_miP = s->payload_miN = 0x1122334455667788ULL;
}

static void
key_dmr_aes(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0x24;
    load_aes_slot(s, 0);
    load_aes_slot(s, 1);
}

static void
key_p25_des(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0x81;
    s->R = s->RR = 0x0123456789ABCDULL;
    s->payload_miP = s->payload_miN = 0x1122334455667788ULL;
}

static void
key_p25_aes256(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0x84;
    load_aes_slot(s, 0);
    load_aes_slot(s, 1);
}

static void
key_p25_aes128(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0x89;
    load_aes_slot(s, 0);
    load_aes_slot(s, 1);
}

static void
key_p25_rc4(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0xAA;
    s->R = s->RR = 0x0102030405ULL;
    s->payload_miP = s->payload_miN = 0x1122334455667788ULL;
    s->dropL = s->dropR = 256;
}

typedef struct {
    const char* name;
    key_setup_fn setup;
} key_case;

/* Item 1: YSF V/D Type 1 decodes through the NXDN decoder but has no cipher. */
static int
test_ysf_ehr_gets_no_nxdn_cipher(void) {
    static const key_case cases[] = {
        {"forced scrambler", key_forced_scrambler},
        {"nxdn cipher 1", key_nxdn_cipher1},
        {"nxdn cipher 2", key_nxdn_cipher2},
        {"nxdn cipher 3", key_nxdn_cipher3},
    };
    int rc = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        rc |= capture_ambe(DSD_SYNC_NXDN_POS, 0, DSD_VOICE_PROTOCOL_YSF, NULL, 8, &g_base);
        rc |= capture_ambe(DSD_SYNC_NXDN_POS, 0, DSD_VOICE_PROTOCOL_YSF, cases[c].setup, 8, &g_trace);
        if (!traces_equal(&g_base, &g_trace)) {
            DSD_FPRINTF(stderr, "ysf ehr: %s changed clear YSF voice\n", cases[c].name);
            rc = 1;
        }
        // Positive control: the same setting does transform NXDN.
        rc |= capture_ambe(DSD_SYNC_NXDN_POS, 0, DSD_VOICE_PROTOCOL_NXDN, NULL, 8, &g_base);
        rc |= capture_ambe(DSD_SYNC_NXDN_POS, 0, DSD_VOICE_PROTOCOL_NXDN, cases[c].setup, 8, &g_trace);
        if (!traces_all_differ(&g_base, &g_trace)) {
            DSD_FPRINTF(stderr, "ysf ehr control: %s left NXDN voice untouched\n", cases[c].name);
            rc = 1;
        }
    }

    // The scrambler state stays NXDN's: YSF voice does not seed or advance the NXDN MI.
    reset_state(DSD_SYNC_NXDN_POS, 0);
    key_forced_scrambler(&g_state);
    run_ambe(DSD_VOICE_PROTOCOL_YSF, 4);
    rc |= expect_int("ysf ehr leaves the nxdn mi alone", (long long)g_state.payload_miN, 0);

    // Scrambled dPMR halves borrow the same decoder and keep their scrambler.
    rc |= capture_ambe(DSD_SYNC_NXDN_POS, 0, DSD_VOICE_PROTOCOL_DPMR, NULL, 8, &g_base);
    rc |= capture_ambe(DSD_SYNC_NXDN_POS, 0, DSD_VOICE_PROTOCOL_DPMR, key_nxdn_cipher1, 8, &g_trace);
    if (!traces_all_differ(&g_base, &g_trace)) {
        DSD_FPRINTF(stderr, "scrambled dpmr lost its scrambler\n");
        rc = 1;
    }
    return rc;
}

/* Item 2: clear dPMR and P25 Phase 2 share the slot path with DMR but none of DMR's transforms. */
static int
test_slot_path_dmr_transforms_stay_on_dmr(void) {
    static const key_case cases[] = {
        {"forced basic privacy", key_forced_basic_privacy},
        {"forced hytera", key_forced_hytera},
        {"tg key autoload", key_tg_autoload},
        {"retevis", key_retevis},
        {"tyt ap", key_tyt_ap},
        {"baofeng pc5", key_baofeng},
        {"tyt ep", key_tyt_ep},
        {"kenwood", key_kenwood},
        {"anytone", key_anytone},
        {"dmr rc4", key_dmr_rc4},
        {"dmr des", key_dmr_des},
        {"dmr aes", key_dmr_aes},
    };

    static const struct {
        const char* name;
        int synctype;
        int slot;
        dsd_voice_protocol owner;
    } owners[] = {
        {"clear dpmr", DSD_SYNC_DPMR_FS2_POS, 0, DSD_VOICE_PROTOCOL_DPMR},
        {"p25p2 slot 1", DSD_SYNC_P25P2_POS, 0, DSD_VOICE_PROTOCOL_P25P2},
        {"p25p2 slot 2", DSD_SYNC_P25P2_POS, 1, DSD_VOICE_PROTOCOL_P25P2},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (int slot = 0; slot < 2; slot++) {
            // Positive control: the setting transforms DMR voice on this slot.
            rc |= capture_ambe(DSD_SYNC_DMR_BS_VOICE_POS, slot, DSD_VOICE_PROTOCOL_DMR, NULL, 6, &g_base);
            rc |= capture_ambe(DSD_SYNC_DMR_BS_VOICE_POS, slot, DSD_VOICE_PROTOCOL_DMR, cases[c].setup, 6, &g_trace);
            if (!traces_all_differ(&g_base, &g_trace)) {
                DSD_FPRINTF(stderr, "dmr control slot %d: %s left DMR voice untouched\n", slot + 1, cases[c].name);
                rc = 1;
            }
        }
        for (size_t o = 0; o < sizeof(owners) / sizeof(owners[0]); o++) {
            rc |= capture_ambe(owners[o].synctype, owners[o].slot, owners[o].owner, NULL, 6, &g_base);
            rc |= capture_ambe(owners[o].synctype, owners[o].slot, owners[o].owner, cases[c].setup, 6, &g_trace);
            if (!traces_equal(&g_base, &g_trace)) {
                DSD_FPRINTF(stderr, "%s: %s changed its clear voice\n", owners[o].name, cases[c].name);
                rc = 1;
            }
            if (cases[c].setup == key_tg_autoload) {
                rc |= expect_int("autoload leaves the dmr privacy key alone", (long long)g_state.K, 0);
            }
        }
    }
    return rc;
}

/* P25 Phase 2 keeps its own ciphers on both slots, and DMR keeps the P25 ALG IDs it decrypts. */
static int
test_slot_path_keeps_each_protocols_ciphers(void) {
    static const key_case cases[] = {
        {"p25 des", key_p25_des},
        {"p25 aes-256", key_p25_aes256},
        {"p25 aes-128", key_p25_aes128},
        {"p25 rc4", key_p25_rc4},
    };
    int rc = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (int slot = 0; slot < 2; slot++) {
            rc |= capture_ambe(DSD_SYNC_P25P2_POS, slot, DSD_VOICE_PROTOCOL_P25P2, NULL, 6, &g_base);
            rc |= capture_ambe(DSD_SYNC_P25P2_POS, slot, DSD_VOICE_PROTOCOL_P25P2, cases[c].setup, 6, &g_trace);
            if (!traces_all_differ(&g_base, &g_trace)) {
                DSD_FPRINTF(stderr, "p25p2 slot %d lost %s\n", slot + 1, cases[c].name);
                rc = 1;
            }
            if (cases[c].setup == key_p25_rc4) {
                continue; // P25 RC4 is decrypted on P25 Phase 2 only, as before.
            }
            rc |= capture_ambe(DSD_SYNC_DMR_BS_VOICE_POS, slot, DSD_VOICE_PROTOCOL_DMR, NULL, 6, &g_base);
            rc |= capture_ambe(DSD_SYNC_DMR_BS_VOICE_POS, slot, DSD_VOICE_PROTOCOL_DMR, cases[c].setup, 6, &g_trace);
            if (!traces_all_differ(&g_base, &g_trace)) {
                DSD_FPRINTF(stderr, "dmr slot %d lost %s\n", slot + 1, cases[c].name);
                rc = 1;
            }
        }
    }
    return rc;
}

/* Key selection by signalled ALG ID / KEY ID stays with DMR and P25: a stale DMR KEY ID must not replace the NXDN or
   dPMR scrambler key, or load a key for YSF. */
static int
test_keyloader_stays_with_signalling_protocols(void) {
    static const struct {
        int synctype;
        dsd_voice_protocol owner;
        int selects;
    } cases[] = {
        {DSD_SYNC_NXDN_POS, DSD_VOICE_PROTOCOL_NXDN, 0},        {DSD_SYNC_NXDN_POS, DSD_VOICE_PROTOCOL_DPMR, 0},
        {DSD_SYNC_NXDN_POS, DSD_VOICE_PROTOCOL_YSF, 0},         {DSD_SYNC_DPMR_FS2_POS, DSD_VOICE_PROTOCOL_DPMR, 0},
        {DSD_SYNC_DMR_BS_VOICE_POS, DSD_VOICE_PROTOCOL_DMR, 1},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        reset_state(cases[c].synctype, 0);
        g_state.keyloader = 1;
        g_state.rkey_array[5] = 0xAAAAULL;
        g_state.payload_algid = 0x21;
        g_state.payload_keyid = 5;
        g_state.R = 0x1234ULL;
        run_ambe(cases[c].owner, 1);
        rc |= expect_int("keyloader scope", (long long)g_state.R, cases[c].selects ? 0xAAAA : 0x1234);
    }
    return rc;
}

/* ---- P25 Phase 1 keystream ---- */

static void
p25_aes_key_bytes(const dsd_state* s, uint8_t key[32]) {
    for (int i = 0; i < 8; i++) {
        key[i + 0] = (uint8_t)((s->A1[0] >> (56 - (i * 8))) & 0xFFU);
        key[i + 8] = (uint8_t)((s->A2[0] >> (56 - (i * 8))) & 0xFFU);
        key[i + 16] = (uint8_t)((s->A3[0] >> (56 - (i * 8))) & 0xFFU);
        key[i + 24] = (uint8_t)((s->A4[0] >> (56 - (i * 8))) & 0xFFU);
    }
}

/* A reference P25 Phase 1 keystream, generated longer than any superframe reads. */
static int
p25p1_reference_keystream(const dsd_state* s, uint8_t ks[512]) {
    DSD_MEMSET(ks, 0, 512);
    uint8_t key[32];
    p25_aes_key_bytes(s, key);
    switch (s->payload_algid) {
        case 0x81: des_ofb_keystream_output(s->payload_miP, s->R, ks, 40); return 19;
        case 0x83: tdea_tofb_keystream_output(s->payload_miP, key, ks, 40); return 19;
        case 0x9F: des_xl_keystream_output(s->payload_miP, s->R, ks, s->xl_is_hdu); return 11;
        case 0x84: aes_ofb_keystream_output(s->aes_iv, key, ks, DSD_AES_KEY_256, 20); return 27;
        case 0x89: aes_ofb_keystream_output(s->aes_iv, key, ks, DSD_AES_KEY_128, 20); return 27;
        default: return -1;
    }
}

/* Octet offset of IMBE frame f (0-17) of a superframe: 11 octets a frame, 2 skipped for each LSD (after frames 8 and
   17, counting from 1). */
static int
p25p1_frame_offset(int start, int f) {
    return start + (11 * f) + ((f >= 8) ? 2 : 0) + ((f >= 17) ? 2 : 0);
}

/* One LDU1 + LDU2 superframe as the P25 Phase 1 decoder drives the vocoder; frames in skip_mask are dropped before the
   vocoder as the non-standard C0 word is. */
static void
run_p25p1_superframe(uint32_t skip_mask) {
    for (int f = 0; f < 18; f++) {
        if (f == 0) {
            g_state.p25vc = 0; // LDU1
        }
        if (f == 9) {
            g_state.p25vc = 9; // LDU2
        }
        if ((skip_mask & (1U << f)) != 0U) {
            dsd_mbe_p25p1_skip_voice_frame(&g_state);
        } else {
            processMbeFrame(&g_opts, &g_state, DSD_VOICE_PROTOCOL_P25P1, g_imbe_fr[f], NULL, NULL);
        }
        if (f == 7 || f == 16) {
            g_state.octet_counter += 2; // LSD
            g_state.dropL += 2;
        }
    }
    g_state.p25vc = 0;
    g_state.dropL = 267;
    g_state.octet_counter = 0;
}

static int
check_p25p1_superframe(const char* name, key_setup_fn setup, uint32_t skip_mask) {
    int rc = 0;
    reset_state(DSD_SYNC_P25P1_POS, 0);
    setup(&g_state);
    uint8_t ks[512];
    const int start = p25p1_reference_keystream(&g_state, ks);
    if (start < 0 || trace_begin() != 0) {
        return 1;
    }
    run_p25p1_superframe(skip_mask);
    run_p25p1_superframe(skip_mask); // the next LDU1 regenerates the keystream
    rc |= trace_end(&g_trace);

    int want_frames = 0;
    for (int f = 0; f < 18; f++) {
        want_frames += ((skip_mask & (1U << f)) == 0U) ? 1 : 0;
    }
    rc |= expect_int(name, g_trace.frames, 2LL * want_frames);
    rc |= expect_int("p25p1 superframe exhausted frames", g_trace.exhausted, 0);
    int line = 0;
    for (int pass = 0; pass < 2 && rc == 0; pass++) {
        for (int f = 0; f < 18; f++) {
            if ((skip_mask & (1U << f)) != 0U) {
                continue;
            }
            char plain[88];
            const int off = p25p1_frame_offset(start, f);
            for (int b = 0; b < 88; b++) {
                const int ks_bit = (ks[off + (b / 8)] >> (7 - (b % 8))) & 1;
                plain[b] = (char)(g_imbe_d[f][b] ^ ks_bit);
            }
            char want[24];
            imbe_hex(plain, want);
            if (strcmp(g_trace.data[line], want) != 0) {
                DSD_FPRINTF(stderr, "%s: pass %d frame %d got %s want %s\n", name, pass + 1, f + 1, g_trace.data[line],
                            want);
                rc = 1;
            }
            line++;
        }
    }
    return rc;
}

static void
key_p1_aes128(dsd_state* s) {
    key_p25_aes128(s);
}

static void
key_p1_aes256(dsd_state* s) {
    key_p25_aes256(s);
}

static void
key_p1_des(dsd_state* s) {
    key_p25_des(s);
}

static void
key_p1_tdea(dsd_state* s) {
    key_p25_aes256(s);
    s->payload_algid = 0x83;
    s->payload_miP = 0x1122334455667788ULL;
}

static void
key_p1_desxl(dsd_state* s) {
    key_p25_des(s);
    s->payload_algid = 0x9F;
}

/* Item 5: AES decrypts all 18 frames of the superframe, the 18th included (it read zeros past 14 blocks).
   Item 6: a frame dropped before the vocoder takes its keystream with it, including the one that generates it. */
static int
test_p25p1_superframe_keystream(void) {
    int rc = 0;
    rc |= check_p25p1_superframe("p25p1 aes-128 superframe", key_p1_aes128, 0U);
    rc |= check_p25p1_superframe("p25p1 aes-256 superframe", key_p1_aes256, 0U);
    rc |= check_p25p1_superframe("p25p1 des superframe", key_p1_des, 0U);
    rc |= check_p25p1_superframe("p25p1 tdea superframe", key_p1_tdea, 0U);
    rc |= check_p25p1_superframe("p25p1 des-xl superframe", key_p1_desxl, 0U);
    rc |= check_p25p1_superframe("p25p1 aes-128 frame 4 skipped", key_p1_aes128, 1U << 3);
    rc |= check_p25p1_superframe("p25p1 aes-128 frame 1 skipped", key_p1_aes128, 1U << 0);
    rc |= check_p25p1_superframe("p25p1 des ldu2 frames skipped", key_p1_des, (1U << 9) | (1U << 17));
    // A retained-carrier call can withhold a whole LDU1 while its identity is pending; LDU2 starts at word 10.
    rc |= check_p25p1_superframe("p25p1 aes ldu1 gated", key_p1_aes256, 0x1FFU);
    rc |= check_p25p1_superframe("p25p1 tdea ldu1 gated", key_p1_tdea, 0x1FFU);
    rc |= check_p25p1_superframe("p25p1 des-xl ldu1 gated", key_p1_desxl, 0x1FFU);

    // RC4 keeps its drop position across a skipped frame too.
    reset_state(DSD_SYNC_P25P1_POS, 0);
    key_p25_rc4(&g_state);
    g_state.dropL = 267;
    dsd_mbe_p25p1_skip_voice_frame(&g_state);
    rc |= expect_int("p25p1 rc4 skip drop", g_state.dropL, 267 + 11);
    rc |= expect_int("p25p1 rc4 skip vc", g_state.p25vc, 1);
    return rc;
}

/* Item 3: YSF full rate decodes through the P25 Phase 1 decoder; a P25 keystream left behind by an encrypted call that
   faded without a terminator is not P25 voice's to apply, and its position stays put. */
static int
test_ysf_full_rate_ignores_stale_p25_keystream(void) {
    int rc = 0;
    reset_state(DSD_SYNC_P25P1_POS, 0);
    if (trace_begin() != 0) {
        return 1;
    }
    for (int f = 0; f < 300; f++) {
        processMbeFrame(&g_opts, &g_state, DSD_VOICE_PROTOCOL_YSF, g_imbe_fr[f % FIXTURE_FRAMES], NULL, NULL);
    }
    rc |= trace_end(&g_base);

    reset_state(DSD_SYNC_P25P1_POS, 0);
    key_p25_aes256(&g_state);
    g_state.p25vc = 5;
    g_state.octet_counter = 200;
    DSD_MEMSET(g_state.ks_octetL, 0xA5, sizeof(g_state.ks_octetL));
    if (trace_begin() != 0) {
        return 1;
    }
    for (int f = 0; f < 300; f++) {
        processMbeFrame(&g_opts, &g_state, DSD_VOICE_PROTOCOL_YSF, g_imbe_fr[f % FIXTURE_FRAMES], NULL, NULL);
    }
    rc |= trace_end(&g_trace);
    if (!traces_equal(&g_base, &g_trace) || g_trace.frames != 300) {
        DSD_FPRINTF(stderr, "ysf full rate: stale p25 keystream changed clear YSF voice\n");
        rc = 1;
    }
    rc |= expect_int("ysf full rate leaves the p25 octet counter", g_state.octet_counter, 200);
    rc |= expect_int("ysf full rate leaves the p25 frame counter", g_state.p25vc, 5);
    return rc;
}

/* Item 3: a P25 Phase 1 read past the generated keystream mutes the frame instead of reading past it. */
static int
test_p25p1_keystream_read_is_bounded(void) {
    int rc = 0;

    // 15 AES blocks are 240 octets: a frame at octet 229 is the last that fits, one at 230 is past the end.
    static const struct {
        int start;
        int decrypted;
    } cases[] = {{229, 1}, {230, 0}, {100000, 0}, {-5, 0}};

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        reset_state(DSD_SYNC_P25P1_POS, 0);
        key_p25_aes128(&g_state);
        g_state.p25vc = 3;
        g_state.octet_counter = cases[c].start;
        if (trace_begin() != 0) {
            return 1;
        }
        processMbeFrame(&g_opts, &g_state, DSD_VOICE_PROTOCOL_P25P1, g_imbe_fr[0], NULL, NULL);
        rc |= trace_end(&g_trace);
        rc |= expect_int("p25p1 bounded frame decrypted", g_trace.frames, cases[c].decrypted);
        rc |= expect_int("p25p1 bounded frame muted", g_trace.exhausted, !cases[c].decrypted);
        rc |= expect_int("p25p1 bounded counter", g_state.octet_counter,
                         cases[c].decrypted ? cases[c].start + 11 : cases[c].start);
        rc |= expect_int("p25p1 bounded vc", g_state.p25vc, 4);
        if (!cases[c].decrypted) {
            float peak = 0.0f;
            for (int i = 0; i < 160; i++) {
                const float a = g_state.audio_out_temp_buf[i] < 0.0f ? -g_state.audio_out_temp_buf[i]
                                                                     : g_state.audio_out_temp_buf[i];
                peak = a > peak ? a : peak;
            }
            rc |= expect_int("p25p1 muted frame is silent", peak < 1e-6f, 1);
        }
    }
    return rc;
}

/* ---- DMR / P25 Phase 2 slot keystream ---- */

static void
key_dmr_hytera_enhanced(dsd_state* s) {
    s->payload_algid = s->payload_algidR = 0x02;
    s->R = s->RR = 0x0102030405ULL;
    s->payload_mi = s->payload_miR = 0x1122334455ULL;
}

/* Item 4: with no late-entry MI refresh (DMRvc never returns to 0), a long call stops at the end of the keystream
   instead of reading past ks_bitstream, and decrypts again once the keystream is refreshed. */
static int
test_dmr_keystream_read_is_bounded(void) {
    static const struct {
        const char* name;
        key_setup_fn setup;
        int frames_in_keystream;
    } cases[] = {
        {"dmr des", key_dmr_des, 20},                     // 1152 bits, 56 a frame
        {"dmr aes-128", key_dmr_aes, 20},                 // 1152 bits
        {"hytera enhanced", key_dmr_hytera_enhanced, 22}, // 1080 bits, 49 a frame
        {"p25 aes-256", key_p25_aes256, 20},              // the P25 Phase 2 / DMR P25-ALG-ID path
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (int slot = 0; slot < 2; slot++) {
            const int frames = 400;
            reset_state(DSD_SYNC_DMR_BS_VOICE_POS, slot);
            cases[c].setup(&g_state);
            if (trace_begin() != 0) {
                return 1;
            }
            run_ambe(DSD_VOICE_PROTOCOL_DMR, frames);
            rc |= trace_end(&g_trace);
            if (g_trace.frames != cases[c].frames_in_keystream
                || g_trace.exhausted != frames - cases[c].frames_in_keystream) {
                DSD_FPRINTF(stderr, "%s slot %d: %d decrypted, %d muted\n", cases[c].name, slot + 1, g_trace.frames,
                            g_trace.exhausted);
                rc = 1;
            }
            const long int counter = (slot == 0) ? g_state.bit_counterL : g_state.bit_counterR;
            rc |= expect_int("dmr bounded counter stays in the keystream", counter <= 1152, 1);

            // A refreshed MI (late entry's LFSR) starts a new keystream: the slot decrypts again.
            if (slot == 0) {
                g_state.DMRvcL = 0;
            } else {
                g_state.DMRvcR = 0;
            }
            if (trace_begin() != 0) {
                return 1;
            }
            run_ambe(DSD_VOICE_PROTOCOL_DMR, 2);
            rc |= trace_end(&g_trace);
            rc |= expect_int("dmr keystream refresh recovers", g_trace.frames, 2);
            rc |= expect_int("dmr keystream refresh not muted", g_trace.exhausted, 0);
        }
    }
    return rc;
}

/* A frame muted for its keystream stages silence, marked as such, in either output format, including when the
   high-pass filter retains speech history. Runs on a fully initialized decoder with the voice chain and staging. */
static int
test_keystream_muted_frame_stages_silence(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    static const struct {
        int stereo;
        int floating_point;
    } modes[] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}};

    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        for (int slot = 0; slot < 2; slot++) {
            initOpts(&opts);
            initState(&state);
            opts.audio_out = 1;
            opts.audio_out_type = 0;
            opts.floating_point = modes[m].floating_point;
            opts.dmr_stereo = modes[m].stereo;
            opts.use_hpf_d = 1;
            opts.audio_gain = opts.audio_gainR = 1.0f;
            state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
            state.currentslot = slot;
            key_dmr_des(&state);
            for (int f = 0; f < 2; f++) {
                processMbeFrame(&opts, &state, DSD_VOICE_PROTOCOL_DMR, NULL, g_ambe_fr[f], NULL);
            }
            // Give the HPF a nonzero history. Feeding the exhausted frame's zeros through it would synthesize a
            // tail; exhaustion must leave both synthesis and short staging silent instead.
            float prior_audio[160];
            for (int i = 0; i < 160; i++) {
                prior_audio[i] = 5000.0f;
            }
            dsd_voice_chain_run(&opts, &state, slot, prior_audio);
            // Stale non-silent samples on the slot, then a frame past the end of the keystream.
            short* staged = (slot == 0) ? state.s_l : state.s_r;
            for (int i = 0; i < 160; i++) {
                staged[i] = 1000;
            }
            if (slot == 0) {
                state.bit_counterL = 1152;
            } else {
                state.bit_counterR = 1152;
            }
            processMbeFrame(&opts, &state, DSD_VOICE_PROTOCOL_DMR, NULL, g_ambe_fr[2], NULL);

            const float* audio = (slot == 0) ? state.audio_out_temp_buf : state.audio_out_temp_bufR;
            for (int i = 0; i < 160; i++) {
                rc |= expect_int("muted frame bypasses retained HPF history", audio[i] < 1e-6f && audio[i] > -1e-6f, 1);
            }
            const int marker = (modes[m].stereo == 1) ? slot : 0;
            rc |= expect_int("muted frame marked silenced", state.mbe_short_silenced[marker], 1);
            if (modes[m].stereo == 1) {
                int nonzero = 0;
                for (int i = 0; i < 160; i++) {
                    nonzero |= staged[i] != 0;
                }
                rc |= expect_int("muted stereo slot stages silence", nonzero, 0);
            }
            freeState(&state);
        }
    }
    return rc;
}

/* A key still being loaded cannot overrule the verdict on a frame whose keystream ended. Exercise the actual FDMA
   mixers: an exhausted frame writes no sink/static WAV and stamps no activity, even under hold, -q and unmute. */
static int g_exhausted_sink_calls;
static size_t g_exhausted_sink_bytes;

static void
capture_exhausted_sink(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    (void)data;
    g_exhausted_sink_calls++;
    g_exhausted_sink_bytes += bytes;
}

static void
process_fdma_test_frame(dsd_voice_protocol protocol, int stereo) {
    if (protocol == DSD_VOICE_PROTOCOL_P25P1) {
        processMbeFrame(&g_opts, &g_state, protocol, g_imbe_fr[0], NULL, NULL);
    } else {
        processMbeFrame(&g_opts, &g_state, protocol, NULL, g_ambe_fr[0], NULL);
    }
    if (stereo) {
        playSynthesizedVoiceSS(&g_opts, &g_state);
    } else {
        playSynthesizedVoiceMS(&g_opts, &g_state);
    }
}

static int
check_fdma_exhaustion_outputs(key_setup_fn setup, int p25, int stereo, int floating) {
    const dsd_voice_protocol protocol = p25 ? DSD_VOICE_PROTOCOL_P25P1 : DSD_VOICE_PROTOCOL_NXDN;
    reset_state(p25 ? DSD_SYNC_P25P1_POS : DSD_SYNC_NXDN_POS, 0);
    setup(&g_state);
    g_opts.audio_out = 1;
    g_opts.audio_out_type = 8;
    g_opts.pulse_digi_out_channels = 2;
    g_opts.floating_point = floating;
    g_opts.static_wav_file = 1;
    g_opts.slot1_on = 1;
    g_opts.reverse_mute = 1;
    g_opts.unmute_encrypted_p25 = 1;
    g_opts.dmr_mute_encL = 0;
    g_state.tg_hold = 123;
    const dsd_call_observation call = {
        .protocol = g_state.synctype,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 123U,
        .policy_target_id = 123U,
    };
    int rc = expect_int("exhaustion output call", dsd_call_state_observe(&g_state, &call, DSD_CALL_BOUNDARY_BEGIN), 1);
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_exhausted_output");
    if (fd < 0) {
        return 1;
    }
    dsd_close(fd);
    SF_INFO info = {0};
    info.samplerate = 8000;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(path, SFM_WRITE, &info);
    if (!wav) {
        (void)remove(path);
        return 1;
    }
    g_opts.wav_out_f = wav;
    dsd_audio_activity_arm();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_exhausted_sink});
    for (int phase = 0; phase < 3; phase++) {
        if (phase == 1) {
            g_state.octet_counter = 240;
            g_state.bit_counterL = 1568;
        } else if (phase == 2) {
            g_state.p25vc = 0;
            g_state.nxdn_new_iv = 1;
        }
        const int audible = phase != 1;
        const sf_count_t before = sf_seek(wav, 0, SEEK_CUR);
        g_exhausted_sink_calls = 0;
        g_exhausted_sink_bytes = 0;
        dsd_audio_activity_reset();
        process_fdma_test_frame(protocol, stereo);
        uint64_t stamp = 0;
        dsd_audio_activity_read(&stamp, NULL);
        rc |= expect_int("exhaustion output verdict", g_state.mbe_slot_muted[0], !audible);
        rc |= expect_int("exhaustion output UDP blocks", g_exhausted_sink_calls, audible);
        rc |= expect_int("exhaustion output UDP format", (long long)g_exhausted_sink_bytes,
                         audible ? (long long)(320U * (floating ? sizeof(float) : sizeof(short))) : 0);
        rc |= expect_int("exhaustion output static WAV", sf_seek(wav, 0, SEEK_CUR) - before, audible ? 160 : 0);
        rc |= expect_int("exhaustion output activity", stamp != 0, audible);
    }
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    g_opts.wav_out_f = NULL;
    sf_close(wav);
    DSD_MEMSET(&info, 0, sizeof(info));
    wav = sf_open(path, SFM_READ, &info);
    rc |= expect_int("exhaustion WAV reopens", wav != NULL, 1);
    if (wav) {
        rc |= expect_int("exhaustion WAV contains only admitted frames", info.frames, 320);
        sf_close(wav);
    }
    (void)remove(path);
    return rc;
}

static int
test_keystream_exhaustion_blocks_fdma_outputs(void) {
    static const key_setup_fn keys[] = {key_nxdn_cipher2, key_nxdn_cipher3, key_p1_aes128, key_p1_aes256};
    int rc = 0;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        for (int stereo = 0; stereo < 2; stereo++) {
            for (int floating = 0; floating < 2; floating++) {
                rc |= check_fdma_exhaustion_outputs(keys[k], k >= 2, stereo, floating);
            }
        }
    }
    return rc;
}

/* An armed manual -S keystream still takes over a slot frame whose signalled cipher has run out of keystream, as it
   replaces the cipher on any other frame. */
static int
test_manual_keystream_takes_over_exhausted_frame(void) {
    int rc = 0;
    for (int slot = 0; slot < 2; slot++) {
        reset_state(DSD_SYNC_DMR_BS_VOICE_POS, slot);
        key_dmr_des(&g_state);
        run_ambe(DSD_VOICE_PROTOCOL_DMR, 1);
        if (slot == 0) {
            g_state.bit_counterL = 1152;
        } else {
            g_state.bit_counterR = 1152;
        }
        straight_mod_xor_keystream_creation(&g_state, "49:123456789ABC80", 0);
        if (trace_begin() != 0) {
            return 1;
        }
        run_ambe(DSD_VOICE_PROTOCOL_DMR, 2);
        rc |= trace_end(&g_trace);
        rc |= expect_int("manual keystream frames played", g_trace.frames, 2);
        rc |= expect_int("manual keystream frames not muted", g_trace.exhausted, 0);
        rc |= expect_int("manual keystream clears the alg id",
                         slot == 0 ? g_state.payload_algid : g_state.payload_algidR, 0);
    }
    return rc;
}

/* The DES frames that do fit still decrypt with their own keystream bits, the last two of them included. */
static int
test_dmr_des_frames_match_reference(void) {
    int rc = 0;
    reset_state(DSD_SYNC_DMR_BS_VOICE_POS, 0);
    key_dmr_des(&g_state);
    uint8_t ks[19 * 8];
    DSD_MEMSET(ks, 0, sizeof(ks));
    des_ofb_keystream_output(g_state.payload_miP, g_state.R, ks, 19);
    if (trace_begin() != 0) {
        return 1;
    }
    run_ambe(DSD_VOICE_PROTOCOL_DMR, 21);
    rc |= trace_end(&g_trace);
    rc |= expect_int("dmr des reference frames", g_trace.frames, 20);
    for (int f = 0; f < 20 && f < g_trace.frames; f++) {
        char plain[49];
        for (int b = 0; b < 49; b++) {
            const int bit = (56 * f) + b;
            plain[b] = (char)(g_ambe_d[f % FIXTURE_FRAMES][b] ^ ((ks[8 + (bit / 8)] >> (7 - (bit % 8))) & 1));
        }
        char want[24];
        ambe_hex(plain, want);
        if (strcmp(g_trace.data[f], want) != 0) {
            DSD_FPRINTF(stderr, "dmr des frame %d got %s want %s\n", f + 1, g_trace.data[f], want);
            rc = 1;
        }
    }
    return rc;
}

/* P25 Phase 2 ciphertext can coincidentally match either DMR clear-silence pattern. It still needs AES, unlike DMR.
   These hard frames encode F801A99F8CE080 (default silence) and 80000000000080 (zero tail); decoding asserts the
   fixtures independently, without requiring the newer mbelib encoder API at build time. */
static int
test_p25p2_aes_does_not_skip_dmr_silence_patterns(void) {
    static const char encoded[2][4][24] = {
        {{0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1},
         {1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0},
         {0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
         {1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {{1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
         {0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0},
         {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
         {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    };
    static const uint64_t words[2] = {0xF801A99F8CE080ULL, 0x80000000000080ULL};
    static const key_setup_fn keys[] = {key_p25_aes128, key_p25_aes256};
    int rc = 0;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        for (int slot = 0; slot < 2; slot++) {
            for (int p25 = 0; p25 < 2; p25++) {
                const dsd_voice_protocol owner = p25 ? DSD_VOICE_PROTOCOL_P25P2 : DSD_VOICE_PROTOCOL_DMR;
                reset_state(p25 ? DSD_SYNC_P25P2_POS : DSD_SYNC_DMR_BS_VOICE_POS, slot);
                keys[k](&g_state);
                uint8_t key[32];
                uint8_t ks[160];
                p25_aes_key_bytes(&g_state, key);
                aes_ofb_keystream_output(slot ? g_state.aes_ivR : g_state.aes_iv, key, ks,
                                         k == 0 ? DSD_AES_KEY_128 : DSD_AES_KEY_256, 10);
                if (trace_begin() != 0) {
                    return 1;
                }
                // Include an exhausted P25 frame whose ciphertext also matches silence: it must be muted.
                for (int f = 0; f < 21; f++) {
                    char fr[4][24];
                    DSD_MEMCPY(fr, encoded[f & 1], sizeof(fr));
                    processMbeFrame(&g_opts, &g_state, owner, NULL, fr, NULL);
                }
                rc |= trace_end(&g_trace);
                rc |= expect_int("silence-pattern frames", g_trace.frames, p25 ? 20 : 21);
                rc |= expect_int("silence-pattern exhausted", g_trace.exhausted, p25 ? 1 : 0);
                for (int f = 0; f < g_trace.frames && f < (p25 ? 20 : 21); f++) {
                    char plain[49];
                    for (int b = 0; b < 49; b++) {
                        const int bit = (f * 56) + b;
                        const int mask = p25 ? ((ks[16 + (bit / 8)] >> (7 - (bit % 8))) & 1) : 0;
                        plain[b] = (char)(((words[f & 1] >> (55 - b)) & 1U) ^ (unsigned)mask);
                    }
                    char want[24];
                    ambe_hex(plain, want);
                    rc |= expect_int("silence-pattern plaintext matches owner", strcmp(g_trace.data[f], want), 0);
                }
                rc |= expect_int("silence-pattern counter", slot ? g_state.bit_counterR : g_state.bit_counterL,
                                 p25 ? 1120 : 1152);
            }
        }
    }
    return rc;
}

/* A missing NXDN replacement IV ends the 32-word session: do not repeatedly apply the last word or use unused
   cipher-block padding. A fresh IV restores the next session. The same bound applies to DES and AES. */
static int
test_nxdn_cipher23_session_exhaustion(void) {
    static const key_setup_fn keys[] = {key_nxdn_cipher2, key_nxdn_cipher3};
    int rc = 0;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        reset_state(DSD_SYNC_NXDN_POS, 0);
        keys[k](&g_state);
        uint8_t ks[240];
        const int discard = k == 0 ? 8 : 16;
        if (k == 0) {
            des_ofb_keystream_output(g_state.payload_miN, g_state.R, ks, 26);
        } else {
            aes_ofb_keystream_output(g_state.aes_iv, g_state.aes_key, ks, DSD_AES_KEY_256, 15);
        }
        if (trace_begin() != 0) {
            return 1;
        }
        run_ambe(DSD_VOICE_PROTOCOL_NXDN, 40);
        rc |= trace_end(&g_trace);
        rc |= expect_int("nxdn session frames", g_trace.frames, 32);
        rc |= expect_int("nxdn exhausted frames", g_trace.exhausted, 8);
        rc |= expect_int("nxdn exhausted position stays put", g_state.bit_counterL, 1568);
        for (int f = 0; f < 32 && f < g_trace.frames; f++) {
            char plain[49];
            for (int b = 0; b < 49; b++) {
                const int bit = (f * 49) + b;
                plain[b] = (char)(g_ambe_d[f % FIXTURE_FRAMES][b] ^ ((ks[discard + (bit / 8)] >> (7 - (bit % 8))) & 1));
            }
            char want[24];
            ambe_hex(plain, want);
            rc |= expect_int("nxdn session plaintext", strcmp(g_trace.data[f], want), 0);
        }
        rc |= expect_int("nxdn exhausted frame marked silent", g_state.mbe_short_silenced[0], 1);
        for (int i = 0; i < 160; i++) {
            rc |= expect_int("nxdn exhausted PCM is silent",
                             g_state.audio_out_temp_buf[i] < 1e-6f && g_state.audio_out_temp_buf[i] > -1e-6f, 1);
        }

        g_state.nxdn_new_iv = 1;
        if (trace_begin() != 0) {
            return 1;
        }
        run_ambe(DSD_VOICE_PROTOCOL_NXDN, 2);
        rc |= trace_end(&g_trace);
        rc |= expect_int("nxdn IV refresh recovers", g_trace.frames, 2);
        rc |= expect_int("nxdn IV refresh not exhausted", g_trace.exhausted, 0);
        rc |= expect_int("nxdn IV refresh position", g_state.bit_counterL, 98);

        static const long bad_positions[] = {-1, 1520, 1568, 100000};
        for (size_t p = 0; p < sizeof(bad_positions) / sizeof(bad_positions[0]); p++) {
            g_state.bit_counterL = bad_positions[p];
            if (trace_begin() != 0) {
                return 1;
            }
            run_ambe(DSD_VOICE_PROTOCOL_NXDN, 1);
            rc |= trace_end(&g_trace);
            rc |= expect_int("nxdn invalid position mutes", g_trace.exhausted, 1);
            rc |= expect_int("nxdn invalid position does not synthesize", g_trace.frames, 0);
            rc |= expect_int("nxdn invalid position stays put", g_state.bit_counterL, bad_positions[p]);
        }
    }
    return rc;
}

/* Raw .amb/.dmb playback: DMR basic privacy (-b) is an AMBE+2 transform and never touches D-STAR's AMBE 2400 frames. */
static int
play_file_audio(const char cookie[4], unsigned long long basic_key, float audio[160]) {
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_crypto_scope_play");
    if (fd < 0) {
        return 1;
    }
    FILE* f = dsd_test_fdopen(fd, "wb");
    if (f == NULL) {
        (void)dsd_close(fd);
        (void)remove(path);
        return 1;
    }
    reset_state(DSD_SYNC_NONE, 0);
    g_opts.mbe_out_f = f;
    int rc = (fwrite(cookie, 1, 4, f) == 4) ? 0 : 1;
    for (int i = 0; i < 8 && rc == 0; i++) {
        saveAmbe2450Data(&g_opts, &g_state, g_ambe_d[i]);
    }
    if (fclose(f) != 0) {
        rc = 1;
    }
    g_opts.mbe_out_f = NULL;

    reset_state(DSD_SYNC_NONE, 0);
    g_opts.floating_point = 0;
    g_state.K = basic_key;
    g_state.optind = 1;
    char* argv[] = {"dsd-neo", path};
    playMbeFiles(&g_opts, &g_state, 2, argv);
    DSD_MEMCPY(audio, g_state.audio_out_temp_buf, sizeof(float) * 160U);
    (void)remove(path);
    return rc;
}

static int
audio_matches(const float a[160], const float b[160]) {
    for (int i = 0; i < 160; i++) {
        const float d = a[i] - b[i];
        if (d > 1e-3f || d < -1e-3f) {
            return 0;
        }
    }
    return 1;
}

static int
test_file_playback_basic_privacy_stays_on_ambe2450(void) {
    static float clear[160];
    static float keyed[160];
    int rc = 0;
    rc |= play_file_audio(".dmb", 0ULL, clear);
    rc |= play_file_audio(".dmb", 0x55ULL, keyed);
    rc |= expect_int("dstar playback ignores basic privacy", audio_matches(clear, keyed), 1);
    rc |= play_file_audio(".amb", 0ULL, clear);
    rc |= play_file_audio(".amb", 0x55ULL, keyed);
    rc |= expect_int("ambe+2 playback keeps basic privacy", audio_matches(clear, keyed), 0);
    return rc;
}

int
main(void) {
    if (build_fixtures() != 0) {
        return 1;
    }
    int rc = 0;
    rc |= test_ysf_ehr_gets_no_nxdn_cipher();
    rc |= test_slot_path_dmr_transforms_stay_on_dmr();
    rc |= test_slot_path_keeps_each_protocols_ciphers();
    rc |= test_keyloader_stays_with_signalling_protocols();
    rc |= test_p25p1_superframe_keystream();
    rc |= test_ysf_full_rate_ignores_stale_p25_keystream();
    rc |= test_p25p1_keystream_read_is_bounded();
    rc |= test_dmr_keystream_read_is_bounded();
    rc |= test_dmr_des_frames_match_reference();
    rc |= test_p25p2_aes_does_not_skip_dmr_silence_patterns();
    rc |= test_nxdn_cipher23_session_exhaustion();
    rc |= test_keystream_muted_frame_stages_silence();
    rc |= test_keystream_exhaustion_blocks_fdma_outputs();
    rc |= test_manual_keystream_takes_over_exhausted_frame();
    rc |= test_file_playback_basic_privacy_stays_on_ambe2450();

    dsd_state_ext_free_all(&g_state);
    if (g_state.rc2_context != NULL) {
        free(g_state.rc2_context);
        g_state.rc2_context = NULL;
    }
    if (rc == 0) {
        DSD_FPRINTF(stderr, "CORE_MBE_CRYPTO_SCOPE: OK\n");
    }
    return rc;
}
