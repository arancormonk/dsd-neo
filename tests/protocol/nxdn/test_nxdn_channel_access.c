// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN channel access parameters (NXDN TS 1-A §6.5.36) through the real element decoder and channel resolver: a
 * SITE_INFO sets them in either notation, a change drops the channel map rows DFA arithmetic learned under the old
 * ones (never an imported row), and CCH_INFO prints its Curr/New/Add/Del status the same way in both notations
 * (TS 1-A §6.4.3.3).
 */

#include <dsd-neo/core/nxdn_channel_access.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../test_support/test_support.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void NXDN_Elements_Content_decode(dsd_opts* opts, dsd_state* state, const uint8_t* ElementsContent,
                                  size_t elements_bits);

/* GPS reports are not under test; the stubs keep the link free of their library. */
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

/* Two sites of one Global system: system 2, sites 0x21 and 0x22. */
enum {
    SITE_A = (2 << 12) | 0x21,
    SITE_B = (2 << 12) | 0x22,
};

/* DFA codes: base 1 is 100 MHz, base 3 is 400 MHz; step 2 is 1.25 kHz. */
enum {
    BASE_100_MHZ = 1,
    BASE_400_MHZ = 3,
    STEP_1250_HZ = 2,
};

static int
expect_long(const char* tag, long got, long want) {
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

static void
clear_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->nxdn_last_ran = (unsigned int)-1;
    DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", " ");
}

/* A SITE_INFO (0x18) naming @p location, its Channel Access Information in either notation. */
static void
site_info(dsd_opts* opts, dsd_state* state, uint32_t location, uint32_t rcn, uint32_t base, uint32_t step) {
    uint8_t bits[200];
    DSD_MEMSET(bits, 0, sizeof(bits));
    put_bits(bits, 2, 0x18U, 6);
    put_bits(bits, 8, location, 24);
    put_bits(bits, 88, rcn, 1);
    put_bits(bits, 89, step, 2);
    put_bits(bits, 91, base, 3);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
}

/* A DFA CCH_INFO (0x1A) naming @p location, with the 6-bit status @p sts and channel 1 at OFN @p ofn1. */
static void
cch_info_dfa(dsd_opts* opts, dsd_state* state, uint32_t location, uint32_t sts, uint32_t ofn1, uint32_t ifn1) {
    uint8_t bits[200];
    DSD_MEMSET(bits, 0, sizeof(bits));
    put_bits(bits, 2, 0x1AU, 6);
    put_bits(bits, 8, location, 24);
    put_bits(bits, 32, sts, 6);
    put_bits(bits, 38, 1U, 2); // 12.5 kHz
    put_bits(bits, 40, ofn1, 16);
    put_bits(bits, 56, ifn1, 16);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
}

/*
 * Another site's parameters, the same channel numbers: what the first site's computed is not what the second's
 * channel 10 is.
 */
static int
test_parameter_change_drops_learned_frequencies(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    cch_info_dfa(opts, state, SITE_A, 0x22U, 10U, 20U); // Curr, channel 1 present: resolves and learns OFN 10
    rc |= expect_long("site A channel 10 learned", state->trunk_chan_map[10], 100012500L);
    rc |= expect_long("site A channel 10", nxdn_channel_to_frequency_quiet(state, 10U), 100012500L);

    site_info(opts, state, SITE_B, 1U, BASE_400_MHZ, STEP_1250_HZ);
    rc |= expect_long("site A row dropped with its parameters", state->trunk_chan_map[10], 0L);
    rc |= expect_long("site B channel 10", nxdn_channel_to_frequency_quiet(state, 10U), 400012500L);
    rc |= expect_long("site B channel 10 verbose", nxdn_channel_to_frequency(opts, state, 10U), 400012500L);
    return rc;
}

/* A SITE_INFO in channel notation after a DFA one switches the notation and ends DFA resolution. */
static int
test_channel_notation_after_dfa(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    rc |= expect_long("DFA notation", state->nxdn_rcn, 1);
    rc |= expect_long("DFA channel 10", nxdn_channel_to_frequency_quiet(state, 10U), 100012500L);

    site_info(opts, state, SITE_A, 0U, 0U, 0U);
    rc |= expect_long("channel notation", state->nxdn_rcn, 0);
    rc |= expect_long("channel notation has no base", state->nxdn_base_freq, 0);
    rc |= expect_long("channel notation has no step", state->nxdn_step, 0);
    rc |= expect_long("DFA row dropped", state->trunk_chan_map[10], 0L);
    rc |= expect_long("channel 10 not resolved by DFA", nxdn_channel_to_frequency_quiet(state, 10U), 0L);
    rc |= expect_long("parameters still a SITE_INFO's", dsd_nxdn_ca_valid(state), 1);
    return rc;
}

