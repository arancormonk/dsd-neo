// SPDX-License-Identifier: ISC
#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"
#include "pc4_internal.h"
#include "vendor_ap_key_parse.h"

//NOTE: This mode DOES NOT work over a repeater, simplex only
//repeaters may or will attempt to correct the frame errors
void
tyt16_ambe2_codeword_keystream(const dsd_state* state, char ambe_fr[4][24], int fnum) {

    char interleaved[72];
    DSD_MEMSET(interleaved, 0, sizeof(interleaved));

    //interleave the frame
    for (int8_t i = 0; i < DSD_AMBE_2450_DIBITS; i++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];
        interleaved[(i * 2) + 0] = ambe_fr[map->high_row][map->high_col];
        interleaved[(i * 2) + 1] = ambe_fr[map->low_row][map->low_col];
    }

    uint8_t ks_bytes[10];
    DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));
    uint8_t ks[80];
    DSD_MEMSET(ks, 0, sizeof(ks));

    ks_bytes[0] = (state->H >> 8) & 0xFF;
    ks_bytes[1] = (state->H >> 0) & 0xFF;

    //copy same bytes into rest of byte array
    for (int16_t i = 2; i < 10; i++) {
        ks_bytes[i] = ks_bytes[i % 2];
    }

    //convert byte array into a bit array
    unpack_byte_array_into_bit_array(ks_bytes, ks, 10);

    //set ks idx position (-1)
    int idx = 0;
    if (fnum == 0) {
        idx = 79;
    } else {
        idx = 71;
    }

    //apply keystream to interleave
    for (int8_t i = 0; i < 72; i++) {
        interleaved[i] ^= ks[idx--];
    }

    //deinterleave back into ambe_fr frame
    int k = 0;
    for (int8_t i = 0; i < DSD_AMBE_2450_DIBITS; i++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];
        ambe_fr[map->high_row][map->high_col] = interleaved[k++];
        ambe_fr[map->low_row][map->low_col] = interleaved[k++];
    }
}

void
tyt_ap_pc4_keystream_creation(dsd_state* state, const char* input, int show_keys) {
    if (state == NULL || input == NULL) {
        return;
    }

    dsd_vendor_ap_key parsed;
    const int parse_rc = dsd_vendor_ap_key_parse(input, &parsed);
    if (parse_rc != DSD_VENDOR_AP_KEY_OK) {
        DSD_FPRINTF(stderr, "DMR TYT AP (PC4) key parse failed: expected 32 or 64 hex characters\n");
        state->tyt_ap = 0;
        return;
    }

    if (parsed.nhex == 64U) {
        pc4_tyt_set_key(parsed.hex, parsed.nhex);

        uint8_t key_bytes[32];
        char key_text[65];
        DSD_MEMSET(key_bytes, 0, sizeof(key_bytes));
        (void)dsd_parse_hex_bytes_exact((const char*)parsed.hex, parsed.nhex, key_bytes, sizeof(key_bytes));
        DSD_FPRINTF(stderr, "DMR TYT AP (PC4) 256-bit key loaded with forced application: %s\n",
                    dsd_secret_format_byte_hex(key_text, sizeof key_text, show_keys, key_bytes, sizeof(key_bytes)));
    } else {
        unsigned char key1[16];
        DSD_MEMSET(key1, 0, sizeof(key1));
        unsigned char key2[16];
        DSD_MEMSET(key2, 0, sizeof(key2));

        if (dsd_parse_hex_bytes_exact((const char*)parsed.hex, parsed.nhex, key1, sizeof(key1)) != 0) {
            DSD_FPRINTF(stderr, "DMR TYT AP (PC4) key parse failed: invalid 128-bit key\n");
            state->tyt_ap = 0;
            return;
        }
        for (int i = 0; i < 16; i++) {
            key2[i] = key1[15 - i];
        }

        pc4_tyt_set_key(key2, sizeof(key2));

        char key_text[33];
        DSD_FPRINTF(stderr, "DMR TYT AP (PC4) 128-bit key loaded with forced application: %s\n",
                    dsd_secret_format_byte_hex(key_text, sizeof key_text, show_keys, key1, sizeof(key1)));
    }
    state->tyt_ap = 1;
}

