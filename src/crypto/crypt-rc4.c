// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * rc4.c         Crypthings
 * RC4 Alg
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/rc4.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void
rc4_voice_decrypt(int drop, uint8_t keylength, uint8_t messagelength, const uint8_t key[], const uint8_t cipher[],
                  uint8_t plain[]) {
    int i, j;
    unsigned int count;
    uint8_t t;

    if (drop < 0) {
        drop = 0;
    }

    //init Sbox
    uint8_t S[256];
    for (i = 0; i < 256; i++) {
        S[i] = i;
    }

    //Key Scheduling
    j = 0;
    for (i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % keylength]) % 256;
        t = S[i];
        S[i] = S[j];
        S[j] = t;
    }

    //Drop Bytes and Cipher Byte XOR
    i = j = 0;
    unsigned int total = (unsigned int)messagelength + (unsigned int)drop;
    for (count = 0; count < total; count++) {
        i = (i + 1) % 256;
        j = (j + S[i]) % 256;
        t = S[i];
        S[i] = S[j];
        S[j] = t;
        if (count >= (unsigned int)drop) {
            uint8_t b = S[(S[i] + S[j]) % 256];
            plain[count - drop] = b ^ cipher[count - drop];
        }
    }
}

//this is for PDU usage
void
rc4_block_output(int drop, int keylen, int meslen, const uint8_t* key, uint8_t* output_blocks) {
    int i, j, x;
    unsigned int count;
    unsigned int keylength = (unsigned int)keylen;
    unsigned int messagelength = (unsigned int)meslen;
    unsigned int S[256];

    if (drop < 0) {
        drop = 0;
    }

    for (i = 0; i < 256; i++) {
        S[i] = i;
    }

    j = 0;
    for (i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % keylength]) % 256;
        unsigned int temp = S[i];
        S[i] = S[j];
        S[j] = temp;
    }

    //Generate Keystream
    i = 0;
    j = 0;
    x = 0;
    unsigned int total = messagelength + (unsigned int)drop;
    for (count = 0; count < total; count++) {
        i = (i + 1) % 256;
        j = (j + S[i]) % 256;
        unsigned int temp = S[i];
        S[i] = S[j];
        S[j] = temp;
        //Collect Output blocks
        if (count >= (unsigned int)drop) {
            unsigned int byte = S[(S[i] + S[j]) % 256];
            output_blocks[x++] = byte;
        }
    }
}

//This is now verified to work after changing the drop byte value from 256 to 0.
//also, had to change the application to not skip the additional 7 bits like DMRA or P25 does.
void
hytera_enhanced_rc4_setup(dsd_opts* opts, dsd_state* state, unsigned long long int key_value,
                          unsigned long long int mi_value) {

    UNUSED(opts);
    uint8_t key[5];
    DSD_MEMSET(key, 0, sizeof(key));
    uint8_t kiv[5];
    DSD_MEMSET(kiv, 0, sizeof(kiv));
    uint8_t mi[5];
    DSD_MEMSET(mi, 0, sizeof(mi));
    uint8_t ks[135];
    DSD_MEMSET(ks, 0, sizeof(ks));

    //load key_value into key array
    key[0] = ((key_value & 0xFF00000000) >> 32UL);
    key[1] = ((key_value & 0xFF000000) >> 24);
    key[2] = ((key_value & 0xFF0000) >> 16);
    key[3] = ((key_value & 0xFF00) >> 8);
    key[4] = ((key_value & 0xFF) >> 0);

    //load mi_value into mi array
    mi[0] = ((mi_value & 0xFF00000000) >> 32UL);
    mi[1] = ((mi_value & 0xFF000000) >> 24);
    mi[2] = ((mi_value & 0xFF0000) >> 16);
    mi[3] = ((mi_value & 0xFF00) >> 8);
    mi[4] = ((mi_value & 0xFF) >> 0);

    //pointer to the ks_octet storage
    uint8_t* ks_octets;
    if (state->currentslot == 0) {
        ks_octets = state->ks_octetL;
    } else {
        ks_octets = state->ks_octetR;
    }

    //NOTE: Drop Byte value is 0
    rc4_block_output(0, 5, 135, key, ks);

    for (int i = 0; i < 5; i++) {
        kiv[i] = key[i] ^ mi[i];
    }

    for (int i = 0; i < 135; i++) {
        ks_octets[i] = kiv[i % 5] ^ ks[i];
    }

    //debug
    // {
    // }

    //NULL pointer to ks_octets
    ks_octets = NULL;

    //end line break
}
