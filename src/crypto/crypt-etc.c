// SPDX-License-Identifier: ISC
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"

static int
parse_decimal_u32_strict(const char* token, uint32_t* out) {
    if (token == NULL || out == NULL || token[0] == '\0') {
        return 0;
    }

    uint64_t value = 0;
    for (const unsigned char* p = (const unsigned char*)token; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        value = (value * 10ULL) + (uint64_t)(*p - '0');
        if (value > UINT32_MAX) {
            return 0;
        }
    }

    *out = (uint32_t)value;
    return 1;
}

void
ken_dmr_scrambler_keystream_creation(dsd_state* state, char* input) {
    /*
  SLOT 1 Protected LC  FLCO=0x00 FID=0x20 <--this link appears to indicate scrambler usage from Kenwood on DMR
  DMR PDU Payload [80][20][40][00][00][01][00][00][01] SB: 00000000000 - 000;

  SLOT 1 TGT=1 SRC=1 FLCO=0x00 FID=0x00 SVC=0x00 Group Call <--different call, no scrambler from same Kenwood Radio
  DMR PDU Payload [00][00][00][00][00][01][00][00][01]

  For This, we could possible transition this to not be enforced
  since we may have a positive indicator in link control, 
  but needs further samples and validation
  */

    int lfsr = 0, bit = 0;
    sscanf(input, "%d", &lfsr);
    fprintf(stderr, "DMR Kenwood 15-bit Scrambler Key %05d with Forced Application\n", lfsr);

    for (int i = 0; i < 882; i++) {
        state->static_ks_bits[0][i] = lfsr & 0x1;
        state->static_ks_bits[1][i] = lfsr & 0x1;
        bit = ((lfsr >> 1) ^ (lfsr >> 0)) & 1;
        lfsr = ((lfsr >> 1) | (bit << 14));
    }

    state->ken_sc = 1;
}

void
anytone_bp_keystream_creation(dsd_state* state, char* input) {
    uint16_t key = 0;
    uint16_t kperm = 0;

    sscanf(input, "%hX", &key);
    key &= 0xFFFF; //truncate to 16-bits

    //calculate key permutation using simple operations
    uint8_t nib1, nib2, nib3, nib4;

    //nib 1 and 3 are simple inversions
    nib1 = ~(key >> 12) & 0xF;
    nib3 = ~(key >> 4) & 0xF;

    //nib 2 and 4 are +8 and mod 16 (& 0xF)
    nib2 = (((key >> 8) & 0xF) + 8) % 16;
    nib4 = (((key >> 0) & 0xF) + 8) % 16;

    //debug
    // fprintf (stderr, "{%01X, %01X, %01X, %01X}", nib1, nib2, nib3, nib4);

    kperm = nib1;
    kperm <<= 4;
    kperm |= nib2;
    kperm <<= 4;
    kperm |= nib3;
    kperm <<= 4;
    kperm |= nib4;

    //load bits into static keystream
    for (int i = 0; i < 16; i++) {
        state->static_ks_bits[0][i] = (kperm >> (15 - i)) & 1;
        state->static_ks_bits[1][i] = (kperm >> (15 - i)) & 1;
    }

    fprintf(stderr, "DMR Anytone Basic 16-bit Key 0x%04X with Forced Application\n", key);
    state->any_bp = 1;
}

