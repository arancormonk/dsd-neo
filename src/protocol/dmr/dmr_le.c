// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * dmr_le.c
 * DMR Late Entry MI Fragment Assembly, Procesing, and related Alg functions
 *
 * LWVMOBILE
 * 2022-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
dmr_slot_is_kirisun_call(const dsd_state* state, uint8_t slot_idx) {
    // Gate Kirisun handling using current-call slot metadata only.
    // payload_algid* can persist across call boundaries and must not steer SB/RC parsing.
    if (slot_idx == 0U) {
        return state->dmr_fid == 0x0A;
    }

    return state->dmr_fidR == 0x0A;
}

static int
dmr_algid_is_kirisun(int algid) {
    return algid == 0x35 || algid == 0x36 || algid == 0x37;
}

static void
dmr_run_lfsr_for_verified_alg(dsd_state* state, int algid) {
    if (algid == 0x22) {
        LFSR64(state);
        DSD_FPRINTF(stderr, "\n");
    }

    if (algid == 0x24 || algid == 0x25) {
        LFSR128d(state);
        DSD_FPRINTF(stderr, "\n");
    }
}

static uint64_t
dmr_pack_le_fragments(const dsd_state* state, uint8_t slot_idx, uint8_t vc_base) {
    static const uint8_t shifts[9] = {32, 28, 24, 20, 16, 12, 8, 4, 0};
    uint64_t packed = 0;
    uint8_t shift_idx = 0;

    for (uint8_t frag_col = 0; frag_col < 3; frag_col++) {
        for (uint8_t frag_row = 0; frag_row < 3; frag_row++) {
            packed |= (uint64_t)state->late_entry_mi_fragment[slot_idx][vc_base + frag_row][frag_col]
                      << shifts[shift_idx++];
        }
    }

    return packed;
}

static void
dmr_decode_golay_triplet(uint64_t mi_test, uint64_t go_test, int g[3], uint64_t* mi_corrected, uint64_t* go_corrected,
                         uint8_t mi_bits[36]) {
    unsigned char mi_go_bits[24];

    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 12; i++) {
            mi_go_bits[i] = ((mi_test << (i + j * 12)) & 0x800000000) >> 35;
            mi_go_bits[i + 12] = ((go_test << (i + j * 12)) & 0x800000000) >> 35;
        }

        g[j] = Golay_24_12_decode(mi_go_bits) ? 1 : 0;

        for (int i = 0; i < 12; i++) {
            *mi_corrected = *mi_corrected << 1;
            *mi_corrected |= mi_go_bits[i];
            *go_corrected = *go_corrected << 1;
            *go_corrected |= mi_go_bits[i + 12];
            mi_bits[i + (j * 12)] = mi_go_bits[i];
        }
    }
}

static int
dmr_golay_triplet_all_pass(const int g[3]) {
    return g[0] && g[1] && g[2];
}

static void
dmr_maybe_infer_algid_from_key(dsd_state* state, uint8_t slot_idx, unsigned long long mi_final) {
    if (slot_idx == 0U) {
        const unsigned int so = state->dmr_so;
        const int so_enc_or_unknown = (so == 0) || ((so & 0x40U) != 0);
        if (state->payload_algid == 0 && state->R != 0 && so_enc_or_unknown) {
            state->payload_algid = (state->R <= 0xFFFFFFFFFFULL) ? 0x21 : 0x22;
            state->payload_keyid = 0xFF;
            state->payload_mi = mi_final;
        }
        return;
    }

    const unsigned int so = state->dmr_soR;
    const int so_enc_or_unknown = (so == 0) || ((so & 0x40U) != 0);
    if (state->payload_algidR == 0 && state->RR != 0 && so_enc_or_unknown) {
        state->payload_algidR = (state->RR <= 0xFFFFFFFFFFULL) ? 0x21 : 0x22;
        state->payload_keyidR = 0xFF;
        state->payload_miR = mi_final;
    }
}

