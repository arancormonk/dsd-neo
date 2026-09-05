// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <string.h>

int
main(void) {
    dsd_scan_options parsed = {0};
    char error[192] = {0};
    assert(dsd_scan_options_parse("", DSD_SCAN_MODE_INHERIT, 1, &parsed, error, sizeof(error)) == 0);
    assert(parsed.values.present == 0);
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
    /* `-b`/`-H` decide DMR encrypted-audio muting as the CLI switches do: explicit zero mutes,
     * material unmutes. `-1`/`-R` load keys without claiming that decision. */
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 1);
    assert(dsd_scan_options_parse("-b 7", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 0);
    assert(dsd_scan_options_parse("-H 0000000000", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert((parsed.values.present & DSD_SCAN_OPT_MUTE_DMR) && parsed.values.mute_dmr == 1);
    assert(dsd_scan_options_parse("-1 0123456789", DSD_SCAN_MODE_DMR, 1, &parsed, error, sizeof(error)) == 0);
    assert(!(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
    assert(dsd_scan_options_parse("-R 5", DSD_SCAN_MODE_NXDN48, 1, &parsed, error, sizeof(error)) == 0);
    assert(!(parsed.values.present & DSD_SCAN_OPT_MUTE_DMR));
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
                   {"-G 'unterminated", DSD_SCAN_MODE_DMR, 1},
                   {"-G", DSD_SCAN_MODE_DMR, 1},
                   {"-G ''", DSD_SCAN_MODE_DMR, 1},
                   {"-G -F", DSD_SCAN_MODE_DMR, 1},
                   {"-K --no-force-key", DSD_SCAN_MODE_DMR, 1},
                   {"-k --not-a-switch-SENSITIVE", DSD_SCAN_MODE_DMR, 1},
                   {"-F --strict-crc", DSD_SCAN_MODE_DMR, 1},
                   {"--dmr-force-algid 21 -0 --dmr-force-algid 21", DSD_SCAN_MODE_DMR, 1},
                   {"--scan-voice-only=yes", DSD_SCAN_MODE_DMR, 1},
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
