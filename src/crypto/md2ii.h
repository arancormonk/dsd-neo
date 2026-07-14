// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CRYPTO_MD2II_H_
#define DSD_NEO_SRC_CRYPTO_MD2II_H_

#include <stddef.h>
#include <stdint.h>

enum { DSD_MD2II_MAX_BLOCK_SIZE = 264 };

typedef struct {
    size_t block_len;
    uint8_t checksum;
    size_t buffered;
    uint8_t h2[DSD_MD2II_MAX_BLOCK_SIZE];
    uint8_t h1[DSD_MD2II_MAX_BLOCK_SIZE * 3U];
} dsd_md2ii_ctx;

int dsd_md2ii_init(dsd_md2ii_ctx* ctx, size_t block_len);
void dsd_md2ii_update(dsd_md2ii_ctx* ctx, const uint8_t* input, size_t len);
void dsd_md2ii_final(dsd_md2ii_ctx* ctx, uint8_t* out, size_t out_len);
int dsd_md2ii_hash(const uint8_t* input, size_t len, size_t block_len, uint8_t* out, size_t out_len);

#endif /* DSD_NEO_SRC_CRYPTO_MD2II_H_ */
