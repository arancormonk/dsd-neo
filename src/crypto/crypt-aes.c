// SPDX-License-Identifier: Unlicense
/*-------------------------------------------------------------------------------
 * aes.c         Tinier AES
 * Easy To Use (and configurable) AES Output and Utility Bit/Byte Functions
 *
 * Modified Tiny AES code for more variable nk/nr/nb values, all in one file
 * https://github.com/kokke/tiny-AES-c
 *-----------------------------------------------------------------------------*/

/*
  This is free and unencumbered software released into the public domain.

  Anyone is free to copy, modify, publish, use, compile, sell, or
  distribute this software, either in source code form or as a compiled
  binary, for any purpose, commercial or non-commercial, and by any
  means.

  In jurisdictions that recognize copyright laws, the author or authors
  of this software dedicate any and all copyright interest in the
  software to the public domain. We make this dedication for the benefit
  of the public at large and to the detriment of our heirs and
  successors. We intend this dedication to be an overt act of
  relinquishment in perpetuity of all present and future rights to this
  software under copyright law.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
  OTHER DEALINGS IN THE SOFTWARE.

  For more information, please refer to <http://unlicense.org/>

*/

#include <dsd-neo/crypto/aes.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

#define AES_BLOCKLEN        16
#define AES_NB              4U
#define AES_ROUND_KEY_BYTES 240U

typedef struct {
    unsigned nk;
    uint8_t nr;
} aes_params_t;

struct AES_ctx {
    uint8_t RoundKey[AES_ROUND_KEY_BYTES];
};

typedef uint8_t state_t[4][4];

static const uint8_t sbox[256] = {
    //0     1    2      3     4    5     6     7      8    9     A      B    C     D     E     F
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const uint8_t rsbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39,
    0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2,
    0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e, 0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76,
    0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc,
    0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d,
    0x84, 0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c,
    0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41, 0x4f,
    0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73, 0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85,
    0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62,
    0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd,
    0x5a, 0xf4, 0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f, 0x60,
    0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61, 0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6,
    0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

static const uint8_t Rcon[11] = {0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

#define getSBoxValue(num)  (sbox[(num)])
#define getSBoxInvert(num) (rsbox[(num)])

static aes_params_t
aes_params_for_type(int type) {
    if (type == 0) {
        return (aes_params_t){4U, 10U};
    }
    if (type == 1) {
        return (aes_params_t){6U, 12U};
    }
    return (aes_params_t){8U, 14U};
}

static int
aes_key_schedule_params_valid(unsigned Nk, uint8_t Nr, unsigned round_words) {
    return (Nk == 4U || Nk == 6U || Nk == 8U) && (Nr == 10U || Nr == 12U || Nr == 14U)
           && round_words <= (AES_ROUND_KEY_BYTES / 4U) && Nk <= round_words;
}

static void
KeyExpansion(uint8_t* RoundKey, const uint8_t* Key, unsigned Nk, uint8_t Nr) {
    unsigned i;
    uint8_t tempa[4]; // Used for the column/row operations
    const unsigned round_words = AES_NB * ((unsigned)Nr + 1U);

    if (!RoundKey || !Key || !aes_key_schedule_params_valid(Nk, Nr, round_words)) {
        return;
    }

    // The first round key is the key itself.
    for (i = 0; i < Nk; ++i) {
        RoundKey[(i * 4) + 0] = Key[(i * 4) + 0];
        RoundKey[(i * 4) + 1] = Key[(i * 4) + 1];
        RoundKey[(i * 4) + 2] = Key[(i * 4) + 2];
        RoundKey[(i * 4) + 3] = Key[(i * 4) + 3];
    }

    // All other round keys are found from the previous round keys.
    for (i = Nk; i < round_words; ++i) {
        {
            const unsigned k = (i - 1) * 4;
            tempa[0] = RoundKey[k + 0];
            tempa[1] = RoundKey[k + 1];
            tempa[2] = RoundKey[k + 2];
            tempa[3] = RoundKey[k + 3];
        }

        if (i % Nk == 0) {
            // This function shifts the 4 bytes in a word to the left once.
            // [a0,a1,a2,a3] becomes [a1,a2,a3,a0]

            // Function RotWord()
            {
                const uint8_t u8tmp = tempa[0];
                tempa[0] = tempa[1];
                tempa[1] = tempa[2];
                tempa[2] = tempa[3];
                tempa[3] = u8tmp;
            }

            // SubWord() is a function that takes a four-byte input word and
            // applies the S-box to each of the four bytes to produce an output word.

            // Function Subword()
            {
                tempa[0] = getSBoxValue(tempa[0]);
                tempa[1] = getSBoxValue(tempa[1]);
                tempa[2] = getSBoxValue(tempa[2]);
                tempa[3] = getSBoxValue(tempa[3]);
            }

            tempa[0] = tempa[0] ^ Rcon[i / Nk];
        }

        if (Nk == 8) //only run if using AES256 (Nk == 8)
        {
            if (i % Nk == 4) {
                // Function Subword()
                {
                    tempa[0] = getSBoxValue(tempa[0]);
                    tempa[1] = getSBoxValue(tempa[1]);
                    tempa[2] = getSBoxValue(tempa[2]);
                    tempa[3] = getSBoxValue(tempa[3]);
                }
            }
        }

        const unsigned j = i * 4;
        const unsigned k = (i - Nk) * 4;
        RoundKey[j + 0] = RoundKey[k + 0] ^ tempa[0];
        RoundKey[j + 1] = RoundKey[k + 1] ^ tempa[1];
        RoundKey[j + 2] = RoundKey[k + 2] ^ tempa[2];
        RoundKey[j + 3] = RoundKey[k + 3] ^ tempa[3];
    }
}

static void
AddRoundKey(uint8_t round, state_t* state, const uint8_t* RoundKey) {
    uint8_t i, j;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            (*state)[i][j] ^= RoundKey[(round * AES_NB * 4U) + ((unsigned)i * AES_NB) + j];
        }
    }
}

