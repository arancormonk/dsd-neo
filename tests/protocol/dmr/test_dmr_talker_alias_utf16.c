// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR talker alias, UTF-16 format: the alias text must be transcoded by dsd-neo rather than
 * handed unit-by-unit to fprintf("%lc") (the crash path of issue #358). A surrogate pair is
 * one character, an unpaired half is U+FFFD, and the stored alias counts them that way.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

int
dsd_event_enrich_alias(dsd_state* state, uint8_t slot, uint64_t epoch, const char* alias) {
    dsd_call_snapshot call;
    if (state == NULL || state->event_history_s == NULL || slot >= DSD_CALL_STATE_SLOT_COUNT || alias == NULL
        || dsd_call_state_get(state, slot, &call) <= 0 || call.epoch != epoch) {
        return 0;
    }
    DSD_SNPRINTF(state->event_history_s[slot].Event_History_Items[0].alias,
                 sizeof(state->event_history_s[slot].Event_History_Items[0].alias), "%s", alias);
    DSD_SNPRINTF(state->generic_talker_alias[slot], sizeof(state->generic_talker_alias[slot]), "%s", alias);
    state->event_history_s[slot].revision++;
    return 1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_unicode_supported(void) {
    return 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors) {
    (void)opts;
    (void)state;
    (void)lcw_bits;
    (void)irrecoverable_errors;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif

static void
seed_active_call(dsd_state* state, uint8_t slot, uint32_t source, uint32_t target) {
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) >= 0);
    state->event_history_s[slot].Event_History_Items[0].source_id = source;
    state->event_history_s[slot].Event_History_Items[0].target_id = target;
}

static void
value_to_bits_msb(uint8_t* bits_out, size_t bit_offset, size_t bits_out_sz, uint32_t value, uint8_t bit_count) {
    assert(bit_offset + bit_count <= bits_out_sz);
    for (uint8_t b = 0; b < bit_count; b++) {
        bits_out[bit_offset + b] = (uint8_t)((value >> (bit_count - 1u - b)) & 1u);
    }
}

/* What U+4739, the pair U+1F600, a lone high surrogate and 'A' must look like on stderr. */
static const char kExpected[] = "\xE4\x9C\xB9"
                                "\xF0\x9F\x98\x80"
                                "\xEF\xBF\xBD"
                                "A";

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(opts != NULL && st != NULL);
    st->event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    assert(st->event_history_s != NULL);
    st->currentslot = 0;

    /* The alias block store holds bits; five UTF-16 code units, MSB first. */
    static const uint16_t units[] = {0x4739U, 0xD83DU, 0xDE00U, 0xD800U, 0x0041U};
    DSD_MEMSET(st->dmr_pdu_sf[0], 0, sizeof(st->dmr_pdu_sf[0]));
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        value_to_bits_msb(st->dmr_pdu_sf[0], i * 16U, sizeof(st->dmr_pdu_sf[0]), units[i], 16);
    }
    seed_active_call(st, 0U, 700001U, 0U);

    char buf[1024];
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "dmr_talker_alias_utf16") == 0);
    dmr_talker_alias_lc_decode(opts, st, 0, 0, 16, 5);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, sizeof buf) == 0);

    const char* marker = " Talker Alias: ";
    const char* p = strstr(buf, marker);
    if (p == NULL || memcmp(p + strlen(marker), kExpected, sizeof kExpected - 1U) != 0) {
        DSD_FPRINTF(stderr, "talker alias printed: %s\n", buf);
        assert(0 && "UTF-16 alias was not transcoded as expected");
    }

    /* Three non-ASCII characters (not four units) plus 'A', then the "; " the decoder appends. */
    if (strcmp(st->generic_talker_alias[0], "***A; ") != 0) {
        DSD_FPRINTF(stderr, "stored alias: '%s'\n", st->generic_talker_alias[0]);
        assert(0 && "stored alias must count a surrogate pair as one character");
    }

    /* Observing a call lazily allocates the call-state extension. */
    dsd_state_ext_free_all(st);
    free(st->event_history_s);
    free(st);
    free(opts);
    DSD_FPRINTF(stderr, "DMR_TALKER_ALIAS_UTF16: PASS\n");
    return 0;
}
