// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#include <dsd-neo/fec/BCH_63_16.hpp>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"

static BCH_63_16_11&
p25p1_bch_instance(void) {
    static BCH_63_16_11 bch;
    return bch;
}

static int decode_nid_codeword(const char* bch_code, int* new_nac, char* new_duid, unsigned char parity,
                               int* error_count, bool* bch_decode_failed);

/**
 * @brief Valid DUID values from TIA-102.BAAA-A Table 8-4.
 *
 * The 4-bit DUID field encodes the frame type. Only 7 of the 16 possible
 * values are defined by the standard; all others indicate a BCH miscorrection
 * or protocol error. This table provides O(1) validation indexed directly by
 * the 4-bit DUID value (0x0-0xF).
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

namespace {
constexpr unsigned char k_duid_parity_table[16] = {
    0, 0, 0, 0, // 0x0..0x3
    0, 1, 0, 0, // 0x4..0x7
    0, 0, 1, 0, // 0x8..0xB
    0, 0, 0, 0  // 0xC..0xF
};

inline unsigned char
duid_parity_value(unsigned char x, unsigned char y) {
    return k_duid_parity_table[(x << 2) + y];
}

} // namespace

static int
received_nac(const char* bch_code) {
    int nac = 0;
    for (int i = 0; i < 12; i++) {
        nac <<= 1;
        nac |= (int)(bch_code[i] ? 1 : 0);
    }
    return nac;
}

static bool
valid_observed_nac(int nac) {
    return nac > 0 && nac <= 0xFFF && nac != 0xFFF;
}

static void
set_received_nac(char* bch_code, int nac) {
    for (int i = 0; i < 12; i++) {
        bch_code[i] = (char)((nac >> (11 - i)) & 1);
    }
}

static int
clamp_reliability(uint8_t reliab) {
    return (int)reliab;
}

static int
score_bit_changes(const char* original, const char* candidate, const uint8_t* reliab, int n) {
    int score = 0;
    for (int i = 0; i < n; i++) {
        if (original[i] != candidate[i]) {
            score += clamp_reliability(reliab[i]);
        }
    }
    return score;
}

static int
count_bit_changes(const char* original, const char* candidate, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (original[i] != candidate[i]) {
            count++;
        }
    }
    return count;
}

static int
candidate_flip_allowed(const char* original, const char* candidate, const uint8_t* reliab, int n) {
    int changed = count_bit_changes(original, candidate, n);
    if (changed == 0) {
        return 1;
    }

    int threshold = p25p1_get_erasure_threshold();
    int score = score_bit_changes(original, candidate, reliab, n);
    return score <= threshold * changed;
}

static int
compare_reliability_index(const uint8_t* reliab, int a, int b) {
    int rel_a = clamp_reliability(reliab[a]);
    int rel_b = clamp_reliability(reliab[b]);
    if (rel_a != rel_b) {
        return rel_a - rel_b;
    }
    return a - b;
}

static int
build_soft_nid_pool(const uint8_t* reliab, int* pool, int max_pool) {
    int order[63];
    for (int i = 0; i < 63; i++) {
        order[i] = i;
    }
    for (int i = 0; i < 63; i++) {
        for (int j = i + 1; j < 63; j++) {
            if (compare_reliability_index(reliab, order[j], order[i]) < 0) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    int threshold = p25p1_get_erasure_threshold();
    int selected[63] = {0};
    int count = 0;
    for (int i = 0; i < 63 && count < max_pool; i++) {
        if (clamp_reliability(reliab[order[i]]) < threshold) {
            pool[count++] = order[i];
            selected[i] = 1;
        }
    }
    for (int i = 0; i < 63 && count < 6; i++) {
        if (!selected[i]) {
            pool[count++] = order[i];
        }
    }
    return count;
}

namespace {

struct SoftNidCandidate {
    int found;
    int result;
    int nac;
    char duid[3];
    int error_count;
    int score;
    int changes;
};

} // namespace

static void
soft_nid_consider_candidate(const char* scoring_code, const char* candidate, const uint8_t* reliab,
                            unsigned char parity, uint8_t parity_reliab, SoftNidCandidate* best) {
    int decoded_nac = 0;
    char decoded_duid[3] = {0};
    int decoded_errors = 0;
    int result = decode_nid_codeword(candidate, &decoded_nac, decoded_duid, parity, &decoded_errors, 0);
    if (result <= 0) {
        return;
    }

    int score = score_bit_changes(scoring_code, candidate, reliab, 63);
    if (result == NID_PARITY_OVERRIDE) {
        score += (int)parity_reliab;
    }
    int changes = count_bit_changes(scoring_code, candidate, 63);

    if (!best->found || score < best->score || (score == best->score && result == NID_OK && best->result != NID_OK)
        || (score == best->score && result == best->result && decoded_errors < best->error_count)
        || (score == best->score && result == best->result && decoded_errors == best->error_count
            && changes < best->changes)) {
        best->found = 1;
        best->result = result;
        best->nac = decoded_nac;
        best->duid[0] = decoded_duid[0];
        best->duid[1] = decoded_duid[1];
        best->duid[2] = decoded_duid[2];
        best->error_count = decoded_errors;
        best->score = score;
        best->changes = changes;
    }
}

static void
soft_nid_search_from_base(const char* scoring_code, const char* base_code, const uint8_t* reliab, const int* pool,
                          int pool_count, unsigned char parity, uint8_t parity_reliab, SoftNidCandidate* best) {
    int max_mask = 1 << pool_count;
    for (int mask = 0; mask < max_mask; mask++) {
        int weight = 0;
        for (int bit = 0; bit < pool_count; bit++) {
            if ((mask & (1 << bit)) != 0) {
                weight++;
            }
        }
        if (weight > 3) {
            continue;
        }

        char candidate[63];
        DSD_MEMCPY(candidate, base_code, 63);
        for (int bit = 0; bit < pool_count; bit++) {
            if ((mask & (1 << bit)) != 0) {
                candidate[pool[bit]] ^= 1;
            }
        }

        if (!candidate_flip_allowed(scoring_code, candidate, reliab, 63)) {
            continue;
        }
        soft_nid_consider_candidate(scoring_code, candidate, reliab, parity, parity_reliab, best);
    }
}

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
static int
decode_nid_codeword(const char* bch_code, int* new_nac, char* new_duid, unsigned char parity, int* error_count,
                    bool* bch_decode_failed) {
    if (bch_decode_failed != 0) {
        *bch_decode_failed = false;
    }

    // Decode using local BCH implementation
    char decoded[16];
    BCH_63_16_Result bch_result = p25p1_bch_instance().decode_with_result(bch_code, decoded);

    if (!bch_result.success) {
        // BCH decode failed (>11 errors or Chien search mismatch)
        *error_count = 0;
        if (bch_decode_failed != 0) {
            *bch_decode_failed = true;
        }
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

    // Extract the DUID from positions 12-15. The DUID is represented as
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
    unsigned char expected_parity = duid_parity_value(new_duid_0, new_duid_1);

    if (expected_parity == parity) {
        // BCH decoded, valid DUID, parity matches - full success
        return NID_OK;
    }

    // Parity disagrees. The final parity bit is outside the BCH-protected
    // codeword, so accept the corrected NID and expose the condition to the
    // caller for diagnostics.
    return NID_PARITY_OVERRIDE;
}

int
check_NID_with_error_count(const char* bch_code, int* new_nac, char* new_duid, unsigned char parity, int* error_count) {
    return decode_nid_codeword(bch_code, new_nac, new_duid, parity, error_count, 0);
}

int
check_NID_with_observed_nac(const char* bch_code, int observed_nac, int* new_nac, char* new_duid, unsigned char parity,
                            int* error_count) {
    bool bch_decode_failed = false;
    int result = decode_nid_codeword(bch_code, new_nac, new_duid, parity, error_count, &bch_decode_failed);
    if (result != NID_DECODE_FAIL || !bch_decode_failed || !valid_observed_nac(observed_nac)
        || received_nac(bch_code) == observed_nac) {
        return result;
    }

    char retry_code[63];
    for (int i = 0; i < 63; i++) {
        retry_code[i] = bch_code[i];
    }
    set_received_nac(retry_code, observed_nac);

    return decode_nid_codeword(retry_code, new_nac, new_duid, parity, error_count, 0);
}

int
check_NID_with_observed_nac_soft(const char* bch_code, const uint8_t* reliab63, int observed_nac, int* new_nac,
                                 char* new_duid, unsigned char parity, uint8_t parity_reliab, int* error_count) {
    int hard_result = check_NID_with_observed_nac(bch_code, observed_nac, new_nac, new_duid, parity, error_count);
    if (hard_result > 0 || reliab63 == 0) {
        return hard_result;
    }

    int pool[8];
    int pool_count = build_soft_nid_pool(reliab63, pool, 8);
    if (pool_count <= 0) {
        return hard_result;
    }

    SoftNidCandidate best = {0, NID_DECODE_FAIL, 0, {0, 0, 0}, 0, 0, 0};
    soft_nid_search_from_base(bch_code, bch_code, reliab63, pool, pool_count, parity, parity_reliab, &best);

    if (valid_observed_nac(observed_nac) && received_nac(bch_code) != observed_nac) {
        char retry_code[63];
        DSD_MEMCPY(retry_code, bch_code, 63);
        set_received_nac(retry_code, observed_nac);
        // Score Chase flips against the trusted NAC rewrite, not the raw received word.
        soft_nid_search_from_base(retry_code, retry_code, reliab63, pool, pool_count, parity, parity_reliab, &best);
    }

    if (!best.found) {
        return hard_result;
    }

    *new_nac = best.nac;
    new_duid[0] = best.duid[0];
    new_duid[1] = best.duid[1];
    new_duid[2] = best.duid[2];
    *error_count = best.error_count;
    return best.result;
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
check_NID(const char* bch_code, int* new_nac, char* new_duid, unsigned char parity) {
    int dummy_error_count;
    return check_NID_with_error_count(bch_code, new_nac, new_duid, parity, &dummy_error_count);
}
