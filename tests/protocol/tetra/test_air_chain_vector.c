// SPDX-License-Identifier: GPL-3.0-or-later
/* Fixed TETRA BSCH type-5 vector.  Keeping this literal outside the IQ fixture
 * generator prevents a generator/decoder ordering change from validating
 * itself.  The vector represents the transmit sequence convolutional encode,
 * RCPC puncture, block interleave, then scramble. */
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char type5_text[] =
    "1011100110110010011101011001001111111010111101101001000011110011"
    "01111001010100010110000000111011010011110110110000010100";

static const char sysinfo_type5_text[] =
    "1000000110100010000111000011011000101011101001011000000111010101"
    "1100110110011011101100010111101101011101001010011000011011011110"
    "1011000100000101100001110001100101101101010100101001100100100100"
    "100001101111000101011111";

/* One live TMO BNCH SCH/HD block published by GopherTrunk's real-air
 * regression. Each character is one dibit (MSB first); MCC=250, MNC=13 and
 * base colour=13 produce extended colour 262144845. */
static const char real_bnch_dibits[] =
    "3010010010212222332100333330103131330313323331131332011332013201"
    "12011103013102212002313230110333100200111110";

static int
fail(const char *message)
{
    fprintf(stderr, "TETRA air-chain vector: %s\n", message);
    return 1;
}

/* Direct form of EN 300 392-2 section 8.2.5 equations 8.41/8.42.  This
 * deliberately stores p(-31)..p(n) as a history rather than shifting an
 * LFSR, so a mirrored register/tap convention cannot validate itself. */
static int
check_scrambler_recurrence(uint16_t mcc, uint16_t mnc, uint8_t colour)
{
    static const uint8_t taps[] = {
        1, 2, 4, 5, 7, 8, 10, 11, 12, 16, 22, 23, 26, 32
    };
    enum { PN_BITS = 200 };
    uint8_t history[PN_BITS + 32] = {0};
    uint8_t actual[PN_BITS] = {0};
    uint32_t extended = ((uint32_t)(mcc & 0x3ffu) << 20)
                      | ((uint32_t)(mnc & 0x3fffu) << 6)
                      | (uint32_t)(colour & 0x3fu);

    /* p(-31)=p(-30)=1; p(-29)=e(30), ..., p(0)=e(1).
     * In the packed extended colour code e(1) is bit 29 and e(30) bit 0. */
    history[0] = 1;
    history[1] = 1;
    for (int j = 0; j < 30; ++j)
        history[2 + j] = (uint8_t)((extended >> j) & 1u);
    for (int k = 1; k <= PN_BITS; ++k) {
        uint8_t pn = 0;
        int idx = k + 31;
        for (size_t j = 0; j < sizeof(taps); ++j)
            pn ^= history[idx - taps[j]];
        history[idx] = pn;
    }

    tetra_descramble(actual, PN_BITS,
                      tetra_compute_scramb_seed(mcc, mnc, colour));
    return memcmp(actual, history + 32, PN_BITS) == 0;
}