static void
dmr_print_le_mi_mismatch(unsigned long long old_mi, unsigned long long new_mi, uint8_t mi_crc_ok, uint8_t slot_number) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " Slot %u PI/LFSR and Late Entry MI Mismatch - %08llX : %08llX ", slot_number, old_mi, new_mi);
    if (mi_crc_ok == 1) {
        DSD_FPRINTF(stderr, "(CRC OK)");
    } else {
        DSD_FPRINTF(stderr, "(CRC ERR)");
    }
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
dmr_process_verified_mi_slot(dsd_state* state, uint8_t slot_idx, unsigned long long mi_final, uint8_t mi_crc_ok) {
    unsigned long long* payload_mi = NULL;
    int algid = 0;
    uint8_t slot_number = 1;

    if (slot_idx == 0U) {
        payload_mi = &state->payload_mi;
        algid = state->payload_algid;
        slot_number = 1;
    } else {
        payload_mi = &state->payload_miR;
        algid = state->payload_algidR;
        slot_number = 2;
    }

    if (algid == 0) {
        return;
    }

    if (*payload_mi != mi_final) {
        dmr_print_le_mi_mismatch(*payload_mi, mi_final, mi_crc_ok, slot_number);
        if (mi_crc_ok == 1) {
            *payload_mi = mi_final;
        }
    }

    // Run expansions afterwards, or LE verification won't match up properly.
    dmr_run_lfsr_for_verified_alg(state, algid);
}

static void
dmr_print_hytera_refresh(const dsd_state* state, uint8_t slot_idx) {
    const char* slot_label = (slot_idx == 0U) ? " Slot 1" : " Slot 2";
    const int algid = (slot_idx == 0U) ? state->payload_algid : state->payload_algidR;
    const int keyid = (slot_idx == 0U) ? state->payload_keyid : state->payload_keyidR;
    const unsigned long long mi = (slot_idx == 0U) ? state->payload_mi : state->payload_miR;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "%s", slot_label);
    DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X;", algid, keyid);
    DSD_FPRINTF(stderr, " MI(40): %010llX;", mi);
    DSD_FPRINTF(stderr, " Hytera Enhanced;");
    DSD_FPRINTF(stderr, "%s\n", KNRM);
}

static void
dmr_print_kirisun_refresh(const dsd_state* state, uint8_t slot_idx) {
    const char* slot_label = (slot_idx == 0U) ? " Slot 1" : " Slot 2";
    const int algid = (slot_idx == 0U) ? state->payload_algid : state->payload_algidR;
    const int keyid = (slot_idx == 0U) ? state->payload_keyid : state->payload_keyidR;
    const unsigned long long mi = (slot_idx == 0U) ? state->payload_mi : state->payload_miR;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "%s", slot_label);
    DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X;", algid, keyid);
    DSD_FPRINTF(stderr, " MI(32): %08llX;", mi);
    DSD_FPRINTF(stderr, " Kirisun");
    if (algid == 0x36) {
        DSD_FPRINTF(stderr, " Advanced;");
    } else if (algid == 0x37) {
        DSD_FPRINTF(stderr, " Universal;");
    } else {
        DSD_FPRINTF(stderr, " Encryption;");
    }
    DSD_FPRINTF(stderr, "%s\n", KNRM);
}

static void
dmr_alg_refresh_slot(dsd_state* state, uint8_t slot_idx) {
    int* dmrvc = NULL;
    int algid = 0;
    unsigned long long* payload_mi = NULL;

    if (slot_idx == 0U) {
        state->dropL = 256;
        if (state->K1 != 0) {
            state->DMRvcL = 0;
        }
        dmrvc = &state->DMRvcL;
        algid = state->payload_algid;
        payload_mi = &state->payload_mi;
    } else {
        state->dropR = 256;
        if (state->K1 != 0) {
            state->DMRvcR = 0;
        }
        dmrvc = &state->DMRvcR;
        algid = state->payload_algidR;
        payload_mi = &state->payload_miR;
    }

    if (algid == 0x21) {
        LFSR(state);
        DSD_FPRINTF(stderr, "\n");
    }

    if (algid == 0x02) {
        *dmrvc = 0;
        dmr_print_hytera_refresh(state, slot_idx);
    }

    if (dmr_algid_is_kirisun(algid)) {
        *dmrvc = 0;
        *payload_mi = kirisun_lfsr(*payload_mi);
        dmr_print_kirisun_refresh(state, slot_idx);
    }
}