/* A row the channel map import set is the operator's: no parameter change drops it, even one equal to a DFA value. */
static int
test_imported_rows_untouched(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    // The importer writes rows through the same setter (dsd_import.c).
    dsd_state_set_trunk_chan_freq(state, 5U, 155500000L);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    rc |= expect_long("imported row wins over DFA", nxdn_channel_to_frequency_quiet(state, 5U), 155500000L);
    rc |= expect_long("DFA channel 10", nxdn_channel_to_frequency_quiet(state, 10U), 100012500L);
    rc |= expect_long("DFA channel 11", nxdn_channel_to_frequency_quiet(state, 11U), 100013750L);
    // A later import lists channel 11 at the frequency DFA had learned: the row is the import's now.
    dsd_state_set_trunk_chan_freq(state, 11U, 100013750L);

    site_info(opts, state, SITE_B, 1U, BASE_400_MHZ, STEP_1250_HZ);
    rc |= expect_long("imported row kept", state->trunk_chan_map[5], 155500000L);
    rc |= expect_long("re-imported row kept", state->trunk_chan_map[11], 100013750L);
    rc |= expect_long("learned row dropped", state->trunk_chan_map[10], 0L);
    rc |= expect_long("imported row still resolves", nxdn_channel_to_frequency_quiet(state, 5U), 155500000L);
    rc |= expect_long("new site resolves channel 10", nxdn_channel_to_frequency_quiet(state, 10U), 400012500L);
    return rc;
}

/* The same SITE_INFO heard again changes nothing: the rows it computed stay, and so does the generation. */
static int
test_repeated_site_info_keeps_rows(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    (void)nxdn_channel_to_frequency_quiet(state, 10U);
    const uint32_t generation = dsd_nxdn_ca_generation(state);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    rc |= expect_long("repeat keeps the generation", (long)dsd_nxdn_ca_generation(state), (long)generation);
    rc |= expect_long("repeat keeps the row", state->trunk_chan_map[10], 100012500L);
    rc |= expect_long("parameters record their site", (long)state->nxdn_ca.location_id, (long)SITE_A);
    return rc;
}

/* Invalidated parameters resolve nothing by DFA and drop what they learned; the notation stays for the layouts. */
static int
test_invalidate_stops_dfa(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    (void)nxdn_channel_to_frequency_quiet(state, 10U);
    const uint32_t generation = dsd_nxdn_ca_generation(state);
    dsd_nxdn_ca_invalidate(state);
    rc |= expect_long("invalid", dsd_nxdn_ca_valid(state), 0);
    rc |= expect_long("generation moved", dsd_nxdn_ca_generation(state) != generation, 1);
    rc |= expect_long("notation kept", state->nxdn_rcn, 1);
    rc |= expect_long("row dropped", state->trunk_chan_map[10], 0L);
    rc |= expect_long("no DFA resolution", nxdn_channel_to_frequency_quiet(state, 10U), 0L);
    rc |= expect_long("no DFA resolution verbose", nxdn_channel_to_frequency(opts, state, 10U), 0L);
    rc |= expect_long("nothing learned", state->trunk_chan_map[10], 0L);

    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    rc |= expect_long("valid again", dsd_nxdn_ca_valid(state), 1);
    rc |= expect_long("resolves again", nxdn_channel_to_frequency_quiet(state, 10U), 100012500L);

    // A reset forgets the parameters and their rows (the site identity's clear points).
    dsd_nxdn_ca_reset(state);
    rc |= expect_long("reset invalid", dsd_nxdn_ca_valid(state), 0);
    rc |= expect_long("reset notation", state->nxdn_rcn, 0);
    rc |= expect_long("reset row dropped", state->trunk_chan_map[10], 0L);
    return rc;
}

