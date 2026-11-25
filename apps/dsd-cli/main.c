// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/exitflag.h>
#ifdef USE_RADIO
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_device_select.h>
#include <string.h>
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frontend.h"

#ifdef USE_RADIO
/* Readable devices that carry @p e's serial. */
static int
rtl_list_serial_count(const struct rtl_device_entry* list, int count, const struct rtl_device_entry* e) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        n += (list[i].readable && strcmp(list[i].serial, e->serial) == 0) ? 1 : 0;
    }
    return n;
}

/* One --rtl-list line (rtl_device_list_line()). Returns 1 when its serial is shared with another readable device. */
static int
rtl_list_print_entry(const struct rtl_device_entry* list, int count, const struct rtl_device_entry* e) {
    const int shared = e->readable && rtl_list_serial_count(list, count, e) > 1;
    char line[RTL_DEVICE_LINE_SIZE];
    rtl_device_list_line(e, shared, line, sizeof line);
    DSD_FPRINTF(stdout, "%s\n", line);
    return shared;
}

/* --rtl-list: every RTL-SDR librtlsdr enumerates. 0 (an empty list too), or 1 without RTL-SDR support or when the
   enumeration failed, as --airspy-list. */
static int
rtl_list_run(void) {
    int total = 0;
    if (rtl_device_list(NULL, 0, &total) < 0) {
        DSD_FPRINTF(stderr, "RTL-SDR support unavailable or enumeration failed.\n");
        return 1;
    }
    if (total == 0) {
        DSD_FPRINTF(stderr, "No RTL-SDR devices found.\n");
        return 0;
    }
    struct rtl_device_entry* list = (struct rtl_device_entry*)calloc((size_t)total, sizeof *list);
    if (!list) {
        DSD_FPRINTF(stderr, "Out of memory listing the RTL-SDR devices.\n");
        return 1;
    }
    const int count = rtl_device_list(list, total, NULL);
    int duplicates = 0;
    int unreadable = 0;
    for (int i = 0; i < count; i++) {
        duplicates |= rtl_list_print_entry(list, count, &list[i]);
        unreadable += list[i].readable ? 0 : 1;
    }
    free(list);
    if (duplicates) {
        DSD_FPRINTF(stderr,
                    "Dongles that share a serial cannot be told apart: give each its own serial with rtl_eeprom "
                    "-s <serial> (with only that dongle plugged in).\n");
    }
    if (unreadable > 0) {
        DSD_FPRINTF(stderr, "%d device(s) could not be read (in use by another program, or no permission).\n",
                    unreadable);
    }
    return count < 0 ? 1 : 0;
}
#endif

int
main(int argc, char** argv) {
    dsd_opts* opts = calloc(1, sizeof(dsd_opts));
    dsd_state* state = calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "Failed to allocate memory for opts/state\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);
    /* initState() logged which buffer it could not allocate; a state missing one cannot decode. */
    if (state->init_failed || dsd_exitflag_load() != 0) {
        freeState(state);
        freeOpts(opts);
        free(opts);
        free(state);
        return 1;
    }

    int exit_rc = 1;
    int bootstrap_rc = dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &exit_rc);
    if (bootstrap_rc != DSD_BOOTSTRAP_CONTINUE) {
        /* No run, so no engine cleanup: close what the options opened before the parse stopped (#782). -P opens
           its per-call WAVs only at the run's start (issue #708), so none is left behind as an empty TEMP file. */
        dsd_engine_release_outputs(opts, state);
        freeState(state);
        freeOpts(opts);
        free(opts);
        free(state);
        return exit_rc;
    }
    int rc;
    if (opts->airspy_list) {
#ifdef USE_RADIO
        uint64_t serials[256];
        int count = rtl_device_airspy_list(serials, 256);
        for (int i = 0; i < count && i < 256; ++i) {
            DSD_FPRINTF(stdout, "Airspy serial=%016" PRIX64 "\n", serials[i]);
        }
        if (count <= 0) {
            DSD_FPRINTF(stderr, count < 0 ? "Airspy support unavailable or enumeration failed.\n"
                                          : "No Airspy devices found.\n");
        }
        rc = count < 0 ? 1 : 0;
#else
        DSD_FPRINTF(stderr, "Airspy support unavailable in this build.\n");
        rc = 1;
#endif
        dsd_engine_release_outputs(opts, state);
    } else if (opts->rtl_list) {
#ifdef USE_RADIO
        rc = rtl_list_run();
#else
        DSD_FPRINTF(stderr, "RTL-SDR support unavailable in this build.\n");
        rc = 1;
#endif
        dsd_engine_release_outputs(opts, state);
    } else {
        rc = dsd_cli_frontend_run(opts, state);
    }
    freeState(state);
    freeOpts(opts);
    free(opts);
    free(state);
    return rc;
}