//gather ambe_fr mi fragments for processing
void
dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                           uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]) {

    uint8_t slot = state->currentslot;
    uint8_t slot_idx = (slot >= 2) ? 1 : slot;

    (void)dsd_dmr_apply_forced_algid(state);

    //collect our fragments and place them into storage
    if (vc < 8) {
        state->late_entry_mi_fragment[slot_idx][vc][0] = (uint64_t)ConvertBitIntoBytes(&ambe_fr[3][0], 4);
        state->late_entry_mi_fragment[slot_idx][vc][1] = (uint64_t)ConvertBitIntoBytes(&ambe_fr2[3][0], 4);
        state->late_entry_mi_fragment[slot_idx][vc][2] = (uint64_t)ConvertBitIntoBytes(&ambe_fr3[3][0], 4);
    }

    if (vc == 6) {
        dmr_late_entry_mi(opts, state);
    }
}

void
dmr_late_entry_mi(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);

    uint8_t slot = state->currentslot;
    uint8_t slot_idx = (slot >= 2U) ? 1U : slot;
    int g[3];

    uint64_t mi_test = dmr_pack_le_fragments(state, slot_idx, 1);
    uint64_t go_test = dmr_pack_le_fragments(state, slot_idx, 4);
    uint64_t mi_corrected = 0;
    uint64_t go_corrected = 0;

    uint8_t mi_bits[36];
    DSD_MEMSET(mi_bits, 0, sizeof(mi_bits));
    uint8_t mi_crc_cmp = 0;
    uint8_t mi_crc_ext = 1;
    uint8_t mi_crc_ok = 0;

    dmr_decode_golay_triplet(mi_test, go_test, g, &mi_corrected, &go_corrected, mi_bits);

    unsigned long long int mi_final = 0;
    mi_final = (mi_corrected >> 4) & 0xFFFFFFFF;

    mi_crc_ext = (uint8_t)ConvertBitIntoBytes(&mi_bits[32], 4);
    mi_crc_cmp = crc4(mi_bits, 32);
    if (mi_crc_ext == mi_crc_cmp) {
        mi_crc_ok = 1;
    }

    //debug -- working now
    // DSD_FPRINTF(stderr, " LE MI: %09llX; CRC EXT: %X; CRC CMP: %X; \n", mi_corrected, mi_crc_ext, mi_crc_cmp);

    const int golay_all_pass = dmr_golay_triplet_all_pass(g);

    // If PI/SB didn't provide ALG/Key ID but we have a valid LE MI and a manually provided key,
    // infer the ALG ID from key size so users don't need to force `-0` in common RC4/DES cases.
    if (golay_all_pass && mi_crc_ok == 1 && state->M == 0) {
        dmr_maybe_infer_algid_from_key(state, slot_idx, mi_final);
    }

    if (golay_all_pass) {
        dmr_process_verified_mi_slot(state, slot_idx, mi_final, mi_crc_ok);
    }

    // Run LFSR even if Golay fails.
    else if (slot == 0 && state->payload_algid != 0) {
        dmr_run_lfsr_for_verified_alg(state, state->payload_algid);
    } else if (slot == 1 && state->payload_algidR != 0) {
        dmr_run_lfsr_for_verified_alg(state, state->payload_algidR);
    }
}

void
dmr_refresh_algids_on_error(dsd_opts* opts, dsd_state* state) {
    for (uint8_t slot = 0; slot < 2; slot++) {
        int algid = (slot == 0) ? state->payload_algid : state->payload_algidR;
        if (algid >= 0x21) {
            state->currentslot = slot;
            dmr_alg_refresh(opts, state);
        } else if (algid == 0x02) {
            // hytera_enhanced_alg_refresh reads state->currentslot to pick the slot MI.
            state->currentslot = slot;
            hytera_enhanced_alg_refresh(state);
            dmr_alg_refresh(opts, state);
        }
    }
}

