// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#include <dsd-neo/fec/BCH_63_16.hpp>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>

// Ideas taken from http://op25.osmocom.org/trac/wiki.png/browser/op25/gr-op25/lib/decoder_ff_impl.cc
// See also p25_training_guide.pdf page 48.
// See also tia-102-baaa-a-project_25-fdma-common_air_interface.pdf page 40.

static BCH_63_16_11 bch;

/**
 * @brief Valid DUID values from TIA-102.BAAA-A Table 8-4.
 *
 * The 4-bit DUID field encodes the frame type. Only 7 of the 16 possible
 * values are defined by the standard; all others indicate a BCH miscorrection
 * or protocol error. This table provides O(1) validation indexed directly by
 * the 4-bit DUID value (0x0–0xF).
 *
 * Valid DUIDs:
 *   0x0 = HDU  (Header Data Unit)
 *   0x3 = TDU  (Simple Terminator Data Unit)
 *   0x5 = LDU1 (Logical Link Data Unit 1)
 *   0x7 = TSDU (Trunking Signaling Data Unit)
 *   0xA = LDU2 (Logical Link Data Unit 2)
 *   0xC = PDU  (Packet Data Unit)
 *   0xF = TDULC (Terminator Data Unit with Link Control)
 */
static const bool DUID_VALID[16] = {
    true,  false, false, true,  /* 0=HDU, 1=inv, 2=inv, 3=TDU */
    false, true,  false, true,  /* 4=inv, 5=LDU1, 6=inv, 7=TSDU */
    false, false, true,  false, /* 8=inv, 9=inv, A=LDU2, B=inv */
    true,  false, false, true   /* C=PDU, D=inv, E=inv, F=TDULC */
};

/**
 * Convenience class to calculate the parity of the DUID values. Keeps a table with the expected outcomes
 * for fast lookup.
 */
class ParityTable {
  private:
    unsigned char table[16];

    unsigned char
    get_index(unsigned char x, unsigned char y) {
        return (x << 2) + y;
    }

  public:
    ParityTable() {
        for (unsigned int i = 0; i < sizeof(table); i++) {
            table[i] = 0;
        }
        table[get_index(1, 1)] = 1;
        table[get_index(2, 2)] = 1;
    }

    unsigned char
    get_value(unsigned char x, unsigned char y) {
        return table[get_index(x, y)];
    }

} parity_table;

/**
 * @brief Decode and validate a P25 NID codeword with correction count reporting.
 *
 * Performs BCH(63,16,23) error correction on the 63-bit NID codeword,
 * validates the decoded DUID against the set of defined frame types
 * (TIA-102.BAAA-A Table 8-4), and checks the parity bit for consistency.
 * The final parity bit is not part of the BCH(63,16) codeword. Match
 * sdrtrunk's P25 Phase 1 NID handling by accepting a parity disagreement
 * after BCH correction succeeds and the decoded DUID is a defined primary
 * DUID. The caller still receives NID_PARITY_OVERRIDE for diagnostics.
 *
 * @param bch_code    Input: 63 bytes, each containing one bit of the NID.
 * @param new_nac     Output: decoded 12-bit NAC value after error correction.
 * @param new_duid    Output: 3-char buffer for the decoded DUID string (e.g., "11").
 * @param parity      Input: the 64th parity bit read from the air interface.
 * @param error_count Output: number of BCH errors corrected (valid when result > 0).
 * @return NidResult code: NID_OK (1), NID_PARITY_OVERRIDE (2),
 *         or NID_DECODE_FAIL (0).
 */
int
check_NID_with_error_count(char* bch_code, int* new_nac, char* new_duid, unsigned char parity, int* error_count) {

    // Decode using local BCH implementation
    char decoded[16];
    BCH_63_16_Result bch_result = bch.decode(bch_code, decoded);

    if (!bch_result.success) {
        // BCH decode failed (>11 errors or Chien search mismatch)
        *error_count = 0;
        return NID_DECODE_FAIL;
    }

    // Report the number of corrected errors to the caller
    *error_count = bch_result.error_count;

    // Extract the NAC from the decoded output. It's a 12-bit number
    // starting from position 0 (MSB first).
    int nac = 0;
    for (int i = 0; i < 12; i++) {
        nac <<= 1;
        nac |= (int)decoded[i];
    }
    *new_nac = nac;

    // Extract the DUID from positions 12–15. The DUID is represented as
    // two dibit characters for compatibility with the dispatch layer.
    unsigned char new_duid_0 = (((int)decoded[12]) << 1) + ((int)decoded[13]);
    unsigned char new_duid_1 = (((int)decoded[14]) << 1) + ((int)decoded[15]);
    new_duid[0] = new_duid_0 + '0';
    new_duid[1] = new_duid_1 + '0';
    new_duid[2] = 0; // Null terminate

    // Validate the decoded DUID against the set of defined frame types.
    // A DUID not in the valid set indicates a BCH miscorrection artifact.
    unsigned char duid_value = (new_duid_0 << 2) | new_duid_1;
    if (!DUID_VALID[duid_value]) {
        *error_count = 0;
        return NID_DECODE_FAIL;
    }

    // Check the parity bit against the expected value for this DUID.
    // Per TIA-102.BAAA-A Table 8-4: P=1 for LDU1 (0x5) and LDU2 (0xA),
    // P=0 for all other defined DUIDs.
    unsigned char expected_parity = parity_table.get_value(new_duid_0, new_duid_1);

    if (expected_parity == parity) {
        // BCH decoded, valid DUID, parity matches - full success
        return NID_OK;
    }

    // Parity disagrees. The final parity bit is outside the BCH-protected
    // codeword, so accept the corrected NID and expose the condition to the
    // caller for diagnostics.
    return NID_PARITY_OVERRIDE;
}

/**
 * @brief Backward-compatible 4-parameter wrapper for check_NID_with_error_count().
 *
 * Calls the full 5-parameter version with a local dummy error_count variable.
 *
 * @param bch_code Input: 63 bytes, each containing one bit of the NID.
 * @param new_nac  Output: decoded 12-bit NAC value.
 * @param new_duid Output: 3-char buffer for the decoded DUID string.
 * @param parity   Input: the 64th parity bit.
 * @return NidResult code (same semantics as the 5-parameter version).
 */
int
check_NID(char* bch_code, int* new_nac, char* new_duid, unsigned char parity) {
    int dummy_error_count;
    return check_NID_with_error_count(bch_code, new_nac, new_duid, parity, &dummy_error_count);
}