int
main(void)
{
    uint8_t type5[CODED_BITS];
    uint8_t type4[CODED_BITS];
    uint8_t type3[CODED_BITS];
    uint16_t costs[CODED_BITS];
    uint16_t mother[MOTHER_BITS];
    uint8_t decoded[TYPE2_BITS];

    if (!check_scrambler_recurrence(0, 0, 0)
        || !check_scrambler_recurrence(250, 13, 13)
        || !check_scrambler_recurrence(460, 4242, 17)
        || !check_scrambler_recurrence(1023, 16383, 63))
        return fail("scrambler differs from direct ETSI recurrence");

    uint8_t real_type5[SYSINFO_CODED_BITS];
    uint8_t real_type4[SYSINFO_CODED_BITS];
    uint8_t real_type3[SYSINFO_CODED_BITS];
    uint16_t real_costs[SYSINFO_CODED_BITS];
    uint16_t real_mother[SYSINFO_MOTHER_BITS];
    uint8_t real_decoded[SYSINFO_TYPE2_BITS];
    if (strlen(real_bnch_dibits) != SYSINFO_CODED_BITS / 2)
        return fail("invalid real BNCH literal length");
    for (int i = 0; i < SYSINFO_CODED_BITS / 2; ++i) {
        if (real_bnch_dibits[i] < '0' || real_bnch_dibits[i] > '3')
            return fail("invalid real BNCH dibit");
        uint8_t dibit = (uint8_t)(real_bnch_dibits[i] - '0');
        uint8_t first = (dibit >> 1) & 1u;
        real_type5[i * 2] = first;
        real_type5[i * 2 + 1] = first ^ (dibit & 1u);
    }
    memcpy(real_type4, real_type5, sizeof(real_type4));
    tetra_descramble(real_type4, SYSINFO_CODED_BITS,
                      tetra_compute_scramb_seed(250, 13, 13));
    tetra_block_deinterleave(real_type4, real_type3,
                             SYSINFO_CODED_BITS, 101);
    tetra_hard_bits_to_soft(real_type3, real_costs, SYSINFO_CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, real_costs,
                                    SYSINFO_CODED_BITS, real_mother,
                                    SYSINFO_MOTHER_BITS) < 0)
        return fail("real BNCH depuncture failed");
    if (tetra_viterbi_decode_soft(real_mother, SYSINFO_MOTHER_BITS,
                                  real_decoded, SYSINFO_TYPE2_BITS)
        != SYSINFO_TYPE2_BITS)
        return fail("real BNCH Viterbi length mismatch");
    if (tetra_crc16_ccitt_bits(real_decoded, SYSINFO_TYPE1_BITS + 16)
        != 0x1D0FU)
        return fail("real BNCH CRC mismatch");

    if (strlen(type5_text) != CODED_BITS)
        return fail("invalid literal length");
    for (int i = 0; i < CODED_BITS; ++i) {
        if (type5_text[i] != '0' && type5_text[i] != '1')
            return fail("invalid literal bit");
        type5[i] = (uint8_t)(type5_text[i] - '0');
    }

    /* Receive chain reverses the air-interface transmit operations. */
    memcpy(type4, type5, sizeof(type4));
    tetra_descramble(type4, CODED_BITS, 3U);
    tetra_block_deinterleave(type4, type3, CODED_BITS, 11);
    tetra_hard_bits_to_soft(type3, costs, CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, costs,
                                    CODED_BITS, mother, MOTHER_BITS) < 0)
        return fail("depuncture failed");
    if (tetra_viterbi_decode_soft(mother, MOTHER_BITS, decoded,
                                  TYPE2_BITS) != TYPE2_BITS)
        return fail("Viterbi length mismatch");
    if (tetra_crc16_ccitt_bits(decoded, TYPE1_BITS + 16) != 0x1D0FU)
        return fail("CRC mismatch");

    /* Pin the historical regression too: deinterleaving before descrambling
     * must not decode this air-interface vector as a valid BSCH. */
    uint8_t legacy_type4[CODED_BITS];
    uint8_t legacy_type3[CODED_BITS];
    uint16_t legacy_costs[CODED_BITS];
    uint16_t legacy_mother[MOTHER_BITS];
    uint8_t legacy_decoded[TYPE2_BITS];
    memcpy(legacy_type4, type5, sizeof(legacy_type4));
    tetra_block_deinterleave(legacy_type4, legacy_type3, CODED_BITS, 11);
    tetra_descramble(legacy_type3, CODED_BITS, 3U);
    tetra_hard_bits_to_soft(legacy_type3, legacy_costs, CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, legacy_costs,
                                    CODED_BITS, legacy_mother,
                                    MOTHER_BITS) < 0)
        return fail("legacy-order depuncture failed");
    if (tetra_viterbi_decode_soft(legacy_mother, MOTHER_BITS, legacy_decoded,
                                  TYPE2_BITS) != TYPE2_BITS)
        return fail("legacy-order Viterbi length mismatch");
    if (tetra_crc16_ccitt_bits(legacy_decoded, TYPE1_BITS + 16) == 0x1D0FU)
        return fail("legacy deinterleave-before-descramble order was accepted");

    dsd_opts *opts = (dsd_opts *)calloc(1, sizeof(*opts));
    dsd_state *state = (dsd_state *)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return fail("allocation failed");
    }
    if (!tetra_bsch_parse(decoded, TYPE1_BITS, opts, state)) {
        free(opts);
        free(state);
        return fail("BSCH parse failed");
    }
    if (state->tetra_mcc != 460 || state->tetra_mnc != 4242
        || state->tetra_colour != 17 || state->tetra_tn != 3
        || state->tetra_fn != 9 || state->tetra_mn != 37
        || state->tetra_lfsr_seed != 0x73109247U) {
        free(opts);
        free(state);
        return fail("decoded BSCH fields mismatch");
    }

    uint8_t sys_type5[SYSINFO_CODED_BITS];
    uint8_t sys_type4[SYSINFO_CODED_BITS];
    uint8_t sys_type3[SYSINFO_CODED_BITS];
    uint16_t sys_costs[SYSINFO_CODED_BITS];
    uint16_t sys_mother[SYSINFO_MOTHER_BITS];
    uint8_t sys_decoded[SYSINFO_TYPE2_BITS];
    if (strlen(sysinfo_type5_text) != SYSINFO_CODED_BITS) {
        free(opts);
        free(state);
        return fail("invalid SYSINFO literal length");
    }
    for (int i = 0; i < SYSINFO_CODED_BITS; ++i) {
        if (sysinfo_type5_text[i] != '0' && sysinfo_type5_text[i] != '1') {
            free(opts);
            free(state);
            return fail("invalid SYSINFO literal bit");
        }
        sys_type5[i] = (uint8_t)(sysinfo_type5_text[i] - '0');
    }

    memcpy(sys_type4, sys_type5, sizeof(sys_type4));
    tetra_descramble(sys_type4, SYSINFO_CODED_BITS, state->tetra_lfsr_seed);
    tetra_block_deinterleave(sys_type4, sys_type3, SYSINFO_CODED_BITS, 101);
    tetra_hard_bits_to_soft(sys_type3, sys_costs, SYSINFO_CODED_BITS);
    if (tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, sys_costs,
                                    SYSINFO_CODED_BITS, sys_mother,
                                    SYSINFO_MOTHER_BITS) < 0) {
        free(opts);
        free(state);
        return fail("SYSINFO depuncture failed");
    }
    if (tetra_viterbi_decode_soft(sys_mother, SYSINFO_MOTHER_BITS, sys_decoded,
                                  SYSINFO_TYPE2_BITS) != SYSINFO_TYPE2_BITS) {
        free(opts);
        free(state);
        return fail("SYSINFO Viterbi length mismatch");
    }
    if (tetra_crc16_ccitt_bits(sys_decoded, SYSINFO_TYPE1_BITS + 16)
        != 0x1D0FU) {
        free(opts);
        free(state);
        return fail("SYSINFO CRC mismatch");
    }
    tetra_mac_parse_schd(sys_decoded, SYSINFO_TYPE1_BITS, 1, opts, state);

    const int ok = state->tetra_sysinfo_known == 1
                   && state->tetra_sysinfo_main_carrier == 321
                   && state->tetra_freq_band == 4
                   && state->tetra_freq_offset == 1
                   && state->tetra_duplex_spacing == 5
                   && state->tetra_num_csch == 2
                   && state->tetra_ms_txpwr_max == 6
                   && state->tetra_rxlev_access_min == 9
                   && state->tetra_cck_valid == 1
                   && state->tetra_cck_id == 0xBEEFU
                   && state->tetra_la == 777
                   && state->tetra_subscr_class == 0x1234U
                   && state->tetra_bs_service_det == 0x567U;
    free(opts);
    free(state);
    return ok ? 0 : fail("decoded SYSINFO fields mismatch");
}
