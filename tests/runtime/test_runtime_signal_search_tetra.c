// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Signal search TETRA identification in the runtime records (issue #748): the TETRA range value, the TETRA site note
   joined to a visit's site record, its withdrawal, and the text a detection shows, within the hit's and the journal's
   site field. */

#include <assert.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DOT " \xC2\xB7 "

static void
range_validates(const dsd_signal_search_range* r, const char* want) {
    char error[160];
    const int rc = dsd_signal_search_validate(r, error, sizeof error);
    assert(want ? rc < 0 && strcmp(error, want) == 0 : rc == 0 && error[0] == '\0');
}

/* digital 10 is TETRA, for NFM ranges only; an AM range keeps "analog only", and 0-9 keep their meaning. */
static void
test_tetra_range_value(void) {
    dsd_signal_search_range r;
    dsd_signal_search_range_default(&r);
    for (int digital = 0; digital <= DSD_SEARCH_DIGITAL_TETRA; ++digital) {
        r.digital = digital;
        range_validates(&r, NULL);
        assert(dsd_signal_search_range_is_tetra(&r) == (digital == DSD_SEARCH_DIGITAL_TETRA));
    }
    _Static_assert(DSD_SEARCH_DIGITAL_TETRA == 10, "digital 10 is TETRA");
    r.digital = 11;
    range_validates(&r, "AM requires analog-only monitoring");
    r.digital = -1;
    range_validates(&r, "AM requires analog-only monitoring");
    r.am = 1;
    r.bandwidth_hz = 10000;
    r.squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    r.digital = DSD_SEARCH_DIGITAL_TETRA;
    range_validates(&r, "AM requires analog-only monitoring");
    assert(!dsd_signal_search_range_is_tetra(&r));
    r.digital = 0;
    range_validates(&r, NULL);
    assert(!dsd_signal_search_range_is_tetra(NULL));
}

/* A config needs TETRA when an enabled range is TETRA; a disabled TETRA range does not count. */
static void
test_config_needs_tetra(void) {
    dsd_signal_search_config config;
    DSD_MEMSET(&config, 0, sizeof config);
    config.session_id = 1;
    config.range_count = 2;
    dsd_signal_search_range_default(&config.ranges[0]);
    dsd_signal_search_range_default(&config.ranges[1]);
    assert(!dsd_signal_search_config_needs_tetra(&config));
    config.ranges[1].digital = DSD_SEARCH_DIGITAL_TETRA;
    assert(dsd_signal_search_config_needs_tetra(&config));
    config.ranges[1].enabled = 0;
    assert(!dsd_signal_search_config_needs_tetra(&config));
    /* Ranges past range_count are not the config's. */
    config.ranges[1].enabled = 1;
    config.range_count = 1;
    assert(!dsd_signal_search_config_needs_tetra(&config));
    assert(!dsd_signal_search_config_needs_tetra(NULL));
    config.range_count = 2;
    char error[160];
    assert(dsd_signal_search_validate_config(&config, error, sizeof error) == 0);
}

static dsd_signal_search_site_note
tmo_note(uint16_t mcc, uint16_t mnc) {
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof note);
    note.kind = DSD_SEARCH_SITE_NOTE_TETRA;
    note.tetra.mode = DSD_SEARCH_TETRA_TMO;
    note.tetra.mcc = mcc;
    note.tetra.mnc = mnc;
    return note;
}

static dsd_signal_search_site_note
dmo_note(uint8_t route, uint8_t presence_only) {
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof note);
    note.kind = DSD_SEARCH_SITE_NOTE_TETRA;
    note.tetra.mode = DSD_SEARCH_TETRA_DMO;
    note.tetra.route = route;
    note.tetra.presence_only = presence_only;
    return note;
}

static dsd_signal_search_site_note
withdraw_note(void) {
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof note);
    note.kind = DSD_SEARCH_SITE_NOTE_TETRA_WITHDRAW;
    return note;
}

static int
shows(const dsd_signal_search_site* record, const char* want) {
    char text[sizeof(((dsd_signal_search_hit*)0)->site)];
    const size_t n = dsd_signal_search_site_format(record, text, sizeof text);
    return strcmp(text, want) == 0 && n == strlen(want);
}

/* Each TMO variant: the main carrier in MHz with five decimals, no main carrier (no LA then), the LA only when the
   note has it, and the colour-0 / no-SYSINFO identity alone. */
