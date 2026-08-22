// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Motorola APX embedded alias: the UTF-16BE alias text must be transcoded by dsd-neo rather
 * than handed unit-by-unit to fprintf("%lc") (the crash path of issue #358).
 *
 * The alias octets are scrambled over the air; apx_embedded_alias_unscramble() is the pure
 * inverse step, so the test searches for an encoding of the wanted text through it rather than
 * duplicating the lookup table.
 */

#include <assert.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

int
dsd_event_enrich_alias(dsd_state* state, uint8_t slot, uint64_t epoch, const char* alias) {
    (void)state;
    (void)slot;
    (void)epoch;
    (void)alias;
    return 0;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_unicode_supported(void) {
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors) {
    (void)opts;
    (void)state;
    (void)lcw_bits;
    (void)irrecoverable_errors;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

/* UTF-16BE: U+4739, U+1F600 as a surrogate pair, a lone high surrogate, then 'A'. */
static const uint8_t kAlias[] = {0x47, 0x39, 0xD8, 0x3D, 0xDE, 0x00, 0xD8, 0x00, 0x00, 0x41};

static const char kExpected[] = "\xE4\x9C\xB9"
                                "\xF0\x9F\x98\x80"
                                "\xEF\xBF\xBD"
                                "A";

/* Layout of the alias record as apx_embedded_alias_decode() reads it. */
#define ALIAS_OCTETS  ((uint16_t)sizeof kAlias)
#define FQSUID_BITS   56
#define CRC_BITS      16
#define RECORD_BITS   (FQSUID_BITS + (ALIAS_OCTETS * 8) + CRC_BITS)
#define RECORD_OFFSET 72
#define ALIAS_OFFSET  (RECORD_OFFSET + FQSUID_BITS)
#define CRC_OFFSET    (RECORD_OFFSET + RECORD_BITS - CRC_BITS)
#define INPUT_BITS    (RECORD_OFFSET + RECORD_BITS)

static void
value_to_bits_msb(uint8_t* bits_out, size_t bit_offset, size_t bits_out_sz, uint32_t value, uint8_t bit_count) {
    assert(bit_offset + bit_count <= bits_out_sz);
    for (uint8_t b = 0; b < bit_count; b++) {
        bits_out[bit_offset + b] = (uint8_t)((value >> (bit_count - 1u - b)) & 1u);
    }
}

/*
 * Each decoded octet depends on its own encoded octet and on the ones before it, so the
 * encoding is found one octet at a time.
 */
static void
find_encoding(uint8_t* encoded, size_t count) {
    uint8_t decoded[ALIAS_OCTETS];
    DSD_MEMSET(encoded, 0, count);
    for (size_t i = 0; i < count; i++) {
        int found = 0;
        for (unsigned candidate = 0; candidate < 256U; candidate++) {
            encoded[i] = (uint8_t)candidate;
            apx_embedded_alias_unscramble(encoded, ALIAS_OCTETS, decoded, sizeof decoded);
            if (decoded[i] == kAlias[i]) {
                found = 1;
                break;
            }
        }
        assert(found);
    }
}

/*
 * The record length is measured off the air, and the decoder reads the alias out of the
 * 3072-octet superframe store. Fewer than nine record octets used to wrap the alias count to
 * 65534 and a long record ran past the 200-octet scratch buffers.
 */
static void
decode_record_of(dsd_opts* opts, dsd_state* st, size_t record_bits) {
    const size_t input_bits = sizeof(st->dmr_pdu_sf[0]);
    const size_t crc_offset = RECORD_OFFSET + record_bits - CRC_BITS;
    uint8_t* input = (uint8_t*)calloc(input_bits, 1U);
    assert(input != NULL);
    assert(RECORD_OFFSET + record_bits <= input_bits);
    /* Every alias word non-zero (0x0101), the way the caller measures the record length. */
    for (size_t bit = ALIAS_OFFSET + 7U; bit < crc_offset; bit += 8U) {
        input[bit] = 1U;
    }
    const uint16_t crc = dsd_crc_ccitt16_bits(&input[RECORD_OFFSET], record_bits - CRC_BITS);
    value_to_bits_msb(input, crc_offset, input_bits, crc, 16);

    char buf[4096];
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "apx_alias_len") == 0);
    apx_embedded_alias_decode(opts, st, 0, (int16_t)record_bits, input);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, sizeof buf) == 0);

    assert(strstr(buf, "Alias CRC Error") == NULL);
    assert(strstr(buf, " Alias: ") != NULL);
    free(input);
}

static void
test_alias_length_from_air_is_bounded(dsd_opts* opts, dsd_state* st) {
    /* Only the FQSUID, with its tail doubling as the CRC: no alias octets at all. */
    decode_record_of(opts, st, FQSUID_BITS);
    /* More alias octets than the scratch buffers hold. */
    decode_record_of(opts, st, FQSUID_BITS + (250U * 8U) + CRC_BITS);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(opts != NULL && st != NULL);
    st->event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    assert(st->event_history_s != NULL);

    uint8_t encoded[ALIAS_OCTETS];
    find_encoding(encoded, sizeof encoded);

    /* Zero header bits, a zero FQSUID (so no call lookup runs), the scrambled alias, the CRC. */
    uint8_t input[INPUT_BITS];
    DSD_MEMSET(input, 0, sizeof input);
    for (size_t i = 0; i < sizeof encoded; i++) {
        value_to_bits_msb(input, ALIAS_OFFSET + (i * 8U), sizeof input, encoded[i], 8);
    }
    const uint16_t crc = dsd_crc_ccitt16_bits(&input[RECORD_OFFSET], (size_t)(RECORD_BITS - CRC_BITS));
    value_to_bits_msb(input, CRC_OFFSET, sizeof input, crc, 16);

    char buf[1024];
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "apx_alias_utf16") == 0);
    apx_embedded_alias_decode(opts, st, 0, (int16_t)RECORD_BITS, input);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, sizeof buf) == 0);

    assert(strstr(buf, "Alias CRC Error") == NULL);
    const char* marker = " Alias: ";
    const char* p = strstr(buf, marker);
    if (p == NULL || memcmp(p + strlen(marker), kExpected, sizeof kExpected - 1U) != 0) {
        DSD_FPRINTF(stderr, "apx alias printed: %s\n", buf);
        assert(0 && "UTF-16 alias was not transcoded as expected");
    }

    test_alias_length_from_air_is_bounded(opts, st);

    free(st->event_history_s);
    free(st);
    free(opts);
    DSD_FPRINTF(stderr, "CORE_APX_EMBEDDED_ALIAS_UTF16: PASS\n");
    return 0;
}
