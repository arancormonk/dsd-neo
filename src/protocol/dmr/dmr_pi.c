// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * dmr_pi.c
 * DMR Privacy Indicator and LFSR Function
 *
 * LFSR code courtesy of https://github.com/mattames/LFSR/
 *
 * LWVMOBILE
 * 2022-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
dmr_pi_update_sync_times_if_tuned(const dsd_opts* opts, dsd_state* state) {
    if (opts->p25_is_tuned == 1) {
        state->last_vc_sync_time = time(NULL);
        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    }
}

static unsigned long long int
dmr_pi_extract_mi32(const uint8_t pi_byte[]) {
    return ((unsigned long long int)pi_byte[3] << 24ULL) | ((unsigned long long int)pi_byte[4] << 16ULL)
           | ((unsigned long long int)pi_byte[5] << 8ULL) | ((unsigned long long int)pi_byte[6] << 0ULL);
}

static unsigned long long int
dmr_pi_extract_mi40(const uint8_t pi_byte[]) {
    return ((unsigned long long int)pi_byte[3] << 32ULL) | ((unsigned long long int)pi_byte[4] << 24ULL)
           | ((unsigned long long int)pi_byte[5] << 16ULL) | ((unsigned long long int)pi_byte[6] << 8ULL)
           | ((unsigned long long int)pi_byte[7] << 0ULL);
}