static void
test_tmo_format(void) {
    dsd_signal_search_site record;
    dsd_signal_search_site_clear(&record);
    dsd_signal_search_site_note note = tmo_note(228, 8889);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(strcmp(record.protocol, "TETRA") == 0 && record.tetra.mode == DSD_SEARCH_TETRA_TMO);
    assert(shows(&record, "MCC 228" DOT "MNC 8889"));
    assert(!dsd_signal_search_site_apply(&record, &note));

    note.tetra.la_valid = 1;
    note.tetra.la = 12;
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_HZ;
    note.tetra.main_hz = 426706250;
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "Main carrier 426.70625 MHz" DOT "MCC 228" DOT "MNC 8889" DOT "LA 12"));
    assert(!dsd_signal_search_site_apply(&record, &note));

    /* No LA part without la_valid; the stored LA is not read. */
    note.tetra.la_valid = 0;
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "Main carrier 426.70625 MHz" DOT "MCC 228" DOT "MNC 8889"));
    /* A value the note does not mark valid makes no difference to the record. */
    note.tetra.la = 99;
    assert(!dsd_signal_search_site_apply(&record, &note));

    /* No conventional-access main carrier: the LA is omitted. */
    note.tetra.la_valid = 1;
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_NO_CA;
    note.tetra.main_hz = 426706250;
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "No main carrier" DOT "MCC 228" DOT "MNC 8889"));

    /* SYSINFO with no main carrier shown: the LA alone. */
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_NONE;
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "MCC 228" DOT "MNC 8889" DOT "LA 99"));

    /* A whole-kHz carrier keeps its five decimals. */
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_HZ;
    note.tetra.main_hz = 390000000;
    note.tetra.la_valid = 0;
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "Main carrier 390.00000 MHz" DOT "MCC 228" DOT "MNC 8889"));

    /* Another identity replaces the record whole. */
    note = tmo_note(1, 2);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "MCC 1" DOT "MNC 2"));
}

/* DMO: the route of a checked DMAC-SYNC, or "presence signal" for a presence-only confirmation. */
static void
test_dmo_format(void) {
    static const struct {
        uint8_t route;
        uint8_t presence_only;
        const char* text;
    } cases[] = {
        {DSD_SEARCH_TETRA_ROUTE_DIRECT, 0, "Direct"},
        {DSD_SEARCH_TETRA_ROUTE_REPEATER, 0, "Via repeater"},
        {DSD_SEARCH_TETRA_ROUTE_GATEWAY, 0, "Via gateway"},
        {DSD_SEARCH_TETRA_ROUTE_REPEATER_GATEWAY, 0, "Via repeater/gateway"},
        {DSD_SEARCH_TETRA_ROUTE_NONE, 1, "presence signal"},
        {DSD_SEARCH_TETRA_ROUTE_GATEWAY, 1, "presence signal"},
        {DSD_SEARCH_TETRA_ROUTE_NONE, 0, ""},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        dsd_signal_search_site record;
        dsd_signal_search_site_clear(&record);
        dsd_signal_search_site_note note = dmo_note(cases[i].route, cases[i].presence_only);
        /* TMO fields in a DMO note are not the record's. */
        note.tetra.mcc = 228;
        note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_HZ;
        note.tetra.main_hz = 426706250;
        assert(dsd_signal_search_site_apply(&record, &note));
        assert(strcmp(record.protocol, "TETRA") == 0 && record.tetra.mode == DSD_SEARCH_TETRA_DMO);
        assert(shows(&record, cases[i].text));
    }
    /* The route follows the latest checked DMAC-SYNC. */
    dsd_signal_search_site record;
    dsd_signal_search_site_clear(&record);
    dsd_signal_search_site_note note = dmo_note(DSD_SEARCH_TETRA_ROUTE_REPEATER, 0);
    assert(dsd_signal_search_site_apply(&record, &note));
    note = dmo_note(DSD_SEARCH_TETRA_ROUTE_DIRECT, 0);
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "Direct"));
    /* A mode the hooks do not define is no note. */
    note.tetra.mode = 3;
    assert(!dsd_signal_search_site_apply(&record, &note));
    note.tetra.mode = 0;
    assert(!dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "Direct"));
}

/* A withdrawal empties the record's text and stays: no later TETRA note of the visit, of either mode, undoes it. Only
   clearing the record (a new landing) or another protocol's note replaces it. */
