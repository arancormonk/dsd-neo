// SPDX-License-Identifier: GPL-3.0-or-later
/* Fixed TETRA BSCH type-5 vector.  Keeping this literal outside the IQ fixture
 * generator prevents a generator/decoder ordering change from validating
 * itself.  The vector represents the transmit sequence convolutional encode,
 * RCPC puncture, block interleave, then scramble. */
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_mac_pdu.h"
#include "tetra_state.h"

enum {
    CODED_BITS = 120,
    MOTHER_BITS = 320,
    TYPE1_BITS = 60,
    TYPE2_BITS = 80,
    SYSINFO_CODED_BITS = 216,
    SYSINFO_MOTHER_BITS = 576,
    SYSINFO_TYPE1_BITS = 124,
    SYSINFO_TYPE2_BITS = 144,
};

static const char type5_text[] = "1011100110110010011101011001001111111010111101101001000011110011"
                                 "01111001010100010110000000111011010011110110110000010100";

static const char sysinfo_type5_text[] = "1000000110100010000111000011011000101011101001011000000111010101"
                                         "1100110110011011101100010111101101011101001010011000011011011110"
                                         "1011000100000101100001110001100101101101010100101001100100100100"
                                         "100001101111000101011111";

/* One live TMO BNCH SCH/HD block published by GopherTrunk's real-air
 * regression. Each character is one dibit (MSB first); MCC=250, MNC=13 and
 * base colour=13 produce extended colour 262144845. */
static const char real_bnch_dibits[] = "3010010010212222332100333330103131330313323331131332011332013201"
                                       "12011103013102212002313230110333100200111110";

_Static_assert(sizeof(type5_text) == CODED_BITS + 1, "the BSCH vector is 120 bits");
_Static_assert(sizeof(sysinfo_type5_text) == SYSINFO_CODED_BITS + 1, "the SYSINFO vector is 216 bits");
_Static_assert(sizeof(real_bnch_dibits) == (SYSINFO_CODED_BITS / 2) + 1, "the BNCH vector is 108 dibits");

static int
fail(const char* message) {
    DSD_FPRINTF(stderr, "TETRA air-chain vector: %s\n", message);
    return 1;
}

static void
release(dsd_opts* opts, dsd_state* state) {
    if (state) {
        dsd_state_ext_free_all(state);
    }
    free(state);
    free(opts);
}

/* Descrambling and deinterleaving of hard bits, through the soft-cost functions the receiver uses. */
static void
descramble_bits(uint8_t* bits, int len, uint32_t seed) {
    uint16_t costs[256];
    tetra_hard_bits_to_soft(bits, costs, len);
    tetra_descramble_soft(costs, len, seed);
    for (int i = 0; i < len; i++) {
        bits[i] = costs[i] == 0xFFFFu ? 1u : 0u;
    }
}

static void
deinterleave_bits(const uint8_t* in, uint8_t* out, int len, int a) {
    uint16_t costs[256];
    uint16_t deint[256];
    tetra_hard_bits_to_soft(in, costs, len);
    tetra_block_deinterleave_soft(costs, deint, len, a);
    for (int i = 0; i < len; i++) {
        out[i] = deint[i] == 0xFFFFu ? 1u : 0u;
    }
}

/* Direct form of EN 300 392-2 section 8.2.5 equations 8.41/8.42.  This
 * deliberately stores p(-31)..p(n) as a history rather than shifting an
 * LFSR, so a mirrored register/tap convention cannot validate itself. */
static int
check_scrambler_recurrence(uint16_t mcc, uint16_t mnc, uint8_t colour) {
    static const uint8_t taps[] = {1, 2, 4, 5, 7, 8, 10, 11, 12, 16, 22, 23, 26, 32};

    enum { PN_BITS = 200 };

    uint8_t history[PN_BITS + 32] = {0};
    uint8_t actual[PN_BITS] = {0};
    uint32_t extended =
        ((uint32_t)(mcc & 0x3ffu) << 20) | ((uint32_t)(mnc & 0x3fffu) << 6) | (uint32_t)(colour & 0x3fu);

    /* p(-31)=p(-30)=1; p(-29)=e(30), ..., p(0)=e(1).
     * In the packed extended colour code e(1) is bit 29 and e(30) bit 0. */
    history[0] = 1;
    history[1] = 1;
    for (int j = 0; j < 30; ++j) {
        history[2 + j] = (uint8_t)((extended >> j) & 1u);
    }
    for (int k = 1; k <= PN_BITS; ++k) {
        uint8_t pn = 0;
        int idx = k + 31;
        for (size_t j = 0; j < sizeof(taps); ++j) {
            pn ^= history[idx - taps[j]];
        }
        history[idx] = pn;
    }

    descramble_bits(actual, PN_BITS, tetra_compute_scramb_seed(mcc, mnc, colour));
    return memcmp(actual, history + 32, PN_BITS) == 0;
}

