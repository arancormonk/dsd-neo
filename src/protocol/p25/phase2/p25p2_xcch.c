// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25p2_xcch.c
 * Phase 2 SACCH/FACCH/LCCH Handling
 *
 * LWVMOBILE
 * 2022-09 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_activity.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25_xcch.h>
#include <dsd-neo/protocol/p25/p25p2_mac_parse.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

static const char* P25P2_EMPTY_CALL_STRING = "                     ";

static int
p25p2_xcch_slot_valid(int slot) {
    return slot >= 0 && slot <= 1;
}

static void
p25p2_xcch_set_slot_audio_allowed(const dsd_opts* opts, dsd_state* state, int slot, int allow_audio) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    state->p25_p2_audio_allowed[slot] = (allow_audio != 0 && p25_crypto_audio_permitted(opts, state, slot)) ? 1 : 0;
}

static int
p25p2_xcch_slot_audio_allowed(const dsd_opts* opts, const dsd_state* state, int slot) {
    int alg = 0;

    if (!state || slot < 0 || slot > 1) {
        return 0;
    }

    if (slot == 0) {
        alg = state->payload_algid;
    } else {
        alg = state->payload_algidR;
    }

    return dsd_p25p2_decode_audio_allowed(opts, state, slot, alg);
}

static unsigned long long int
p25p2_xcch_build_mi(const unsigned long long int mac[24]) {
    return (mac[1] << 56) | (mac[2] << 48) | (mac[3] << 40) | (mac[4] << 32) | (mac[5] << 24) | (mac[6] << 16)
           | (mac[7] << 8) | (mac[8] << 0);
}

static uint32_t
p25p2_xcch_src_from_mac(const unsigned long long int mac[24]) {
    return (uint32_t)((mac[13] << 16) | (mac[14] << 8) | mac[15]);
}

static int
p25p2_xcch_tg_from_mac(const unsigned long long int mac[24]) {
    return (int)((mac[16] << 8) | mac[17]);
}

static int
p25p2_xcch_extract_opcode(const int* payload) {
    return ((payload[0] & 1) << 2) | ((payload[1] & 1) << 1) | (payload[2] & 1);
}

static int
p25p2_xcch_extract_mac_offset(const int* payload) {
    return ((payload[3] & 1) << 2) | ((payload[4] & 1) << 1) | (payload[5] & 1);
}

static int
p25p2_xcch_extract_res(const int* payload) {
    return ((payload[6] & 1) << 1) | (payload[7] & 1);
}

static void
p25p2_xcch_unpack_mac_bits(const int* payload, int full_octets, int tail_start, unsigned long long int mac[24]) {
    int byte = 0;
    int k = 0;

    for (int j = 0; j < full_octets; j++) {
        for (int i = 0; i < 8; i++) {
            byte = (byte << 1) | (payload[k++] & 1);
        }
        mac[j] = (unsigned long long int)byte;
        byte = 0;
    }

    mac[full_octets] =
        (unsigned long long int)(((payload[tail_start] & 1) << 7) | ((payload[tail_start + 1] & 1) << 6)
                                 | ((payload[tail_start + 2] & 1) << 5) | ((payload[tail_start + 3] & 1) << 4));
}

static void
p25p2_xcch_print_payload_dump(const char* label, int mac_offset, int res, const unsigned long long int mac[24]) {
    DSD_FPRINTF(stderr, "\n %s OFFSET: %d RES: %d \n ", label, mac_offset, res);
    for (int i = 0; i < 24; i++) {
        if (i == 12) {
            DSD_FPRINTF(stderr, "\n ");
        }
        DSD_FPRINTF(stderr, "[%02llX]", mac[i]);
    }
}

static int
p25p2_xcch_get_slot_algid(const dsd_state* state, int slot) {
    return (slot == 0) ? state->payload_algid : state->payload_algidR;
}

static int
p25p2_xcch_get_slot_keyid(const dsd_state* state, int slot) {
    return (slot == 0) ? state->payload_keyid : state->payload_keyidR;
}

static unsigned long long int
p25p2_xcch_get_slot_mi(const dsd_state* state, int slot) {
    return (slot == 0) ? state->payload_miP : state->payload_miN;
}

static int
p25p2_xcch_get_slot_tg(const dsd_state* state, int slot) {
    return (slot == 0) ? state->lasttg : state->lasttgR;
}

