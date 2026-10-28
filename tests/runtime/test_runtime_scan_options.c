// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    size_t count;
    size_t offsets[4], lengths[4];
    int whole[4];
    char paths[4][64];
} file_spans;

static int
collect_file_span(void* context, const char* option, const char* path, size_t offset, size_t length, int whole) {
    file_spans* spans = context;
    assert(spans->count < 4);
    assert(strcmp(option, "-K") == 0 || strcmp(option, "-G") == 0 || strcmp(option, "--dmr-tg-key-csv") == 0);
    const size_t n = spans->count++;
    spans->offsets[n] = offset;
    spans->lengths[n] = length;
    spans->whole[n] = whole;
    DSD_SNPRINTF(spans->paths[n], sizeof spans->paths[n], "%s", path);
    return 0;
}

static void
check_file_spans(void) {
    const char* text = "-H 0123456789 -K \"a b.csv\" --dmr-tg-key-csv='x.csv' -G foo\" bar\".csv";
    const char* raw[] = {"\"a b.csv\"", "--dmr-tg-key-csv='x.csv'", "foo\" bar\".csv"};
    const char* paths[] = {"a b.csv", "x.csv", "foo bar.csv"};
    file_spans spans = {0};
    assert(dsd_scan_options_visit_files(text, &spans, collect_file_span) == 0 && spans.count == 3);
    for (size_t i = 0; i < spans.count; ++i) {
        assert(spans.lengths[i] == strlen(raw[i]));
        assert(memcmp(text + spans.offsets[i], raw[i], spans.lengths[i]) == 0);
        assert(strcmp(spans.paths[i], paths[i]) == 0);
        assert(spans.whole[i] == (i == 1));
    }
    spans.count = 0;
    assert(dsd_scan_options_visit_files("-K", &spans, collect_file_span) == -1 && spans.count == 0);
    assert(dsd_scan_options_visit_files("-K a.csv -K", &spans, collect_file_span) == -1 && spans.count == 1);
}

/* --- Issue #637: spreadsheet-quoted options cells --- */

/* Parse a cell as a loader does: @p legacy is what the loader has always parsed, @p cell the trimmed cell. */
static int
parse_cell(const char* legacy, const char* cell, unsigned int mode, dsd_scan_options* out) {
    const size_t length = strlen(cell);
    const int wrapped = length >= 2 && cell[0] == '"' && cell[length - 1] == '"';
    char error[192];
    return dsd_scan_options_parse_cell(legacy, wrapped ? cell + 1 : NULL, wrapped ? length - 2 : 0, mode, 1, out, error,
                                       sizeof(error));
}

/* A spreadsheet saves a cell holding a double quote wrapped in double quotes, each quote inside doubled. Both loaders
 * read such a cell's content; every cell either loader accepted before keeps its meaning. */
static void
check_csv_quoted_cells(void) {
    dsd_scan_options parsed;
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    /* A trunk-scan target's loader parses the cell without its outer quotes; a channel map's parses it as it is. */
    static const char quoted[] = "\"--dcc-allow 4/7 --dcc-name \"\"7=St Mary's\"\"\"";
    char interior[sizeof quoted];
    DSD_SNPRINTF(interior, sizeof interior, "%s", quoted + 1);
    interior[strlen(interior) - 1] = '\0';
    assert(parse_cell(interior, quoted, DSD_SCAN_MODE_DMR, &parsed) == 0);
    assert(parsed.values.receive.names.count == 1
           && strcmp(parsed.values.receive.names.rules[0].name, "St Mary's") == 0);
    assert(parse_cell(quoted, quoted, DSD_SCAN_MODE_DMR, &parsed) == 0);
    assert(parsed.values.receive.names.count == 1
           && strcmp(parsed.values.receive.names.rules[0].name, "St Mary's") == 0);
    /* The issue's NFM row saved by a spreadsheet. */
    static const char tone[] =
        "\"--squelch noise+3 --nfm-bandwidth-hz 8000 --tone-allow 97.4/141.3 --tone-name \"\"141.3=20R Traveler\"\"\"";
    assert(parse_cell(tone, tone, DSD_SCAN_MODE_NFM, &parsed) == 0);
    assert(parsed.values.receive.names.count == 1
           && strcmp(parsed.values.receive.names.rules[0].name, "20R Traveler") == 0);
    /* A wrapped cell with no quote inside: the channel map took it as one token before and refused it. */
    assert(parse_cell("\"--dcc-allow 4/7\"", "\"--dcc-allow 4/7\"", DSD_SCAN_MODE_DMR, &parsed) == 0);
    assert(parsed.values.receive.filters[1].enabled);

    /* Cells the loaders accepted before keep their reading: the channel map's one grouped token (a name with a
     * space), and a trunk-scan target's path with two literal quotes inside single quotes. */
    static const char grouped[] = "\"--dcc-name=7=Hospital West\"";
    assert(!dsd_scan_options_cell_csv_quoted(grouped, grouped + 1, strlen(grouped) - 2));
    assert(parse_cell(grouped, grouped, DSD_SCAN_MODE_DMR, &parsed) == 0);
    assert(strcmp(parsed.values.receive.names.rules[0].name, "Hospital West") == 0);
    static const char literal[] = "-K 'a\"\"b.csv'";
    assert(!dsd_scan_options_cell_csv_quoted(literal, literal, strlen(literal)));
    assert(parse_cell(literal, "\"-K 'a\"\"b.csv'\"", DSD_SCAN_MODE_DMR, &parsed) == 0);
    assert(strcmp(parsed.hex_file, "a\"\"b.csv") == 0);
    /* Malformed cells are still refused: an undoubled quote inside, and an unbalanced decoded quote. */
    assert(parse_cell("\"--dcc-name \"7=A\" x\"", "\"--dcc-name \"7=A\" x\"", DSD_SCAN_MODE_DMR, &parsed) != 0);
    assert(parse_cell("\"--dcc-name \"\"7=A\"", "\"--dcc-name \"\"7=A\"", DSD_SCAN_MODE_DMR, &parsed) != 0);
    assert(!dsd_scan_options_cell_csv_quoted("x", NULL, 0));

    /* File operands of a quoted cell are reported at their place in the cell's content, doubled quotes included. */
    static const char keys[] = "\"--dcc-name \"\"7=A B\"\" -K \"\"old file.csv\"\" --dmr-tg-key-csv=map.csv\"";
    const char* content = keys + 1;
    const size_t length = strlen(keys) - 2;
    file_spans spans = {0};
    int quoted_cell = 0;
    assert(dsd_scan_options_visit_cell_files(keys, content, length, &spans, collect_file_span, &quoted_cell) == 0);
    assert(quoted_cell == 1 && spans.count == 2);
    assert(strcmp(spans.paths[0], "old file.csv") == 0 && !spans.whole[0]);
    assert(spans.lengths[0] == strlen("\"\"old file.csv\"\"")
           && memcmp(content + spans.offsets[0], "\"\"old file.csv\"\"", spans.lengths[0]) == 0);
    assert(strcmp(spans.paths[1], "map.csv") == 0 && spans.whole[1]);
    assert(memcmp(content + spans.offsets[1], "--dmr-tg-key-csv=map.csv", spans.lengths[1]) == 0
           && spans.offsets[1] + spans.lengths[1] == length);
    /* Any other cell is visited as before. */
    spans.count = 0;
    assert(dsd_scan_options_visit_cell_files("-K a.csv", NULL, 0, &spans, collect_file_span, &quoted_cell) == 0);
    assert(quoted_cell == 0 && spans.count == 1 && spans.offsets[0] == 3);
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* A row's service names judged over the configured policies (issue #637): each filter kind and the naming table the
 * row sets replace the configured ones, and an nfm row's tone policy the configured tone filter. */
static void
check_unreachable_row_names(void) {
    dsd_scan_options row;
    char error[192];
    char out[128];
    dsd_scan_option_baseline configured;
    DSD_MEMSET(&configured, 0, sizeof(configured));
    DSD_MEMSET(&row, 0, sizeof(row));
    unsigned int seen = 0;
    assert(dsd_receive_option_apply(&configured.receive, &seen, "--dcc-block", "7", error, sizeof(error)) == 0);
    /* An inherited filter strands the row's own name; alone, the row strands nothing. */
    assert(dsd_scan_options_parse("--dcc-name 7=Hospital", DSD_SCAN_MODE_DMR, 1, &row, error, sizeof(error)) == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, &configured, out, sizeof(out)) == 1);
    assert(strcmp(out, "DCC 7") == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, NULL, out, sizeof(out)) == 0);
    /* The row's explicit disable lets the code, and its name, through. */
    assert(dsd_scan_options_parse("--no-dcc-filter --dcc-name 7=Hospital", DSD_SCAN_MODE_DMR, 1, &row, error,
                                  sizeof(error))
           == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, &configured, out, sizeof(out)) == 0);
    /* The row's own filter strands an inherited name. */
    DSD_MEMSET(&configured, 0, sizeof(configured));
    assert(dsd_receive_names_parse("--dcc-name 7=Hospital", &configured.receive.names, error, sizeof(error)) == 0);
    assert(dsd_scan_options_parse("--dcc-allow 2/14", DSD_SCAN_MODE_DMR, 1, &row, error, sizeof(error)) == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, &configured, out, sizeof(out)) == 1);
    assert(dsd_scan_option_unreachable_names(NULL, &configured, out, sizeof(out)) == 0);
    /* The row's naming table replaces the inherited one whole. */
    assert(dsd_scan_options_parse("--dcc-allow 2/14 --dcc-name 14=Municipality", DSD_SCAN_MODE_DMR, 1, &row, error,
                                  sizeof(error))
           == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, &configured, out, sizeof(out)) == 0);

    /* Tone names: the configured tone filter, or the nfm row's own policy in its place. */
    DSD_MEMSET(&configured, 0, sizeof(configured));
    configured.tone_filter = DSD_TONE_FILTER_ALLOW;
    assert(dsd_tone_set_parse("97.4", &configured.tone_set, NULL, 0) == 0);
    assert(dsd_scan_options_parse("--tone-name '141.3=20R Traveler'", DSD_SCAN_MODE_NFM, 1, &row, error, sizeof(error))
           == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, &configured, out, sizeof(out)) == 1);
    assert(strcmp(out, "CTCSS 141.3") == 0);
    assert(dsd_scan_options_parse("--tone-allow 97.4/141.3 --tone-name '141.3=20R Traveler'", DSD_SCAN_MODE_NFM, 1,
                                  &row, error, sizeof(error))
           == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, &configured, out, sizeof(out)) == 0);
    assert(dsd_scan_options_parse("--no-tone-filter --tone-name '141.3=20R Traveler'", DSD_SCAN_MODE_NFM, 1, &row,
                                  error, sizeof(error))
           == 0);
    assert(dsd_scan_option_unreachable_names(&row.values, &configured, out, sizeof(out)) == 0);

    /* The preview summary says so; without stranded names it reads as before. */
    assert(
        dsd_scan_options_parse("--dcc-block 7 --dcc-name 7=Hospital", DSD_SCAN_MODE_DMR, 1, &row, error, sizeof(error))
        == 0);
    char summary[256];
    char plain[256];
    dsd_scan_option_receive_summary_against(&row.values, NULL, summary, sizeof(summary));
    dsd_scan_option_receive_summary(&row.values, plain, sizeof(plain));
    assert(strncmp(summary, plain, strlen(plain)) == 0
           && strcmp(summary + strlen(plain), "; never shown (filtered out): DCC 7") == 0);
    assert(dsd_scan_options_parse("--dcc-name 7=Hospital", DSD_SCAN_MODE_DMR, 1, &row, error, sizeof(error)) == 0);
    dsd_scan_option_receive_summary_against(&row.values, NULL, summary, sizeof(summary));
    dsd_scan_option_receive_summary(&row.values, plain, sizeof(plain));
    assert(strcmp(summary, plain) == 0);

    /* A long summary and a long list: the note keeps whole identifiers and its overflow mark; the summary gives way. */
    char options[1024] =
        "--slot-allow 1 --slot-block 2 --nac-allow 100/101/102/103/104/105/106/107/108/109/10A/10B/10C/"
        "10D/10E/10F --nac-block 100/101/102/103/104/105/106/107/108/109/10A/10B/10C/10D/10E/10F";
    for (unsigned int nac = 0x100; nac <= 0x10F; nac++) {
        const size_t used = strlen(options);
        DSD_SNPRINTF(options + used, sizeof(options) - used, " --nac-name %03X=N%03X", nac, nac);
    }
    assert(dsd_scan_options_parse(options, DSD_SCAN_MODE_P25, 1, &row, error, sizeof(error)) == 0);
    dsd_scan_option_receive_summary_against(&row.values, NULL, summary, sizeof(summary));
    const char* note = strstr(summary, "; never shown (filtered out): NAC 100, ");
    assert(note && strlen(summary) < sizeof(summary));
    const size_t tail = strlen(summary);
    assert(tail > 5 && strcmp(summary + tail - 5, ", ...") == 0);
    for (const char* entry = note + strlen("; never shown (filtered out): "); strncmp(entry, "...", 3) != 0;) {
        assert(strncmp(entry, "NAC 10", 6) == 0 && strncmp(entry + 7, ", ", 2) == 0);
        entry += 9;
    }
    DSD_SECURE_ZERO(&row, sizeof(row));
}