static void
dmr_pi_handle_kirisun(dsd_opts* opts, dsd_state* state, const uint8_t pi_byte[]) {
    const uint8_t alg = pi_byte[0];
    const uint8_t so = pi_byte[2];
    const uint32_t target = ((uint32_t)pi_byte[7] << 16U) | ((uint32_t)pi_byte[8] << 8U) | ((uint32_t)pi_byte[9] << 0U);
    const uint8_t key_hash = (uint8_t)((alg * target) & 0xFFU);
    const uint32_t mi = (uint32_t)dmr_pi_extract_mi32(pi_byte);

    if (state->currentslot == 0) {
        state->dmr_so = so;
        state->payload_algid = alg;
        state->payload_keyid = key_hash;
        state->payload_mi = mi;
    } else {
        state->dmr_soR = so;
        state->payload_algidR = alg;
        state->payload_keyidR = key_hash;
        state->payload_miR = mi;
    }

    DSD_FPRINTF(stderr, "%s ", KYEL);
    DSD_FPRINTF(stderr, "\n Slot %d", state->currentslot + 1);
    DSD_FPRINTF(stderr, " DMR PI H- ALG ID: %02X; KEY ID: %02X; MI(32): %08X;", alg, key_hash, mi);
    DSD_FPRINTF(stderr, " Kirisun ");
    if (alg == 0x36) {
        DSD_FPRINTF(stderr, "Advanced;");
    } else if (alg == 0x37) {
        DSD_FPRINTF(stderr, "Universal;");
    } else {
        DSD_FPRINTF(stderr, "Encryption;");
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    // Use Kirisun-specific LE handling for SB/MI refresh.
    opts->dmr_le = 3;
}

static uint8_t
dmr_pi_hytera_checksum(const uint8_t pi_byte[]) {
    uint8_t checksum = 0;

    for (int i = 0; i < 9; i++) {
        checksum = (uint8_t)((checksum + pi_byte[i]) & 0xFFU);
    }

    checksum = (uint8_t)(~checksum & 0xFFU);
    checksum++;
    return checksum;
}

static void
dmr_pi_hytera_print_key(const dsd_state* state) {
    if (state->currentslot == 0 && state->R != 0) {
        DSD_FPRINTF(stderr, "Key: %010llX; ", state->R);
    }

    if (state->currentslot == 1 && state->RR != 0) {
        DSD_FPRINTF(stderr, "Key: %010llX; ", state->RR);
    }
}

static void
dmr_pi_handle_hytera(dsd_opts* opts, dsd_state* state, const uint8_t pi_byte[]) {
    if (state->currentslot == 0) {
        state->dmr_so |= 0x40; //OR the enc bit onto the SO
        state->payload_algid = pi_byte[0];
        state->payload_keyid = pi_byte[2];
        state->payload_mi = dmr_pi_extract_mi40(pi_byte);
    } else {
        state->dmr_soR |= 0x40; //OR the enc bit onto the SO
        state->payload_algidR = pi_byte[0];
        state->payload_keyidR = pi_byte[2];
        state->payload_miR = dmr_pi_extract_mi40(pi_byte);
    }

    DSD_FPRINTF(stderr, "%s ", KYEL);
    DSD_FPRINTF(stderr, "\n Slot %d", state->currentslot + 1);
    DSD_FPRINTF(stderr, " DMR PI H- ALG ID: %02X; KEY ID: %02X; MI(40): %02X%02X%02X%02X%02X;", pi_byte[0], pi_byte[2],
                pi_byte[3], pi_byte[4], pi_byte[5], pi_byte[6], pi_byte[7]);

    if (dmr_pi_hytera_checksum(pi_byte) == pi_byte[9]) {
        DSD_FPRINTF(stderr, " Hytera Enhanced; ");
        dmr_pi_hytera_print_key(state);

        //disable late entry for DMRA (hopefully, there aren't any systems running both DMRA and Hytera Enhanced mixed together)
        opts->dmr_le = 2;
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " (Checksum Err);");
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
dmr_pi_dmra_print_family(int algid) {
    //check for any values that aren't 0x2X but just 0x0X
    //going to be very generic here to avoid any particular vendors using 0x2X and not 0x0X
    if (algid & 0x20) {
        DSD_FPRINTF(stderr, " DMRA");
    } else {
        DSD_FPRINTF(stderr, " DMRA Compatible");
    }
}

static void
dmr_pi_dmra_normalize_algid(int* algid) {
    if ((*algid & 0x07) == 0x01) {
        DSD_FPRINTF(stderr, " RC4;");
        *algid = 0x21;
    } else if ((*algid & 0x07) == 0x02) {
        DSD_FPRINTF(stderr, " DES;");
        *algid = 0x22;
    } else if ((*algid & 0x07) == 0x04) {
        DSD_FPRINTF(stderr, " AES-128;");
        *algid = 0x24;
    } else if ((*algid & 0x07) == 0x05) {
        DSD_FPRINTF(stderr, " AES-256;");
        *algid = 0x25;
    }
}

static void
dmr_pi_dmra_expand_iv_if_needed(dsd_state* state, int algid) {
    //expand the 32-bit MI to a 64-bit DES IV
    if (algid == 0x22) {
        DSD_FPRINTF(stderr, "\n");
        LFSR64(state);
    }

    //expand the 32-bit MI to a 128-bit AES IV
    if (algid == 0x24 || algid == 0x25) {
        DSD_FPRINTF(stderr, "\n");
        LFSR128d(state);
    }
}

static void
dmr_pi_handle_dmra_slot(dsd_state* state, const uint8_t pi_byte[], int slot_number, int* payload_algid,
                        int* payload_keyid, unsigned long long int* payload_mi) {
    *payload_algid = pi_byte[0];
    *payload_keyid = pi_byte[2];
    *payload_mi = dmr_pi_extract_mi32(pi_byte);

    if (*payload_algid < 0x26) {
        DSD_FPRINTF(stderr, "%s ", KYEL);
        DSD_FPRINTF(stderr, "\n Slot %d", slot_number);
        DSD_FPRINTF(stderr, " DMR PI H- ALG ID: %02X; KEY ID: %02X; MI(32): %08llX;", *payload_algid, *payload_keyid,
                    *payload_mi);

        dmr_pi_dmra_print_family(*payload_algid);
        dmr_pi_dmra_normalize_algid(payload_algid);
        DSD_FPRINTF(stderr, "%s ", KNRM);
        dmr_pi_dmra_expand_iv_if_needed(state, *payload_algid);
    }

    if (*payload_algid >= 0x26) {
        *payload_algid = 0;
        *payload_keyid = 0;
        *payload_mi = 0;
    }
}

static void
dmr_pi_handle_dmra(dsd_state* state, const uint8_t pi_byte[]) {
    if (state->currentslot == 0) {
        dmr_pi_handle_dmra_slot(state, pi_byte, 1, &state->payload_algid, &state->payload_keyid, &state->payload_mi);
    }

    if (state->currentslot == 1) {
        dmr_pi_handle_dmra_slot(state, pi_byte, 2, &state->payload_algidR, &state->payload_keyidR, &state->payload_miR);
    }
}

void
dmr_pi(dsd_opts* opts, dsd_state* state, uint8_t PI_BYTE[], uint32_t CRCCorrect, uint32_t IrrecoverableErrors) {
    const uint8_t MFID = PI_BYTE[1];

    if (IrrecoverableErrors != 0) {
        return;
    }

    //update cc amd vc sync time for trunking purposes (particularly Con+)
    dmr_pi_update_sync_times_if_tuned(opts, state);

    if (MFID == 0x0A && CRCCorrect == 1) { //Kirisun
        dmr_pi_handle_kirisun(opts, state, PI_BYTE);
        return;
    }

    if (MFID == 0x68) { //Hytera Enhanced
        dmr_pi_handle_hytera(opts, state, PI_BYTE);
        return;
    }

    if (MFID == 0x10) { //DMRA
        dmr_pi_handle_dmra(state, PI_BYTE);
    }
}

void
LFSR(dsd_state* state) {
    unsigned long long int lfsr = 0;
    if (state->currentslot == 0) {
        lfsr = state->payload_mi;
    } else {
        lfsr = state->payload_miR;
    }

    uint8_t cnt = 0;

    for (cnt = 0; cnt < 32; cnt++) {
        // Polynomial is C(x) = x^32 + x^4 + x^2 + 1
        unsigned long long int bit = ((lfsr >> 31) ^ (lfsr >> 3) ^ (lfsr >> 1)) & 0x1;
        lfsr = (lfsr << 1) | bit;
    }

    lfsr &= 0xFFFFFFFF;

    if (state->currentslot == 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, " Slot 1");
        DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X;", state->payload_algid, state->payload_keyid);
        DSD_FPRINTF(stderr, " MI(32): %08llX;", lfsr);
        DSD_FPRINTF(stderr, " RC4;");
        DSD_FPRINTF(stderr, "%s", KNRM);
        state->payload_mi = lfsr;
    }

    if (state->currentslot == 1) {

        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, " Slot 2");
        DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X;", state->payload_algidR, state->payload_keyidR);
        DSD_FPRINTF(stderr, " MI(32): %08llX;", lfsr);
        DSD_FPRINTF(stderr, " RC4;");
        DSD_FPRINTF(stderr, "%s", KNRM);
        state->payload_miR = lfsr;
    }
}

