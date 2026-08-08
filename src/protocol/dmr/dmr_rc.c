// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Standalone DMR Reverse Channel (RC) burst handling.
 *
 * The standalone RC burst (ETSI TS 102 361-1 V2.6.1, clause 6.4.1) is a
 * 96-bit / 10 ms inbound burst centred in the 30 ms slot:
 *
 *   RC_a(16) | EMB_a(8) | SYNC(48) | EMB_b(8) | RC_b(16)
 *
 * The 48-bit SYNC sits at the normal burst-centre position, so the regular
 * sync correlator detects it (DMR_MS_RC_SYNC). The 32-bit RC PDU carries a
 * 4-bit RC command plus a 7-bit CRC (mask 0x7A), protected by the Reverse
 * Channel Single Burst BPTC (clause B.2.2.2). It is used by MSs for TXI
 * ("cease transmission") and closed-loop power-control signalling
 * (ETSI TS 102 361-4, table 6.32).
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/colors.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* Dibit offsets inside the 48-dibit burst window. */
#define DMR_RC_DIBIT_RC_A   0U  /* 8 dibits */
#define DMR_RC_DIBIT_EMB_A  8U  /* 4 dibits */
#define DMR_RC_DIBIT_SYNC   12U /* 24 dibits */
#define DMR_RC_DIBIT_EMB_B  36U /* 4 dibits */
#define DMR_RC_DIBIT_RC_B   40U /* 8 dibits */
#define DMR_RC_DIBIT_COUNT  48U
#define DMR_RC_CACHED_COUNT 36U /* RC_a + EMB_a + SYNC already demodulated */

int
dmr_rc_decode_pdu(const uint8_t interleaved_bits[32], uint8_t* out_command, uint32_t* out_hex) {
    if (interleaved_bits == NULL) {
        return DMR_RC_DECODE_FEC_ERR;
    }

    uint8_t in[32];
    uint8_t out[32];
    for (int i = 0; i < 32; i++) {
        in[i] = interleaved_bits[i] & 1U;
    }
    DSD_MEMSET(out, 0, sizeof(out));

    /* Reverse Channel Single Burst BPTC: Hamming(16,11,4) row plus odd
     * column-parity row (ETSI TS 102 361-1 clause B.2.2.2). */
    const uint32_t irrecoverable = BPTC_16x2_Extract_Data(in, out, 1);

    uint32_t hex = 0;
    for (int i = 0; i < 11; i++) {
        hex = (hex << 1) | (out[i] & 1U);
    }
    if (out_hex != NULL) {
        *out_hex = hex;
    }
    if (irrecoverable != 0U) {
        return DMR_RC_DECODE_FEC_ERR;
    }

    /* Bits 0..3 carry the RC command; bits 4..10 the CRC-7, masked with 0x7A
     * (ETSI TS 102 361-1 clauses B.3.13 and B.3.12, table B.21). */
    uint8_t crc_extracted = 0;
    for (int i = 0; i < 7; i++) {
        crc_extracted = (uint8_t)((crc_extracted << 1) | (out[i + 4] & 1U));
    }
    crc_extracted ^= 0x7AU;
    const uint8_t crc_computed = crc7(out, 4);
    if (crc_extracted != crc_computed) {
        return DMR_RC_DECODE_CRC_ERR;
    }

    if (out_command != NULL) {
        *out_command = (uint8_t)(hex >> 7);
    }
    return DMR_RC_DECODE_OK;
}

/* Copy the cached prefix (RC_a + EMB_a + SYNC) and read the 12 trailing
 * dibits live. Returns 0 if the payload history is too short.
 *
 * Caveat: the rolling payload buffer periodically rewinds its write pointer
 * (dsd_dibit.c); if that happens between sync detection and this read, the
 * cached prefix can span stale pre-rewind dibits. Worst case is a one-shot
 * spurious FEC/CRC-error line, so it is not worth restructuring for. */
static int
dmr_rc_collect_dibits(dsd_opts* opts, dsd_state* state, int dibits[48]) {
    if (state->dmr_payload_buf == NULL || state->dmr_payload_p == NULL
        || state->dmr_payload_p - state->dmr_payload_buf < (ptrdiff_t)DMR_RC_CACHED_COUNT) {
        return 0;
    }

    const int* cached = state->dmr_payload_p - DMR_RC_CACHED_COUNT;
    for (size_t i = 0; i < DMR_RC_CACHED_COUNT; i++) {
        dibits[i] = cached[i] & 3;
    }
    for (size_t i = DMR_RC_CACHED_COUNT; i < DMR_RC_DIBIT_COUNT; i++) {
        dsd_dibit_soft_t soft;
        dibits[i] = getDibitSoft(opts, state, &soft) & 3;
    }
    if (opts->inverted_dmr == 1) {
        for (size_t i = 0; i < DMR_RC_DIBIT_COUNT; i++) {
            dibits[i] = (dibits[i] ^ 2) & 3;
        }
    }
    return 1;
}