static void
SubBytes(state_t* state) {
    uint8_t i, j;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            (*state)[j][i] = getSBoxValue((*state)[j][i]);
        }
    }
}

static void
ShiftRows(state_t* state) {
    uint8_t temp;

    // Rotate first row 1 columns to left
    temp = (*state)[0][1];
    (*state)[0][1] = (*state)[1][1];
    (*state)[1][1] = (*state)[2][1];
    (*state)[2][1] = (*state)[3][1];
    (*state)[3][1] = temp;

    // Rotate second row 2 columns to left
    temp = (*state)[0][2];
    (*state)[0][2] = (*state)[2][2];
    (*state)[2][2] = temp;

    temp = (*state)[1][2];
    (*state)[1][2] = (*state)[3][2];
    (*state)[3][2] = temp;

    // Rotate third row 3 columns to left
    temp = (*state)[0][3];
    (*state)[0][3] = (*state)[3][3];
    (*state)[3][3] = (*state)[2][3];
    (*state)[2][3] = (*state)[1][3];
    (*state)[1][3] = temp;
}

static uint8_t
xtime(uint8_t x) {
    return ((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static void
MixColumns(state_t* state) {
    uint8_t i;
    for (i = 0; i < 4; ++i) {
        const uint8_t t = (*state)[i][0];
        const uint8_t Tmp = (*state)[i][0] ^ (*state)[i][1] ^ (*state)[i][2] ^ (*state)[i][3];
        uint8_t Tm = (*state)[i][0] ^ (*state)[i][1];
        Tm = xtime(Tm);
        (*state)[i][0] ^= Tm ^ Tmp;
        Tm = (*state)[i][1] ^ (*state)[i][2];
        Tm = xtime(Tm);
        (*state)[i][1] ^= Tm ^ Tmp;
        Tm = (*state)[i][2] ^ (*state)[i][3];
        Tm = xtime(Tm);
        (*state)[i][2] ^= Tm ^ Tmp;
        Tm = (*state)[i][3] ^ t;
        Tm = xtime(Tm);
        (*state)[i][3] ^= Tm ^ Tmp;
    }
}

static uint8_t
aes_multiply(uint8_t x, uint8_t y) {
    uint8_t result = 0;
    while (y != 0) {
        if ((y & 1U) != 0) {
            result ^= x;
        }
        x = xtime(x);
        y >>= 1;
    }
    return result;
}

static void
InvMixColumns(state_t* state) {
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t a = (*state)[i][0];
        const uint8_t b = (*state)[i][1];
        const uint8_t c = (*state)[i][2];
        const uint8_t d = (*state)[i][3];

        (*state)[i][0] = aes_multiply(a, 0x0e) ^ aes_multiply(b, 0x0b) ^ aes_multiply(c, 0x0d) ^ aes_multiply(d, 0x09);
        (*state)[i][1] = aes_multiply(a, 0x09) ^ aes_multiply(b, 0x0e) ^ aes_multiply(c, 0x0b) ^ aes_multiply(d, 0x0d);
        (*state)[i][2] = aes_multiply(a, 0x0d) ^ aes_multiply(b, 0x09) ^ aes_multiply(c, 0x0e) ^ aes_multiply(d, 0x0b);
        (*state)[i][3] = aes_multiply(a, 0x0b) ^ aes_multiply(b, 0x0d) ^ aes_multiply(c, 0x09) ^ aes_multiply(d, 0x0e);
    }
}

static void
InvSubBytes(state_t* state) {
    uint8_t i, j;
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 4; ++j) {
            (*state)[j][i] = getSBoxInvert((*state)[j][i]);
        }
    }
}