static int
p25p2_xcch_get_slot_src(const dsd_state* state, int slot) {
    return (slot == 0) ? state->lastsrc : state->lastsrcR;
}

static int
p25p2_xcch_emit_active(dsd_opts* opts, dsd_state* state, int type, int slot, const unsigned long long int mac[24]) {
    struct p25p2_mac_voice_identity identity;
    if (p25p2_mac_decode_voice_identity(type, mac, &identity) == 1) {
        return p25_sm_emit_active_call(opts, state, slot, identity.tg, identity.dst, identity.src, identity.is_group,
                                       identity.svc_bits);
    }
    return p25_sm_emit_active(opts, state, slot);
}

static void
p25p2_xcch_set_slot_tg(dsd_state* state, int slot, int tg) {
    if (slot == 0) {
        state->lasttg = tg;
    } else {
        state->lasttgR = tg;
    }
}

static void
p25p2_xcch_set_slot_src_if_nonzero(dsd_state* state, int slot, uint32_t src) {
    if (src == 0) {
        return;
    }

    if (slot == 0) {
        state->lastsrc = (int)src;
    } else {
        state->lastsrcR = (int)src;
    }
}

static void
p25p2_xcch_clear_slot_source(dsd_state* state, int slot) {
    if (slot == 0) {
        state->lastsrc = 0;
    } else {
        state->lastsrcR = 0;
    }
}

static void
p25p2_xcch_set_slot_drop(dsd_state* state, int slot, int value) {
    if (slot == 0) {
        state->dropL = value;
    } else {
        state->dropR = value;
    }
}

static void
p25p2_xcch_set_slot_burst(dsd_state* state, int slot, int value) {
    if (slot == 0) {
        state->dmrburstL = value;
    } else {
        state->dmrburstR = value;
    }
}

static void
p25p2_xcch_reset_slot_gain(const dsd_opts* opts, dsd_state* state, int slot) {
    if (opts->floating_point != 1) {
        return;
    }

    if (slot == 0) {
        state->aout_gain = opts->audio_gain;
    } else {
        state->aout_gainR = opts->audio_gain;
    }
}

static void
p25p2_xcch_close_slot_mbe_out(dsd_opts* opts, dsd_state* state, int slot) {
    if (slot == 0) {
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    } else {
        if (opts->mbe_out_fR != NULL) {
            closeMbeOutFileR(opts, state);
        }
    }
}

static void
p25p2_xcch_flush_partial_audio_on_hangtime(dsd_opts* opts, dsd_state* state, int slot) {
    int audio_allowed[2];

    if (!opts || !state) {
        return;
    }

    audio_allowed[0] = state->p25_p2_audio_allowed[0];
    audio_allowed[1] = state->p25_p2_audio_allowed[1];

    // Flush before MAC_HANGTIME changes burst 21 to 22; SS18 single-slot
    // duplication uses those active burst hints. Unlike release, hangtime stays
    // on the VC, so restore the existing audio gates after the flush.
    dsd_p25p2_flush_partial_audio_slot(opts, state, slot);

    state->p25_p2_audio_allowed[0] = audio_allowed[0];
    state->p25_p2_audio_allowed[1] = audio_allowed[1];
}

static void
p25p2_xcch_handle_mac_hangtime_slot(dsd_opts* opts, dsd_state* state, int slot) {
    p25p2_xcch_flush_partial_audio_on_hangtime(opts, state, slot);
    p25p2_xcch_set_slot_burst(state, slot, 22);
    p25p2_xcch_close_slot_mbe_out(opts, state, slot);
}

static void
p25p2_xcch_blank_slot_call_string(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    DSD_SNPRINTF(state->call_string[slot], sizeof(state->call_string[slot]), "%s", P25P2_EMPTY_CALL_STRING);
}

static void
p25p2_xcch_clear_slot_idle_metadata(dsd_state* state, uint8_t slot, int clear_slot_ids) {
    if (!state || !p25p2_xcch_slot_valid(slot)) {
        return;
    }

    p25p2_xcch_blank_slot_call_string(state, slot);
    (void)clear_slot_ids;
    p25p2_xcch_clear_slot_source(state, slot);
    state->p25_call_is_packet[slot] = 0;
    state->p25_service_options_valid[slot] = 0;
    if (slot == 0) {
        state->dmr_so = 0;
    } else {
        state->dmr_soR = 0;
    }
}