static void
test_withdraw_latches(void) {
    dsd_signal_search_site record;
    dsd_signal_search_site_clear(&record);
    dsd_signal_search_site_note note = tmo_note(228, 8889);
    note.tetra.la_valid = 1;
    note.tetra.la = 12;
    assert(dsd_signal_search_site_apply(&record, &note));
    dsd_signal_search_site_note withdraw = withdraw_note();
    assert(dsd_signal_search_site_apply(&record, &withdraw));
    assert(record.withdrawn && strcmp(record.protocol, "TETRA") == 0 && shows(&record, ""));
    assert(!dsd_signal_search_site_apply(&record, &withdraw));
    assert(!dsd_signal_search_site_apply(&record, &note));
    dsd_signal_search_site_note other = tmo_note(1, 2);
    assert(!dsd_signal_search_site_apply(&record, &other));
    dsd_signal_search_site_note dmo = dmo_note(DSD_SEARCH_TETRA_ROUTE_DIRECT, 0);
    assert(!dsd_signal_search_site_apply(&record, &dmo));
    assert(record.withdrawn && shows(&record, ""));

    /* A withdrawal before any site note withdraws too. */
    dsd_signal_search_site_clear(&record);
    assert(dsd_signal_search_site_apply(&record, &withdraw));
    assert(!dsd_signal_search_site_apply(&record, &dmo) && shows(&record, ""));

    /* Another protocol's note replaces the record, as it replaces any. */
    dsd_signal_search_site_note p25;
    DSD_MEMSET(&p25, 0, sizeof p25);
    p25.kind = DSD_SEARCH_SITE_NOTE_P25_NETWORK;
    p25.wacn = 0xBEE00;
    p25.sysid = 0x1A3;
    assert(dsd_signal_search_site_apply(&record, &p25));
    assert(!record.withdrawn && strcmp(record.protocol, "P25") == 0 && shows(&record, "WACN BEE00" DOT "SYS 1A3"));
    /* And a clear starts the next visit with nothing withdrawn. */
    assert(dsd_signal_search_site_apply(&record, &withdraw));
    dsd_signal_search_site_clear(&record);
    assert(!record.withdrawn && dsd_signal_search_site_apply(&record, &note));
    assert(shows(&record, "MCC 228" DOT "MNC 8889" DOT "LA 12"));
}

/* The longest TMO line and the no-main-carrier one, in UTF-8 bytes, fit the hit's site field and the journal's (which
   stores sizeof the hit's field, NUL included) without truncation. */
static void
test_worst_case_bytes(void) {
    static const char worst[] = "Main carrier 1002.38750 MHz" DOT "MCC 1023" DOT "MNC 16383" DOT "LA 16383";
    static const char no_ca[] = "No main carrier" DOT "MCC 1023" DOT "MNC 16383";
    dsd_signal_search_site record;
    dsd_signal_search_site_clear(&record);
    dsd_signal_search_site_note note = tmo_note(1023, 16383);
    note.tetra.la_valid = 1;
    note.tetra.la = 16383;
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_HZ;
    note.tetra.main_hz = 1002387500;
    assert(dsd_signal_search_site_apply(&record, &note));
    const dsd_signal_search_hit hit = {0};
    _Static_assert(sizeof hit.site == 96, "the hit's site field is 96 bytes");
    char text[sizeof hit.site];
    const size_t n = dsd_signal_search_site_format(&record, text, sizeof text);
    assert(n == sizeof worst - 1 && strcmp(text, worst) == 0);
    assert(n == 64 && n + 1 <= sizeof hit.site);
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_NO_CA;
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(dsd_signal_search_site_format(&record, text, sizeof text) == sizeof no_ca - 1);
    assert(strcmp(text, no_ca) == 0);
    /* The largest uint32 carrier still fits, five decimals and all. */
    note.tetra.main_state = DSD_SEARCH_TETRA_MAIN_HZ;
    note.tetra.main_hz = UINT32_MAX;
    note.tetra.mcc = UINT16_MAX;
    note.tetra.mnc = UINT16_MAX;
    note.tetra.la = UINT16_MAX;
    assert(dsd_signal_search_site_apply(&record, &note));
    assert(dsd_signal_search_site_format(&record, text, sizeof text) < sizeof hit.site - 1);
    assert(shows(&record, "Main carrier 4294.96730 MHz" DOT "MCC 65535" DOT "MNC 65535" DOT "LA 65535"));
}

int
main(void) {
    test_tetra_range_value();
    test_config_needs_tetra();
    test_tmo_format();
    test_dmo_format();
    test_withdraw_latches();
    test_worst_case_bytes();
    return 0;
}