//Expand a 32-bit MI into a 64-bit IV for DES
void
LFSR64(dsd_state* state) {
    {
        unsigned long long int lfsr = 0;

        if (state->currentslot == 0) {
            lfsr = (uint64_t)state->payload_mi;
        } else {
            lfsr = (uint64_t)state->payload_miR;
        }

        uint8_t cnt = 0;

        for (cnt = 0; cnt < 32; cnt++) {
            unsigned long long int bit = ((lfsr >> 31) ^ (lfsr >> 21) ^ (lfsr >> 1) ^ (lfsr >> 0)) & 0x1;
            lfsr = (lfsr << 1) | bit;
        }

        if (state->currentslot == 0) {
            DSD_FPRINTF(stderr, "%s", KYEL);
            DSD_FPRINTF(stderr, " Slot 1");
            DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X;", state->payload_algid, state->payload_keyid);
            DSD_FPRINTF(stderr, " MI(64): %016llX;", lfsr);
            DSD_FPRINTF(stderr, " DES;");
            DSD_FPRINTF(stderr, "%s", KNRM);
            state->payload_mi = lfsr & 0xFFFFFFFF; //truncate for next repitition and le verification
            state->payload_miP = lfsr;
            state->DMRvcL = 0;
        }

        if (state->currentslot == 1) {
            DSD_FPRINTF(stderr, "%s", KYEL);
            DSD_FPRINTF(stderr, " Slot 2");
            DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X;", state->payload_algidR, state->payload_keyidR);
            DSD_FPRINTF(stderr, " MI(64): %016llX;", lfsr);
            DSD_FPRINTF(stderr, " DES;");
            DSD_FPRINTF(stderr, "%s", KNRM);
            state->payload_miR = lfsr & 0xFFFFFFFF; //truncate for next repitition and le verification
            state->payload_miN = lfsr;
            state->DMRvcR = 0;
        }
    }
}