/* The display resolver answers as the resolver does and keeps nothing. */
static int
test_display_resolver_keeps_nothing(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    const uint64_t seq = state->trunk_chan_map_seq;
    rc |= expect_long("display resolves", nxdn_channel_to_frequency_display(state, 10U), 100012500L);
    rc |= expect_long("display learns no row", state->trunk_chan_map[10], 0L);
    rc |= expect_long("display leaves the map", (long)(state->trunk_chan_map_seq - seq), 0L);
    rc |= expect_long("display record empty", state->nxdn_dfa_learned.count, 0);
    dsd_state_set_trunk_chan_freq(state, 12U, 160000000L);
    rc |= expect_long("display reads the map", nxdn_channel_to_frequency_display(state, 12U), 160000000L);
    rc |= expect_long("display null", nxdn_channel_to_frequency_display(NULL, 12U), 0L);
    return rc;
}

/* A full record still answers by arithmetic; it only stops keeping rows it could not drop later. */
static int
test_full_record_answers_without_keeping(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    for (uint16_t ch = 1U; ch <= (uint16_t)DSD_NXDN_DFA_LEARNED_MAX; ch++) {
        (void)nxdn_channel_to_frequency_quiet(state, ch);
    }
    rc |= expect_long("record full", state->nxdn_dfa_learned.count, DSD_NXDN_DFA_LEARNED_MAX);
    const uint16_t extra = (uint16_t)(DSD_NXDN_DFA_LEARNED_MAX + 1);
    rc |= expect_long("past the record resolves", nxdn_channel_to_frequency_quiet(state, extra),
                      100000000L + (long)extra * 1250L);
    rc |= expect_long("past the record not kept", state->trunk_chan_map[extra], 0L);

    site_info(opts, state, SITE_B, 1U, BASE_400_MHZ, STEP_1250_HZ);
    long kept = 0;
    for (uint16_t ch = 1U; ch <= (uint16_t)DSD_NXDN_DFA_LEARNED_MAX; ch++) {
        kept += state->trunk_chan_map[ch] != 0 ? 1 : 0;
    }
    rc |= expect_long("every learned row dropped", kept, 0L);
    rc |= expect_long("map rows untracked", (long)state->trunk_chan_map_used_count, 0L);
    return rc;
}

/* An SRV_INFO (0x19) naming @p location. */
static void
srv_info(dsd_opts* opts, dsd_state* state, uint32_t location) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));
    put_bits(bits, 2, 0x19U, 6);
    put_bits(bits, 8, location, 24);
    NXDN_Elements_Content_decode(opts, state, bits, sizeof(bits));
}

/*
 * The parameters belong to this site. Following a call, another site's SITE_INFO heard on the traffic channel (with
 * other base and step) changes nothing, and its CCH_INFO's channels are resolved for the log only; back on this site's
 * control channel, this site's CCH_INFO resolves its channel with this site's parameters before any new SITE_INFO.
 */
static int
test_foreign_site_info_while_following_keeps_parameters(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->trunk_enable = 1;
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    const uint32_t generation = dsd_nxdn_ca_generation(state);

    opts->trunk_is_tuned = 1; // a call followed to a traffic channel
    state->trunk_vc_freq[0] = 400012500L;
    site_info(opts, state, SITE_B, 1U, BASE_400_MHZ, STEP_1250_HZ);
    rc |= expect_long("foreign SITE_INFO keeps the generation", (long)dsd_nxdn_ca_generation(state), (long)generation);
    rc |= expect_long("foreign SITE_INFO keeps the base", state->nxdn_base_freq, BASE_100_MHZ);
    rc |= expect_long("foreign SITE_INFO keeps this site", (long)state->nxdn_site.location_id, (long)SITE_A);
    cch_info_dfa(opts, state, SITE_B, 0x22U, 10U, 20U); // the other site's Curr
    rc |= expect_long("foreign CCH_INFO learns no row", state->trunk_chan_map[10], 0L);
    rc |= expect_long("foreign CCH_INFO learns no IFN row", state->trunk_chan_map[20], 0L);

    opts->trunk_is_tuned = 0; // back on this site's control channel
    state->trunk_vc_freq[0] = 0;
    cch_info_dfa(opts, state, SITE_A, 0x22U, 10U, 20U);
    rc |= expect_long("this site's channel 10 with its own parameters", state->trunk_chan_map[10], 100012500L);
    rc |= expect_long("this site's control channel", state->trunk_cc_freq, 100012500L);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    return rc;
}

/*
 * Trunking switched off during a follow leaves the tuned flag set (ui_handle_trunk_toggle()), but nothing is followed
 * any more: once a carrier loss cleared the site, the site's own SITE_INFO latches it and its parameters again.
 */
