// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_DSP_SYNC_HAMMING_INTERNAL_H
#define DSD_NEO_DSP_SYNC_HAMMING_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Backends of <dsd-neo/dsp/sync_hamming.h>, exposed for equivalence tests. `_scalar` is the
 * per-dibit reference (and the fallback for unusual bytes or lengths); `_portable` is the
 * remap kernel with the 8-byte-word packer that targets without SSE2 use. Each returns the
 * same value as the public entry point for every input. */
int dsd_sync_hamming_distance_scalar(const char* buf, const char* pat, int len);
int dsd_qpsk_sync_hamming_with_remaps_scalar(const char* buf, const char* pat_norm, const char* pat_inv, int len);
int dsd_qpsk_sync_hamming_with_remaps_portable(const char* buf, const char* pat_norm, const char* pat_inv, int len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_DSP_SYNC_HAMMING_INTERNAL_H */
