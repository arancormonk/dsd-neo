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
 * Whether @p name (a final path component) is one of the private upload inputs dsd-neo makes in --rdio-mode both: it
 * ends in ".rdio-upload-audio" or ".rdio-upload-meta" (exact case) after at least one other character. Neither
 * suffix is one an rdio-scanner watch reads (6.6.3 and v8 look at the last extension only: ".json" for a
 * trunk-recorder watch, ".mp3" for SDRTrunk and DSDPlus, the configured one, ".wav" by default, for the default
 * type). The leftover cleanup removes only names this answers 1 for, never this process's own (by its token), and
 * another run's only once it is old enough that no upload can still read it.
 */
int dsd_rdio_upload_private_name(const char* name);

/*
 * This process's token, which every private input name it makes carries ("<base>.<token>[.<index>].rdio-upload-…",
 * 16 uppercase hex digits, drawn once at first use), into @p out: 0, or -1 when @p out holds fewer than 17 bytes.
 */
int dsd_rdio_upload_private_token_text(char* out, size_t out_size);

/*
 * The token in a private input name (dsd_rdio_upload_private_name()), copied into @p out (17 bytes or more): 1, or 0
 * when the name carries none. An index after the token is one or two digits.
 */
int dsd_rdio_upload_private_name_token(const char* name, char* out, size_t out_size);

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