static void
p25p2_xcch_clear_idle_metadata_if_stale(dsd_state* state, uint8_t slot, double idle_observed_m, int clear_slot_ids) {
    if (!state || !p25p2_xcch_slot_valid(slot)) {
        return;
    }
    if (p25_sm_slot_grant_newer_than(slot, idle_observed_m)) {
        return;
    }

    p25p2_xcch_clear_slot_idle_metadata(state, slot, clear_slot_ids);
}

static void
p25p2_xcch_clear_slot_gps(dsd_state* state, int slot) {
    state->dmr_embedded_gps[slot][0] = '\0';
    state->dmr_lrrp_gps[slot][0] = '\0';
}

static void
p25p2_xcch_set_slot_crypto_from_mac(dsd_opts* opts, dsd_state* state, int slot, const unsigned long long int mac[24]) {
    const int algid = (int)mac[10];
    const int keyid = (int)((mac[11] << 8) | mac[12]);
    const uint64_t mi = p25p2_xcch_build_mi(mac);
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE2, slot, algid, keyid, mi,
                             p25p2_xcch_get_slot_tg(state, slot));
}

static void
p25p2_xcch_log_slot_encryption(const dsd_opts* opts, dsd_state* state, int slot) {
    int algid = p25p2_xcch_get_slot_algid(state, slot);
    int keyid = p25p2_xcch_get_slot_keyid(state, slot);
    unsigned long long int mi = p25p2_xcch_get_slot_mi(state, slot);
    unsigned long long int key = (slot == 0) ? state->R : state->RR;

    if (algid == 0x80 || algid == 0x0) {
        return;
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n         ALG ID: 0x%02X", algid);
    DSD_FPRINTF(stderr, " KEY ID: 0x%04X", keyid);
    DSD_FPRINTF(stderr, " MI: 0x%016llX", mi);
    DSD_FPRINTF(stderr, " MPTT");

    if (key != 0 && algid == 0xAA) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s", dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, key, 10U, 1));
    }
    if (key != 0 && algid == 0x81) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s", dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, key, 16U, 1));
    }

    if ((algid == 0x84 || algid == 0x89) && state->aes_key_loaded[slot] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        const unsigned long long segments[4] = {state->A1[slot], state->A2[slot], state->A3[slot], state->A4[slot]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, opts->show_keys, segments,
                                                   (algid == 0x84) ? 4U : 2U));
    }

    if (algid == 0x84 || algid == 0x89) {
        p25_lfsr128_slot(state, slot);
    }
}

static void
p25p2_xcch_reset_ptt_slot_state(dsd_state* state, int slot) {
    state->fourv_counter[slot] = 0;
    state->voice_counter[slot] = 0;
    p25p2_xcch_set_slot_drop(state, slot, 256);
}

static void
p25p2_xcch_handle_ptt_slot(dsd_opts* opts, dsd_state* state, const unsigned long long int mac[24], int slot,
                           int always_set_burst) {
    uint32_t src = p25p2_xcch_src_from_mac(mac);
    int tg = p25p2_xcch_tg_from_mac(mac);
    int allow_audio = 0;

    p25p2_xcch_reset_ptt_slot_state(state, slot);
    if (always_set_burst) {
        p25p2_xcch_set_slot_burst(state, slot, 20);
    }

    DSD_FPRINTF(stderr, "\n VCH %d - ", slot + 1);
    p25p2_xcch_set_slot_src_if_nonzero(state, slot, src);
    p25p2_xcch_set_slot_tg(state, slot, tg);

    DSD_FPRINTF(stderr, "TG %d ", p25p2_xcch_get_slot_tg(state, slot));
    DSD_FPRINTF(stderr, "SRC %d ", src);

    p25p2_xcch_set_slot_crypto_from_mac(opts, state, slot, mac);
    p25p2_xcch_log_slot_encryption(opts, state, slot);
    p25p2_xcch_reset_slot_gain(opts, state, slot);

    allow_audio = p25p2_xcch_slot_audio_allowed(opts, state, slot);
    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, allow_audio);

    if (!always_set_burst && allow_audio) {
        p25p2_xcch_set_slot_burst(state, slot, 20);
    }
}