void
tyt_ep_aes_keystream_creation(dsd_state* state, const char* input, int show_keys) {
    char buf[1024];
    DSD_SNPRINTF(buf, sizeof(buf), "%s", input);

    char* pEnd;
    unsigned long long int K1 = strtoull(buf, &pEnd, 16);
    unsigned long long int K2 = strtoull(pEnd, &pEnd, 16);

    //
    uint8_t static_key[32];
    DSD_MEMSET(static_key, 0, sizeof(static_key));

    //static key value
    static_key[0] = 0x6e;
    static_key[1] = 0x02;
    static_key[2] = 0x8d;
    static_key[3] = 0x8a;
    static_key[4] = 0xca;
    static_key[5] = 0xeb;
    static_key[6] = 0x9b;
    static_key[7] = 0xbe;
    static_key[8] = 0x42;
    static_key[9] = 0x72;
    static_key[10] = 0xfb;
    static_key[11] = 0x82;
    static_key[12] = 0x64;
    static_key[13] = 0x56;
    static_key[14] = 0x31;
    static_key[15] = 0xfa;

    //the key value provided by user
    uint8_t user_key[16];
    DSD_MEMSET(user_key, 0, sizeof(user_key));

    //Load user key into array to manipulate
    for (int i = 0; i < 8; i++) {
        user_key[i + 0] = (K1 >> (56 - (i * 8))) & 0xFF;
        user_key[i + 8] = (K2 >> (56 - (i * 8))) & 0xFF;
    }

    uint8_t input_register[16];
    DSD_MEMSET(input_register, 0, sizeof(input_register));

    //manipulate user provided key by loading bytes in reverse order into the input_register
    for (int i = 0; i < 16; i++) {
        input_register[15 - i] = user_key[i];
    }

    uint8_t ks_bytes[16];
    DSD_MEMSET(ks_bytes, 0, sizeof(ks_bytes));

    //create keystream
    aes_ofb_keystream_output(input_register, static_key, ks_bytes, DSD_AES_KEY_128, 1);
    uint8_t ks_bits[128];
    DSD_MEMSET(ks_bits, 0, sizeof(ks_bits));
    unpack_byte_array_into_bit_array(ks_bytes, ks_bits, 16);

    pc4_tyt_set_static_keystream(ks_bits);

    const unsigned long long segments[2] = {K1, K2};
    char key_text[34];
    DSD_FPRINTF(stderr, "DMR TYT EP (AES-128) key loaded with forced application: %s\n",
                dsd_secret_format_u64_segments(key_text, sizeof key_text, show_keys, segments, 2U));
    state->tyt_ep = 1;
}

int
tyt_ap_pc4_apply_frame49(const dsd_state* state, char ambe_d[49]) {
    if (state == NULL || ambe_d == NULL || state->tyt_ap != 1) {
        return 0;
    }
    if (dmr_ambe49_should_skip_crypto(ambe_d) == 1) {
        return 0;
    }

    short frame1_cipher[49];
    for (int i = 0; i < 49; i++) {
        frame1_cipher[i] = (short)(unsigned char)ambe_d[i];
    }
    pc4_tyt_decrypt_frame49(frame1_cipher);

    DSD_MEMSET(ambe_d, 0, 49 * sizeof(char));
    for (int i = 0; i < 49; i++) {
        ambe_d[i] = (char)(frame1_cipher[i] & 1);
    }
    return 1;
}

int
tyt_ep_aes_apply_frame49(const dsd_state* state, char ambe_d[49]) {
    if (state == NULL || ambe_d == NULL || state->tyt_ep != 1) {
        return 0;
    }
    if (dmr_ambe49_should_skip_crypto(ambe_d) == 1) {
        return 0;
    }

    pc4_tyt_apply_static_keystream(ambe_d);
    return 1;
}