int
main(void) {
    uint8_t type5[CODED_BITS];
    uint8_t type4[CODED_BITS];
    uint8_t type3[CODED_BITS];
    uint16_t costs[CODED_BITS];
    uint16_t mother[MOTHER_BITS];
    uint8_t decoded[TYPE2_BITS];

    if (!check_scrambler_recurrence(0, 0, 0) || !check_scrambler_recurrence(250, 13, 13)
        || !check_scrambler_recurrence(460, 4242, 17) || !check_scrambler_recurrence(1023, 16383, 63)) {
        return fail("scrambler differs from direct ETSI recurrence");
    }

    uint8_t real_type5[SYSINFO_CODED_BITS];
    uint8_t real_type4[SYSINFO_CODED_BITS];
    uint8_t real_type3[SYSINFO_CODED_BITS];
    uint16_t real_costs[SYSINFO_CODED_BITS];
    uint16_t real_mother[SYSINFO_MOTHER_BITS];
    uint8_t real_decoded[SYSINFO_TYPE2_BITS];
    for (int i = 0; i < SYSINFO_CODED_BITS / 2; ++i) {
        if (real_bnch_dibits[i] < '0' || real_bnch_dibits[i] > '3') {
            return fail("invalid real BNCH dibit");
        }
        uint8_t dibit = (uint8_t)(real_bnch_dibits[i] - '0');
        uint8_t first = (dibit >> 1) & 1u;
        real_type5[(size_t)i * 2U] = first;
        real_type5[((size_t)i * 2U) + 1U] = first ^ (dibit & 1u);
    }
    DSD_MEMCPY(real_type4, real_type5, sizeof(real_type4));
    descramble_bits(real_type4, SYSINFO_CODED_BITS, tetra_compute_scramb_seed(250, 13, 13));
    deinterleave_bits(real_type4, real_type3, SYSINFO_CODED_BITS, 101);
    tetra_hard_bits_to_soft(real_type3, real_costs, SYSINFO_CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, real_costs, SYSINFO_CODED_BITS, real_mother,
                                    SYSINFO_MOTHER_BITS)
        < 0) {
        return fail("real BNCH depuncture failed");
    }
    if (tetra_viterbi_decode_soft(real_mother, SYSINFO_MOTHER_BITS, real_decoded, SYSINFO_TYPE2_BITS)
        != SYSINFO_TYPE2_BITS) {
        return fail("real BNCH Viterbi length mismatch");
    }
    if (tetra_crc16_ccitt_bits(real_decoded, SYSINFO_TYPE1_BITS + 16) != 0x1D0FU) {
        return fail("real BNCH CRC mismatch");
    }
    for (int i = 0; i < CODED_BITS; ++i) {
        if (type5_text[i] != '0' && type5_text[i] != '1') {
            return fail("invalid literal bit");
        }
        type5[i] = (uint8_t)(type5_text[i] - '0');
    }

    /* Receive chain reverses the air-interface transmit operations. */
    DSD_MEMCPY(type4, type5, sizeof(type4));
    descramble_bits(type4, CODED_BITS, 3U);
    deinterleave_bits(type4, type3, CODED_BITS, 11);
    tetra_hard_bits_to_soft(type3, costs, CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, costs, CODED_BITS, mother, MOTHER_BITS) < 0) {
        return fail("depuncture failed");
    }
    if (tetra_viterbi_decode_soft(mother, MOTHER_BITS, decoded, TYPE2_BITS) != TYPE2_BITS) {
        return fail("Viterbi length mismatch");
    }
    if (tetra_crc16_ccitt_bits(decoded, TYPE1_BITS + 16) != 0x1D0FU) {
        return fail("CRC mismatch");
    }

    /* Pin the historical regression too: deinterleaving before descrambling
     * must not decode this air-interface vector as a valid BSCH. */
    uint8_t legacy_type4[CODED_BITS];
    uint8_t legacy_type3[CODED_BITS];
    uint16_t legacy_costs[CODED_BITS];
    uint16_t legacy_mother[MOTHER_BITS];
    uint8_t legacy_decoded[TYPE2_BITS];
    DSD_MEMCPY(legacy_type4, type5, sizeof(legacy_type4));
    deinterleave_bits(legacy_type4, legacy_type3, CODED_BITS, 11);
    descramble_bits(legacy_type3, CODED_BITS, 3U);
    tetra_hard_bits_to_soft(legacy_type3, legacy_costs, CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, legacy_costs, CODED_BITS, legacy_mother, MOTHER_BITS) < 0) {
        return fail("legacy-order depuncture failed");
    }
    if (tetra_viterbi_decode_soft(legacy_mother, MOTHER_BITS, legacy_decoded, TYPE2_BITS) != TYPE2_BITS) {
        return fail("legacy-order Viterbi length mismatch");
    }
    if (tetra_crc16_ccitt_bits(legacy_decoded, TYPE1_BITS + 16) == 0x1D0FU) {
        return fail("legacy deinterleave-before-descramble order was accepted");
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        release(opts, state);
        return fail("allocation failed");
    }
    if (!tetra_bsch_parse(decoded, TYPE1_BITS, opts, state)) {
        release(opts, state);
        return fail("BSCH parse failed");
    }
    if (tetra_st(state)->mcc != 460 || tetra_st(state)->mnc != 4242 || tetra_st(state)->colour != 17
        || tetra_st(state)->tn != 3 || tetra_st(state)->fn != 9 || tetra_st(state)->mn != 37
        || tetra_st(state)->lfsr_seed != 0x73109247U) {
        release(opts, state);
        return fail("decoded BSCH fields mismatch");
    }

    uint8_t sys_type5[SYSINFO_CODED_BITS];
    uint8_t sys_type4[SYSINFO_CODED_BITS];
    uint8_t sys_type3[SYSINFO_CODED_BITS];
    uint16_t sys_costs[SYSINFO_CODED_BITS];
    uint16_t sys_mother[SYSINFO_MOTHER_BITS];
    uint8_t sys_decoded[SYSINFO_TYPE2_BITS];
    for (int i = 0; i < SYSINFO_CODED_BITS; ++i) {
        if (sysinfo_type5_text[i] != '0' && sysinfo_type5_text[i] != '1') {
            release(opts, state);
            return fail("invalid SYSINFO literal bit");
        }
        sys_type5[i] = (uint8_t)(sysinfo_type5_text[i] - '0');
    }

    DSD_MEMCPY(sys_type4, sys_type5, sizeof(sys_type4));
    descramble_bits(sys_type4, SYSINFO_CODED_BITS, tetra_st(state)->lfsr_seed);
    deinterleave_bits(sys_type4, sys_type3, SYSINFO_CODED_BITS, 101);
    tetra_hard_bits_to_soft(sys_type3, sys_costs, SYSINFO_CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, sys_costs, SYSINFO_CODED_BITS, sys_mother,
                                    SYSINFO_MOTHER_BITS)
        < 0) {
        release(opts, state);
        return fail("SYSINFO depuncture failed");
    }
    if (tetra_viterbi_decode_soft(sys_mother, SYSINFO_MOTHER_BITS, sys_decoded, SYSINFO_TYPE2_BITS)
        != SYSINFO_TYPE2_BITS) {
        release(opts, state);
        return fail("SYSINFO Viterbi length mismatch");
    }
    if (tetra_crc16_ccitt_bits(sys_decoded, SYSINFO_TYPE1_BITS + 16) != 0x1D0FU) {
        release(opts, state);
        return fail("SYSINFO CRC mismatch");
    }
    tetra_mac_broadcast bc;
    const tetra_mac_sysinfo* const si = &bc.sysinfo;
    const int sysinfo_fields =
        tetra_mac_broadcast_decode(sys_decoded, SYSINFO_TYPE1_BITS, &bc) && bc.type == TETRA_MAC_BC_SYSINFO
        && si->main_carrier == 321 && si->band == 4 && si->offset == 1 && si->duplex_spacing == 5 && si->num_scch == 2
        && si->ms_txpwr_max == 6 && si->rxlev_access_min == 9 && si->cck_flag == 1 && si->hyperframe_or_cck == 0xBEEFU
        && si->mle_present && si->la == 777 && si->subscriber_class == 0x1234U && si->bs_service == 0x567U;
    tetra_mac_parse_schd(sys_decoded, SYSINFO_TYPE1_BITS, 1, opts, state);

    const int ok = sysinfo_fields && tetra_st(state)->sysinfo_known == 1 && tetra_st(state)->sysinfo_main_carrier == 321
                   && tetra_st(state)->freq_band == 4 && tetra_st(state)->freq_offset == 1 && tetra_st(state)->la == 777
                   && !tetra_st(state)->hyperframe_valid;
    release(opts, state);
    return ok ? 0 : fail("decoded SYSINFO fields mismatch");
}
