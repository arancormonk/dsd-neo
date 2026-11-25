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

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
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
 * Which rule chose a call's rdio-scanner system (issue #931): --rdio-system-id (BASE), --rdio-analog-system-id
 * (ANALOG), or the call's frequency in kHz under --rdio-system-by-frequency (FREQUENCY). The sidecar's short_name
 * follows the rule, never a comparison of numbers: a kHz equal to a configured ID is still a frequency system.
 */
typedef enum {
    DSD_RDIO_SYSTEM_RULE_BASE = 0,
    DSD_RDIO_SYSTEM_RULE_ANALOG,
    DSD_RDIO_SYSTEM_RULE_FREQUENCY,
} dsd_rdio_system_rule;

typedef struct {
    int id;
    dsd_rdio_system_rule rule;
} dsd_rdio_system_choice;

/*
 * The rdio-scanner system of the call @p event (NULL: none) under @p opts, decided once per export and handed to the
 * sidecar and the API job alike: an analog row (DSD_SYNC_ANALOG) with opts->rdio_analog_system_id above 0 takes that
 * (ANALOG); else with opts->rdio_system_by_frequency a row whose frequency is at least 1 kHz and whose kHz, rounded
 * down, fit 1..2147483647 takes that kHz (FREQUENCY); else opts->rdio_system_id (BASE). NULL @p opts: id 0, BASE.
 */
dsd_rdio_system_choice dsd_rdio_resolve_call_system(const dsd_opts* opts, const Event_History* event);

/*
 * For tests: @p observer (NULL for none) sees every upload job as it is released, sent, dropped or abandoned at the
 * stop, with what is left of its API key and URL copies after their wipe, just before the job is freed. A build with
 * no libcurl queues no jobs and never calls it.
 */
typedef void (*dsd_rdio_upload_release_observer)(const char* api_key, size_t api_key_size, const char* api_url,
                                                 size_t api_url_size);
void dsd_rdio_upload_set_release_observer(dsd_rdio_upload_release_observer observer);

/*
 * For tests: @p observer (NULL for none) sees each rdio-scanner sidecar the moment it is in place under its public
 * name, its path in @p meta_path, as a DirWatch could first take the pair. Called on the exporting thread.
 */
typedef void (*dsd_rdio_sidecar_published_observer)(const char* meta_path);
void dsd_rdio_set_sidecar_published_observer(dsd_rdio_sidecar_published_observer observer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_RDIO_EXPORT_INTERNAL_H */
