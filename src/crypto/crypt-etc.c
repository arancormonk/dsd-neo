// SPDX-License-Identifier: ISC
#include <ctype.h>
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

static char*
trim_ascii_ws(char* s) {
    if (s == NULL) {
        return NULL;
    }
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

static int
hex_nibble_value(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int
parse_hex_bytes_strict(const char* input, uint8_t* out, size_t out_cap, size_t* out_len) {
    if (input == NULL || out == NULL || out_cap == 0 || out_len == NULL) {
        return 0;
    }

    *out_len = 0;
    int have_hi = 0;
    int hi = 0;

    for (const unsigned char* p = (const unsigned char*)input; *p != '\0'; p++) {
        if (isspace(*p)) {
            continue;
        }
        int nib = hex_nibble_value((int)*p);
        if (nib < 0) {
            return 0;
        }

        if (!have_hi) {
            hi = nib;
            have_hi = 1;
        } else {
            if (*out_len >= out_cap) {
                return 0;
            }
            out[(*out_len)++] = (uint8_t)((hi << 4) | nib);
            have_hi = 0;
            hi = 0;
        }
    }

    if (have_hi) {
        if (*out_len >= out_cap) {
            return 0;
        }
        out[(*out_len)++] = (uint8_t)(hi << 4);
    }

    return (*out_len > 0U) ? 1 : 0;
}

static void
unpack_bytes_to_bits(const uint8_t* input, uint8_t* output, int len) {
    int k = 0;
    for (int i = 0; i < len; i++) {
        output[k++] = (uint8_t)((input[i] >> 7) & 1U);
        output[k++] = (uint8_t)((input[i] >> 6) & 1U);
        output[k++] = (uint8_t)((input[i] >> 5) & 1U);
        output[k++] = (uint8_t)((input[i] >> 4) & 1U);
        output[k++] = (uint8_t)((input[i] >> 3) & 1U);
        output[k++] = (uint8_t)((input[i] >> 2) & 1U);
        output[k++] = (uint8_t)((input[i] >> 1) & 1U);
        output[k++] = (uint8_t)((input[i] >> 0) & 1U);
    }
}

int
dmr_parse_static_keystream_spec(const char* input, uint8_t out_bits[882], int* out_mod, int* out_frame_mode,
                                int* out_frame_off, int* out_frame_step, char* err, size_t err_cap) {
    if (out_bits == NULL || out_mod == NULL || out_frame_mode == NULL || out_frame_off == NULL
        || out_frame_step == NULL) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "internal parser argument error");
        }
        return 0;
    }

    if (err != NULL && err_cap > 0) {
        err[0] = '\0';
    }

    *out_mod = 0;
    *out_frame_mode = 0;
    *out_frame_off = 0;
    *out_frame_step = 0;
    memset(out_bits, 0, 882 * sizeof(uint8_t));

    if (input == NULL || input[0] == '\0') {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "keystream spec is empty");
        }
        return 0;
    }

    char spec[512];
    (void)snprintf(spec, sizeof spec, "%s", input);

    char* saveptr = NULL;
    char* len_tok = dsd_strtok_r(spec, ":", &saveptr);
    char* hex_tok = dsd_strtok_r(NULL, ":", &saveptr);
    char* off_tok = dsd_strtok_r(NULL, ":", &saveptr);
    char* step_tok = dsd_strtok_r(NULL, ":", &saveptr);
    char* extra_tok = dsd_strtok_r(NULL, ":", &saveptr);

    if (len_tok == NULL || hex_tok == NULL) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "expected bits:hex[:offset[:step]]");
        }
        return 0;
    }
    if (extra_tok != NULL) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "too many ':' fields (max 4)");
        }
        return 0;
    }

    len_tok = trim_ascii_ws(len_tok);
    hex_tok = trim_ascii_ws(hex_tok);
    if (off_tok != NULL) {
        off_tok = trim_ascii_ws(off_tok);
    }
    if (step_tok != NULL) {
        step_tok = trim_ascii_ws(step_tok);
    }

    uint32_t parsed_len = 0;
    if (parse_decimal_u32_strict(len_tok, &parsed_len) != 1 || parsed_len == 0 || parsed_len > 882U) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "length must be decimal 1..882 bits");
        }
        return 0;
    }

    if (hex_tok == NULL || hex_tok[0] == '\0') {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "missing keystream hex bytes");
        }
        return 0;
    }

    uint32_t frame_off = 0;
    uint32_t frame_step = 0;
    int frame_mode = 0;

    if (off_tok != NULL && off_tok[0] != '\0') {
        frame_mode = 1;
        if (parse_decimal_u32_strict(off_tok, &frame_off) != 1) {
            if (err != NULL && err_cap > 0) {
                (void)snprintf(err, err_cap, "offset must be decimal bits");
            }
            return 0;
        }
        if (step_tok != NULL && step_tok[0] != '\0') {
            if (parse_decimal_u32_strict(step_tok, &frame_step) != 1) {
                if (err != NULL && err_cap > 0) {
                    (void)snprintf(err, err_cap, "step must be decimal bits");
                }
                return 0;
            }
        } else {
            frame_step = 49U;
        }
    } else if (step_tok != NULL && step_tok[0] != '\0') {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "step requires offset");
        }
        return 0;
    }

    uint8_t ks_bytes[112];
    memset(ks_bytes, 0, sizeof(ks_bytes));
    size_t parsed_hex_bytes = 0;
    if (parse_hex_bytes_strict(hex_tok, ks_bytes, sizeof(ks_bytes), &parsed_hex_bytes) != 1) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "invalid hex bytes for keystream");
        }
        return 0;
    }

    uint8_t ks_unpacked[896];
    memset(ks_unpacked, 0, sizeof(ks_unpacked));
    uint16_t unpack_len = (uint16_t)(parsed_len / 8U);
    if ((parsed_len % 8U) != 0U) {
        unpack_len++;
    }
    if ((size_t)unpack_len > parsed_hex_bytes) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "hex bytes shorter than requested bit length");
        }
        return 0;
    }
    unpack_bytes_to_bits(ks_bytes, ks_unpacked, unpack_len);
    for (uint32_t i = 0; i < parsed_len; i++) {
        out_bits[i] = (uint8_t)(ks_unpacked[i] & 1U);
    }

    int mod = (int)parsed_len;
    if (frame_mode == 1) {
        frame_off %= parsed_len;
        frame_step %= parsed_len;
    }
    *out_mod = mod;
    *out_frame_mode = frame_mode;
    *out_frame_off = (int)frame_off;
    *out_frame_step = (int)frame_step;
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

    uint8_t parsed_bits[882];
    int parsed_mod = 0;
    int parsed_frame_mode = 0;
    int parsed_frame_off = 0;
    int parsed_frame_step = 0;
    char err[128];
    if (dmr_parse_static_keystream_spec(input, parsed_bits, &parsed_mod, &parsed_frame_mode, &parsed_frame_off,
                                        &parsed_frame_step, err, sizeof err)
        != 1) {
        if (err[0] != '\0') {
            fprintf(stderr, "Straight KS parse failure (%s)\n", err);
        }
        fprintf(stderr, "Straight KS String Malformed! No KS Created!\n");
        return;
    }

    for (int i = 0; i < parsed_mod; i++) {
        state->static_ks_bits[0][i] = parsed_bits[i];
        state->static_ks_bits[1][i] = parsed_bits[i];
    }

    fprintf(stderr, "AMBE Straight XOR %d-bit Keystream: ", parsed_mod);
    int unpack_len = parsed_mod / 8;
    if ((parsed_mod % 8) != 0) {
        unpack_len++;
    }
    for (int i = 0; i < unpack_len; i++) {
        uint8_t out = 0;
        for (int j = 0; j < 8; j++) {
            const int bi = (i * 8) + j;
            out = (uint8_t)(out << 1);
            if (bi < parsed_mod) {
                out |= (uint8_t)(parsed_bits[bi] & 1U);
            }
        }
        fprintf(stderr, "%02X", out);
    }
    if (parsed_frame_mode == 1) {
        fprintf(stderr, " with Frame Align (offset=%d, step=%d)", parsed_frame_off, parsed_frame_step);
    }
    fprintf(stderr, " with Forced Application \n");

    state->straight_ks = 1;
    state->straight_mod = parsed_mod;
    state->straight_frame_mode = parsed_frame_mode;
    state->straight_frame_off = parsed_frame_off;
    state->straight_frame_step = parsed_frame_step;
}

