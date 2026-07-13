// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "md2ii.h"

#include <dsd-neo/core/safe_api.h>

static const uint8_t k_md2ii_sbox[256] = {
    13,  199, 11,  67,  237, 193, 164, 77,  115, 184, 141, 222, 73,  38,  147, 36,  150, 87,  21,  104, 12,  61,
    156, 101, 111, 145, 119, 22,  207, 35,  198, 37,  171, 167, 80,  30,  219, 28,  213, 121, 86,  29,  214, 242,
    6,   4,   89,  162, 110, 175, 19,  157, 3,   88,  234, 94,  144, 118, 159, 239, 100, 17,  182, 173, 238, 68,
    16,  79,  132, 54,  163, 52,  9,   58,  57,  55,  229, 192, 170, 226, 56,  231, 187, 158, 70,  224, 233, 245,
    26,  47,  32,  44,  247, 8,   251, 20,  197, 185, 109, 153, 204, 218, 93,  178, 212, 137, 84,  174, 24,  120,
    130, 149, 72,  180, 181, 208, 255, 189, 152, 18,  143, 176, 60,  249, 27,  227, 128, 139, 243, 253, 59,  123,
    172, 108, 211, 96,  138, 10,  215, 42,  225, 40,  81,  65,  90,  25,  98,  126, 154, 64,  124, 116, 122, 5,
    1,   168, 83,  190, 131, 191, 244, 240, 235, 177, 155, 228, 125, 66,  43,  201, 248, 220, 129, 188, 230, 62,
    75,  71,  78,  34,  31,  216, 254, 136, 91,  114, 106, 46,  217, 196, 92,  151, 209, 133, 51,  236, 33,  252,
    127, 179, 69,  7,   183, 105, 146, 97,  39,  15,  205, 112, 200, 166, 223, 45,  48,  246, 186, 41,  148, 140,
    107, 76,  85,  95,  194, 142, 50,  49,  134, 23,  135, 169, 221, 210, 203, 63,  165, 82,  161, 202, 53,  14,
    206, 232, 103, 102, 195, 117, 250, 99,  0,   74,  160, 241, 2,   113,
};

static void
md2ii_transform(dsd_md2ii_ctx* ctx) {
    uint8_t t = 0;
    ctx->buffered = 0;
    for (size_t round = 0; round < ctx->block_len + 2U; round++) {
        for (size_t i = 0; i < ctx->block_len * 3U; i++) {
            t = ctx->h1[i] ^= k_md2ii_sbox[t];
        }
        t = (uint8_t)(t + round);
    }
}

int
dsd_md2ii_init(dsd_md2ii_ctx* ctx, size_t block_len) {
    if (!ctx || block_len == 0U || block_len > DSD_MD2II_MAX_BLOCK_SIZE) {
        return 0;
    }
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->block_len = block_len;
    return 1;
}

void
dsd_md2ii_update(dsd_md2ii_ctx* ctx, const uint8_t* input, size_t len) {
    if (!ctx || (!input && len != 0U) || ctx->block_len == 0U) {
        return;
    }

    size_t pos = 0;
    while (pos < len) {
        while (pos < len && ctx->buffered < ctx->block_len) {
            const uint8_t byte = input[pos++];
            const size_t offset = ctx->buffered++;
            ctx->h1[offset + ctx->block_len] = byte;
            ctx->h1[offset + (ctx->block_len * 2U)] = (uint8_t)(byte ^ ctx->h1[offset]);
            ctx->checksum = ctx->h2[offset] ^= k_md2ii_sbox[byte ^ ctx->checksum];
        }
        if (ctx->buffered == ctx->block_len) {
            md2ii_transform(ctx);
        }
    }
}

void
dsd_md2ii_final(dsd_md2ii_ctx* ctx, uint8_t* out, size_t out_len) {
    if (!ctx || (!out && out_len != 0U) || ctx->block_len == 0U) {
        return;
    }

    uint8_t padding[DSD_MD2II_MAX_BLOCK_SIZE];
    const size_t padding_len = ctx->block_len - ctx->buffered;
    DSD_MEMSET(padding, (uint8_t)padding_len, sizeof(padding));
    dsd_md2ii_update(ctx, padding, padding_len);
    dsd_md2ii_update(ctx, ctx->h2, ctx->block_len);

    if (out_len > ctx->block_len) {
        out_len = ctx->block_len;
    }
    if (out_len != 0U) {
        DSD_MEMCPY(out, ctx->h1, out_len);
    }
}

int
dsd_md2ii_hash(const uint8_t* input, size_t len, size_t block_len, uint8_t* out, size_t out_len) {
    dsd_md2ii_ctx ctx;
    if ((!input && len != 0U) || (!out && out_len != 0U) || out_len > block_len || !dsd_md2ii_init(&ctx, block_len)) {
        return 0;
    }
    dsd_md2ii_update(&ctx, input, len);
    dsd_md2ii_final(&ctx, out, out_len);
    return 1;
}
