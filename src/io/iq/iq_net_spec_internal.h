// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Private pieces of the network I/Q spec (iq_net_spec.c), shared with the network I/Q reader
 * (src/io/radio/iq_net_reader.c) and its test.
 */

#ifndef DSD_NEO_IO_IQ_IQ_NET_SPEC_INTERNAL_H
#define DSD_NEO_IO_IQ_IQ_NET_SPEC_INTERNAL_H

#include <dsd-neo/io/iq_net_spec.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Split text in place at every ':' into at most max_fields fields, empty ones kept ("a::b" is three fields). Returns
 * the field count, or max_fields + 1 when there are more (fields then holds the first max_fields).
 */
size_t iq_net_split_fields(char* text, char** fields, size_t max_fields);

enum { IQ_NET_HZ_OK = 0, IQ_NET_HZ_SYNTAX = -1, IQ_NET_HZ_RANGE = -2, IQ_NET_HZ_FRACTION = -3 };

/*
 * Parse plain decimal Hz, `[0-9]*('.'[0-9]*)?` with at least one digit, then one optional suffix character from
 * suffixes (k/K = 10^3, m/M = 10^6, g/G = 10^9), in exact integer arithmetic. With whole_only a value that is not a
 * whole number of Hz is IQ_NET_HZ_FRACTION; without it the value rounds half up to a whole Hz. Above UINT32_MAX is
 * IQ_NET_HZ_RANGE. *out_hz is written only on IQ_NET_HZ_OK.
 */
int iq_net_parse_hz(const char* text, const char* suffixes, int whole_only, uint32_t* out_hz);

/* Whether a reader can run spec: a kind, a wire format, a non-empty host, a port in 0..65535 (not 0 for TCP). */
int iq_net_spec_usable(const dsd_iq_net_spec* spec);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_IQ_IQ_NET_SPEC_INTERNAL_H */
