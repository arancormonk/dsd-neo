/* Minimal osmocom/core/conv.h - vendored adapter for dsd-neo
 * Provides the struct layout and prototype needed by the osmo viterbi
 * helper code (viterbi_cch.c / viterbi_tch.c). This is intentionally
 * minimal and maps through to the in-tree Viterbi implementation.
 */
#ifndef OSMOCOM_CORE_CONV_H
#define OSMOCOM_CORE_CONV_H

#include <stdint.h>

struct osmo_conv_code {
    int N; /* output bits per input bit (usually 2) */
    int K; /* constraint length */
    const uint8_t (*next_output)[2];
    const uint8_t (*next_state)[2];
    int len; /* number of input symbols */
};

/* Decode a convolutional code described by `code`.
 * - `input` is an array of signed soft values (int8_t) of length `code->len`.
 * - `output` is a bit-packed buffer where decoded bits are written.
 * Returns a non-negative cost on success.
 */
int osmo_conv_decode(struct osmo_conv_code *code, const int8_t *input, uint8_t *output);

#endif /* OSMOCOM_CORE_CONV_H */