/* --- Issue #521: per-row and per-target squelch (--squelch-db) --- */

static int
count_file_spans(void* context, const char* option, const char* path, size_t offset, size_t length, int whole) {
    (void)option;
    (void)path;
    (void)offset;
    (void)length;
    (void)whole;
    ++*(size_t*)context;
    return 0;
}

/* --squelch on a row or target (issue #518 follow-up): the squelch grammar, stored as --squelch-db stores it for a level
 * (whole dB, off as 0) and as a mode and margin for auto, which only an analog row takes, and noise, which only an nfm
 * row takes. It shares --squelch-db's option: a row names one squelch. The diagnostic names the option and never
 * echoes the value. */
static void
check_squelch_setting_option(void) {
    dsd_scan_options parsed;
    char error[128];
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    assert(dsd_scan_options_parse("--squelch auto+6", DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH && parsed.values.squelch_mode == DSD_SQUELCH_MODE_AUTO);
    assert(parsed.values.squelch_margin_db == 6 && parsed.values.squelch_db == 0);
    assert(dsd_scan_options_parse("--squelch=auto", DSD_SCAN_MODE_AM, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.squelch_mode == DSD_SQUELCH_MODE_AUTO
           && parsed.values.squelch_margin_db == DSD_SQUELCH_MARGIN_DEFAULT_DB);
    assert(dsd_scan_options_parse("--squelch -60", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.squelch_mode == DSD_SQUELCH_MODE_LEVEL && parsed.values.squelch_db == -60);
    assert(dsd_scan_options_parse("--squelch off", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH && parsed.values.squelch_mode == DSD_SQUELCH_MODE_LEVEL
           && parsed.values.squelch_db == 0);
    assert(dsd_scan_options_parse("--squelch -60 --strict-crc", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error))
           == 0);

    assert(dsd_scan_options_parse("--squelch noise+14", DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH && parsed.values.squelch_mode == DSD_SQUELCH_MODE_NOISE);
    assert(parsed.values.squelch_margin_db == 14 && parsed.values.squelch_db == 0);
    assert(dsd_scan_options_parse("--squelch=noise", DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.squelch_mode == DSD_SQUELCH_MODE_NOISE
           && parsed.values.squelch_margin_db == DSD_SQUELCH_MARGIN_DEFAULT_DB);

    const char* rejected_everywhere[] = {"--squelch auto+2", "--squelch auto+31", "--squelch noise+2",
                                         "--squelch -60.5",  "--squelch 5",       "--squelch SENSITIVE",
                                         "--squelch -101",   "--squelch noisy"};
    for (size_t i = 0; i < sizeof(rejected_everywhere) / sizeof(rejected_everywhere[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse(rejected_everywhere[i], DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error)) < 0);
        assert(parsed.values.present == 0 && strncmp(error, "--squelch: ", 11) == 0);
        assert(strstr(error, "SENSITIVE") == NULL);
    }
    /* auto on a digital row: its channel never shows its noise. */
    assert(dsd_scan_options_parse("--squelch auto", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);
    assert(strstr(error, "auto[+N] (nfm and am rows)") != NULL);
    /* noise on an am or digital row: no discriminator to quiet. */
    assert(dsd_scan_options_parse("--squelch noise", DSD_SCAN_MODE_AM, 1, &parsed, error, sizeof(error)) < 0);
    assert(strstr(error, "noise[+N] (nfm rows)") != NULL);
    assert(dsd_scan_options_parse("--squelch noise+8", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error)) < 0);
    assert(
        dsd_scan_options_parse("--squelch noise --squelch-db -60", DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error))
        < 0);
    assert(strstr(error, "duplicate option") != NULL);
    /* One squelch per row, whichever spellings. */
    assert(
        dsd_scan_options_parse("--squelch auto --squelch-db -60", DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error))
        < 0);
    assert(strstr(error, "duplicate option") != NULL);
    assert(
        dsd_scan_options_parse("--squelch-db -60 --squelch auto+6", DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error))
        < 0);
}

/* The row squelch takes whole dB from -100 to 0 (the rtl_sql key's spelling), 0 switches it off, and
 * omitting it inherits. Both the separate-token and the = spellings work, and a negative number
 * is the one exception to "a following token that starts with - is a switch". */
static void
check_squelch_option(void) {
    dsd_scan_options parsed = {0};
    char error[192] = {0};

    const struct {
        const char* text;
        int db;
    } accepted[] = {{"--squelch-db -60", -60}, {"--squelch-db=-60", -60},   {"--squelch-db 0", 0},
                    {"--squelch-db=0", 0},     {"--squelch-db -100", -100}, {"--squelch-db '-45'", -45}};

    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse(accepted[i].text, DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
        assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH);
        assert(parsed.values.squelch_db == accepted[i].db);
    }

    /* Allowed on every declared class and on blank rows, conventional or trunked alike. */
    for (unsigned int mode = DSD_SCAN_MODE_INHERIT; mode <= DSD_SCAN_MODE_M17; mode++) {
        for (int conventional = 0; conventional <= 1; conventional++) {
            DSD_MEMSET(&parsed, 0, sizeof(parsed));
            assert(dsd_scan_options_parse("--squelch-db -70", mode, conventional, &parsed, error, sizeof(error)) == 0);
            assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH && parsed.values.squelch_db == -70);
        }
    }

    /* The exception belongs to the squelch value alone: -4 after --squelch-db is its value,
     * never the DMR force switch, while a bare -4 is still the force switch. */
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    assert(dsd_scan_options_parse("--squelch-db -4", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH && parsed.values.squelch_db == -4);
    assert(parsed.values.force == 0);
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    assert(dsd_scan_options_parse("-4 --squelch-db -4", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == (DSD_SCAN_OPT_FORCE | DSD_SCAN_OPT_SQUELCH));
    assert(parsed.values.force == 1 && parsed.values.squelch_db == -4);
    /* -0 and -1 are switches as well and read the same way after --squelch-db: values, so the
     * key that would follow -1 is a stray positional argument. */
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    assert(dsd_scan_options_parse("--squelch-db -0", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH && parsed.values.squelch_db == 0);
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    assert(dsd_scan_options_parse("--squelch-db -1", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH && parsed.values.squelch_db == -1);
    assert(dsd_scan_options_parse("--squelch-db -1 0123456789", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error))
           < 0);
    /* -4 is not a P25 switch, but as a squelch value it is fine on a P25 row. */
    assert(dsd_scan_options_parse("--squelch-db -4", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error)) == 0);
    assert(dsd_scan_options_parse("-4", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error)) < 0);
    /* Other value-taking switches still refuse a negative number as their value. */
    assert(dsd_scan_options_parse("--scan-max-visit-ms -60", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);
    assert(dsd_scan_options_parse("-G -60", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);

    const char* rejected[] = {"--squelch-db +5",
                              "--squelch-db 5",
                              "--squelch-db=5",
                              "--squelch-db -101",
                              "--squelch-db=-101",
                              "--squelch-db -5.5",
                              "--squelch-db=-5.5",
                              "--squelch-db -60dB",
                              "--squelch-db 1e1",
                              "--squelch-db -",
                              "--squelch-db=",
                              "--squelch-db",
                              "--squelch-db --strict-crc",
                              "--squelch-db -60 --squelch-db -50",
                              "--squelch-db=-60 --squelch-db 0",
                              "--squelch-db SENSITIVE"};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        DSD_MEMSET(error, 0, sizeof(error));
        assert(dsd_scan_options_parse(rejected[i], DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);
        assert(parsed.values.present == 0);
        /* The diagnostic names the switch and never echoes the value. */
        assert(strncmp(error, "--squelch-db: ", 14) == 0);
        assert(strstr(error, "SENSITIVE") == NULL);
    }
    assert(
        dsd_scan_options_parse("--squelch-db -60 --squelch-db -50", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error))
        < 0);
    assert(strcmp(error, "--squelch-db: duplicate option") == 0);
    /* A bad number gets the same diagnostic however it is spelled; only a token that is not a
     * number at all reads as a missing value. */
    const char* out_of_contract[] = {"--squelch-db 5",     "--squelch-db=5",     "--squelch-db -5.5",
                                     "--squelch-db=-5.5",  "--squelch-db -101",  "--squelch-db -1e1",
                                     "--squelch-db -60.0", "--squelch-db=-60.0", "--squelch-db +5"};
    for (size_t i = 0; i < sizeof(out_of_contract) / sizeof(out_of_contract[0]); i++) {
        assert(dsd_scan_options_parse(out_of_contract[i], DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);
        assert(strcmp(error, "--squelch-db: expects whole dB from -100 to 0 (0 = off)") == 0);
    }
    const char* missing[] = {"--squelch-db", "--squelch-db -", "--squelch-db --strict-crc", "--squelch-db -G",
                             "--squelch-db -60dB"};
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
        assert(dsd_scan_options_parse(missing[i], DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);
        assert(strcmp(error, "--squelch-db: requires a valid argument") == 0);
    }

    /* The file visitor walks the same grammar, or the Qt/Android inspector would reject a row
     * the importer accepts (or accept one it rejects). */
    size_t visited = 0;
    assert(dsd_scan_options_visit_files("--squelch-db -60", &visited, count_file_spans) == 0 && visited == 0);
    assert(dsd_scan_options_visit_files("--squelch-db=-60", &visited, count_file_spans) == 0 && visited == 0);
    assert(dsd_scan_options_visit_files("--squelch-db -4 -G groups.csv", &visited, count_file_spans) == 0);
    assert(visited == 1);
    /* A malformed number is still the value (the parser's setter rejects it), not a switch. */
    assert(dsd_scan_options_visit_files("--squelch-db -5.5 -G groups.csv", &visited, count_file_spans) == 0);
    assert(visited == 2);
    visited = 0;
    assert(dsd_scan_options_visit_files("--squelch-db", &visited, count_file_spans) == -1);
    assert(dsd_scan_options_visit_files("--squelch-db --strict-crc", &visited, count_file_spans) == -1);
    assert(dsd_scan_options_visit_files("--squelch-db -60x", &visited, count_file_spans) == -1);
    assert(dsd_scan_options_visit_files("-G -60", &visited, count_file_spans) == -1);
    assert(visited == 0);
    file_spans spans = {0};
    const char* text = "--squelch-db -60 -K keys.csv";
    assert(dsd_scan_options_visit_files(text, &spans, collect_file_span) == 0 && spans.count == 1);
    assert(spans.offsets[0] == strlen("--squelch-db -60 -K ") && strcmp(spans.paths[0], "keys.csv") == 0);
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* --- Issue #526: NFM rows --- */

/* An NFM row takes the options that mean something for an analog channel: the visit cap, the
 * squelch and its own channel width, on -Y rows and on nfm-conventional targets alike. */
static void
check_nfm_row_accepts_analog_options(void) {
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    for (int conventional = 0; conventional <= 1; conventional++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse("--scan-max-visit-ms 5000 --squelch-db -60 --nfm-bandwidth-hz 12500",
                                      DSD_SCAN_MODE_NFM, conventional, &parsed, error, sizeof(error))
               == 0);
        assert(parsed.values.present == (DSD_SCAN_OPT_MAX_VISIT | DSD_SCAN_OPT_SQUELCH | DSD_SCAN_OPT_BANDWIDTH));
        assert(parsed.values.max_visit_ms == 5000 && parsed.values.squelch_db == -60);
        assert(parsed.values.channel_bw_hz == 12500);
    }

    const struct {
        const char* text;
        int hz;
    } widths[] = {{"--nfm-bandwidth-hz=11250", 11250},
                  {"--nfm-bandwidth-hz 8000", 8000},
                  {"--nfm-bandwidth-hz 25000", 25000},
                  {"--nfm-bandwidth-hz '16000'", 16000}};

    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse(widths[i].text, DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) == 0);
        assert(parsed.values.present == DSD_SCAN_OPT_BANDWIDTH && parsed.values.channel_bw_hz == widths[i].hz);
    }
    /* The file visitor walks the width as a plain value, never a file operand. */
    size_t visited = 0;
    assert(dsd_scan_options_visit_files("--nfm-bandwidth-hz 12500 --squelch-db -60", &visited, count_file_spans) == 0);
    assert(visited == 0);
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* Keys, forcing, CRC policy, talkgroup groups, data/encrypted-call policy and the voice gate
 * describe digital frames an analog channel never carries: each is refused on an nfm and an am row
 * with the same diagnostic a wrong-mode digital switch gets, and the value is never echoed. */
static void
check_analog_rows_reject_digital_options(void) {
    static const char* const refused[] = {"-b 1",
                                          "-H 0123456789",
                                          "-1 0123456789",
                                          "-R 5",
                                          "-K keys.csv",
                                          "-k keys.csv",
                                          "--dmr-tg-key-csv map.csv",
                                          "--dmr-tg-key-clear",
                                          "--no-decryption-keys",
                                          "--key-profile-ref SENSITIVE",
                                          "-4",
                                          "-0",
                                          "--dmr-force-algid 21",
                                          "--no-force-key",
                                          "-F",
                                          "--strict-crc",
                                          "-^",
                                          "-G groups.csv",
                                          "-e",
                                          "--no-data-calls",
                                          "--enc-follow",
                                          "--enc-lockout",
                                          "--scan-voice-only",
                                          "--no-scan-voice-only",
                                          "--scan-voice-qualify-ms 1000",
                                          "--scan-voice-hold-ms 1000"};
    static const dsd_scan_mode analog[] = {DSD_SCAN_MODE_NFM, DSD_SCAN_MODE_AM};
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    for (size_t m = 0; m < sizeof(analog) / sizeof(analog[0]); m++) {
        for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            for (int conventional = 0; conventional <= 1; conventional++) {
                DSD_MEMSET(&parsed, 0, sizeof(parsed));
                DSD_MEMSET(error, 0, sizeof(error));
                assert(dsd_scan_options_parse(refused[i], analog[m], conventional, &parsed, error, sizeof(error)) < 0);
                assert(parsed.values.present == 0);
                char want[64];
                const size_t name_len = strcspn(refused[i], " =");
                DSD_SNPRINTF(want, sizeof(want), "%.*s: not supported for this mode/target", (int)name_len, refused[i]);
                if (strcmp(error, want) != 0) {
                    DSD_FPRINTF(stderr, "'%s' on %s: got '%s', want '%s'\n", refused[i], dsd_scan_mode_name(analog[m]),
                                error, want);
                    assert(0);
                }
                assert(strstr(error, "SENSITIVE") == NULL);
            }
        }
    }
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* The width is an analog setting: a digital or blank row that names it is told which mode it
 * needs, the value must be whole Hz inside the NFM range, and one row carries it once. */
static void
check_nfm_bandwidth_option_contract(void) {
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    for (unsigned int mode = DSD_SCAN_MODE_INHERIT; mode <= DSD_SCAN_MODE_LAST; mode++) {
        if (dsd_scan_mode_is_analog((dsd_scan_mode)mode)) {
            continue;
        }
        for (int conventional = 0; conventional <= 1; conventional++) {
            DSD_MEMSET(&parsed, 0, sizeof(parsed));
            assert(dsd_scan_options_parse("--nfm-bandwidth-hz 12500", mode, conventional, &parsed, error, sizeof(error))
                   < 0);
            assert(strcmp(error, "--nfm-bandwidth-hz: needs mode nfm") == 0);
            assert(parsed.values.present == 0);
        }
    }
    static const char* const invalid[] = {"--nfm-bandwidth-hz 7999",    "--nfm-bandwidth-hz 25001",
                                          "--nfm-bandwidth-hz=0",       "--nfm-bandwidth-hz 12.5k",
                                          "--nfm-bandwidth-hz 12500Hz", "--nfm-bandwidth-hz +12500",
                                          "--nfm-bandwidth-hz 0x30d4",  "--nfm-bandwidth-hz ' 12500'",
                                          "--nfm-bandwidth-hz 1e4",     "--nfm-bandwidth-hz SENSITIVE"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse(invalid[i], DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) < 0);
        if (strcmp(error, "--nfm-bandwidth-hz: expects whole Hz from 8000 to 25000") != 0) {
            DSD_FPRINTF(stderr, "'%s': got '%s'\n", invalid[i], error);
            assert(0);
        }
        assert(parsed.values.present == 0);
    }
    /* A negative number is a missing value, as for every switch but the squelch. */
    const char* missing[] = {"--nfm-bandwidth-hz", "--nfm-bandwidth-hz -12500", "--nfm-bandwidth-hz --squelch-db -60"};
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
        assert(dsd_scan_options_parse(missing[i], DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) < 0);
        assert(strcmp(error, "--nfm-bandwidth-hz: requires a valid argument") == 0);
    }
    assert(dsd_scan_options_parse("--nfm-bandwidth-hz 12500 --nfm-bandwidth-hz=12500", DSD_SCAN_MODE_NFM, 1, &parsed,
                                  error, sizeof(error))
           < 0);
    assert(strcmp(error, "--nfm-bandwidth-hz: duplicate option") == 0);
    /* The squelch stays legal on every class, the analog one included. */
    for (unsigned int mode = DSD_SCAN_MODE_INHERIT; mode <= DSD_SCAN_MODE_LAST; mode++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse("--squelch-db -70", mode, 0, &parsed, error, sizeof(error)) == 0);
        assert(parsed.values.present == DSD_SCAN_OPT_SQUELCH);
    }
    /* Anything past the last class is refused outright. */
    assert(dsd_scan_options_parse("", DSD_SCAN_MODE_LAST + 1U, 1, &parsed, error, sizeof(error)) < 0);
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* A row width in range may still be one the DSP rate cannot filter: the check names the width,
 * the rate, the widest width that rate fits and the fix, as the front end's own refusal does.
 * A row without a width, a digital row and an unknown rate have nothing to check. */
