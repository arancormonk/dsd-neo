// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * ez.cpp
 * EZPWD R-S bridge and ISCH map lookup
 *
 * original copyrights for portions used below (OP25, EZPWD)
 *
 * LWVMOBILE
 * 2022-09 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <climits>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/ez.h>
#include <dsd-neo/fec/p25p2_isch.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ezpwd/rs"

namespace {

std::vector<uint8_t>*
ess_a_instance(void) {
    static std::vector<uint8_t>* instance = []() noexcept -> std::vector<uint8_t>* {
        try {
            return new std::vector<uint8_t>(28, 0);
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

std::vector<uint8_t>*
ess_b_instance(void) {
    static std::vector<uint8_t>* instance = []() noexcept -> std::vector<uint8_t>* {
        try {
            return new std::vector<uint8_t>(16, 0);
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

ezpwd::RS<63, 35>*
rs28_instance(void) {
    static ezpwd::RS<63, 35>* instance = []() noexcept -> ezpwd::RS<63, 35>* {
        try {
            return new ezpwd::RS<63, 35>();
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

enum {
    RS28_LOAD = 35,  /* RS(63,35) data symbols */
    RS28_ROOTS = 28, /* RS(63,35) parity symbols */
    RS28_SIZE = 63,
};

/*
 * FACCH and SACCH are RS(63,35) codes, shortened at the front and punctured at the back. In full RS(63,35)
 * coordinates the shortened symbols 0..pad-1 are zero and never sent, the payload is pad..34, the sent parity is
 * 35..35+n_parity-1 and the remaining parity is punctured (not sent). ezpwd decodes the shortened code natively:
 * given only the n_payload data symbols it treats the pad as known zero and fails any decode that would correct a
 * pad symbol. Its symbol index is the full index minus the pad.
 */
struct AcchLayout {
    int pad;
    int n_payload;
    int n_parity;
};

const AcchLayout kFacchLayout = {9, 26, 19};
const AcchLayout kSacchLayout = {5, 30, 22};

void
bits_to_hexbits(const int* bits, int n_hexbits, uint8_t* out) {
    for (int i = 0; i < n_hexbits; i++) {
        uint8_t hexbit = 0;
        for (int j = 0; j < 6; j++) {
            hexbit = (uint8_t)((hexbit << 1) | (bits[(i * 6) + j] & 1));
        }
        out[i] = hexbit;
    }
}

void
hexbits_to_bits(const uint8_t* hexbits, int n_hexbits, int* bits) {
    for (int i = 0; i < n_hexbits; i++) {
        for (int j = 0; j < 6; j++) {
            bits[(i * 6) + j] = (hexbits[i] >> (5 - j)) & 1;
        }
    }
}

int
count_changed(const uint8_t* before, const uint8_t* after, int n) {
    int changed = 0;
    for (int i = 0; i < n; i++) {
        changed += (before[i] != after[i]) ? 1 : 0;
    }
    return changed;
}

/*
 * Erasures in full RS(63,35) coordinates to ezpwd's: shortened symbols are known, so a shortened, out-of-range or
 * repeated position is dropped. At most RS28_ROOTS positions are kept.
 */
int
acch_native_erasures(const AcchLayout& layout, const int* erasures, int n_erasures, unsigned* native) {
    int n_native = 0;
    for (int i = 0; erasures != nullptr && i < n_erasures && n_native < RS28_ROOTS; i++) {
        if (erasures[i] < layout.pad || erasures[i] >= RS28_SIZE) {
            continue;
        }
        unsigned position = (unsigned)(erasures[i] - layout.pad);
        int repeated = 0;
        for (int j = 0; j < n_native; j++) {
            repeated |= (native[j] == position) ? 1 : 0;
        }
        if (!repeated) {
            native[n_native++] = position;
        }
    }
    return n_native;
}

/* The punctured parity symbols, in ezpwd's coordinates. */
int
acch_punctured_erasures(const AcchLayout& layout, unsigned* native) {
    int n_native = 0;
    for (int full = RS28_LOAD + layout.n_parity; full < RS28_SIZE; full++) {
        native[n_native++] = (unsigned)(full - layout.pad);
    }
    return n_native;
}

bool
is_erasure(unsigned position, const unsigned* erasures, int n_erasures) {
    for (int i = 0; i < n_erasures; i++) {
        if (erasures[i] == position) {
            return true;
        }
    }
    return false;
}

/*
 * The sent symbols, erasures aside, in which the received word differs from a codeword (its payload @p data and sent
 * parity @p check). ezpwd's coordinates: payload 0..n_payload-1, sent parity from n_payload on.
 */
int
acch_unerased_distance(const AcchLayout& layout, const uint8_t* received_data, const uint8_t* received_parity,
                       const uint8_t* data, const uint8_t* check, const unsigned* erasures, int n_erasures) {
    int distance = 0;
    for (int i = 0; i < layout.n_payload; i++) {
        if (received_data[i] != data[i] && !is_erasure((unsigned)i, erasures, n_erasures)) {
            distance++;
        }
    }
    for (int j = 0; j < layout.n_parity; j++) {
        if (received_parity[j] != check[j] && !is_erasure((unsigned)(layout.n_payload + j), erasures, n_erasures)) {
            distance++;
        }
    }
    return distance;
}

/*
 * Decode one shortened codeword with ezpwd's erasure positions. On success the payload bits take the corrected
 * payload and the return value is the number of sent symbols (payload and parity) the decode changed; on failure
 * nothing is written and the return value is < 0.
 *
 * A decode is accepted only when the codeword its payload encodes to lies within 2 * errors + erasures <= 28 of the
 * received word, the radius inside which a codeword is unique. ezpwd's own verdict is not enough: its
 * Berlekamp-Massey occasionally decodes past that radius (for FACCH, about 1.5% of words with 10 errors and the 9
 * punctured erasures), and now and then (a few random words in 1e7, issue #804) it reports a decode whose payload
 * and corrected parity form no codeword at all, 19 or more symbols from the received word. Callers take a passing
 * decode as proof of the burst, so the codeword is rebuilt from the payload and measured against what was received.
 */
int
acch_decode(const AcchLayout& layout, int* payload, const int* parity, const unsigned* erasures, int n_erasures) {
    ezpwd::RS<63, 35>* rs = rs28_instance();
    if (rs == nullptr) {
        return -2;
    }
    uint8_t data[RS28_LOAD] = {0};
    uint8_t parity_hb[RS28_ROOTS] = {0};
    bits_to_hexbits(payload, layout.n_payload, data);
    bits_to_hexbits(parity, layout.n_parity, parity_hb);
    uint8_t received_data[RS28_LOAD];
    uint8_t received_parity[RS28_ROOTS];
    DSD_MEMCPY(received_data, data, sizeof(received_data));
    DSD_MEMCPY(received_parity, parity_hb, sizeof(received_parity));
    unsigned positions[RS28_ROOTS] = {0}; /* ezpwd reads the erasures here and writes the correction positions */
    DSD_MEMCPY(positions, erasures, (size_t)n_erasures * sizeof(positions[0]));

    if (rs->decode(data, (unsigned)layout.n_payload, parity_hb, positions, (unsigned)n_erasures) < 0) {
        return -1;
    }
    uint8_t check[RS28_ROOTS] = {0};
    rs->encode(data, (unsigned)layout.n_payload, check);
    const int distance =
        acch_unerased_distance(layout, received_data, received_parity, data, check, erasures, n_erasures);
    if ((2 * distance) + n_erasures > RS28_ROOTS) {
        return -1;
    }
    hexbits_to_bits(data, layout.n_payload, payload);
    return count_changed(received_data, data, layout.n_payload)
           + count_changed(received_parity, check, layout.n_parity);
}

int
acch_decode_list(const AcchLayout& layout, int* payload, const int* parity, const int* erasures, int n_erasures) {
    unsigned native[RS28_ROOTS];
    int n_native = acch_native_erasures(layout, erasures, n_erasures, native);
    return acch_decode(layout, payload, parity, native, n_native);
}

int
acch_decode_fixed(const AcchLayout& layout, int* payload, const int* parity) {
    unsigned native[RS28_ROOTS];
    int n_native = acch_punctured_erasures(layout, native);
    return acch_decode(layout, payload, parity, native, n_native);
}

void
acch_encode(const AcchLayout& layout, const int* payload, int* parity) {
    ezpwd::RS<63, 35>* rs = rs28_instance();
    if (rs == nullptr) {
        /* No codec (its allocation failed at first use): all-zero parity, as documented in ez.h. */
        for (int i = 0; i < layout.n_parity * 6; i++) {
            parity[i] = 0;
        }
        return;
    }
    uint8_t data[RS28_LOAD] = {0};
    uint8_t parity_hb[RS28_ROOTS] = {0};
    bits_to_hexbits(payload, layout.n_payload, data);
    rs->encode(data, (unsigned)layout.n_payload, parity_hb);
    hexbits_to_bits(parity_hb, layout.n_parity, parity);
}

} // namespace

/**
 * Reed-Solomon correction of an ESS section with optional erasures.
 *
 * ESS uses RS(44,16,29) over GF(64):
 *   - 16 payload hexbits (96 bits from ESS_B)
 *   - 28 parity hexbits (168 bits from ESS_A)
 *
 * The ezpwd RS<63,35> decoder maps ESS_B to positions 0-15 and ESS_A to 16-43.
 *
 * @param payload   96-bit payload array (ESS_B, converted to 16 hexbits).
 * @param parity    168-bit parity array (ESS_A, converted to 28 hexbits).
 * @param erasures  Erasure position array (RS symbol positions 0-43).
 * @param n_erasures Number of erasures (must be <= 28).
 * @return Number of symbols corrected (erasures that held the right value not counted), or < 0 on failure.
 */
int
ez_rs28_ess(int payload[96], int parity[168], const int* erasures, int n_erasures) {
    int ec = -2;
    int i, j, k;
    uint8_t b;
    std::vector<uint8_t>* ess_a = ess_a_instance();
    std::vector<uint8_t>* ess_b = ess_b_instance();
    if (ess_a == nullptr || ess_b == nullptr) {
        return ec;
    }
    ezpwd::RS<63, 35>* rs = rs28_instance();
    if (rs == nullptr) {
        return ec;
    }

    std::vector<int> ErasuresDyn;
    for (i = 0; erasures != nullptr && i < n_erasures && i < 28; i++) {
        ErasuresDyn.push_back(erasures[i]);
    }

    /* Convert payload bits to hexbits (16 hexbits = 96 bits) */
    k = 0;
    for (i = 0; i < 16; i++) {
        b = 0;
        for (j = 0; j < 6; j++) {
            b = b << 1;
            b = b + payload[k];
            k++;
        }
        (*ess_b)[i] = b;
    }

    /* Convert parity bits to hexbits (28 hexbits = 168 bits) */
    k = 0;
    for (i = 0; i < 28; i++) {
        uint8_t a = 0;
        for (j = 0; j < 6; j++) {
            a = a << 1;
            a = a + parity[k];
            k++;
        }
        (*ess_a)[i] = a;
    }

    /* Decode with erasures. ezpwd counts every erasure as a correction; count the symbols it changed instead, the
       unit of the FACCH and SACCH decoders. */
    uint8_t received_b[16];
    uint8_t received_a[28];
    DSD_MEMCPY(received_b, ess_b->data(), sizeof(received_b));
    DSD_MEMCPY(received_a, ess_a->data(), sizeof(received_a));
    ec = rs->decode(*ess_b, *ess_a, ErasuresDyn);
    if (ec >= 0) {
        ec = count_changed(received_b, ess_b->data(), 16) + count_changed(received_a, ess_a->data(), 28);
    }

    /* Convert ESS_B back to bits */
    k = 0;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 6; j++) {
            b = ((*ess_b)[i] >> (5 - j)) & 0x1;
            payload[k] = b;
            k++;
        }
    }

    return ec;
}

/**
 * Reed-Solomon correction of a FACCH section: RS(63,35) shortened by 9 (0..8) and punctured by 9 (54..62).
 *
 * @param payload   156-bit payload array (RS symbols 9..34).
 * @param parity    114-bit sent parity array (RS symbols 35..53).
 * @param erasures  Erasure positions in full RS(63,35) coordinates; shortened ones (0..8) are ignored.
 * @param n_erasures Number of erasures.
 * @return Number of sent symbols corrected, or < 0 on failure.
 */
int
ez_rs28_facch(int payload[156], int parity[114], const int* erasures, int n_erasures) {
    return acch_decode_list(kFacchLayout, payload, parity, erasures, n_erasures);
}

/**
 * Reed-Solomon correction of a SACCH section: RS(63,35) shortened by 5 (0..4) and punctured by 6 (57..62).
 *
 * @param payload   180-bit payload array (RS symbols 5..34).
 * @param parity    132-bit sent parity array (RS symbols 35..56).
 * @param erasures  Erasure positions in full RS(63,35) coordinates; shortened ones (0..4) are ignored.
 * @param n_erasures Number of erasures.
 * @return Number of sent symbols corrected, or < 0 on failure.
 */
int
ez_rs28_sacch(int payload[180], int parity[132], const int* erasures, int n_erasures) {
    return acch_decode_list(kSacchLayout, payload, parity, erasures, n_erasures);
}

int
ez_rs28_facch_fixed(int payload[156], int parity[114]) {
    return acch_decode_fixed(kFacchLayout, payload, parity);
}

int
ez_rs28_sacch_fixed(int payload[180], int parity[132]) {
    return acch_decode_fixed(kSacchLayout, payload, parity);
}

void
ez_rs28_facch_encode(const int payload[156], int parity[114]) {
    acch_encode(kFacchLayout, payload, parity);
}

void
ez_rs28_sacch_encode(const int payload[180], int parity[132]) {
    acch_encode(kSacchLayout, payload, parity);
}

namespace {

enum {
    P2_SUPERFRAME_BITS = 4320, /* the scrambling sequence restarts every superframe */
    P2_TIMESLOT_BITS = 360,
    P2_SEQUENCE_START = 20, /* a timeslot's burst bit 0 takes sequence bit 20 + 360 * T */
    P2_CODE_BITS = 44,
};

/* Where a burst carries its codeword bits, in order (payload, then sent parity): runs of consecutive burst bits around
   the DUID and, for FACCH, the gap at 138..179. The layouts p25p2_frame.c gathers FACCH and SACCH by. */
struct BitRun {
    int first;
    int count;
};

const BitRun kFacchRuns[] = {{2, 72}, {76, 62}, {180, 22}, {202, 42}, {246, 72}};
const BitRun kSacchRuns[] = {{2, 72}, {76, 108}, {184, 60}, {246, 72}};

/*
 * The superframe sequence as masks over the code: sequence bit j is the parity of (code & masks[j]). The scrambler
 * (p25p2_generate_scramble_bits()) is a Fibonacci LFSR, x^44 + x^34 + x^20 + x^15 + x^9 + x^4 + 1, loaded with the code
 * and read from its bit 43; each register bit is tracked as the code bits it XORs.
 */
void
seed_sequence_masks(uint64_t* masks) {
    uint64_t reg[P2_CODE_BITS];
    for (int i = 0; i < P2_CODE_BITS; i++) {
        reg[i] = 1ULL << i;
    }
    for (int j = 0; j < P2_SUPERFRAME_BITS; j++) {
        masks[j] = reg[43];
        const uint64_t feedback = reg[33] ^ reg[19] ^ reg[14] ^ reg[8] ^ reg[3] ^ reg[43];
        for (int i = P2_CODE_BITS - 1; i > 0; i--) {
            reg[i] = reg[i - 1];
        }
        reg[0] = feedback;
    }
}

void
seed_set_bit(uint64_t* row, int bit) {
    row[bit / 64] |= 1ULL << (bit % 64);
}

int
seed_get_bit(const uint64_t* row, int bit) {
    return (int)((row[bit / 64] >> (bit % 64)) & 1ULL);
}

/* The check rows of one code: parity bit i of a codeword is the XOR of the payload bits whose unit payload encodes to a
   parity with bit i set, so that XOR plus parity bit i is 0. */
void
seed_check_rows(ez_p25p2_seed_tables* tables, int kind, const AcchLayout& layout) {
    const int n_payload = layout.n_payload * 6;
    const int n_parity = layout.n_parity * 6;
    int parity[RS28_ROOTS * 6] = {0};
    for (int j = 0; j < n_payload; j++) {
        int unit[RS28_LOAD * 6] = {0};
        unit[j] = 1;
        acch_encode(layout, unit, parity);
        for (int i = 0; i < n_parity; i++) {
            if (parity[i] != 0) {
                seed_set_bit(tables->check[kind][i], j);
            }
        }
    }
    for (int i = 0; i < n_parity; i++) {
        seed_set_bit(tables->check[kind][i], n_payload + i);
    }
}

void
seed_burst_bits(ez_p25p2_seed_tables* tables, int kind, const BitRun* runs, size_t n_runs) {
    int k = 0;
    for (size_t r = 0; r < n_runs; r++) {
        for (int i = 0; i < runs[r].count; i++) {
            tables->burst_bit[kind][k++] = (uint16_t)(runs[r].first + i);
        }
    }
}

/* The seed rows of one code: what each check row XORs of the scrambling bits, at every superframe slot. */
void
seed_code_rows(ez_p25p2_seed_tables* tables, int kind, const uint64_t* masks) {
    const int n_bits = tables->payload_bits[kind] + tables->checks[kind];
    for (int t = 0; t < EZ_P25P2_SEED_SLOTS; t++) {
        const int start = P2_SEQUENCE_START + (P2_TIMESLOT_BITS * t);
        for (int i = 0; i < tables->checks[kind]; i++) {
            uint64_t row = 0;
            for (int k = 0; k < n_bits; k++) {
                if (seed_get_bit(tables->check[kind][i], k) != 0) {
                    row ^= masks[(start + tables->burst_bit[kind][k]) % P2_SUPERFRAME_BITS];
                }
            }
            tables->seed[kind][t][i] = row;
        }
    }
}

void
seed_tables_kind(ez_p25p2_seed_tables* tables, int kind, const AcchLayout& layout, const BitRun* runs, size_t n_runs,
                 const uint64_t* masks) {
    tables->payload_bits[kind] = layout.n_payload * 6;
    tables->checks[kind] = layout.n_parity * 6;
    seed_burst_bits(tables, kind, runs, n_runs);
    seed_check_rows(tables, kind, layout);
    seed_code_rows(tables, kind, masks);
}

} // namespace

const ez_p25p2_seed_tables*
ez_p25p2_seed_tables_get(void) {
    static const ez_p25p2_seed_tables* tables = []() noexcept -> const ez_p25p2_seed_tables* {
        if (rs28_instance() == nullptr) {
            return nullptr;
        }
        try {
            ez_p25p2_seed_tables* built = new ez_p25p2_seed_tables();
            seed_sequence_masks(built->sequence);
            seed_tables_kind(built, EZ_P25P2_SEED_FACCH, kFacchLayout, kFacchRuns,
                             sizeof(kFacchRuns) / sizeof(kFacchRuns[0]), built->sequence);
            seed_tables_kind(built, EZ_P25P2_SEED_SACCH, kSacchLayout, kSacchRuns,
                             sizeof(kSacchRuns) / sizeof(kSacchRuns[0]), built->sequence);
            return built;
        } catch (...) {
            return nullptr;
        }
    }();
    return tables;
}

/* One-time initialized ISCH lookup table (P25 (40,9,16) codewords, <dsd-neo/fec/p25p2_isch.h>) */
static inline const std::unordered_map<uint64_t, int>&
isch_table() {
    static const std::unordered_map<uint64_t, int> m = []() {
        std::unordered_map<uint64_t, int> built;
        for (int i = 0; i < DSD_P25P2_I_ISCH_CODEWORDS; i++) {
            built.emplace(dsd_p25p2_i_isch_codewords[i], i);
        }
        built.emplace(DSD_P25P2_S_ISCH, -2); // S-ISCH
        return built;
    }();
    return m;
}

//I-ISCH Lookup, borrowed from OP25 (now w/ error correction)
int
isch_lookup(uint64_t isch) {
    const auto& m = isch_table();
    auto it = m.find(isch);
    if (it != m.end()) {
        return it->second; // exact match
    }
    // No exact match: find smallest Hamming distance, correct up to 7 bits
    int decoded = -2;
    int popmin = 40;
    for (const auto& kv : m) {
        int popct = dsd_popcount64(isch ^ kv.first);
        if ((popct <= 7) && (popct < popmin)) {
            decoded = kv.second;
            popmin = popct;
        }
    }
    return decoded;
}

static int
isch_weighted_mismatch_cost(uint64_t received, uint64_t candidate, const uint8_t reliab40[40]) {
    int cost = 0;
    uint64_t diff = received ^ candidate;
    for (int i = 0; i < 40; i++) {
        uint64_t mask = 1ULL << (39 - i);
        if ((diff & mask) != 0) {
            cost += (int)reliab40[i];
        }
    }
    return cost;
}

int
isch_lookup_soft(uint64_t isch, const uint8_t reliab40[40]) {
    const auto& m = isch_table();
    auto it = m.find(isch);
    if (it != m.end()) {
        return it->second; // exact hard matches remain authoritative
    }
    if (reliab40 == 0) {
        return isch_lookup(isch);
    }

    int decoded = -2;
    int best_cost = INT_MAX;
    int best_popct = 40;
    for (const auto& kv : m) {
        int popct = dsd_popcount64(isch ^ kv.first);
        if (popct > 7) {
            continue;
        }
        int cost = isch_weighted_mismatch_cost(isch, kv.first, reliab40);
        if (cost < best_cost || (cost == best_cost && popct < best_popct)
            || (cost == best_cost && popct == best_popct && kv.second < decoded)) {
            decoded = kv.second;
            best_cost = cost;
            best_popct = popct;
        }
    }
    return decoded;
}
