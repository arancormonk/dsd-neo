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

/* One-time initialized ISCH lookup table (P25 (40,9,16) codewords) */
static inline const std::unordered_map<uint64_t, int>&
isch_table() {
    static const std::unordered_map<uint64_t, int> m = {
        {0x184229d461ULL, 0},   {0x18761451f6ULL, 1},   {0x181ae27e2fULL, 2},   {0x182edffbb8ULL, 3},
        {0x18df8a7510ULL, 4},   {0x18ebb7f087ULL, 5},   {0x188741df5eULL, 6},   {0x18b37c5ac9ULL, 7},
        {0x1146a44f13ULL, 8},   {0x117299ca84ULL, 9},   {0x111e6fe55dULL, 10},  {0x112a5260caULL, 11},
        {0x11db07ee62ULL, 12},  {0x11ef3a6bf5ULL, 13},  {0x1183cc442cULL, 14},  {0x11b7f1c1bbULL, 15},
        {0x1a4a2e239eULL, 16},  {0x1a7e13a609ULL, 17},  {0x1a12e589d0ULL, 18},  {0x1a26d80c47ULL, 19},
        {0x1ad78d82efULL, 20},  {0x1ae3b00778ULL, 21},  {0x1a8f4628a1ULL, 22},  {0x1abb7bad36ULL, 23},
        {0x134ea3b8ecULL, 24},  {0x137a9e3d7bULL, 25},  {0x13166812a2ULL, 26},  {0x1322559735ULL, 27},
        {0x13d300199dULL, 28},  {0x13e73d9c0aULL, 29},  {0x138bcbb3d3ULL, 30},  {0x13bff63644ULL, 31},
        {0x1442f705efULL, 32},  {0x1476ca8078ULL, 33},  {0x141a3cafa1ULL, 34},  {0x142e012a36ULL, 35},
        {0x14df54a49eULL, 36},  {0x14eb692109ULL, 37},  {0x14879f0ed0ULL, 38},  {0x14b3a28b47ULL, 39},
        {0x1d467a9e9dULL, 40},  {0x1d72471b0aULL, 41},  {0x1d1eb134d3ULL, 42},  {0x1d2a8cb144ULL, 43},
        {0x1ddbd93fecULL, 44},  {0x1defe4ba7bULL, 45},  {0x1d831295a2ULL, 46},  {0x1db72f1035ULL, 47},
        {0x164af0f210ULL, 48},  {0x167ecd7787ULL, 49},  {0x16123b585eULL, 50},  {0x162606ddc9ULL, 51},
        {0x16d7535361ULL, 52},  {0x16e36ed6f6ULL, 53},  {0x168f98f92fULL, 54},  {0x16bba57cb8ULL, 55},
        {0x1f4e7d6962ULL, 56},  {0x1f7a40ecf5ULL, 57},  {0x1f16b6c32cULL, 58},  {0x1f228b46bbULL, 59},
        {0x1fd3dec813ULL, 60},  {0x1fe7e34d84ULL, 61},  {0x1f8b15625dULL, 62},  {0x1fbf28e7caULL, 63},
        {0x084d62c339ULL, 64},  {0x08795f46aeULL, 65},  {0x0815a96977ULL, 66},  {0x082194ece0ULL, 67},
        {0x08d0c16248ULL, 68},  {0x08e4fce7dfULL, 69},  {0x08880ac806ULL, 70},  {0x08bc374d91ULL, 71},
        {0x0149ef584bULL, 72},  {0x017dd2dddcULL, 73},  {0x011124f205ULL, 74},  {0x0125197792ULL, 75},
        {0x01d44cf93aULL, 76},  {0x01e0717cadULL, 77},  {0x018c875374ULL, 78},  {0x01b8bad6e3ULL, 79},
        {0x0a456534c6ULL, 80},  {0x0a7158b151ULL, 81},  {0x0a1dae9e88ULL, 82},  {0x0a29931b1fULL, 83},
        {0x0ad8c695b7ULL, 84},  {0x0aecfb1020ULL, 85},  {0x0a800d3ff9ULL, 86},  {0x0ab430ba6eULL, 87},
        {0x0341e8afb4ULL, 88},  {0x0375d52a23ULL, 89},  {0x03192305faULL, 90},  {0x032d1e806dULL, 91},
        {0x03dc4b0ec5ULL, 92},  {0x03e8768b52ULL, 93},  {0x038480a48bULL, 94},  {0x03b0bd211cULL, 95},
        {0x044dbc12b7ULL, 96},  {0x0479819720ULL, 97},  {0x041577b8f9ULL, 98},  {0x04214a3d6eULL, 99},
        {0x04d01fb3c6ULL, 100}, {0x04e4223651ULL, 101}, {0x0488d41988ULL, 102}, {0x04bce99c1fULL, 103},
        {0x0d493189c5ULL, 104}, {0x0d7d0c0c52ULL, 105}, {0x0d11fa238bULL, 106}, {0x0d25c7a61cULL, 107},
        {0x0dd49228b4ULL, 108}, {0x0de0afad23ULL, 109}, {0x0d8c5982faULL, 110}, {0x0db864076dULL, 111},
        {0x0645bbe548ULL, 112}, {0x06718660dfULL, 113}, {0x061d704f06ULL, 114}, {0x06294dca91ULL, 115},
        {0x06d8184439ULL, 116}, {0x06ec25c1aeULL, 117}, {0x0680d3ee77ULL, 118}, {0x06b4ee6be0ULL, 119},
        {0x0f41367e3aULL, 120}, {0x0f750bfbadULL, 121}, {0x0f19fdd474ULL, 122}, {0x0f2dc051e3ULL, 123},
        {0x0fdc95df4bULL, 124}, {0x0fe8a85adcULL, 125}, {0x0f845e7505ULL, 126}, {0x0fb063f092ULL, 127},
        {0x575d57f7ffULL, -2}}; // S-ISCH
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
