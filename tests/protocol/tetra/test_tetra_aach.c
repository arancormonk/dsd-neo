// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The access assignment channel (AACH) of a normal downlink burst: the shortened (30,14) Reed-Muller code of
 * EN 300 392-2 clause 8.2.3.2 and the ACCESS-ASSIGN PDU of clause 21.4.7.2 it carries.
 *
 * - AACH words received off air from MCC 228, MNC 8889 (tests/fixtures/iq/tetra_call_real_air_72k.iq), descrambled,
 *   decode to what that cell sent: common control on its main control channel; on a call's timeslot the traffic usage
 *   marker 51 the MAC-RESOURCE assigned, and assigned control between the call's talkers, two of those words with bit
 *   errors the code corrects. They come off air, not from an encoder, so an encoder and decoder that share a wrong
 *   generator fail here.
 * - The code: systematic, minimum distance 8 over every nonzero codeword, an error of up to 3 bits corrected, and an
 *   error of 4 refused rather than corrected to another codeword.
 * - The downlink usage of table 21.82: header 00 is common control; otherwise field 1 is the downlink usage marker,
 *   0 unallocated, 1 assigned control, 2 common control, 3 common and assigned control, any other a traffic usage
 *   marker.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <stdint.h>
#include <stdio.h>

#include "tetra_mac_pdu.h"

enum { RM_BITS = 30, RM_INFO_BITS = 14 };

static uint32_t
word_of(const char* text) {
    uint32_t w = 0;
    for (int i = 0; i < RM_BITS; i++) {
        assert(text[i] == '0' || text[i] == '1');
        w = (w << 1) | (uint32_t)(text[i] - '0');
    }
    assert(text[RM_BITS] == '\0');
    return w;
}

static int
weight(uint32_t w) {
    int n = 0;
    for (; w != 0U; w &= w - 1U) {
        n++;
    }
    return n;
}

typedef struct {
    const char* received; /* descrambled type-2 bits b2(1) to b2(30) */
    unsigned header;
    unsigned field1;
    unsigned field2;
    int corrected;
    tetra_dl_usage usage;
    unsigned usage_marker;
} real_air_word;

/* Frames 1 to 17 of the fixture: TN1 is the main control channel, TN2 the call's traffic channel. */
static const real_air_word k_real_air[] = {
    /* TN1: common control, access fields 9 and 9. */
    {"000010010010010100001010101111", 0U, 9U, 9U, 0, TETRA_DL_COMMON_CONTROL, 0U},
    /* TN2 while the call's speech is sent: traffic usage marker 51 both ways. */
    {"111100111100111011010101101101", 3U, 51U, 51U, 0, TETRA_DL_TRAFFIC, 51U},
    {"111100111100111011010101101001", 3U, 51U, 51U, 1, TETRA_DL_TRAFFIC, 51U},
    /* TN2, downlink traffic 51 with the uplink assigned only. */
    {"101100110010010111000111000111", 2U, 51U, 9U, 0, TETRA_DL_TRAFFIC, 51U},
    /* TN2 between the talkers: assigned control (SCH/F), the uplink assigned only, or carrying traffic 51. */
    {"100000010010010100000111110101", 2U, 1U, 9U, 0, TETRA_DL_ASSIGNED_CONTROL, 0U},
    {"100000010010010101000111110101", 2U, 1U, 9U, 1, TETRA_DL_ASSIGNED_CONTROL, 0U},
    {"100000010010010101000111100101", 2U, 1U, 9U, 2, TETRA_DL_ASSIGNED_CONTROL, 0U},
    {"110000011100111000010101011111", 3U, 1U, 51U, 0, TETRA_DL_ASSIGNED_CONTROL, 0U},
};

static void
test_real_air_words(void) {
    for (size_t i = 0; i < sizeof k_real_air / sizeof k_real_air[0]; i++) {
        const real_air_word* w = &k_real_air[i];
        uint16_t pdu = 0xFFFFU;
        const int corrected = tetra_rm3014_decode(word_of(w->received), &pdu);
        assert(corrected == w->corrected);
        assert(pdu == (uint16_t)((w->header << 12) | (w->field1 << 6) | w->field2));
        uint8_t um = 0xFFU;
        assert(tetra_access_assign_dl_usage(pdu, &um) == w->usage);
        assert(um == w->usage_marker);
        /* What the decoder corrected is a codeword: re-encoding gives the received word back but for those bits. */
        assert(weight(tetra_rm3014_encode(pdu) ^ word_of(w->received)) == w->corrected);
    }
}