void
dmr_alg_refresh(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);

    if (state->currentslot == 0U || state->currentslot == 1U) {
        dmr_alg_refresh_slot(state, state->currentslot);
    }
}

void
dmr_alg_reset(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);

    state->dropL = 256;
    state->dropR = 256;
    state->DMRvcL = 0;
    state->DMRvcR = 0;
    // state->payload_miP = 0; //running these clears out before we can create a new keystream
    // state->payload_miN = 0; //running these clears out before we can create a new keystream
}

typedef struct {
    uint8_t slot;
    uint8_t slot_idx;
    uint8_t power;
    uint8_t sbrc_interleaved[32];
    uint8_t sbrc_return[32];
    uint8_t sbrc_retcrc[32];
    uint32_t irr_err;
    uint32_t sbrc_hex;
    uint16_t crc_extracted;
    uint16_t crc_computed;
    uint8_t crc7_okay;
    uint8_t crc3_okay;
} dmr_sbrc_data;

static void
dmr_sbrc_init_data(dmr_sbrc_data* data, const dsd_state* state, uint8_t power) {
    data->slot = state->currentslot;
    data->slot_idx = (data->slot >= 2U) ? 1U : data->slot;
    data->power = power;
    DSD_MEMSET(data->sbrc_interleaved, 0, sizeof(data->sbrc_interleaved));
    DSD_MEMSET(data->sbrc_return, 0, sizeof(data->sbrc_return));
    DSD_MEMSET(data->sbrc_retcrc, 0, sizeof(data->sbrc_retcrc));
    data->irr_err = 0;
    data->sbrc_hex = 0;
    data->crc_extracted = 7777;
    data->crc_computed = 9999;
    data->crc7_okay = 0;
    data->crc3_okay = 0;

    for (int i = 0; i < 32; i++) {
        data->sbrc_interleaved[i] = state->dmr_embedded_signalling[data->slot_idx][5][i + 8];
    }
}

static int
dmr_sbrc_extract_data(dmr_sbrc_data* data) {
    if (data->power == 0U) {
        data->irr_err = BPTC_16x2_Extract_Data(data->sbrc_interleaved, data->sbrc_return, 0);
        return 1;
    }
    if (data->power == 1U) {
        data->irr_err = BPTC_16x2_Extract_Data(data->sbrc_interleaved, data->sbrc_return, 1);
        return 1;
    }
    return 0;
}

static void
dmr_sbrc_prepare_crc_input(dmr_sbrc_data* data) {
    for (int i = 0; i < 8; i++) {
        data->sbrc_retcrc[i] = data->sbrc_return[i + 3];
    }
}

static void
dmr_sbrc_compute_crc(dmr_sbrc_data* data) {
    if (data->power == 1U) {
        data->crc_extracted = 0;
        for (int i = 0; i < 7; i++) {
            data->crc_extracted = data->crc_extracted << 1;
            data->crc_extracted = data->crc_extracted | data->sbrc_return[i + 4];
        }
        data->crc_extracted = data->crc_extracted ^ 0x7A;
        data->crc_computed = crc7((uint8_t*)data->sbrc_return, 4); // #187 fix
        if (data->crc_extracted == data->crc_computed) {
            data->crc7_okay = 1;
        }
        return;
    }

    data->crc_extracted = 0;
    for (int i = 0; i < 3; i++) {
        data->crc_extracted = data->crc_extracted << 1;
        data->crc_extracted = data->crc_extracted | data->sbrc_return[i]; // first 3 most significant bits
    }
    data->crc_computed = crc3((uint8_t*)data->sbrc_retcrc, 8); // working now seems consistent as well
    if (data->crc_extracted == data->crc_computed) {
        data->crc3_okay = 1;
    }
}

static void
dmr_sbrc_compute_hex(dmr_sbrc_data* data) {
    for (int i = 0; i < 11; i++) {
        data->sbrc_hex = data->sbrc_hex << 1;
        data->sbrc_hex |= data->sbrc_return[i] & 1;
    }
}