//Expand a 32-bit MI into a 128-bit IV for AES
void
LFSR128d(dsd_state* state) {
    unsigned long long int lfsr = 0;

    if (state->currentslot == 0) {
        lfsr = state->payload_mi;
    } else {
        lfsr = state->payload_miR;
    }

    unsigned long long int next_mi = 0;

    //start packing aes_iv
    if (state->currentslot == 0) {
        state->aes_iv[0] = (lfsr >> 24) & 0xFF;
        state->aes_iv[1] = (lfsr >> 16) & 0xFF;
        state->aes_iv[2] = (lfsr >> 8) & 0xFF;
        state->aes_iv[3] = (lfsr >> 0) & 0xFF;
    } else if (state->currentslot == 1) {
        state->aes_ivR[0] = (lfsr >> 24) & 0xFF;
        state->aes_ivR[1] = (lfsr >> 16) & 0xFF;
        state->aes_ivR[2] = (lfsr >> 8) & 0xFF;
        state->aes_ivR[3] = (lfsr >> 0) & 0xFF;
    }

    int cnt = 0;
    int x = 32;
    for (cnt = 0; cnt < 96; cnt++) {
        //32,22,2,1
        unsigned long long int bit = ((lfsr >> 31) ^ (lfsr >> 21) ^ (lfsr >> 1) ^ (lfsr >> 0)) & 0x1;
        lfsr = (lfsr << 1) | bit;

        //continue packing aes_iv
        if (state->currentslot == 0) {
            state->aes_iv[x / 8] = (state->aes_iv[x / 8] << 1) + bit;
        } else if (state->currentslot == 1) {
            state->aes_ivR[x / 8] = (state->aes_ivR[x / 8] << 1) + bit;
        }
        x++;
    }

    //assign the next 32-bit short MI from 4,5,6,7 so it'll match up with OTA late entry
    if (state->currentslot == 0) {
        next_mi = ((unsigned long long int)state->aes_iv[4] << 24) | ((unsigned long long int)state->aes_iv[5] << 16)
                  | ((unsigned long long int)state->aes_iv[6] << 8) | ((unsigned long long int)state->aes_iv[7] << 0);
    }
    if (state->currentslot == 1) {
        next_mi = ((unsigned long long int)state->aes_ivR[4] << 24) | ((unsigned long long int)state->aes_ivR[5] << 16)
                  | ((unsigned long long int)state->aes_ivR[6] << 8) | ((unsigned long long int)state->aes_ivR[7] << 0);
    }

    if (state->currentslot == 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, " Slot 1");
        DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X; MI(128): ", state->payload_algid,
                    state->payload_keyid);
        for (x = 0; x < 16; x++) {
            DSD_FPRINTF(stderr, "%02X", state->aes_iv[x]);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, ";");

        if (state->payload_algid == 0x24) {
            DSD_FPRINTF(stderr, " AES-128;");
        } else {
            DSD_FPRINTF(stderr, " AES-256;");
        }

        state->payload_mi = next_mi;
        state->DMRvcL = 0;
    }

    if (state->currentslot == 1) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, " Slot 2");
        DSD_FPRINTF(stderr, " DMR PI C- ALG ID: %02X; KEY ID: %02X; MI(128): ", state->payload_algidR,
                    state->payload_keyidR);
        for (x = 0; x < 16; x++) {
            DSD_FPRINTF(stderr, "%02X", state->aes_ivR[x]);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
        DSD_FPRINTF(stderr, ";");

        if (state->payload_algidR == 0x24) {
            DSD_FPRINTF(stderr, " AES-128;");
        } else {
            DSD_FPRINTF(stderr, " AES-256;");
        }

        state->payload_miR = next_mi;
        state->DMRvcR = 0;
    }
}

static unsigned long long int
hytera_lfsr(uint8_t* mi, const uint8_t* taps, uint8_t len) {

    for (uint8_t i = 0; i < len; i++) {
        uint8_t bit = (mi[i] >> 7) & 1;
        mi[i] <<= 1;
        if (bit) {
            mi[i] ^= taps[i % 5];
        }
        mi[i] |= bit;
    }

    unsigned long long int mi_value = 0;
    for (uint8_t i = 0; i < 5; i++) {
        mi_value <<= 8;
        mi_value |= mi[i];
    }

    //debug
    // DSD_FPRINTF(stderr, " Next MI: %010llX \n", mi_value);

    return mi_value;
}

void
hytera_enhanced_alg_refresh(dsd_state* state) {
    uint8_t mi[5];
    DSD_MEMSET(mi, 0, sizeof(mi));
    unsigned long long int mi_value = 0;
    if (state->currentslot == 0) {
        mi_value = state->payload_mi;
    } else {
        mi_value = state->payload_miR;
    }

    //load mi_value into mi array
    mi[0] = ((mi_value & 0xFF00000000) >> 32UL);
    mi[1] = ((mi_value & 0xFF000000) >> 24);
    mi[2] = ((mi_value & 0xFF0000) >> 16);
    mi[3] = ((mi_value & 0xFF00) >> 8);
    mi[4] = ((mi_value & 0xFF) >> 0);

    //calculate the next MI value
    uint8_t taps[5];
    DSD_MEMSET(taps, 0, sizeof(taps));

    //the tap values
    taps[0] = 0x12;
    taps[1] = 0x24;
    taps[2] = 0x48;
    taps[3] = 0x22;
    taps[4] = 0x14;
    mi_value = hytera_lfsr(mi, taps, 5);

    if (state->currentslot == 0) {
        state->payload_mi = mi_value;
    } else {
        state->payload_miR = mi_value;
    }
}

uint32_t
kirisun_lfsr(unsigned long long int mi) {
    uint32_t lfsr = (uint32_t)mi;
    uint32_t next_mi = 0;
    const uint32_t taps = 0xD459C4F1U;

    for (int i = 0; i < 4; i++) {
        uint8_t out = 0;
        for (int j = 0; j < 8; j++) {
            const uint32_t temp = lfsr << 1U;
            const uint8_t msb = (uint8_t)((lfsr >> 31U) & 1U);
            if (msb != 0U) {
                out |= (uint8_t)(1U << j);
                lfsr = temp ^ taps;
            } else {
                lfsr = temp;
            }
        }
        next_mi <<= 8U;
        next_mi |= out;
    }

    return next_mi;
}
