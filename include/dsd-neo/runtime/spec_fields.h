// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The fields of a colon-separated input or output spec (`-i rtl:0:851M:22`, `-o udp:host:port`).
 *
 * A spec names each setting by its place, so an empty field is a place the spec leaves at its default:
 * `rtl::851M` is the default device at 851 MHz. A tokenizer that skips empty fields (strtok) would read the
 * frequency as the device instead and every later field one place early, which is why every spec reads its fields
 * through dsd_spec_split_fields() (issue #743 B7).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SPEC_FIELDS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SPEC_FIELDS_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The highest port number a spec field can name. */
#define DSD_SPEC_PORT_MAX 65535

/**
 * @brief Split @p spec at every ':' into its fields, empty ones included.
 *
 * @p spec is copied into @p buf, whose colons become the fields' ends, and @p fields gets a pointer to each field in
 * order, at most @p max_fields of them: `rtl::851M` is "rtl", "" and "851M"; a spec of n colons has n + 1 fields, so
 * "" is one empty field and a trailing ':' ends in one.
 *
 * @return The number of fields in @p spec, which is more than @p max_fields when only the first @p max_fields were
 *         stored; -1 when an argument is NULL, @p max_fields is 0, or @p spec does not fit @p buf (nothing stored).
 */
int dsd_spec_split_fields(const char* spec, char* buf, size_t buf_size, char** fields, size_t max_fields);

/**
 * @brief Read a port field: a whole decimal number from @p min_port to DSD_SPEC_PORT_MAX.
 *
 * Nothing else is a port: not an empty field, a sign, hex, trailing text (`12x`) or a number out of range. The caller
 * reads an empty field as its default before it gets here.
 *
 * @return 0 with the port in @p out, -1 when @p field is not such a port (@p out unchanged).
 */
int dsd_spec_parse_port(const char* field, int min_port, int* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_SPEC_FIELDS_H_ */
