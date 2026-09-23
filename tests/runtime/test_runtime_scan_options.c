// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
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

/* The row squelch uses the rtl_sql contract: whole dB from -100 to 0, 0 switches it off, and
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

int
main(void) {
    check_file_spans();
    check_squelch_option();
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
    /* Every accepted direct switch arms decryption, including explicit zero. */
    assert(parsed.values.present & DSD_SCAN_OPT_MUTE_P25);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 0);
    assert(dsd_scan_options_parse("-b 7", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 0);
    assert(dsd_scan_options_parse("-H 0000000000", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 0);
    assert(dsd_scan_options_parse("-1 0123456789", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 0);
    assert(dsd_scan_options_parse("-R 5", DSD_SCAN_MODE_NXDN48, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 0);
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
