// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * rdio_export.c's module-private pieces that tests reach directly. Include it as "rdio_export_internal.h" from
 * src/runtime.
 */

#ifndef DSD_NEO_RUNTIME_RDIO_EXPORT_INTERNAL_H
#define DSD_NEO_RUNTIME_RDIO_EXPORT_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dsd_rdio_upload_holds()'s name rule, a pure string function: whether @p job_path (a queued upload's WAV) and @p path
 * name the same file, by their final path components. @p win selects Windows rules ('\\' also separates, ASCII case is
 * ignored), so RUNTIME_RDIO_EXPORT checks both rule sets on every host. A NULL path, "" or one ending in a separator
 * names no file and matches nothing.
 */
int dsd_rdio_upload_names_same_file(const char* job_path, const char* path, int win);

/*
 * For tests: @p observer (NULL for none) sees every upload job as it is released, sent, dropped or abandoned at the
 * stop, with what is left of its API key and URL copies after their wipe, just before the job is freed. A build with
 * no libcurl queues no jobs and never calls it.
 */
typedef void (*dsd_rdio_upload_release_observer)(const char* api_key, size_t api_key_size, const char* api_url,
                                                 size_t api_url_size);
void dsd_rdio_upload_set_release_observer(dsd_rdio_upload_release_observer observer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_RDIO_EXPORT_INTERNAL_H */