static void
dmr_sbrc_print_payload_bits(const dsd_opts* opts, const dmr_sbrc_data* data) {
    if (opts->payload == 1) // hide the SB/RC behind payload printer
    {
        DSD_FPRINTF(stderr, "%s", KCYN);
        if (data->power == 0U) {
            DSD_FPRINTF(stderr, " SB: ");
        }
        if (data->power == 1U) {
            DSD_FPRINTF(stderr, " RC: ");
        }
        for (int i = 0; i < 11; i++) {
            DSD_FPRINTF(stderr, "%d", data->sbrc_return[i]);
        }
        DSD_FPRINTF(stderr, " - %03X; ", data->sbrc_hex);
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, "\n");
    }
}

static void
dmr_sbrc_print_fec_error(const dsd_opts* opts, const dmr_sbrc_data* data) {
    uint32_t sbrcpl = 0;
    for (int i = 0; i < 32; i++) {
        sbrcpl = sbrcpl << 1;
        sbrcpl |= data->sbrc_interleaved[i] & 1;
    }
    if (opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }
    DSD_FPRINTF(stderr, "%s SLOT %d SB/RC (FEC ERR) E:%d; I:%08X D:%03X; %s ", KRED, data->slot + 1, data->irr_err,
                sbrcpl, data->sbrc_hex, KNRM);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }
}

static void
dmr_sbrc_print_rc_command(const dsd_opts* opts, uint32_t sbrc_hex) {
    static const char* rc_commands[] = {" RC: Increase Power By One Step;", " RC: Decrease Power By One Step;",
                                        " RC: Set Power To Highest;",       " RC: Set Power To Lowest;",
                                        " RC: Cease Transmission Command;", " RC: Cease Transmission Request;"};
    const uint32_t rc_value = sbrc_hex >> 7;

    if (opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }
    DSD_FPRINTF(stderr, "%s", KCYN);
    if (rc_value < (sizeof(rc_commands) / sizeof(rc_commands[0]))) {
        DSD_FPRINTF(stderr, "%s", rc_commands[rc_value]);
    } else {
        DSD_FPRINTF(stderr, " RC: Reserved %02X;", rc_value);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
    DSD_FPRINTF(stderr, "\n");
}

static void
dmr_sbrc_print_txi(const dsd_opts* opts, uint8_t sbrc_opcode, uint8_t txi_delay) {
    if (opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " TXI Op: %X -", sbrc_opcode);
    if (sbrc_opcode == 0U) {
        DSD_FPRINTF(stderr, " Null; ");
    } else if (sbrc_opcode == 3U) {
        if (txi_delay != 0U) {
            DSD_FPRINTF(stderr, " BR Delay: %d - %d ms;", txi_delay,
                        txi_delay * 30); // could also indicate superframes until next VC6 pre-emption
        } else {
            DSD_FPRINTF(stderr, "BR Delay: Irrelevant / Send at any time;");
        }

        if (txi_delay == 2U) {
            DSD_FPRINTF(stderr, " SF3, Burst E;");
        }
        if (txi_delay == 4U) {
            DSD_FPRINTF(stderr, " SF3, Burst D;");
        }
        if (txi_delay == 6U) {
            DSD_FPRINTF(stderr, " SF3, Burst C;");
        }
        if (txi_delay == 8U) {
            DSD_FPRINTF(stderr, " SF3, Burst B;");
        }
    } else {
        DSD_FPRINTF(stderr, " Unk; ");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n"); // only during payload
    }
}

static void
dmr_sbrc_print_alg_name(uint8_t alg) {
    if (alg == 1U) {
        DSD_FPRINTF(stderr, " RC4;");
    }
    if (alg == 2U) {
        DSD_FPRINTF(stderr, " DES;");
    }
    if (alg == 4U) {
        DSD_FPRINTF(stderr, " AES128;");
    }
    if (alg == 5U) {
        DSD_FPRINTF(stderr, " AES256;");
    }
}

static void
dmr_sbrc_apply_enc_identifier_slot0(const dsd_opts* opts, dsd_state* state, uint8_t alg, uint8_t key) {
    if (!(state->dmr_so & 0x40) || key == 0U || alg == 0U || state->M != 0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " Slot 1");
    DSD_FPRINTF(stderr, " DMR LE SB ALG ID: %02X; KEY ID: %02X;", alg + 0x20, key);
    dmr_sbrc_print_alg_name(alg);
    DSD_FPRINTF(stderr, "%s ", KNRM);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    state->payload_keyid = key;
    if (state->payload_algid != alg) {
        state->payload_algid = alg + 0x20; // assuming DMRA approved alg values (moto patent)
    }
}

static void
dmr_sbrc_apply_enc_identifier_slot1(const dsd_opts* opts, dsd_state* state, uint8_t alg, uint8_t key) {
    if (!(state->dmr_soR & 0x40) || key == 0U || alg == 0U || state->M != 0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, " Slot 2");
    DSD_FPRINTF(stderr, " DMR LE SB ALG ID: %02X; KEY ID: %02X;", alg + 0x20, key);
    DSD_FPRINTF(stderr, "%s ", KNRM);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    state->payload_keyidR = key;
    if (state->payload_algidR != alg) {
        state->payload_algidR = alg + 0x20; // assuming DMRA approved alg values (moto patent)
    }
}

static void
dmr_sbrc_apply_enc_identifier(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t alg, uint8_t key) {
    if (slot == 0U) {
        dmr_sbrc_apply_enc_identifier_slot0(opts, state, alg, key);
    }
    if (slot == 1U) {
        dmr_sbrc_apply_enc_identifier_slot1(opts, state, alg, key);
    }
}

static void
dmr_sbrc_handle_kirisun_mode(const dsd_opts* opts, dsd_state* state, const dmr_sbrc_data* data, int kirisun_call) {
    if (!(opts->dmr_le == 3 && kirisun_call)) {
        return;
    }

    if (data->irr_err != 0) {
        dmr_sbrc_print_fec_error(opts, data);
        return;
    }

    if (data->power == 0U && data->sbrc_hex != 0U) {
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, " Slot %d", state->currentslot + 1);
        DSD_FPRINTF(stderr, " DMR LE SB Kirisun Encryption Identifier;");
        DSD_FPRINTF(stderr, "%s ", KNRM);
        if (data->slot_idx == 0U) {
            if (state->payload_algid == 0 && (state->dmr_so & 0x40U)) {
                state->payload_algid = 0x35;
            }
        } else if (state->payload_algidR == 0 && (state->dmr_soR & 0x40U)) {
            state->payload_algidR = 0x35;
        }
    }
}

