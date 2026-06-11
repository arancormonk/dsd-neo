// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_BLOCK_CRYPTO_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_BLOCK_CRYPTO_H_

#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    int alg;
    int kid;
    int aes_key_loaded;
    int start;
    int end;
    unsigned long long mi;
    unsigned long long rkey;
    uint8_t aes_key[32];
    uint8_t rc4_iv[9];
} dmr_block_crypto_ctx;

void dmr_block_crypto_load_ctx(const dsd_state* state, uint8_t slot, int blocks, uint8_t block_len,
                               dmr_block_crypto_ctx* ctx);
void dmr_block_crypto_print_info(const dmr_block_crypto_ctx* ctx, int show_keys);
uint8_t dmr_block_crypto_decrypt_payload(dsd_state* state, uint8_t slot, const dmr_block_crypto_ctx* ctx,
                                         int show_keys);

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_BLOCK_CRYPTO_H_ */