static void
InvShiftRows(state_t* state) {
    uint8_t temp;

    temp = (*state)[3][1];
    (*state)[3][1] = (*state)[2][1];
    (*state)[2][1] = (*state)[1][1];
    (*state)[1][1] = (*state)[0][1];
    (*state)[0][1] = temp;

    temp = (*state)[0][2];
    (*state)[0][2] = (*state)[2][2];
    (*state)[2][2] = temp;

    temp = (*state)[1][2];
    (*state)[1][2] = (*state)[3][2];
    (*state)[3][2] = temp;

    temp = (*state)[0][3];
    (*state)[0][3] = (*state)[1][3];
    (*state)[1][3] = (*state)[2][3];
    (*state)[2][3] = (*state)[3][3];
    (*state)[3][3] = temp;
}

// Cipher is the main function that encrypts the PlainText,
// or produces a keystream, depending on application.
static void
Cipher(state_t* state, const uint8_t* RoundKey, uint8_t Nr) {
    uint8_t round = 0;

    // Add the First round key to the state before starting the rounds.
    AddRoundKey(0, state, RoundKey);

    // There will be Nr rounds.
    // The first Nr-1 rounds are identical.
    // These Nr rounds are executed in the loop below.
    // Last one without MixColumns()
    for (round = 1;; ++round) {
        SubBytes(state);
        ShiftRows(state);
        if (round == Nr) {
            break;
        }
        MixColumns(state);
        AddRoundKey(round, state, RoundKey);
    }
    // Add round key to last round
    AddRoundKey(Nr, state, RoundKey);
}

static void
aes_increment_counter_be(uint8_t counter[AES_BLOCKLEN]) {
    if (counter == NULL) {
        return;
    }

    for (int i = AES_BLOCKLEN - 1; i >= 0; i--) {
        counter[i]++;
        if (counter[i] != 0U) {
            break;
        }
    }
}

static void
InvCipher(state_t* state, const uint8_t* RoundKey, uint8_t Nr) {
    AddRoundKey(Nr, state, RoundKey);

    for (uint8_t round = (uint8_t)(Nr - 1U);; --round) {
        InvShiftRows(state);
        InvSubBytes(state);
        AddRoundKey(round, state, RoundKey);
        if (round == 0) {
            break;
        }
        InvMixColumns(state);
    }
}

void
aes_ctr_keystream_output(const uint8_t* counter, const uint8_t* key, uint8_t* output, int type, int nblocks) {
    if (counter == NULL || key == NULL || output == NULL || nblocks <= 0) {
        return;
    }

    aes_params_t params = aes_params_for_type(type);
    struct AES_ctx ctx;
    uint8_t counter_block[AES_BLOCKLEN];

    DSD_MEMSET(ctx.RoundKey, 0, ((size_t)AES_ROUND_KEY_BYTES) * sizeof(uint8_t));
    KeyExpansion(ctx.RoundKey, key, params.nk, params.nr);
    DSD_MEMCPY(counter_block, counter, sizeof(counter_block));

    for (int i = 0; i < nblocks; i++) {
        uint8_t stream[AES_BLOCKLEN];
        const size_t offset = (size_t)i * AES_BLOCKLEN;
        DSD_MEMCPY(stream, counter_block, sizeof(stream));
        Cipher((state_t*)stream, ctx.RoundKey, params.nr);
        DSD_MEMCPY(output + offset, stream, sizeof(stream));
        aes_increment_counter_be(counter_block);
    }
}

