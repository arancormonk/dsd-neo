// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The raw I/Q network input's spec (`iqtcp:` / `iqudp:`) and its wire-format names.
 *
 * The socket-free part of the network I/Q input (<dsd-neo/io/iq_net.h>): the spec parser, its canonical text and the
 * wire formats' names and sizes. It is in every build, radio support or not, so the command line, the config and the
 * input rate policy can read a stream spec wherever they run; a build without radio support refuses the input once
 * it has read it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_NET_SPEC_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_NET_SPEC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest host or bind address a spec holds, in bytes, terminator included. */
#define DSD_IQ_NET_HOST_MAX      256
/** Lowest sample rate a spec takes, in complex samples per second. */
#define DSD_IQ_NET_MIN_RATE_HZ   8000U
/** Fastest sample rate a stream runs, in complex samples per second: the live-device capture ceiling its input ring
 *  and front end are sized for (the radio stream holds the two equal). A faster RATE reads, but no plan runs it
 *  (dsd_iq_plan_general_rate()): a sending program sends one channel, not a whole band. */
#define DSD_IQ_NET_MAX_RATE_HZ   3200000U
/** Room dsd_iq_net_spec_format() needs for any spec, terminator included. */
#define DSD_IQ_NET_SPEC_TEXT_MAX (DSD_IQ_NET_HOST_MAX + 48)

typedef enum {
    DSD_IQ_NET_KIND_NONE = 0,
    DSD_IQ_NET_KIND_TCP = 1, /**< `iqtcp:`: dsd-neo connects to a TCP server */
    DSD_IQ_NET_KIND_UDP = 2, /**< `iqudp:`: dsd-neo binds and receives datagrams */
} dsd_iq_net_kind;

/**
 * Sample formats on the wire, all interleaved I then Q and little-endian on any host. This enum is the network
 * input's own: the replay's `dsd_iq_sample_format` and its converters do not gain these formats.
 */
typedef enum {
    DSD_IQ_NET_FORMAT_NONE = 0,
    DSD_IQ_NET_FORMAT_CU8 = 1,  /**< unsigned 8-bit, (x - 127.5) / 127.5 */
    DSD_IQ_NET_FORMAT_CS8 = 2,  /**< signed 8-bit, x / 128 */
    DSD_IQ_NET_FORMAT_CS16 = 3, /**< signed 16-bit, x / 32768 */
    DSD_IQ_NET_FORMAT_CS32 = 4, /**< signed 32-bit, x / 2^31 */
    DSD_IQ_NET_FORMAT_CF32 = 5, /**< 32-bit float, bounded as bound_cf32_to_f32() bounds it */
} dsd_iq_net_format;

/** A parsed `iqtcp:` / `iqudp:` spec. */
typedef struct dsd_iq_net_spec {
    dsd_iq_net_kind kind;
    char host[DSD_IQ_NET_HOST_MAX]; /**< TCP: the server's host name or IPv4 address; UDP: the address to bind */
    int port;                       /**< 1..65535 from the parser; a UDP reader also takes 0 (any free port) */
    uint32_t rate_hz;               /**< complex samples per second, at least DSD_IQ_NET_MIN_RATE_HZ */
    dsd_iq_net_format format;
    uint32_t freq_hz; /**< the stream's receiver (centre) frequency; 0 = unknown (FREQ omitted) */
} dsd_iq_net_spec;

/** The kind @p text names by its prefix (`iqtcp:` or `iqudp:`), without parsing the rest; NONE for anything else. */
dsd_iq_net_kind dsd_iq_net_spec_kind(const char* text);

/**
 * @brief Parse `iqtcp:HOST:PORT:RATE:FORMAT[:FREQ]` or `iqudp:BIND:PORT:RATE:FORMAT[:FREQ]`.
 *
 * Fields split on every `:` and empty fields are kept, so each sits at its own position. HOST/BIND, PORT, RATE and
 * FORMAT are required; an empty FREQ is the same as none (0). PORT is 1..65535. RATE is a whole number of Hz with an
 * optional k or M suffix (`240k`, `1.152M`), at least DSD_IQ_NET_MIN_RATE_HZ. FORMAT is `cu8`, `cs8`, `cs16`, `cs32`
 * or `cf32` (any case). FREQ is Hz with an optional k, M or G suffix, rounded half up to a whole Hz, within uint32.
 * Numbers are plain decimal: no sign, space, exponent or hex.
 *
 * @param text      The spec.
 * @param out       Receives the spec; written only on success.
 * @param err       Receives a one-line reason naming the field on failure (may be NULL).
 * @param err_size  Capacity of @p err.
 * @return 0 on success, -1 on failure.
 */
int dsd_iq_net_spec_parse(const char* text, dsd_iq_net_spec* out, char* err, size_t err_size);

/** A spec's field, as a refusal names it (dsd_iq_net_spec_parse_field()). */
typedef enum {
    DSD_IQ_NET_FIELD_NONE = 0, /**< none: the spec read */
    DSD_IQ_NET_FIELD_SPEC,     /**< the spec as a whole: no iqtcp:/iqudp: prefix, too long, too many fields */
    DSD_IQ_NET_FIELD_HOST,     /**< HOST (iqtcp) or BIND (iqudp) */
    DSD_IQ_NET_FIELD_PORT,
    DSD_IQ_NET_FIELD_RATE,
    DSD_IQ_NET_FIELD_FORMAT,
    DSD_IQ_NET_FIELD_FREQ,
} dsd_iq_net_field;

/**
 * @brief dsd_iq_net_spec_parse(), also saying which field it refused, so a caller can name its own place for that
 *        field (a config key) without reading the reason's words.
 *
 * @param bad_field Receives the refused field, DSD_IQ_NET_FIELD_NONE on success (may be NULL).
 * @return 0 on success, -1 on failure.
 */
int dsd_iq_net_spec_parse_field(const char* text, dsd_iq_net_spec* out, char* err, size_t err_size,
                                dsd_iq_net_field* bad_field);

/**
 * @brief Write @p spec back as its canonical text: RATE and FREQ in plain Hz, FREQ left out when 0.
 *
 * @return The text's length, or -1 when @p spec is not a valid spec or @p out is too small (DSD_IQ_NET_SPEC_TEXT_MAX
 *         always fits).
 */
int dsd_iq_net_spec_format(const dsd_iq_net_spec* spec, char* out, size_t out_size);

/** Lower-case name of @p format (`cs16`), or NULL for one that is not a wire format. */
const char* dsd_iq_net_format_name(dsd_iq_net_format format);

/** The format @p name names (any case), or DSD_IQ_NET_FORMAT_NONE. */
dsd_iq_net_format dsd_iq_net_format_from_name(const char* name);

/** Bytes in one complex sample of @p format (2, 4 or 8), or 0 for one that is not a wire format. */
size_t dsd_iq_net_format_sample_bytes(dsd_iq_net_format format);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_NET_SPEC_H_ */