void
dmr_rc_assemble_bits(const int dibits[48], uint8_t emb_bits[16], uint8_t rc_bits[32]) {
    for (size_t i = 0; i < 4U; i++) {
        const int a = dibits[DMR_RC_DIBIT_EMB_A + i];
        const int b = dibits[DMR_RC_DIBIT_EMB_B + i];
        emb_bits[i * 2U] = (uint8_t)((a >> 1) & 1);
        emb_bits[(i * 2U) + 1U] = (uint8_t)(a & 1);
        emb_bits[8U + (i * 2U)] = (uint8_t)((b >> 1) & 1);
        emb_bits[8U + (i * 2U) + 1U] = (uint8_t)(b & 1);
    }
    for (size_t i = 0; i < 8U; i++) {
        const int a = dibits[DMR_RC_DIBIT_RC_A + i];
        const int b = dibits[DMR_RC_DIBIT_RC_B + i];
        rc_bits[i * 2U] = (uint8_t)((a >> 1) & 1);
        rc_bits[(i * 2U) + 1U] = (uint8_t)(a & 1);
        rc_bits[16U + (i * 2U)] = (uint8_t)((b >> 1) & 1);
        rc_bits[16U + (i * 2U) + 1U] = (uint8_t)(b & 1);
    }
}

static void
dmr_rc_print(const dsd_opts* opts, int emb_ok, const uint8_t emb_bits[16], int rc_err, uint8_t rc_command,
             uint32_t rc_hex) {
    char timestr[9];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    DSD_FPRINTF(stderr, "%s Sync: %cDMR RC ", timestr, (opts->inverted_dmr == 1) ? '-' : '+');

    if (emb_ok == 1) {
        const uint8_t cc = (uint8_t)((emb_bits[0] << 3) | (emb_bits[1] << 2) | (emb_bits[2] << 1) | (emb_bits[3] << 0));
        DSD_FPRINTF(stderr, "| Color Code=%02d ", cc);
    } else {
        DSD_FPRINTF(stderr, "| Color Code=XX ");
    }

    if (rc_err == DMR_RC_DECODE_OK) {
        const char* name = dmr_rc_command_name(rc_command);
        DSD_FPRINTF(stderr, "%s", KCYN);
        if (name != NULL) {
            DSD_FPRINTF(stderr, "| RC: %s;", name);
        } else {
            DSD_FPRINTF(stderr, "| RC: Reserved %02X;", rc_command);
        }
        DSD_FPRINTF(stderr, "%s", KNRM);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "| RC %s ERR", (rc_err == DMR_RC_DECODE_FEC_ERR) ? "FEC" : "CRC");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (opts->payload == 1) {
        /* PI/LCSS come from the EMB codeword: only trustworthy when the
         * QR(16,7,6) decode succeeded. */
        if (emb_ok == 1) {
            const uint8_t pi = emb_bits[4] & 1U;
            const uint8_t lcss = (uint8_t)((emb_bits[5] << 1) | emb_bits[6]);
            DSD_FPRINTF(stderr, " | PI=%u LCSS=%u", pi, lcss);
        } else {
            DSD_FPRINTF(stderr, " | PI=X LCSS=X");
        }
        DSD_FPRINTF(stderr, " RC PDU=%03X", (unsigned int)rc_hex);
    }
    DSD_FPRINTF(stderr, "\n");
}

void
dmrRC(dsd_opts* opts, dsd_state* state) {
    int dibits[DMR_RC_DIBIT_COUNT];
    if (dmr_rc_collect_dibits(opts, state, dibits) == 0) {
        return;
    }

    uint8_t emb_bits[16];
    uint8_t rc_bits[32];
    dmr_rc_assemble_bits(dibits, emb_bits, rc_bits);

    /* EMB is QR(16,7,6): CC(4), PI(1), LCSS(2) + 9 parity bits. */
    const int emb_ok = QR_16_7_6_decode(emb_bits) ? 1 : 0;

    uint8_t rc_command = 0;
    uint32_t rc_hex = 0;
    const int rc_err = dmr_rc_decode_pdu(rc_bits, &rc_command, &rc_hex);

    dmr_rc_print(opts, emb_ok, emb_bits, rc_err, rc_command, rc_hex);

    if (rc_err == DMR_RC_DECODE_OK) {
        uint8_t cc = 0;
        if (emb_ok == 1) {
            cc = (uint8_t)((emb_bits[0] << 3) | (emb_bits[1] << 2) | (emb_bits[2] << 1) | (emb_bits[3] << 0));
        }
        /* Slot 0 + sentinel IDs: the event row is a slot-less notice, and the
         * dedup ext slot is module-private bookkeeping, so the handler's no-op
         * contract for slot/trunking/call state still holds. */
        dmr_rc_notify_command(opts, state, 0U, DMR_RC_NOTIFY_KEY_STANDALONE, rc_command, emb_ok, cc, time(NULL));
    }

    if (opts->dmr_debug_burst != 0) {
        char line[192];
        if (dmr_debug_format_rc_burst(line, sizeof(line), dibits) != 0U) {
            DSD_FPRINTF(stderr, "%s\n", line);
        }
    }
}