void
aes_ctr_xcrypt_bytes(const uint8_t* counter, const uint8_t* key, uint8_t* data, int type, size_t len) {
    if (counter == NULL || key == NULL || data == NULL || len == 0U) {
        return;
    }

    aes_params_t params = aes_params_for_type(type);
    struct AES_ctx ctx;
    uint8_t counter_block[AES_BLOCKLEN];

    DSD_MEMSET(ctx.RoundKey, 0, ((size_t)AES_ROUND_KEY_BYTES) * sizeof(uint8_t));
    KeyExpansion(ctx.RoundKey, key, params.nk, params.nr);
    DSD_MEMCPY(counter_block, counter, sizeof(counter_block));

    for (size_t offset = 0U; offset < len; offset += AES_BLOCKLEN) {
        uint8_t stream[AES_BLOCKLEN];
        DSD_MEMCPY(stream, counter_block, sizeof(stream));
        Cipher((state_t*)stream, ctx.RoundKey, params.nr);

        size_t block_len = len - offset;
        if (block_len > AES_BLOCKLEN) {
            block_len = AES_BLOCKLEN;
        }
        for (size_t i = 0U; i < block_len; i++) {
            data[offset + i] ^= stream[i];
        }
        aes_increment_counter_be(counter_block);
    }
}

//byte-wise output of AES OFB Keystream
//input iv is a 16-byte uint8_t array of initialization vector
//input key is up to 32-byte uint8_t array of key value
//input type is the type/key len of AES required (0-128, 1-192, 2-256)
//input nblocks is the number of rounds of 16-byte keystream output blocks requried
//output is a uint8_t bytewise array, each round filled with 16-bytes from aes keystream output
void
aes_ofb_keystream_output(const uint8_t* iv, const uint8_t* key, uint8_t* output, int type, int nblocks) {

    int i;
    uint8_t input_register[16]; //OFB Input Register
    DSD_MEMSET(input_register, 0, sizeof(input_register));

    aes_params_t params = aes_params_for_type(type);

    struct AES_ctx ctx;

    //load first round of input_register with received IV (OFB First Input Register)
    DSD_MEMCPY(input_register, iv, ((size_t)16) * sizeof(uint8_t));

    //initialize the key variable for the Cipher function
    DSD_MEMSET(ctx.RoundKey, 0, ((size_t)AES_ROUND_KEY_BYTES) * sizeof(uint8_t));
    KeyExpansion(ctx.RoundKey, key, params.nk, params.nr);

    //execute the cipher function, and copy ciphered input_register to output for required number of rounds
    for (i = 0; i < nblocks; i++) {
        Cipher((state_t*)input_register, ctx.RoundKey,
               params.nr); //input_register is returned as output, and is put back in as object feedback
        DSD_MEMCPY(output + ((size_t)i * 16), input_register,
                   ((size_t)16) * sizeof(uint8_t)); //copy ciphered input_register to output
    }
}

void
aes_ecb_decrypt_blocks(const uint8_t* input, const uint8_t* key, uint8_t* output, int type, int nblocks) {
    if (input == NULL || key == NULL || output == NULL || nblocks <= 0) {
        return;
    }

    aes_params_t params = aes_params_for_type(type);
    struct AES_ctx ctx;
    DSD_MEMSET(ctx.RoundKey, 0, ((size_t)AES_ROUND_KEY_BYTES) * sizeof(uint8_t));
    KeyExpansion(ctx.RoundKey, key, params.nk, params.nr);

    for (int i = 0; i < nblocks; i++) {
        uint8_t block[AES_BLOCKLEN];
        const size_t offset = (size_t)i * AES_BLOCKLEN;
        DSD_MEMCPY(block, input + offset, sizeof(block));
        InvCipher((state_t*)block, ctx.RoundKey, params.nr);
        DSD_MEMCPY(output + offset, block, sizeof(block));
    }
}
