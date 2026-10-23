// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Signal search site notes from NXDN: the current-site location ID of a SITE_INFO, SRV_INFO or CCH_INFO, which the
 * channel decoders pass on only after its CRC, notes the site and system once the transmission is confirmed (a short
 * SACCH CRC alone does not confirm it). An adjacent site notes nothing. The decoder keeps the location (category,
 * system, site and the RAN the site code gives) under the same rule, and never for the reserved category.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_internal.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, const uint8_t* ElementsContent,
                                  size_t elements_bits);

/* The channel plan and GPS reports are not under test; the stubs keep the link free of their libraries. */
/* The frequency every channel maps to: none unless a case sets one. */
static long g_channel_freq;

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency(dsd_opts* opts, dsd_state* state, uint16_t channel) {
    (void)opts;
    (void)state;
    (void)channel;
    return g_channel_freq;
}

long int
// NOLINTNEXTLINE(misc-use-internal-linkage)
nxdn_channel_to_frequency_quiet(dsd_state* state, uint16_t channel) {
    (void)state;
    (void)channel;
    return g_channel_freq;
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

/* Global category: 10-bit system code 5, 12-bit site code 0xABC (NXDN TS 1-A 6.5.2). */
enum { LOCATION_SYS5_SITE_ABC = (5 << 12) | 0xABC };

static void
clear_state(dsd_state* state) {
    dsd_state_ext_free_all(state); // a CAC's control channel and grant keep state in extensions
    DSD_MEMSET(state, 0, sizeof(*state));
    state->nxdn_last_ran = (unsigned int)-1;
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", " ");
}

static int
expect_nothing_kept(const char* tag, const dsd_state* state) {
    int rc = 0;
    if (state->nxdn_last_ran != (unsigned int)-1 || state->nxdn_location_site_code != 0U
        || state->nxdn_location_sys_code != 0U || state->nxdn_location_category[0] != ' '
        || state->nxdn_location_category[1] != '\0') {
        DSD_FPRINTF(stderr, "%s: kept RAN %u site %u sys %u category '%s'\n", tag, state->nxdn_last_ran,
                    (unsigned)state->nxdn_location_site_code, (unsigned)state->nxdn_location_sys_code,
                    state->nxdn_location_category);
        rc = 1;
    }
    return rc;
}

static void
message(dsd_opts* opts, dsd_state* state, uint8_t type, uint32_t location) {
    uint8_t bits[200];
    DSD_MEMSET(bits, 0, sizeof(bits));
    put_bits(bits, 2, type, 6);
    put_bits(bits, 8, location, 24);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
}

static int g_tunes;

/* A voice channel tune the tuner takes at once, as an RTL-family stream's retune moves the tuned frequency. */
static dsd_trunk_tune_result
tune_moves_tuner(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)state;
    (void)ted_sps;
    (void)request_id;
    ++g_tunes;
    opts->trunk_is_tuned = 1;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* The RAN @p ran in the six bits a channel's trellis output carries it in (nxdn_ran_from_trellis()). */
static void
put_ran(uint8_t* trellis, uint32_t ran) {
    put_bits(trellis, 2, ran, 6);
}

/* The received identity record (dsd_state::rx_id): the location of the same checked, confirmed messages (never the RAN
   the site code gives), and the RAN a CAC, a FACCH2/UDCH or a confirmed SACCH carries; a CAC also names its carrier as
   the control channel. A failed CRC names nothing. */
static int
test_received_identity(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;

    static const struct {
        uint32_t location;
        long category;
        long sys;
        long site;
    } k_locations[3] = {
        {LOCATION_SYS3_SITE12, 0, 3, 12},                         // Global
        {(2U << 22) | (0x1234U << 8) | 0x56U, 1, 0x1234, 0x56},   // Regional
        {(1U << 22) | (0x1ABCDU << 5) | 0x1BU, 2, 0x1ABCD, 0x1B}, // Local
    };

    for (int i = 0; i < 3; ++i) {
        clear_state(state);
        state->nxdn_confirmed = 1;
        message(opts, state, 0x18, k_locations[i].location);
        rc |= expect_int("rx-id location parts", (long)id->valid, DSD_RXID_NXDN_LOCATION);
        rc |= expect_int("rx-id location category", id->nxdn_category, k_locations[i].category);
        rc |= expect_int("rx-id location system", (long)id->nxdn_sys, k_locations[i].sys);
        rc |= expect_int("rx-id location site", id->nxdn_site, k_locations[i].site);
    }
    clear_state(state);
    message(opts, state, 0x18, LOCATION_SYS3_SITE12);
    state->nxdn_confirmed = 1;
    message(opts, state, 0x18, (3U << 22) | LOCATION_SYS3_SITE12);
    message(opts, state, 0x1B, LOCATION_SYS3_SITE12);
    rc |= expect_int("rx-id unconfirmed, reserved and adjacent locations note nothing", (long)id->valid, 0);

    uint8_t trellis[400];
    uint8_t m_data[64];
    DSD_MEMSET(trellis, 0, sizeof(trellis));
    DSD_MEMSET(m_data, 0, sizeof(m_data));
    put_ran(trellis, 21U);

    // A CAC: the RAN, and the control channel where the receiver is tuned.
    clear_state(state);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = 451512500U;
    nxdn_handle_cac(opts, state, trellis, m_data, 1U);
    rc |= expect_int("rx-id failed cac notes nothing", (long)id->valid, 0);
    nxdn_handle_cac(opts, state, trellis, m_data, 0U);
    rc |= expect_int("rx-id cac parts", (long)id->valid, DSD_RXID_ACCESS | DSD_RXID_CC_FREQ);
    rc |= expect_int("rx-id cac ran", id->access, 21);
    rc |= expect_int("rx-id cac ran kind", id->access_kind, DSD_ACCESS_CODE_RAN);
    rc |= expect_int("rx-id cac control channel", (long)id->cc_freq_hz, 451512500L);

    // Observation timing: a CAC carrying a group call grant names the control channel it came on, before its own grant
    // moves the receiver to the voice channel.
    clear_state(state);
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_moves_tuner;
    dsd_trunk_tuning_hooks_set(hooks);
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    state->p25_cc_freq = 451512500L;
    state->trunk_cc_freq = 451512500L;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_vc_sync_time = dsd_decode_time() - 4;
    g_channel_freq = 452012500L;
    g_tunes = 0;
    uint8_t grant[400];
    DSD_MEMSET(grant, 0, sizeof(grant));
    put_ran(grant, 21U);
    put_bits(grant, 8 + 2, 0x04, 6);   // VCALL_ASSGN
    put_bits(grant, 8 + 16, 1, 3);     // group call
    put_bits(grant, 8 + 24, 2100, 16); // source
    put_bits(grant, 8 + 40, 1100, 16); // target
    put_bits(grant, 8 + 62, 16, 10);   // channel
    nxdn_handle_cac(opts, state, grant, m_data, 0U);
    rc |= expect_int("rx-id cac grant moved the tuner", (long)opts->rtlsdr_center_freq, 452012500L);
    rc |= expect_int("rx-id cac grant tuned once", g_tunes, 1);
    rc |= expect_int("rx-id cac grant keeps the control channel", (long)id->cc_freq_hz, 451512500L);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    g_channel_freq = 0;
    opts->trunk_enable = 0;
    opts->trunk_tune_group_calls = 0;
    opts->trunk_is_tuned = 0;
    opts->audio_in_type = 0;
    opts->rtlsdr_center_freq = 0U;

    // A FACCH2 / UDCH: its RAN once its CRC passed.
    clear_state(state);
    nxdn_handle_facch2_udch(opts, state, trellis, m_data, 1U, 2U, 1U);
    rc |= expect_int("rx-id failed facch2 notes nothing", (long)id->valid, 0);
    nxdn_handle_facch2_udch(opts, state, trellis, m_data, 2U, 2U, 1U);
    rc |= expect_int("rx-id facch2 ran", (long)(id->valid == DSD_RXID_ACCESS ? id->access : -1), 21);

    // A superframe SACCH: its RAN once the transmission is confirmed.
    clear_state(state);
    state->nxdn_sacch_non_superframe = 0;
    nxdn_handle_sacch(opts, state, trellis, m_data, 3U, 3U);
    rc |= expect_int("rx-id unconfirmed sacch notes nothing", (long)id->valid, 0);
    state->nxdn_confirmed = 1;
    nxdn_handle_sacch(opts, state, trellis, m_data, 3U, 3U);
    rc |= expect_int("rx-id confirmed sacch ran", (long)(id->valid == DSD_RXID_ACCESS ? id->access : -1), 21);
    clear_state(state);
    state->nxdn_confirmed = 1;
    state->nxdn_sacch_non_superframe = 1;
    nxdn_handle_sacch(opts, state, trellis, m_data, 3U, 3U);
    rc |= expect_int("rx-id confirmed non-superframe sacch ran", (long)(id->valid == DSD_RXID_ACCESS ? id->access : -1),
                     21);
    nxdn_handle_sacch(opts, state, trellis, m_data, 3U, 4U);
    rc |= expect_int("rx-id failed sacch keeps the checked ran", id->access, 21);
    return rc;
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    dsd_signal_search_site_set(capture_site);
    int rc = 0;
    static const uint8_t types[3] = {0x18, 0x19, 0x1A}; // SITE_INFO, SRV_INFO, CCH_INFO
    for (int i = 0; i < 3; ++i) {
        // A confirmed transmission's checked message names the site.
        clear_state(state);
        state->nxdn_confirmed = 1;
        g_notes = 0;
        message(opts, state, types[i], LOCATION_SYS3_SITE12);
        rc |= expect_int("confirmed location notes once", g_notes, 1);
        rc |= expect_int("location note kind", g_note.kind, DSD_SEARCH_SITE_NOTE_NXDN);
        rc |= expect_int("location note site", g_note.nxdn_site, 12);
        rc |= expect_int("location note system", (long)g_note.nxdn_sys, 3);
        rc |= expect_int("confirmed location site", state->nxdn_location_site_code, 12);
        rc |= expect_int("confirmed location system", (long)state->nxdn_location_sys_code, 3);
        rc |= expect_int("confirmed location RAN", (long)state->nxdn_last_ran, 12);

        // The Global site code is 12 bits wide.
        clear_state(state);
        state->nxdn_confirmed = 1;
        g_notes = 0;
        message(opts, state, types[i], LOCATION_SYS5_SITE_ABC);
        rc |= expect_int("global 12-bit site note", g_note.nxdn_site, 0xABC);
        rc |= expect_int("global 12-bit site system note", (long)g_note.nxdn_sys, 5);
        rc |= expect_int("global 12-bit site", state->nxdn_location_site_code, 0xABC);
        rc |= expect_int("global 12-bit site RAN", (long)state->nxdn_last_ran, 0xABC % 64);

        // One short SACCH CRC has not confirmed it: decoded, neither kept nor shown.
        clear_state(state);
        g_notes = 0;
        message(opts, state, types[i], LOCATION_SYS3_SITE12);
        rc |= expect_nothing_kept("unconfirmed location", state);
        rc |= expect_int("unconfirmed location notes nothing", g_notes, 0);
    }

    // A reserved category names no site, and gives no RAN.
    clear_state(state);
    state->nxdn_confirmed = 1;
    g_notes = 0;
    message(opts, state, 0x18, (3U << 22) | LOCATION_SYS3_SITE12);
    rc |= expect_int("reserved category notes nothing", g_notes, 0);
    rc |= expect_nothing_kept("reserved category", state);

    // An adjacent site is another site.
    clear_state(state);
    state->nxdn_confirmed = 1;
    g_notes = 0;
    message(opts, state, 0x1B, LOCATION_SYS3_SITE12);
    rc |= expect_int("adjacent site notes nothing", g_notes, 0);

    rc |= test_received_identity(opts, state);

    dsd_signal_search_site_set(NULL);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