static int
test_trunking_off_mid_follow_relearns_the_site(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1; // a call followed to a traffic channel
    opts->trunk_enable = 0;   // trunking switched off; the tuned flag stays
    clear_state(state);       // the carrier loss clears the site and its parameters
    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    rc |= expect_long("site latched with trunking off", dsd_nxdn_site_is(state, SITE_A), 1);
    rc |= expect_long("parameters relearned with trunking off", dsd_nxdn_ca_valid(state), 1);
    rc |= expect_long("channel 10 resolves", nxdn_channel_to_frequency_quiet(state, 10U), 100012500L);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    return rc;
}

/*
 * An SRV_INFO naming a new site, heard on the control channel the receiver chose, latches that site: the parameters
 * the old site's SITE_INFO set no longer resolve a channel until the new site's SITE_INFO sets its own.
 */
static int
test_srv_info_of_a_new_site_stops_old_parameters(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    clear_state(state);
    site_info(opts, state, SITE_B, 1U, BASE_400_MHZ, STEP_1250_HZ);
    rc |= expect_long("site B channel 10", nxdn_channel_to_frequency_quiet(state, 10U), 400012500L);

    srv_info(opts, state, SITE_A);
    rc |= expect_long("site A latched", dsd_nxdn_site_is(state, SITE_A), 1);
    rc |= expect_long("site B parameters not trusted", dsd_nxdn_ca_valid(state), 0);
    rc |= expect_long("site B row dropped", state->trunk_chan_map[10], 0L);
    rc |= expect_long("no DFA resolution meanwhile", nxdn_channel_to_frequency_quiet(state, 10U), 0L);

    site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
    rc |= expect_long("site A parameters", nxdn_channel_to_frequency_quiet(state, 10U), 100012500L);
    return rc;
}

/* The DFA CCH_INFO status: Curr, New, Add and Del from the top four bits, as the channel notation prints them. */
static int
test_cch_info_dfa_status_text(dsd_opts* opts, dsd_state* state) {
    static const struct {
        uint32_t sts;
        const char* want;
    } k_cases[] = {
        {0x22U, "Status: Current BW: 12.5 kHz"},         {0x12U, "Status: New BW: 12.5 kHz"},
        {0x0AU, "Status: Candidate Added BW: 12.5 kHz"}, {0x06U, "Status: Candidate Deleted BW: 12.5 kHz"},
        {0x23U, "Status: Current BW: 12.5 kHz"}, // both channels present
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        clear_state(state);
        site_info(opts, state, SITE_A, 1U, BASE_100_MHZ, STEP_1250_HZ);
        dsd_test_capture_stderr cap;
        char log[2048];
        const int captured = dsd_test_capture_stderr_begin(&cap, "nxdn_cch_status") == 0;
        cch_info_dfa(opts, state, SITE_A, k_cases[i].sts, 10U, 20U);
        if (captured) {
            (void)dsd_test_capture_stderr_end(&cap);
        }
        const int read = captured && dsd_test_capture_stderr_read(&cap, log, sizeof(log)) == 0;
        char label[96];
        DSD_SNPRINTF(label, sizeof(label), "status 0x%02X prints '%s'", (unsigned)k_cases[i].sts, k_cases[i].want);
        rc |= expect_long(label, read && strstr(log, k_cases[i].want) != NULL, 1);
        DSD_SNPRINTF(label, sizeof(label), "status 0x%02X has no channel-present label", (unsigned)k_cases[i].sts);
        rc |= expect_long(label, read && strstr(log, "Current 1") == NULL && strstr(log, "Current 2") == NULL, 1);
    }
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
    int rc = 0;
    rc |= test_parameter_change_drops_learned_frequencies(opts, state);
    rc |= test_channel_notation_after_dfa(opts, state);
    rc |= test_imported_rows_untouched(opts, state);
    rc |= test_repeated_site_info_keeps_rows(opts, state);
    rc |= test_invalidate_stops_dfa(opts, state);
    rc |= test_display_resolver_keeps_nothing(opts, state);
    rc |= test_full_record_answers_without_keeping(opts, state);
    rc |= test_cch_info_dfa_status_text(opts, state);
    rc |= test_foreign_site_info_while_following_keeps_parameters(opts, state);
    rc |= test_trunking_off_mid_follow_relearns_the_site(opts, state);
    rc |= test_srv_info_of_a_new_site_stops_old_parameters(opts, state);

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
