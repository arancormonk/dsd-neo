// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Signal search site notes from NXDN: the current-site location ID of a SITE_INFO, SRV_INFO or CCH_INFO, which the
 * channel decoders pass on only after its CRC, notes the site and system once the transmission is confirmed (a short
 * SACCH CRC alone does not confirm it). An adjacent site notes nothing.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, const uint8_t* ElementsContent,
                                  size_t elements_bits);

/* The channel plan and GPS reports are not under test; the stubs keep the link free of their libraries. */
long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return 0;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    (void)state;
    (void)channel;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes) {
    (void)opts;
    (void)state;
    (void)input;
    (void)slot;
    (void)len_bytes;
    return 0U;
}

static int g_notes;
static dsd_signal_search_site_note g_note;

static void
capture_site(const dsd_state* state, const dsd_signal_search_site_note* note) {
    (void)state;
    ++g_notes;
    g_note = *note;
}

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
put_bits(uint8_t* bits, int at, uint32_t value, int width) {
    for (int i = 0; i < width; ++i) {
        bits[at + i] = (uint8_t)((value >> (width - 1 - i)) & 1U);
    }
}

/* Global category: system code 3, site code 12. */
enum { LOCATION_SYS3_SITE12 = (3 << 12) | 12 };

static void
message(dsd_opts* opts, dsd_state* state, uint8_t type, uint32_t location) {
    uint8_t bits[200];
    DSD_MEMSET(bits, 0, sizeof(bits));
    put_bits(bits, 2, type, 6);
    put_bits(bits, 8, location, 24);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        return 1;
    }
    dsd_signal_search_site_set(capture_site);
    int rc = 0;
    static const uint8_t types[3] = {0x18, 0x19, 0x1A}; // SITE_INFO, SRV_INFO, CCH_INFO
    for (int i = 0; i < 3; ++i) {
        // A confirmed transmission's checked message names the site.
        DSD_MEMSET(state, 0, sizeof(*state));
        state->nxdn_confirmed = 1;
        g_notes = 0;
        message(opts, state, types[i], LOCATION_SYS3_SITE12);
        rc |= expect_int("confirmed location notes once", g_notes, 1);
        rc |= expect_int("location note kind", g_note.kind, DSD_SEARCH_SITE_NOTE_NXDN);
        rc |= expect_int("location note site", g_note.nxdn_site, 12);
        rc |= expect_int("location note system", (long)g_note.nxdn_sys, 3);

        // One short SACCH CRC has not confirmed it: decoded, not shown.
        DSD_MEMSET(state, 0, sizeof(*state));
        g_notes = 0;
        message(opts, state, types[i], LOCATION_SYS3_SITE12);
        rc |= expect_int("unconfirmed location is decoded", state->nxdn_location_site_code, 12);
        rc |= expect_int("unconfirmed location notes nothing", g_notes, 0);
    }

    // A reserved category names no site.
    DSD_MEMSET(state, 0, sizeof(*state));
    state->nxdn_confirmed = 1;
    g_notes = 0;
    message(opts, state, 0x18, (3U << 22) | LOCATION_SYS3_SITE12);
    rc |= expect_int("reserved category notes nothing", g_notes, 0);

    // An adjacent site is another site.
    DSD_MEMSET(state, 0, sizeof(*state));
    state->nxdn_confirmed = 1;
    g_notes = 0;
    message(opts, state, 0x1B, LOCATION_SYS3_SITE12);
    rc |= expect_int("adjacent site notes nothing", g_notes, 0);

    dsd_signal_search_site_set(NULL);
    free(state);
    free(opts);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