static void
check_nfm_bandwidth_rate(void) {
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 25000;
    char error[256] = {0};
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, &row, 48000, error, sizeof(error)) == 0);
    assert(error[0] == '\0');
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, &row, 24000, error, sizeof(error)) < 0);
    assert(strstr(error, "25 kHz") && strstr(error, "24 kHz") && strstr(error, "20.4 kHz"));
    row.channel_bw_hz = 20400;
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, &row, 24000, error, sizeof(error)) == 0);
    row.channel_bw_hz = 13200;
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, &row, 16000, error, sizeof(error)) == 0);
    row.channel_bw_hz = 13300;
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, &row, 16000, error, sizeof(error)) < 0);
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, &row, 0, error, sizeof(error)) == 0);
    row.present = 0;
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, &row, 16000, error, sizeof(error)) == 0);
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_NFM, NULL, 16000, error, sizeof(error)) == 0);
}

/* --- Issue #526: AM rows --- */

/* An am row takes the analog options: the visit cap, the squelch and its own channel width, spelled for its kind
 * (--am-bandwidth-hz, whole Hz in the AM range), on -Y rows and am-conventional targets alike. The width records the
 * demodulator it is for, so an am row's width lands on the AM width and an nfm row's on the NFM one. */
static void
check_am_row_accepts_analog_options(void) {
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    for (int conventional = 0; conventional <= 1; conventional++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse("--scan-max-visit-ms 5000 --squelch-db -60 --am-bandwidth-hz 8333",
                                      DSD_SCAN_MODE_AM, conventional, &parsed, error, sizeof(error))
               == 0);
        assert(parsed.values.present == (DSD_SCAN_OPT_MAX_VISIT | DSD_SCAN_OPT_SQUELCH | DSD_SCAN_OPT_BANDWIDTH));
        assert(parsed.values.max_visit_ms == 5000 && parsed.values.squelch_db == -60);
        assert(parsed.values.channel_bw_hz == 8333 && parsed.values.channel_bw_kind == DSD_ANALOG_DEMOD_AM);
    }

    const struct {
        const char* text;
        int hz;
    } widths[] = {
        {"--am-bandwidth-hz=5000", 5000}, {"--am-bandwidth-hz 20000", 20000}, {"--am-bandwidth-hz '6000'", 6000}};

    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse(widths[i].text, DSD_SCAN_MODE_AM, 1, &parsed, error, sizeof(error)) == 0);
        assert(parsed.values.present == DSD_SCAN_OPT_BANDWIDTH && parsed.values.channel_bw_hz == widths[i].hz);
        assert(parsed.values.channel_bw_kind == DSD_ANALOG_DEMOD_AM);
    }
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    parsed.values.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
    assert(dsd_scan_options_parse("--nfm-bandwidth-hz 12500", DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.channel_bw_hz == 12500 && parsed.values.channel_bw_kind == DSD_ANALOG_DEMOD_FM);
    size_t visited = 0;
    assert(dsd_scan_options_visit_files("--am-bandwidth-hz 8333 --squelch-db -60", &visited, count_file_spans) == 0);
    assert(visited == 0);
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* Each analog kind's width is refused on the other kind's row, as any switch outside a row's class is; a digital or
 * blank row that names the AM width is told which mode it needs; and the value must be whole Hz in the AM range,
 * given once. */
static void
check_am_bandwidth_option_contract(void) {
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    for (int conventional = 0; conventional <= 1; conventional++) {
        assert(dsd_scan_options_parse("--nfm-bandwidth-hz 12500", DSD_SCAN_MODE_AM, conventional, &parsed, error,
                                      sizeof(error))
               < 0);
        assert(strcmp(error, "--nfm-bandwidth-hz: not supported for this mode/target") == 0);
        assert(dsd_scan_options_parse("--am-bandwidth-hz 8000", DSD_SCAN_MODE_NFM, conventional, &parsed, error,
                                      sizeof(error))
               < 0);
        assert(strcmp(error, "--am-bandwidth-hz: not supported for this mode/target") == 0);
    }
    for (unsigned int mode = DSD_SCAN_MODE_INHERIT; mode <= DSD_SCAN_MODE_LAST; mode++) {
        if (dsd_scan_mode_is_analog((dsd_scan_mode)mode)) {
            continue;
        }
        for (int conventional = 0; conventional <= 1; conventional++) {
            DSD_MEMSET(&parsed, 0, sizeof(parsed));
            assert(dsd_scan_options_parse("--am-bandwidth-hz 8000", mode, conventional, &parsed, error, sizeof(error))
                   < 0);
            assert(strcmp(error, "--am-bandwidth-hz: needs mode am") == 0);
            assert(parsed.values.present == 0);
        }
    }
    static const char* const invalid[] = {"--am-bandwidth-hz 4999",   "--am-bandwidth-hz 20001",
                                          "--am-bandwidth-hz=0",      "--am-bandwidth-hz 8.33k",
                                          "--am-bandwidth-hz 8333Hz", "--am-bandwidth-hz +8333",
                                          "--am-bandwidth-hz 0x208d", "--am-bandwidth-hz ' 8333'",
                                          "--am-bandwidth-hz 1e4",    "--am-bandwidth-hz SENSITIVE"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse(invalid[i], DSD_SCAN_MODE_AM, 1, &parsed, error, sizeof(error)) < 0);
        if (strcmp(error, "--am-bandwidth-hz: expects whole Hz from 5000 to 20000") != 0) {
            DSD_FPRINTF(stderr, "'%s': got '%s'\n", invalid[i], error);
            assert(0);
        }
        assert(parsed.values.present == 0);
    }
    const char* missing[] = {"--am-bandwidth-hz", "--am-bandwidth-hz -8000", "--am-bandwidth-hz --squelch-db -60"};
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
        assert(dsd_scan_options_parse(missing[i], DSD_SCAN_MODE_AM, 1, &parsed, error, sizeof(error)) < 0);
        assert(strcmp(error, "--am-bandwidth-hz: requires a valid argument") == 0);
    }
    assert(dsd_scan_options_parse("--am-bandwidth-hz 8000 --am-bandwidth-hz=8000", DSD_SCAN_MODE_AM, 1, &parsed, error,
                                  sizeof(error))
           < 0);
    assert(strcmp(error, "--am-bandwidth-hz: duplicate option") == 0);
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* An am row's width is held to the DSP rate by the AM demodulator's rule, as an nfm row's is by the NFM one. */
static void
check_am_bandwidth_rate(void) {
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_BANDWIDTH;
    row.channel_bw_hz = 20000;
    row.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
    char error[256] = {0};
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_AM, &row, 24000, error, sizeof(error)) == 0);
    assert(error[0] == '\0');
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_AM, &row, 16000, error, sizeof(error)) < 0);
    assert(strstr(error, "AM") && strstr(error, "20 kHz") && strstr(error, "16 kHz"));
    row.channel_bw_hz = 12000;
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_AM, &row, 16000, error, sizeof(error)) == 0);
    /* A digital class has no width to hold. */
    row.channel_bw_hz = 20000;
    assert(dsd_scan_option_width_check(DSD_SCAN_MODE_DMR, &row, 16000, error, sizeof(error)) == 0);
}