static void
dmr_sbrc_handle_standard_payload(const dsd_opts* opts, dsd_state* state, const dmr_sbrc_data* data, uint8_t sbrc_opcode,
                                 uint8_t alg, uint8_t key, uint8_t txi_delay) {
    if (data->sbrc_hex == 0U && data->crc7_okay == 0U) {
        return;
    }

    if (data->crc7_okay == 1U) {
        dmr_sbrc_print_rc_command(opts, data->sbrc_hex);
        return;
    }

    if (data->crc3_okay == 1U && (sbrc_opcode == 0U || sbrc_opcode == 3U)) {
        dmr_sbrc_print_txi(opts, sbrc_opcode, txi_delay);
        return;
    }

    if (sbrc_opcode != 0U && sbrc_opcode != 3U) {
        dmr_sbrc_apply_enc_identifier(opts, state, data->slot, alg, key);
    }
}

static void
dmr_sbrc_handle_standard_mode(const dsd_opts* opts, dsd_state* state, const dmr_sbrc_data* data, uint8_t sbrc_opcode,
                              uint8_t alg, uint8_t key, uint8_t txi_delay, int kirisun_call) {
    // opts->dmr_le is global; fall back to standard SB/RC parsing for non-Kirisun calls.
    if (!(opts->dmr_le == 1 || (opts->dmr_le == 3 && !kirisun_call))) {
        return;
    }

    if (data->irr_err != 0) {
        dmr_sbrc_print_fec_error(opts, data);
        return;
    }

    dmr_sbrc_handle_standard_payload(opts, state, data, sbrc_opcode, alg, key, txi_delay);
}