void
straight_mod_xor_keystream_creation(dsd_state* state, char* input) {
    if (state == NULL || input == NULL) {
        return;
    }

    /* Reset first so malformed input always disables forced static KS. */
    state->straight_ks = 0;
    state->straight_mod = 0;
    state->straight_frame_mode = 0;
    state->straight_frame_off = 0;
    state->straight_frame_step = 0;
    memset(state->static_ks_counter, 0, sizeof(state->static_ks_counter));

    uint16_t len = 0;
    char* curr;
    char* saveptr = NULL;
    int malformed = 0;
    uint32_t frame_off = 0;
    uint32_t frame_step = 0;
    int frame_mode = 0;
    curr = dsd_strtok_r(input, ":", &saveptr); //should be len (mod) of key (decimal)
    uint32_t parsed_len = 0;
    if (curr != NULL && parse_decimal_u32_strict(curr, &parsed_len) == 1) {
        //continue
    } else {
        malformed = 1;
        goto END_KS;
    }

    //len sanity check: must be 1..882 bits
    if (parsed_len == 0 || parsed_len > 882) {
        fprintf(stderr, "Straight KS length must be 1..882 bits (got %u)\n", (unsigned)parsed_len);
        goto END_KS;
    }
    len = (uint16_t)parsed_len;

    curr = dsd_strtok_r(NULL, ":", &saveptr); //should be key in hex
    if (curr != NULL) {
        //continue
    } else {
        malformed = 1;
        goto END_KS;
    }

    // Optional frame-alignment controls:
    //   -S <len:hex:offset[:step]>
    // offset/step are decimal bit positions. If step omitted, defaults to 49
    // so each 49-bit AMBE frame advances by one frame cadence.
    char* off_tok = dsd_strtok_r(NULL, ":", &saveptr);
    if (off_tok != NULL) {
        frame_mode = 1;
        if (parse_decimal_u32_strict(off_tok, &frame_off) != 1) {
            fprintf(stderr, "Straight KS offset must be decimal bits (got '%s')\n", off_tok);
            malformed = 1;
            goto END_KS;
        }
        char* step_tok = dsd_strtok_r(NULL, ":", &saveptr);
        if (step_tok != NULL) {
            if (parse_decimal_u32_strict(step_tok, &frame_step) != 1) {
                fprintf(stderr, "Straight KS step must be decimal bits (got '%s')\n", step_tok);
                malformed = 1;
                goto END_KS;
            }
        } else {
            frame_step = 49U;
        }

        // reject trailing garbage fields to avoid silent misconfiguration
        if (dsd_strtok_r(NULL, ":", &saveptr) != NULL) {
            fprintf(stderr, "Straight KS accepts at most 4 fields: bits:hex[:offset[:step]]\n");
            malformed = 1;
            goto END_KS;
        }
    }

    uint8_t ks_bytes[112];
    memset(ks_bytes, 0, sizeof(ks_bytes));
    parse_raw_user_string(curr, ks_bytes, sizeof(ks_bytes));

    uint8_t ks_bits[896];
    memset(ks_bits, 0, sizeof(ks_bits));

    uint16_t unpack_len = len / 8;
    if (len % 8) {
        unpack_len++;
    }
    unpack_byte_array_into_bit_array(ks_bytes, ks_bits, unpack_len);

    for (uint16_t i = 0; i < len; i++) {
        state->static_ks_bits[0][i] = ks_bits[i];
        state->static_ks_bits[1][i] = ks_bits[i];
    }

    fprintf(stderr, "AMBE Straight XOR %d-bit Keystream: ", len);
    for (uint16_t i = 0; i < unpack_len; i++) {
        fprintf(stderr, "%02X", ks_bytes[i]);
    }
    if (frame_mode) {
        frame_off %= len;
        frame_step %= len;
        fprintf(stderr, " with Frame Align (offset=%u, step=%u)", frame_off, frame_step);
    }
    fprintf(stderr, " with Forced Application \n");

    state->straight_ks = 1;
    state->straight_mod = (int)len;
    state->straight_frame_mode = frame_mode;
    state->straight_frame_off = (int)frame_off;
    state->straight_frame_step = (int)frame_step;

END_KS:

    if (malformed) {
        fprintf(stderr, "Straight KS String Malformed! No KS Created!\n");
    }
}

void
straight_mod_xor_apply_frame49(dsd_state* state, int slot, char ambe_d[49]) {
    if (state == NULL || ambe_d == NULL) {
        return;
    }
    if (state->straight_ks != 1 || state->straight_mod <= 0) {
        return;
    }

    slot = (slot == 1) ? 1 : 0;
    const int mod = state->straight_mod;
    int base = 0;

    if (state->straight_frame_mode == 1) {
        uint32_t frame_ctr = (uint32_t)state->static_ks_counter[slot]++;
        uint32_t off = (uint32_t)((state->straight_frame_off >= 0) ? state->straight_frame_off : 0);
        uint32_t step = (uint32_t)((state->straight_frame_step >= 0) ? state->straight_frame_step : 0);
        off %= (uint32_t)mod;
        step %= (uint32_t)mod;
        const uint64_t mod_u64 = (uint64_t)(uint32_t)mod;
        const uint64_t advance = (((uint64_t)frame_ctr) * ((uint64_t)step)) % mod_u64;
        base = (int)((((uint64_t)off) + advance) % mod_u64);
    } else {
        base = state->static_ks_counter[slot] % mod;
        if (base < 0) {
            base += mod;
        }
        state->static_ks_counter[slot] += 49;
    }

    for (int i = 0; i < 49; i++) {
        int idx = (base + i) % mod;
        ambe_d[i] ^= (char)(state->static_ks_bits[slot][idx] & 1U);
    }
}