static void
test_code_distance(void) {
    int min_weight = RM_BITS;
    for (uint32_t info = 0; info < (1U << RM_INFO_BITS); info++) {
        const uint32_t cw = tetra_rm3014_encode((uint16_t)info);
        /* Systematic: b2(1) to b2(14) are the type-1 bits. */
        assert((cw >> 16) == info && cw < (1U << RM_BITS));
        if (info != 0U && weight(cw) < min_weight) {
            min_weight = weight(cw);
        }
        uint16_t back = 0xFFFFU;
        assert(tetra_rm3014_decode(cw, &back) == 0 && back == info);
    }
    assert(min_weight == 8);
}

/* Every error of one, two and three bits on a codeword is corrected to it. */
static void
check_corrects(uint16_t info) {
    const uint32_t cw = tetra_rm3014_encode(info);
    for (int a = 0; a < RM_BITS; a++) {
        for (int b = a; b < RM_BITS; b++) {
            for (int c = b; c < RM_BITS; c++) {
                const uint32_t e = (1U << a) | (1U << b) | (1U << c);
                uint16_t back = 0xFFFFU;
                assert(tetra_rm3014_decode(cw ^ e, &back) == weight(e) && back == info);
            }
        }
    }
}

/* Every error of four bits on a codeword is refused: no other codeword lies within 3 bits of it. */
static void
check_refuses_four(uint16_t info) {
    const uint32_t cw = tetra_rm3014_encode(info);
    for (int a = 0; a < RM_BITS; a++) {
        for (int b = a + 1; b < RM_BITS; b++) {
            for (int c = b + 1; c < RM_BITS; c++) {
                for (int d = c + 1; d < RM_BITS; d++) {
                    const uint32_t e = (1U << a) | (1U << b) | (1U << c) | (1U << d);
                    uint16_t back = 0x1234U;
                    assert(tetra_rm3014_decode(cw ^ e, &back) == -1 && back == 0x1234U);
                }
            }
        }
    }
}

static void
test_error_correction(void) {
    check_corrects(0x0000U);
    check_corrects(0x3CF3U); /* header 11, 51, 51 */
    check_corrects(0x2049U); /* header 10, 1, 9 */
    check_refuses_four(0x2049U);
    uint16_t back = 0;
    assert(tetra_rm3014_decode(0xFFFFFFFFU, &back) == -1); /* more than 30 bits */
    assert(tetra_rm3014_decode(tetra_rm3014_encode(0x2049U), NULL) == 0);
}

static void
test_downlink_usage(void) {
    for (unsigned header = 0; header < 4U; header++) {
        for (unsigned field1 = 0; field1 < 64U; field1++) {
            const uint16_t pdu = (uint16_t)((header << 12) | (field1 << 6) | 0x2AU);
            uint8_t um = 0xFFU;
            const tetra_dl_usage usage = tetra_access_assign_dl_usage(pdu, &um);
            tetra_dl_usage want = TETRA_DL_TRAFFIC;
            if (header == 0U || field1 == 2U) {
                want = TETRA_DL_COMMON_CONTROL;
            } else if (field1 == 0U) {
                want = TETRA_DL_UNALLOCATED;
            } else if (field1 == 1U) {
                want = TETRA_DL_ASSIGNED_CONTROL;
            } else if (field1 == 3U) {
                want = TETRA_DL_COMMON_ASSIGNED_CONTROL;
            }
            assert(usage == want);
            assert(um == (want == TETRA_DL_TRAFFIC ? field1 : 0U));
        }
    }
    assert(tetra_access_assign_dl_usage(0x3CF3U, NULL) == TETRA_DL_TRAFFIC);
}

int
main(void) {
    test_real_air_words();
    test_code_distance();
    test_error_correction();
    test_downlink_usage();
    DSD_FPRINTF(stdout, "TETRA_AACH: OK\n");
    return 0;
}