static void
dmr_sbrc_write_dsp_output(const dsd_opts* opts, const dsd_state* state, uint8_t slot) {
    if (opts->use_dsp_output == 1) {
        FILE* pFile = dsd_fopen_private(opts->dsp_out_file, "a");
        if (pFile != NULL) {
            DSD_FPRINTF(pFile, "\n%d 99 ", slot + 1); // '99' is SB and RC designation value
            for (int i = 0; i < 12; i++)              // 48 bits (includes CC, PPI, LCSS, and QR)
            {
                uint8_t sbrc_nib = (state->dmr_embedded_signalling[slot][5][(i * 4) + 0] << 3)
                                   | (state->dmr_embedded_signalling[slot][5][(i * 4) + 1] << 2)
                                   | (state->dmr_embedded_signalling[slot][5][(i * 4) + 2] << 1)
                                   | (state->dmr_embedded_signalling[slot][5][(i * 4) + 3] << 0);
                DSD_FPRINTF(pFile, "%X", sbrc_nib);
            }
            fclose(pFile);
        }
    }
}

//handle Single Burst (Voice Burst F) or Reverse Channel Signalling
void
dmr_sbrc(const dsd_opts* opts, dsd_state* state, uint8_t power) {
    dmr_sbrc_data data;
    dmr_sbrc_init_data(&data, state, power);

    // 9.3.2 Pre-emption and power control Indicator (PI)
    // 0 - embedded signalling carries same logical channel information or Null embedded message.
    // 1 - embedded signalling carries RC information for the other logical channel.
    if (dmr_sbrc_extract_data(&data)) {
        dmr_sbrc_prepare_crc_input(&data);
        dmr_sbrc_compute_crc(&data);
        dmr_sbrc_compute_hex(&data);
        dmr_sbrc_print_payload_bits(opts, &data);

        // opcode and alg share bits; alg can still appear when CRC is bad.
        const uint8_t sbrc_opcode = data.sbrc_hex & 0x7;
        const uint8_t alg = data.sbrc_hex & 0x7;
        const uint8_t key = (data.sbrc_hex >> 3) & 0xFF;
        const uint8_t txi_delay = (data.sbrc_hex >> 3) & 0x1F; // middle five are TXI delay
        const int kirisun_call = dmr_slot_is_kirisun_call(state, data.slot_idx);

        dmr_sbrc_handle_kirisun_mode(opts, state, &data, kirisun_call);
        dmr_sbrc_handle_standard_mode(opts, state, &data, sbrc_opcode, alg, key, txi_delay, kirisun_call);
    }

    dmr_sbrc_write_dsp_output(opts, state, data.slot);
}

uint8_t
crc3(uint8_t bits[], unsigned int len) {
    uint8_t crc = 0;
    unsigned int K = 3;
    //x^3+x+1
    const uint8_t poly[4] = {1, 1, 0, 1};
    uint8_t buf[256];
    if (len + K > sizeof(buf)) {
        return 0;
    }
    DSD_MEMSET(buf, 0, sizeof(buf));
    for (unsigned int i = 0; i < len; i++) {
        buf[i] = bits[i];
    }
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i]) {
            for (unsigned int j = 0; j < K + 1; j++) {
                buf[i + j] ^= poly[j];
            }
        }
    }
    for (unsigned int i = 0; i < K; i++) {
        crc = (crc << 1) + buf[len + i];
    }
    return crc;
}

uint8_t
crc4(uint8_t bits[], unsigned int len) {
    uint8_t crc = 0;
    unsigned int K = 4;
    //x^4+x+1
    const uint8_t poly[5] = {1, 0, 0, 1, 1};
    uint8_t buf[256];
    if (len + K > sizeof(buf)) {
        return 0;
    }
    DSD_MEMSET(buf, 0, sizeof(buf));
    for (unsigned int i = 0; i < len; i++) {
        buf[i] = bits[i];
    }
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i]) {
            for (unsigned int j = 0; j < K + 1; j++) {
                buf[i + j] ^= poly[j];
            }
        }
    }
    for (unsigned int i = 0; i < K; i++) {
        crc = (crc << 1) + buf[len + i];
    }
    return crc ^ 0xF; //invert
}
