// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_TETRA_CODEC_ABI_H
#define DSD_NEO_PROTOCOL_TETRA_CODEC_ABI_H

/**
 * @file
 * @brief The interface of a TETRA speech codec library (`--tetra-codec <file>`), version 1.
 *
 * The TETRA speech decoder of ETSI EN 300 395-2 is not part of dsd-neo. A user who has a decoder supplies it as a
 * shared library that exports the four C functions below; tools/tetra/acelp_adapter builds one from a local copy of
 * the ETSI reference code. dsd-neo loads the library once, on its decoder thread, and calls it only from that thread:
 *
 * - `uint32_t dsd_tetra_codec_abi_version(void)` returns DSD_TETRA_CODEC_ABI_VERSION. dsd-neo refuses a library that
 *   reports any other version, or that lacks one of the four functions.
 * - `void* dsd_tetra_codec_create(void)` returns a new decoder instance in its reset state, or NULL when it cannot
 *   make one. dsd-neo holds at most one instance at a time: it makes one when speech first needs it, destroys it at
 *   each carrier boundary and when it leaves TETRA, and makes another for the speech after that.
 * - `int dsd_tetra_codec_decode(void* codec, const uint8_t* bits, int bad_frame, int16_t* pcm)` decodes one 30 ms
 *   speech frame. `bits` holds DSD_TETRA_CODEC_FRAME_BITS bytes, each 0 or 1: bits B1 to B137 of the speech frame
 *   (EN 300 395-2 clause 4.2.2.7, tables 3 and 4), the source decoder's serial input after its bad-frame flag
 *   (clause 8). `bad_frame` is nonzero when the channel decoder found the frame corrupt (its class-2 CRC failed,
 *   clause 6.2) or the frame was stolen for signalling: the decoder then conceals the frame (clause 4.2.3.2) and must
 *   not trust `bits`. On success it writes DSD_TETRA_CODEC_FRAME_SAMPLES signed 16-bit samples at 8000 Hz to `pcm`
 *   and returns 0; any other return is an error, and dsd-neo drops the frame.
 * - `void dsd_tetra_codec_destroy(void* codec)` frees an instance dsd_tetra_codec_create() returned.
 *
 * Plain C calling convention (cdecl on 32-bit Windows) and C linkage. A library includes this header with
 * DSD_TETRA_CODEC_BUILD defined to get the prototypes with the export attribute.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The interface version this header describes. */
#define DSD_TETRA_CODEC_ABI_VERSION     1U
/** Coded bits in one speech frame (EN 300 395-2 table 3). */
#define DSD_TETRA_CODEC_FRAME_BITS      137
/** PCM samples in one speech frame: 30 ms at 8000 Hz. */
#define DSD_TETRA_CODEC_FRAME_SAMPLES   240

/** The exported names, as dsd-neo looks them up. */
#define DSD_TETRA_CODEC_SYM_ABI_VERSION "dsd_tetra_codec_abi_version"
#define DSD_TETRA_CODEC_SYM_CREATE      "dsd_tetra_codec_create"
#define DSD_TETRA_CODEC_SYM_DECODE      "dsd_tetra_codec_decode"
#define DSD_TETRA_CODEC_SYM_DESTROY     "dsd_tetra_codec_destroy"

typedef uint32_t (*dsd_tetra_codec_abi_version_fn)(void);
typedef void* (*dsd_tetra_codec_create_fn)(void);
typedef int (*dsd_tetra_codec_decode_fn)(void* codec, const uint8_t* bits, int bad_frame, int16_t* pcm);
typedef void (*dsd_tetra_codec_destroy_fn)(void* codec);

#ifdef DSD_TETRA_CODEC_BUILD
#ifdef _WIN32
#define DSD_TETRA_CODEC_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define DSD_TETRA_CODEC_EXPORT __attribute__((visibility("default")))
#else
#define DSD_TETRA_CODEC_EXPORT
#endif

DSD_TETRA_CODEC_EXPORT uint32_t dsd_tetra_codec_abi_version(void);
DSD_TETRA_CODEC_EXPORT void* dsd_tetra_codec_create(void);
DSD_TETRA_CODEC_EXPORT int dsd_tetra_codec_decode(void* codec, const uint8_t* bits, int bad_frame, int16_t* pcm);
DSD_TETRA_CODEC_EXPORT void dsd_tetra_codec_destroy(void* codec);
#endif /* DSD_TETRA_CODEC_BUILD */

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_CODEC_ABI_H */