/* --- Issue #527: the tone policy on nfm rows --- */

/* --tone-allow and --tone-block take a '/'-separated list and --no-tone-filter none; all three are one option, so a
 * row names one of them once. They are legal on nfm rows and targets only: a digital or blank row is told which mode
 * they need. A refused list is named by entry number, never echoed. */
static void
check_tone_options(void) {
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    char list[DSD_TONE_LIST_TEXT_MAX + 1];
    for (int conventional = 0; conventional <= 1; conventional++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse("--squelch-db -60 --tone-allow 100/D023", DSD_SCAN_MODE_NFM, conventional,
                                      &parsed, error, sizeof(error))
               == 0);
        assert(parsed.values.present == (DSD_SCAN_OPT_SQUELCH | DSD_SCAN_OPT_TONE));
        assert(parsed.values.tone_filter == DSD_TONE_FILTER_ALLOW);
        assert(dsd_tone_set_format(&parsed.values.tone_set, list, sizeof(list)) > 0);
        assert(strcmp(list, "100.0/D023N") == 0);
    }
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    assert(dsd_scan_options_parse("--tone-block=67.0/D047I", DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_TONE && parsed.values.tone_filter == DSD_TONE_FILTER_BLOCK);
    assert(dsd_tone_set_contains_dcs(&parsed.values.tone_set, 0023, 0));
    /* The explicit row disable: a row that turns a configured policy off. */
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    assert(dsd_scan_options_parse("--no-tone-filter", DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_TONE && parsed.values.tone_filter == DSD_TONE_FILTER_OFF);
    assert(dsd_tone_set_count(&parsed.values.tone_set) == 0);
    /* The list is a plain value, never a file operand. */
    size_t visited = 0;
    assert(dsd_scan_options_visit_files("--tone-allow 100.0/D023N --squelch-db -60", &visited, count_file_spans) == 0);
    assert(visited == 0);

    /* One option, whichever spelling comes second. */
    static const char* const twice[] = {"--tone-allow 100.0 --tone-block 67.0", "--tone-allow 100.0 --no-tone-filter",
                                        "--no-tone-filter --tone-allow=100.0", "--tone-block 67.0 --tone-block 71.9"};
    for (size_t i = 0; i < sizeof(twice) / sizeof(twice[0]); i++) {
        assert(dsd_scan_options_parse(twice[i], DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) < 0);
        assert(strstr(error, ": duplicate option") != NULL);
    }

    /* An am row or am-conventional target runs the AM monitor, which hears no CTCSS or DCS: another analog class's
       option, refused there as the NFM width is. */
    static const struct {
        const char* text;
        const char* why;
    } on_am[] = {
        {"--tone-allow 100.0", "--tone-allow: not supported for this mode/target"},
        {"--tone-block=D023N", "--tone-block: not supported for this mode/target"},
        {"--no-tone-filter", "--no-tone-filter: not supported for this mode/target"},
        {"--am-bandwidth-hz 8333 --tone-allow 100.0", "--tone-allow: not supported for this mode/target"},
    };

    for (int conventional = 0; conventional <= 1; conventional++) {
        for (size_t i = 0; i < sizeof(on_am) / sizeof(on_am[0]); i++) {
            DSD_MEMSET(&parsed, 0, sizeof(parsed));
            assert(dsd_scan_options_parse(on_am[i].text, DSD_SCAN_MODE_AM, conventional, &parsed, error, sizeof(error))
                   < 0);
            if (strcmp(error, on_am[i].why) != 0) {
                DSD_FPRINTF(stderr, "am row '%s': got '%s', want '%s'\n", on_am[i].text, error, on_am[i].why);
                assert(0);
            }
            assert(parsed.values.present == 0);
        }
    }

    /* Digital and blank rows: the mode they need. */
    for (unsigned int mode = DSD_SCAN_MODE_INHERIT; mode <= DSD_SCAN_MODE_LAST; mode++) {
        if (dsd_scan_mode_is_analog((dsd_scan_mode)mode)) {
            continue;
        }
        static const char* const names[] = {"--tone-allow 100.0", "--tone-block D023N", "--no-tone-filter"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            DSD_MEMSET(&parsed, 0, sizeof(parsed));
            assert(dsd_scan_options_parse(names[i], mode, 1, &parsed, error, sizeof(error)) < 0);
            char want[64];
            DSD_SNPRINTF(want, sizeof(want), "%.*s: needs mode nfm", (int)strcspn(names[i], " "), names[i]);
            assert(strcmp(error, want) == 0);
            assert(parsed.values.present == 0);
        }
    }

    /* Refused lists: index-based, the text never echoed. */
    static const struct {
        const char* text;
        const char* why;
    } refused[] = {
        {"--tone-allow 100.0,67.0", "--tone-allow: use / between entries, not commas"},
        {"--tone-allow 67.0/SECRET", "--tone-allow: entry 2 is not a standard CTCSS tone or DCS code"},
        {"--tone-block D023N/D047I", "--tone-block: entry 2 is the same DCS signal as entry 1 (D023N)"},
        {"--tone-block=", "--tone-block: the list is empty"},
        {"--tone-allow", "--tone-allow: requires a valid argument"},
        {"--tone-allow --squelch-db -60", "--tone-allow: requires a valid argument"},
        {"--no-tone-filter=100.0", "--no-tone-filter: takes no argument"},
    };

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        DSD_MEMSET(&parsed, 0, sizeof(parsed));
        assert(dsd_scan_options_parse(refused[i].text, DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) < 0);
        if (strcmp(error, refused[i].why) != 0) {
            DSD_FPRINTF(stderr, "'%s': got '%s', want '%s'\n", refused[i].text, error, refused[i].why);
            assert(0);
        }
        assert(strstr(error, "SECRET") == NULL && parsed.values.present == 0);
    }
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* A tone list a CSV comma cut short (issue #527): the splitter hands the parser the cell up to the comma and leaves the
 * rest in the field after it. Refused when the cell ends with a --tone-allow/--tone-block list and that field goes on
 * with it: any text past the header, a standard tone or code in one of the file's columns, alone or followed by the
 * cell's next option. A cell whose list is not last, a column's own value (an RTL gain, a name), an empty field and a
 * row with no field after are left alone, and the diagnostic names the switch only. */
static void
check_tone_list_split(void) {
    static const struct {
        const char* options;
        const char* next;
        int past_header;
        const char* why;
    } cases[] = {
        {"--tone-allow 100.0", "67.0", 1, "--tone-allow: use / between entries, not commas"},
        {"--squelch-db -60 --tone-block=100.0", "SECRET", 1, "--tone-block: use / between entries, not commas"},
        {"--tone-allow 100.0", " 67.0/D023N", 0, "--tone-allow: use / between entries, not commas"},
        {"--tone-block \"100.0\"", "d047i", 0, "--tone-block: use / between entries, not commas"},
        {"--tone-allow 100.0", "", 1, NULL},
        {"--tone-allow 100.0", "  \r\n", 1, NULL},
        {"--tone-allow 100.0", NULL, 1, NULL},
        {"--tone-allow 100.0", "30", 0, NULL},
        {"--tone-allow 100.0", "Fire", 0, NULL},
        {"--tone-allow 100.0", "161.0", 0, NULL},
        /* 150.0 Hz is a tone (issue #518 follow-up): a column that reads as one is the list's rest. */
        {"--tone-allow 100.0", "150.0", 0, "--tone-allow: use / between entries, not commas"},
        /* A name that starts with a tone or code but has a space in it is the column's own. */
        {"--tone-allow 100.0", "100 Main St", 0, NULL},
        {"--tone-allow 100.0", "67 Fire", 0, NULL},
        {"--tone-block D023N", "D023 Repeater", 0, NULL},
        {"--tone-allow 100.0", "67.0/D023N Fire", 0, NULL},
        {"--tone-allow 100.0", "67 --fire", 0, NULL},
        {"--tone-allow 100.0", "100 -- Main St", 0, NULL},
        {"--tone-allow 100.0", "Fire --squelch-db -60", 0, NULL},
        /* A run of entries that the cell's next option follows is the list's rest and the cell's rest: the comma cut
           both off, whatever the column. */
        {"--tone-block 100.0", "67.0 --squelch-db -60", 0, "--tone-block: use / between entries, not commas"},
        {"--tone-allow 100.0", "67.0/D023N\t--nfm-bandwidth-hz=12500", 0,
         "--tone-allow: use / between entries, not commas"},
        {"--tone-allow 100.0", "d047i  --no-tone-filter", 0, "--tone-allow: use / between entries, not commas"},
        /* One run of entries with no space in it can only be the list's rest, a nonstandard later entry included. */
        {"--tone-allow 100.0", "67.0/161.0", 0, "--tone-allow: use / between entries, not commas"},
        {"--tone-allow 100.0", "67.0 \r\n", 0, "--tone-allow: use / between entries, not commas"},
        {"--tone-allow 100.0 --squelch-db -60", "67.0", 1, NULL},
        {"--no-tone-filter", "67.0", 1, NULL},
        {"--squelch-db -60", "67.0", 1, NULL},
        {"--tone-allow", "67.0", 1, NULL},
        {"--tone-allow 'unterminated", "67.0", 1, NULL},
        {"", "67.0", 1, NULL},
        /* Receive filter lists (issue #637) are judged by the values their filter takes, in any column. */
        {"--nac-allow 293", "370/371", 0, "--nac-allow: use / between entries, not commas"},
        {"--nac-allow 293", "370", 1, "--nac-allow: use / between entries, not commas"},
        /* A lone number within the header is the next column's own (a gain of 7 after a DCC list). */
        {"--dcc-allow 4", "7", 0, NULL},
        {"--dcc-allow 4", "7 --slot-block 2", 0, "--dcc-allow: use / between entries, not commas"},
        {"--dcc-block=4", "7/9 --slot-block 2", 0, "--dcc-block: use / between entries, not commas"},
        {"--ran-allow 1", "64", 0, NULL},
        {"--nac-allow 293", "bandplan.csv", 0, NULL},
        {"--dcc-allow 4", "Fire", 1, "--dcc-allow: use / between entries, not commas"},
        /* A service name's rest cannot be told from a column's own text: only past the header. */
        {"--dcc-name 7=Hospital", "West", 0, NULL},
        {"--dcc-name 7=Hospital", "West", 1, "--dcc-name: commas are not supported in service names"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char error[96] = "";
        const int refused = dsd_scan_options_tone_list_split(cases[i].options, cases[i].next, cases[i].past_header,
                                                             error, sizeof(error));
        if (refused != (cases[i].why != NULL) || (cases[i].why && strcmp(error, cases[i].why) != 0)) {
            DSD_FPRINTF(stderr, "split case %zu: refused=%d error='%s'\n", i, refused, error);
            assert(0);
        }
        assert(strstr(error, "SECRET") == NULL);
    }
    assert(dsd_scan_options_tone_list_split(NULL, "67.0", 1, NULL, 0) == 0);
    assert(dsd_scan_options_tone_list_split("--tone-allow 100.0", "67.0", 1, NULL, 0) == 1);
}

/* What the import previews show of a row's own policy: its mode and its list as displayed, or -1 for a row that runs
 * the configured one. --no-tone-filter shows no list. */
static void
check_tone_summary(void) {
    char list[96];
    assert(dsd_scan_option_tone_summary(NULL, list, sizeof(list)) == -1 && list[0] == '\0');
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    assert(dsd_scan_options_parse("--squelch-db -60", DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) == 0);
    DSD_SNPRINTF(list, sizeof(list), "%s", "stale");
    assert(dsd_scan_option_tone_summary(&parsed.values, list, sizeof(list)) == -1 && list[0] == '\0');
    assert(dsd_scan_options_parse("--tone-allow 100/D023", DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) == 0);
    assert(dsd_scan_option_tone_summary(&parsed.values, list, sizeof(list)) == DSD_TONE_FILTER_ALLOW);
    assert(strcmp(list, "100.0 Hz/D023N") == 0);
    assert(dsd_scan_options_parse("--tone-block=67.0", DSD_SCAN_MODE_NFM, 0, &parsed, error, sizeof(error)) == 0);
    assert(dsd_scan_option_tone_summary(&parsed.values, list, sizeof(list)) == DSD_TONE_FILTER_BLOCK);
    assert(strcmp(list, "67.0 Hz") == 0);
    assert(dsd_scan_options_parse("--no-tone-filter", DSD_SCAN_MODE_NFM, 1, &parsed, error, sizeof(error)) == 0);
    DSD_SNPRINTF(list, sizeof(list), "%s", "stale");
    assert(dsd_scan_option_tone_summary(&parsed.values, list, sizeof(list)) == DSD_TONE_FILTER_OFF && list[0] == '\0');
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
}

/* Issue #708: a target CSV's options cell cannot carry call recording into a scan session: the app owns the folder,
   the journal and deletion, and records per call by Record calls alone. Every class of row refuses them. */
static void
check_recording_switches_refused(void) {
    static const char* const switches[] = {
        "-7 /elsewhere", "-7/elsewhere", "--wav-journal /x/j.jsonl",       "--wav-journal=/x/j.jsonl",
        "-w out.wav",    "-P",           "--rdio-api-delete-after-upload", "--record-unknown-only",
    };
    for (size_t i = 0; i < sizeof switches / sizeof switches[0]; i++) {
        for (unsigned int mode = DSD_SCAN_MODE_INHERIT; mode <= DSD_SCAN_MODE_LAST; mode++) {
            for (int conventional = 0; conventional <= 1; conventional++) {
                dsd_scan_options parsed = {0};
                char error[192] = {0};
                assert(dsd_scan_options_parse(switches[i], mode, conventional, &parsed, error, sizeof(error)) != 0);
                DSD_SECURE_ZERO(&parsed, sizeof(parsed));
            }
        }
    }
}

int
main(void) {
    check_recording_switches_refused();
    check_tone_options();
    check_tone_summary();
    check_tone_list_split();
    check_file_spans();
    check_csv_quoted_cells();
    check_unreachable_row_names();
    check_squelch_option();
    check_squelch_setting_option();
    check_nfm_row_accepts_analog_options();
    check_analog_rows_reject_digital_options();
    check_nfm_bandwidth_option_contract();
    check_nfm_bandwidth_rate();
    check_am_row_accepts_analog_options();
    check_am_bandwidth_option_contract();
    check_am_bandwidth_rate();
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    assert(dsd_scan_options_parse("--dmr-tg-key-csv mapping.csv", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error))
           == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_DMR_MAP) && strcmp(parsed.values.dmr_map_file, "mapping.csv") == 0);
    assert(dsd_scan_options_parse("--dmr-tg-key-clear", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_DMR_MAP) && !parsed.values.dmr_map_file[0]);
    assert(dsd_scan_options_parse("--dmr-tg-key-clear", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error)) != 0);
    assert(dsd_scan_options_parse("--dmr-tg-key-clear --dmr-tg-key-csv map.csv", DSD_SCAN_MODE_DMR, 1, &parsed, error,
                                  sizeof(error))
           != 0);
    assert(dsd_scan_options_parse("", DSD_SCAN_MODE_INHERIT, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == 0);
    assert(dsd_scan_options_parse("-^", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_P25_CANDIDATES);
    assert(dsd_scan_options_parse("-^", DSD_SCAN_MODE_DMR, 0, &parsed, error, sizeof(error)) != 0);
    assert(
        dsd_scan_options_parse("--dmr-force-algid 0x21 -0 -G ./-F", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error))
        == 0);
    assert(parsed.values.force == 0x21 && strcmp(parsed.values.group_file, "./-F") == 0);
    assert(dsd_scan_options_parse("--strict-crc", DSD_SCAN_MODE_DSTAR, 1, &parsed, error, sizeof(error)) == 0);
    assert(dsd_scan_options_parse("-1 '01 23 45 67 89' -0 -F --scan-voice-only --scan-voice-hold-ms=4000",
                                  DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.scalar == 0x0123456789ULL && parsed.values.force == 0x21);
    assert(parsed.values.voice_only == 1 && parsed.values.hold_ms == 4000 && parsed.values.strict_crc == 0);
    assert(dsd_scan_options_parse(
               "-G \"C:\\Radio Lists\\groups.csv\" -K 'relative keys.csv' -k dec.csv -0 --dmr-force-algid 21",
               DSD_SCAN_MODE_DMR, 0, &parsed, error, sizeof(error))
           == 0);
    assert(strcmp(parsed.values.group_file, "C:\\Radio Lists\\groups.csv") == 0);
    assert(strcmp(parsed.hex_file, "relative keys.csv") == 0 && strcmp(parsed.dec_file, "dec.csv") == 0);
    assert(dsd_scan_options_parse("-H 0x0000001f00 -4 -F", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.hytera_digits == 10 && parsed.hytera[0] == 0x1F00 && parsed.values.force == 1);
    assert(dsd_scan_options_parse("-b 0 --no-force-key --strict-crc --no-scan-voice-only", DSD_SCAN_MODE_DMR, 1,
                                  &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.present & DSD_SCAN_OPT_BP);
    assert(parsed.bp == 0 && parsed.values.force == 0 && parsed.values.strict_crc == 1
           && parsed.values.voice_only == 0);
    /* Every accepted direct switch arms decryption, including explicit zero: undecodable P25 stays muted, and the
     * DMR encrypted-audio mute stays the session's, since the vocoder's verdict plays what the key decrypts
     * (issue #655). */
    assert(parsed.values.present & DSD_SCAN_OPT_MUTE_P25);
    assert(!(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
    assert(dsd_scan_options_parse("-b 7", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_P25) && !(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
    assert(dsd_scan_options_parse("-H 0000000000", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_P25) && !(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
    assert(dsd_scan_options_parse("-1 0123456789", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_P25) && !(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
    assert(dsd_scan_options_parse("-R 5", DSD_SCAN_MODE_NXDN48, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_P25) && !(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
    /* Key-file paths share the legacy column limit; the group path is bounded by the option
     * it overrides, and an oversized argument never leaves a partial result behind. */
    {
        char text[DSD_SCAN_OPTIONS_KEY_PATH_MAX + 32];
        const size_t long_len = DSD_SCAN_OPTIONS_KEY_PATH_MAX - 1;
        DSD_MEMSET(text, 0, sizeof(text));
        DSD_MEMCPY(text, "-K ", 3);
        DSD_MEMSET(text + 3, 'k', long_len);
        assert(dsd_scan_options_parse(text, DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
        assert(strlen(parsed.hex_file) == long_len);
        text[3 + long_len] = 'k';
        assert(dsd_scan_options_parse(text, DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);
        assert(strlen(parsed.hex_file) == long_len);
        DSD_MEMCPY(text, "-G ", 3);
        DSD_MEMSET(text + 3, 'g', DSD_SCAN_OPTIONS_GROUP_PATH_MAX);
        text[3 + DSD_SCAN_OPTIONS_GROUP_PATH_MAX] = '\0';
        assert(dsd_scan_options_parse(text, DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) < 0);
        text[3 + DSD_SCAN_OPTIONS_GROUP_PATH_MAX - 1] = '\0';
        assert(dsd_scan_options_parse(text, DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
        assert(strlen(parsed.values.group_file) == DSD_SCAN_OPTIONS_GROUP_PATH_MAX - 1);
    }
    assert(dsd_scan_options_parse("-R 32767 -4", DSD_SCAN_MODE_NXDN48, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.scalar == 32767 && parsed.values.force == 1);
    assert(dsd_scan_options_parse("-R 1", DSD_SCAN_MODE_DPMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(dsd_scan_options_parse("-H 00112233445566778899aabbccddeeff", DSD_SCAN_MODE_P25, 1, &parsed, error,
                                  sizeof(error))
           == 0);
    assert(parsed.hytera_digits == 32 && parsed.hytera[1] == 0x8899aabbccddeeffULL);
    assert(dsd_scan_options_parse("-e --enc-lockout", DSD_SCAN_MODE_DMR, 0, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == (DSD_SCAN_OPT_DATA | DSD_SCAN_OPT_ENC));
    assert(parsed.values.tune_data_calls == 1 && parsed.values.tune_enc_calls == 0);
    assert(dsd_scan_options_parse("--no-data-calls --enc-follow", DSD_SCAN_MODE_P25, 1, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.present == (DSD_SCAN_OPT_DATA | DSD_SCAN_OPT_ENC));
    assert(parsed.values.tune_data_calls == 0 && parsed.values.tune_enc_calls == 1);
    /* The per-visit cap (issue #507) is accepted on every trunk-scan target type: trunked
     * rows (conventional = 0) as well as conventional and untyped ones. */
    assert(dsd_scan_options_parse("--scan-max-visit-ms 20000", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_MAX_VISIT && parsed.values.max_visit_ms == 20000);
    assert(dsd_scan_options_parse("--scan-max-visit-ms 20000", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_MAX_VISIT && parsed.values.max_visit_ms == 20000);
    assert(dsd_scan_options_parse("--scan-max-visit-ms 20000", DSD_SCAN_MODE_INHERIT, 1, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_MAX_VISIT && parsed.values.max_visit_ms == 20000);
    /* 0 disables the cap for that row alone, and both bounds are stored verbatim. */
    assert(dsd_scan_options_parse("--scan-max-visit-ms 0", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_MAX_VISIT && parsed.values.max_visit_ms == 0);
    assert(dsd_scan_options_parse("--scan-max-visit-ms=1000", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_MAX_VISIT && parsed.values.max_visit_ms == 1000);
    assert(dsd_scan_options_parse("--scan-max-visit-ms 3600000", DSD_SCAN_MODE_P25, 0, &parsed, error, sizeof(error))
           == 0);
    assert(parsed.values.present == DSD_SCAN_OPT_MAX_VISIT && parsed.values.max_visit_ms == 3600000);

    const struct {
        const char* text;
        unsigned int mode;
        int conventional;
    } invalid[] = {{"-4 -0", DSD_SCAN_MODE_DMR, 1},
                   {"-0 -4", DSD_SCAN_MODE_DMR, 1},
                   {"-4 -4", DSD_SCAN_MODE_DMR, 1},
                   {"-0 -0", DSD_SCAN_MODE_DMR, 1},
                   {"--no-force-key --no-force-key", DSD_SCAN_MODE_DMR, 1},
                   {"-0 --dmr-force-algid 21 -0", DSD_SCAN_MODE_DMR, 1},
                   {"-0 --dmr-force-algid 24", DSD_SCAN_MODE_DMR, 1},
                   {"--dmr-force-algid 01", DSD_SCAN_MODE_DMR, 1},
                   {"--dmr-force-algid 16", DSD_SCAN_MODE_DMR, 1},
                   {"--dmr-force-algid 100", DSD_SCAN_MODE_DMR, 1},
                   {"-b 256", DSD_SCAN_MODE_DMR, 1},
                   {"-b -1", DSD_SCAN_MODE_DMR, 1},
                   {"-b 1x", DSD_SCAN_MODE_DMR, 1},
                   {"-R 32768", DSD_SCAN_MODE_NXDN48, 1},
                   {"-R 1 -1 0123", DSD_SCAN_MODE_NXDN48, 1},
                   {"-1 12345678901234567", DSD_SCAN_MODE_DMR, 1},
                   {"-H 123", DSD_SCAN_MODE_DMR, 1},
                   {"-H 0011223344", DSD_SCAN_MODE_P25, 1},
                   {"-H 00112233445566778899aabbccddeeff", DSD_SCAN_MODE_NXDN48, 1},
                   {"-b 1 -K keys.csv", DSD_SCAN_MODE_DMR, 1},
                   {"-K keys.csv -1 0123456789", DSD_SCAN_MODE_DMR, 1},
                   {"-b 1 -b 2", DSD_SCAN_MODE_DMR, 1},
                   {"-4", DSD_SCAN_MODE_P25, 1},
                   {"-b 1", DSD_SCAN_MODE_INHERIT, 1},
                   {"-0", DSD_SCAN_MODE_NXDN48, 1},
                   {"-F", DSD_SCAN_MODE_DSTAR, 1},
                   {"--scan-voice-only", DSD_SCAN_MODE_DMR, 0},
                   {"--scan-voice-hold-ms 4000", DSD_SCAN_MODE_P25, 0},
                   {"--scan-voice-hold-ms 99", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-voice-qualify-ms 600001", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-max-visit-ms 999", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-max-visit-ms 1", DSD_SCAN_MODE_P25, 0},
                   {"--scan-max-visit-ms 3600001", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-max-visit-ms -1", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-max-visit-ms", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-max-visit-ms 2000 --scan-max-visit-ms 3000", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-max-visit-ms 20x", DSD_SCAN_MODE_DMR, 1},
                   {"-G 'unterminated", DSD_SCAN_MODE_DMR, 1},
                   {"-G", DSD_SCAN_MODE_DMR, 1},
                   {"-G ''", DSD_SCAN_MODE_DMR, 1},
                   {"-G -F", DSD_SCAN_MODE_DMR, 1},
                   {"-K --no-force-key", DSD_SCAN_MODE_DMR, 1},
                   {"-k --not-a-switch-SENSITIVE", DSD_SCAN_MODE_DMR, 1},
                   {"-F --strict-crc", DSD_SCAN_MODE_DMR, 1},
                   {"--dmr-force-algid 21 -0 --dmr-force-algid 21", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-voice-only=yes", DSD_SCAN_MODE_DMR, 1},
                   {"-e --no-data-calls", DSD_SCAN_MODE_DMR, 1},
                   {"--enc-lockout --enc-follow", DSD_SCAN_MODE_NXDN96, 0},
                   {"-e 1", DSD_SCAN_MODE_DMR, 1},
                   {"-t 0", DSD_SCAN_MODE_DMR, 1},
                   {"-i rtl:0", DSD_SCAN_MODE_DMR, 1},
                   {"-f1", DSD_SCAN_MODE_DMR, 1},
                   {"--frontend terminal", DSD_SCAN_MODE_DMR, 1},
                   {"--not-a-switch-SENSITIVE", DSD_SCAN_MODE_DMR, 1},
                   {"-1 SENSITIVE", DSD_SCAN_MODE_DMR, 1},
                   {"-b 1 ; touch file", DSD_SCAN_MODE_DMR, 1}};

    unsigned char before[sizeof(parsed)];
    DSD_MEMCPY(before, &parsed, sizeof(before));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(dsd_scan_options_parse(invalid[i].text, invalid[i].mode, invalid[i].conventional, &parsed, error,
                                      sizeof(error))
               < 0);
        unsigned char after[sizeof(parsed)];
        DSD_MEMCPY(after, &parsed, sizeof(after));
        assert(memcmp(after, before, sizeof(after)) == 0);
        DSD_SECURE_ZERO(after, sizeof(after));
        assert(strstr(error, "SENSITIVE") == NULL);
    }
    DSD_SECURE_ZERO(&before, sizeof(before));
    DSD_SECURE_ZERO(&parsed, sizeof(parsed));
    return 0;
}