static void
xor_keystream_bits_frame49(const uint8_t* ks_bits, int mod, int frame_mode, int frame_off, int frame_step, int* counter,
                           char ambe_d[49]) {
    if (ks_bits == NULL || counter == NULL || ambe_d == NULL || mod <= 0) {
        return;
    }

    int base = 0;
    if (frame_mode == 1) {
        uint32_t frame_ctr = (uint32_t)(*counter);
        (*counter)++;
        uint32_t off = (uint32_t)((frame_off >= 0) ? frame_off : 0);
        uint32_t step = (uint32_t)((frame_step >= 0) ? frame_step : 0);
        off %= (uint32_t)mod;
        step %= (uint32_t)mod;
        const uint64_t mod_u64 = (uint64_t)(uint32_t)mod;
        const uint64_t advance = (((uint64_t)frame_ctr) * ((uint64_t)step)) % mod_u64;
        base = (int)((((uint64_t)off) + advance) % mod_u64);
    } else {
        base = (*counter) % mod;
        if (base < 0) {
            base += mod;
        }
        *counter += 49;
    }

    for (int i = 0; i < 49; i++) {
        int idx = (base + i) % mod;
        ambe_d[i] ^= (char)(ks_bits[idx] & 1U);
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
    xor_keystream_bits_frame49(state->static_ks_bits[slot], state->straight_mod, state->straight_frame_mode,
                               state->straight_frame_off, state->straight_frame_step, &state->static_ks_counter[slot],
                               ambe_d);
}

static int
vertex_key_map_find_index(const dsd_state* state, unsigned long long key) {
    if (state == NULL || state->vertex_ks_count <= 0) {
        return -1;
    }
    const int count = (state->vertex_ks_count > DSD_VERTEX_KS_MAP_MAX) ? DSD_VERTEX_KS_MAP_MAX : state->vertex_ks_count;
    for (int i = 0; i < count; i++) {
        if (state->vertex_ks_key[i] == key && state->vertex_ks_mod[i] > 0) {
            return i;
        }
    }
    return -1;
}

int
vertex_key_map_apply_frame49(dsd_state* state, int slot, unsigned long long key, char ambe_d[49]) {
    if (state == NULL || ambe_d == NULL) {
        return 0;
    }

    slot = (slot == 1) ? 1 : 0;
    int idx = -1;
    const int active = state->vertex_ks_active_idx[slot];
    if (active >= 0 && active < state->vertex_ks_count && active < DSD_VERTEX_KS_MAP_MAX
        && state->vertex_ks_key[active] == key && state->vertex_ks_mod[active] > 0) {
        idx = active;
    } else {
        idx = vertex_key_map_find_index(state, key);
        if (idx < 0) {
            return 0;
        }
        state->vertex_ks_active_idx[slot] = idx;
        state->vertex_ks_counter[slot] = 0;
    }

    xor_keystream_bits_frame49(state->vertex_ks_bits[idx], state->vertex_ks_mod[idx], state->vertex_ks_frame_mode[idx],
                               state->vertex_ks_frame_off[idx], state->vertex_ks_frame_step[idx],
                               &state->vertex_ks_counter[slot], ambe_d);
    return 1;
}