static void
p25p2_xcch_handle_end_slot(dsd_opts* opts, dsd_state* state, int slot, int clear_call_string) {
    if (slot < 0 || slot >= 2) {
        return;
    }
    dsd_p25p2_flush_partial_audio_slot(opts, state, slot);
    state->fourv_counter[slot] = 0;
    state->voice_counter[slot] = 0;
    p25p2_xcch_set_slot_drop(state, slot, 256);
    p25p2_xcch_set_slot_burst(state, slot, 23);

    DSD_FPRINTF(stderr, "\n VCH %d - ", slot + 1);
    DSD_FPRINTF(stderr, "TG %d ", p25p2_xcch_get_slot_tg(state, slot));
    DSD_FPRINTF(stderr, "SRC %d ", p25p2_xcch_get_slot_src(state, slot));

    p25p2_xcch_clear_slot_source(state, slot);
    p25p2_xcch_close_slot_mbe_out(opts, state, slot);

    if (clear_call_string) {
        p25p2_xcch_blank_slot_call_string(state, slot);
    }

    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, 0);
    p25p2_xcch_reset_slot_gain(opts, state, slot);
    p25p2_xcch_clear_slot_gps(state, slot);
    p25_crypto_reset_slot(state, slot);
}

static void
p25p2_xcch_reset_ptt_voice_counter_sacch(dsd_state* state) {
    if (state->currentslot == 1
        && (state->payload_algidR == 0x81 || state->payload_algidR == 0x84 || state->payload_algidR == 0x89)) {
        state->DMRvcL = 0;
    }

    if (state->currentslot == 0
        && (state->payload_algid == 0x81 || state->payload_algid == 0x84 || state->payload_algid == 0x89)) {
        state->DMRvcR = 0;
    }
}

static void
p25p2_xcch_reset_ptt_voice_counter_facch(dsd_state* state) {
    if (state->currentslot == 0
        && (state->payload_algid == 0x81 || state->payload_algid == 0x84 || state->payload_algid == 0x89)) {
        state->DMRvcL = 0;
    }

    if (state->currentslot == 1
        && (state->payload_algidR == 0x81 || state->payload_algidR == 0x84 || state->payload_algidR == 0x89)) {
        state->DMRvcR = 0;
    }
}

static void
p25p2_xcch_reset_idle_slot_facch(dsd_state* state, uint8_t slot) {
    p25_crypto_reset_slot(state, slot);
    p25p2_xcch_set_slot_burst(state, slot, 24);
    state->fourv_counter[slot] = 0;
    state->voice_counter[slot] = 0;
}

static int
p25p2_xcch_validate_sacch_crc(const dsd_opts* opts, dsd_state* state, const int payload[180],
                              const unsigned long long int smac[24], int opcode, uint8_t slot, int* abort_processing) {
    int err = -2;

    *abort_processing = 0;
    if (state->p2_is_lcch == 0) {
        err = crc12_xb_bridge(payload, 180 - 12);
        if (err != 0 && smac[1] != 0x0) {
            DSD_FPRINTF(stderr, " CRC12 ERR S");
            *abort_processing = 1;
        }
    }

    if (state->p2_is_lcch == 1) {
        err = crc16_lb_bridge(payload, 164);
        if (err != 0 && smac[1] == 0x0) {
            state->p2_is_lcch = 0;
            *abort_processing = 1;
        }

        if (err != 0 && smac[1] != 0x0 && opts->aggressive_framesync == 1) {
            DSD_FPRINTF(stderr, " CRC16 ERR L");
            if (opcode == 0x0) {
                p25p2_xcch_set_slot_audio_allowed(opts, state, slot, 0);
                p25_p2_audio_ring_reset(state, slot);
            }
            state->p2_is_lcch = 0;
            *abort_processing = 1;
        }

        if (err == 0) {
            p25_sm_note_cc_activity(state);
        }
    }

    return err;
}

static int
p25p2_xcch_validate_facch_crc(const dsd_state* state, const int payload[156], const unsigned long long int fmac[24],
                              int* abort_processing) {
    int err = -2;

    *abort_processing = 0;
    if (state->p2_is_lcch == 0) {
        err = crc12_xb_bridge(payload, 156 - 12);
        if (err != 0 && fmac[1] != 0x0) {
            DSD_FPRINTF(stderr, " CRC12 ERR F");
            *abort_processing = 1;
        }
    }

    return err;
}

static void
p25p2_xcch_handle_sacch_mac_signal(dsd_opts* opts, dsd_state* state, unsigned long long int smac[24], int err) {
    DSD_FPRINTF(stderr, " MAC_SIGNAL ");
    if (err != 0) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "CRC16 ERR ");
    }
    DSD_FPRINTF(stderr, "%s", KYEL);
    process_MAC_VPDU(opts, state, 1, smac);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25p2_xcch_handle_sacch_mac_ptt(dsd_opts* opts, dsd_state* state, uint8_t slot, int mac_offset, int res,
                                const unsigned long long int smac[24]) {
    DSD_FPRINTF(stderr, " MAC_PTT ");
    DSD_FPRINTF(stderr, "%s", KGRN);

    state->p25_p2_last_mac_active[slot] = time(NULL);
    state->p25_p2_last_mac_active_m[slot] = dsd_time_now_monotonic_s();

    if (!p25_sm_emit_ptt_call(opts, state, slot, p25p2_xcch_tg_from_mac(smac), 0, (int)p25p2_xcch_src_from_mac(smac), 1,
                              P25_SM_SVC_UNKNOWN)) {
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    p25p2_xcch_handle_ptt_slot(opts, state, smac, slot, 0);

    if (opts->payload == 1) {
        p25p2_xcch_print_payload_dump("MAC_PTT_PAYLOAD_S", mac_offset, res, smac);
    }

    p25p2_xcch_reset_ptt_voice_counter_sacch(state);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25p2_xcch_handle_sacch_mac_end(dsd_opts* opts, dsd_state* state, uint8_t slot, const unsigned long long int smac[24]) {
    const double end_observed_m = dsd_time_now_monotonic_s();
    const int tg = p25p2_xcch_tg_from_mac(smac);
    const int src = (int)p25p2_xcch_src_from_mac(smac);

    DSD_FPRINTF(stderr, " MAC_END_PTT ");
    DSD_FPRINTF(stderr, "%s", KRED);

    if (!p25_sm_emit_end_call_at(opts, state, slot, tg, src, end_observed_m)) {
        DSD_FPRINTF(stderr, "(stale/repeated) ");
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    state->p25_p2_last_end_ptt[slot] = time(NULL);
    p25p2_xcch_handle_end_slot(opts, state, slot, 1);
    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, 0);

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25p2_xcch_handle_sacch_mac_idle(dsd_opts* opts, dsd_state* state, uint8_t slot, unsigned long long int smac[24]) {
    double idle_observed_m = dsd_time_now_monotonic_s();

    dsd_p25p2_flush_partial_audio_slot(opts, state, slot);
    p25p2_xcch_set_slot_burst(state, slot, 24);

    DSD_FPRINTF(stderr, " MAC_IDLE ");
    DSD_FPRINTF(stderr, "%s", KYEL);
    process_MAC_VPDU(opts, state, 1, smac);
    DSD_FPRINTF(stderr, "%s", KNRM);

    p25_sm_emit_idle_at(opts, state, slot, idle_observed_m);
    p25p2_xcch_clear_idle_metadata_if_stale(state, slot, idle_observed_m, 0);
    if (!p25_sm_slot_grant_newer_than(slot, idle_observed_m)) {
        p25_crypto_reset_slot(state, slot);
    }
    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, 0);
}

static void
p25p2_xcch_handle_sacch_mac_active(dsd_opts* opts, dsd_state* state, uint8_t slot, unsigned long long int smac[24]) {
    int allow_audio = 0;

    DSD_FPRINTF(stderr, " MAC_ACTIVE ");
    DSD_FPRINTF(stderr, "%s", KYEL);
    process_MAC_VPDU(opts, state, 1, smac);

    state->p25_p2_last_mac_active[slot] = time(NULL);
    state->p25_p2_last_mac_active_m[slot] = dsd_time_now_monotonic_s();

    DSD_FPRINTF(stderr, "%s", KNRM);

    if (!p25p2_xcch_emit_active(opts, state, 1, slot, smac)) {
        return;
    }
    allow_audio = p25p2_xcch_slot_audio_allowed(opts, state, slot);
    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, allow_audio);
    if (allow_audio) {
        p25p2_xcch_set_slot_burst(state, slot, 21);
    }
}

static void
p25p2_xcch_handle_sacch_mac_hangtime(dsd_opts* opts, dsd_state* state, unsigned long long int smac[24]) {
    int slot = 0;
    if (state->currentslot == 1) {
        slot = 0;
    } else {
        slot = 1;
    }
    p25p2_xcch_handle_mac_hangtime_slot(opts, state, slot);
    p25_sm_emit_hangtime(opts, state, slot);

    DSD_FPRINTF(stderr, " MAC_HANGTIME ");
    DSD_FPRINTF(stderr, "%s", KYEL);
    process_MAC_VPDU(opts, state, 1, smac);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25p2_xcch_handle_facch_mac_ptt(dsd_opts* opts, dsd_state* state, uint8_t slot, int mac_offset, int res,
                                const unsigned long long int fmac[24]) {
    DSD_FPRINTF(stderr, " MAC_PTT  ");
    DSD_FPRINTF(stderr, "%s", KGRN);

    if (!p25_sm_emit_ptt_call(opts, state, slot, p25p2_xcch_tg_from_mac(fmac), 0, (int)p25p2_xcch_src_from_mac(fmac), 1,
                              P25_SM_SVC_UNKNOWN)) {
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    p25p2_xcch_handle_ptt_slot(opts, state, fmac, slot, 1);

    if (opts->payload == 1) {
        p25p2_xcch_print_payload_dump("MAC_PTT_PAYLOAD_F", mac_offset, res, fmac);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    p25p2_xcch_reset_ptt_voice_counter_facch(state);
}

static void
p25p2_xcch_handle_facch_mac_end(dsd_opts* opts, dsd_state* state, uint8_t slot, const unsigned long long int fmac[24]) {
    if (!p25p2_xcch_slot_valid(slot)) {
        return;
    }
    const double end_observed_m = dsd_time_now_monotonic_s();
    const int tg = p25p2_xcch_tg_from_mac(fmac);
    const int src = (int)p25p2_xcch_src_from_mac(fmac);

    DSD_FPRINTF(stderr, " MAC_END_PTT ");
    DSD_FPRINTF(stderr, "%s", KRED);

    const int end_result = p25_sm_emit_facch_end_call_at(opts, state, slot, tg, src, end_observed_m);
    if (end_result == P25_SM_END_IGNORED) {
        DSD_FPRINTF(stderr, "(stale/repeated) ");
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    if (end_result == P25_SM_END_CHANNEL_RELEASED) {
        DSD_FPRINTF(stderr, "(channel release) ");
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    p25p2_xcch_handle_end_slot(opts, state, slot, state->currentslot == 0);
    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, 0);

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25p2_xcch_handle_facch_mac_idle(dsd_opts* opts, dsd_state* state, uint8_t slot, unsigned long long int fmac[24]) {
    if (!p25p2_xcch_slot_valid(slot)) {
        return;
    }

    double idle_observed_m = dsd_time_now_monotonic_s();

    dsd_p25p2_flush_partial_audio_slot(opts, state, slot);
    p25p2_xcch_reset_idle_slot_facch(state, slot);
    p25p2_xcch_clear_slot_source(state, slot);

    DSD_FPRINTF(stderr, " MAC_IDLE ");
    DSD_FPRINTF(stderr, "%s", KYEL);
    process_MAC_VPDU(opts, state, 0, fmac);
    DSD_FPRINTF(stderr, "%s", KNRM);

    p25_sm_emit_idle_at(opts, state, slot, idle_observed_m);
    p25p2_xcch_clear_idle_metadata_if_stale(state, slot, idle_observed_m, 1);
    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, 0);
    p25_p2_audio_ring_reset(state, slot);
}

static void
p25p2_xcch_handle_facch_mac_active(dsd_opts* opts, dsd_state* state, uint8_t slot, unsigned long long int fmac[24]) {
    int allow_audio = 0;

    if (!p25p2_xcch_slot_valid(slot)) {
        return;
    }

    p25p2_xcch_set_slot_burst(state, slot, 21);

    DSD_FPRINTF(stderr, " MAC_ACTIVE ");
    DSD_FPRINTF(stderr, "%s", KYEL);
    process_MAC_VPDU(opts, state, 0, fmac);
    DSD_FPRINTF(stderr, "%s", KNRM);

    if (!p25p2_xcch_emit_active(opts, state, 0, slot, fmac)) {
        return;
    }

    allow_audio = p25p2_xcch_slot_audio_allowed(opts, state, slot);
    p25p2_xcch_set_slot_audio_allowed(opts, state, slot, allow_audio);
}

static void
p25p2_xcch_handle_facch_mac_hangtime(dsd_opts* opts, dsd_state* state, unsigned long long int fmac[24]) {
    int slot = 0;
    if (state->currentslot == 0) {
        slot = 0;
    } else {
        slot = 1;
    }
    p25p2_xcch_handle_mac_hangtime_slot(opts, state, slot);
    p25_sm_emit_hangtime(opts, state, slot);

    DSD_FPRINTF(stderr, " MAC_HANGTIME ");
    DSD_FPRINTF(stderr, "%s", KYEL);
    process_MAC_VPDU(opts, state, 0, fmac);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

void
process_SACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[180]) {
    if (!opts || !state || !payload) {
        return;
    }

    int slot = p25_sacch_to_voice_slot(state->currentslot);
    if (!p25p2_xcch_slot_valid(slot)) {
        return;
    }
    unsigned long long int smac[24] = {0};
    int opcode = 0;
    int mac_offset = 0;
    int res = 0;
    int abort_processing = 0;
    int err = -2;

    p25p2_xcch_unpack_mac_bits(payload, 22, 176, smac);
    opcode = p25p2_xcch_extract_opcode(payload);
    mac_offset = p25p2_xcch_extract_mac_offset(payload);
    res = p25p2_xcch_extract_res(payload);

    err = p25p2_xcch_validate_sacch_crc(opts, state, payload, smac, opcode, slot, &abort_processing);
    if (abort_processing) {
        return;
    }

    if (opcode == 0x0) {
        p25p2_xcch_handle_sacch_mac_signal(opts, state, smac, err);
        return;
    }
    if (err != 0) {
        return;
    }

    switch (opcode) {
        case 0x1: p25p2_xcch_handle_sacch_mac_ptt(opts, state, slot, mac_offset, res, smac); break;
        case 0x2: p25p2_xcch_handle_sacch_mac_end(opts, state, slot, smac); break;
        case 0x3: p25p2_xcch_handle_sacch_mac_idle(opts, state, slot, smac); break;
        case 0x4: p25p2_xcch_handle_sacch_mac_active(opts, state, slot, smac); break;
        case 0x6: p25p2_xcch_handle_sacch_mac_hangtime(opts, state, smac); break;
        default: break;
    }
}

void
process_FACCH_MAC_PDU(dsd_opts* opts, dsd_state* state, int payload[156]) {
    if (!opts || !state || !payload) {
        return;
    }

    int slot = state->currentslot;
    if (!p25p2_xcch_slot_valid(slot)) {
        return;
    }
    unsigned long long int fmac[24] = {0};
    int opcode = 0;
    int mac_offset = 0;
    int res = 0;
    int abort_processing = 0;
    int err = -2;

    p25p2_xcch_unpack_mac_bits(payload, 19, 152, fmac);
    fmac[20] = 0;
    fmac[21] = 0;
    fmac[22] = 0;

    opcode = p25p2_xcch_extract_opcode(payload);
    mac_offset = p25p2_xcch_extract_mac_offset(payload);
    res = p25p2_xcch_extract_res(payload);

    err = p25p2_xcch_validate_facch_crc(state, payload, fmac, &abort_processing);
    if (abort_processing || err != 0) {
        return;
    }

    switch (opcode) {
        case 0x1: p25p2_xcch_handle_facch_mac_ptt(opts, state, slot, mac_offset, res, fmac); break;
        case 0x2: p25p2_xcch_handle_facch_mac_end(opts, state, slot, fmac); break;
        case 0x3: p25p2_xcch_handle_facch_mac_idle(opts, state, slot, fmac); break;
        case 0x4: p25p2_xcch_handle_facch_mac_active(opts, state, slot, fmac); break;
        case 0x6: p25p2_xcch_handle_facch_mac_hangtime(opts, state, fmac); break;
        default: break;
    }
}
