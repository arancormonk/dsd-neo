// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-optin.performance.Padding,clang-analyzer-unix.Errno,clang-analyzer-unix.Stream)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/dmr_late_entry.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <errno.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/platform/platform.h"
#include "test_support.h"

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_byte(const char* tag, unsigned char got, unsigned char want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%02X want 0x%02X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u16(const char* tag, uint16_t got, uint16_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* tag, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%llX want 0x%llX\n", tag, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int condition) {
    if (!condition) {
        DSD_FPRINTF(stderr, "%s: condition failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_u8_bits(const char* tag, const uint8_t* got, const uint8_t* want, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (got[i] != want[i]) {
            DSD_FPRINTF(stderr, "%s[%zu]: got %u want %u\n", tag, i, got[i], want[i]);
            return 1;
        }
    }
    return 0;
}

static int
expect_bits(const char* tag, const char* got, const char* want, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (got[i] != want[i]) {
            DSD_FPRINTF(stderr, "%s[%zu]: got %d want %d\n", tag, i, got[i], want[i]);
            return 1;
        }
    }
    return 0;
}

static int
remove_dir(const char* path) {
#if DSD_PLATFORM_WIN_NATIVE
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static char*
test_getcwd(char* buf, size_t size) {
#if DSD_PLATFORM_WIN_NATIVE
    return _getcwd(buf, (int)size);
#else
    return getcwd(buf, size);
#endif
}

static int
test_chdir(const char* path) {
#if DSD_PLATFORM_WIN_NATIVE
    return _chdir(path);
#else
    return chdir(path);
#endif
}

static int
has_suffix(const char* text, const char* suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len && memcmp(text + text_len - suffix_len, suffix, suffix_len) == 0;
}

static int
read_file_prefix(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) {
        return 1;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    size_t n = fread(out, 1, out_size - 1, f);
    if (n == 0 && ferror(f)) {
        DSD_FPRINTF(stderr, "fread(%s) failed: %s\n", path, strerror(errno));
        fclose(f);
        return 1;
    }
    out[n] = '\0';
    fclose(f);
    return 0;
}

static int
read_file_exact(const char* path, unsigned char* out, size_t out_size, size_t* got) {
    if (!path || !out || !got) {
        return 1;
    }
    *got = 0;
    FILE* f = fopen(path, "rb");
    if (!f) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    *got = fread(out, 1, out_size, f);
    if (ferror(f)) {
        DSD_FPRINTF(stderr, "fread(%s) failed: %s\n", path, strerror(errno));
        fclose(f);
        return 1;
    }
    if (*got == out_size) {
        int extra = fgetc(f);
        if (extra != EOF) {
            DSD_FPRINTF(stderr, "%s has more than %zu bytes\n", path, out_size);
            fclose(f);
            return 1;
        }
        if (ferror(f)) {
            DSD_FPRINTF(stderr, "fgetc(%s) failed: %s\n", path, strerror(errno));
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int
read_file_bytes_prefix(const char* path, unsigned char* out, size_t want) {
    if (!path || !out || want == 0) {
        return 1;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    size_t got = fread(out, 1, want, f);
    if (got != want) {
        DSD_FPRINTF(stderr, "fread(%s) got %zu want %zu\n", path, got, want);
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

static long
file_size_or_negative(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        DSD_FPRINTF(stderr, "fseek(%s) failed: %s\n", path, strerror(errno));
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    fclose(f);
    return size;
}

static int
expect_wav_header(const char* tag, const char* path) {
    unsigned char header[12];
    if (read_file_bytes_prefix(path, header, sizeof header) != 0) {
        return 1;
    }
    int rc = 0;
    rc |= expect_true(tag, memcmp(header, "RIFF", 4) == 0);
    rc |= expect_true(tag, memcmp(header + 8, "WAVE", 4) == 0);
    return rc;
}

static int
find_wav_rename_output_for_string_event(char* out, size_t out_size, const char* dir, const Event_History* item) {
    if (!out || out_size == 0 || !dir || !item) {
        return 0;
    }

    char datestr[9];
    char timestr[7];
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, sizeof datestr);
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, sizeof timestr);

    for (unsigned int n = 0; n <= UINT16_MAX; n++) {
        DSD_SNPRINTF(out, out_size, "%s/%s_%s_%05u_%s_%s_TGT_%s_SRC_%s.wav", dir, datestr, timestr, n,
                     item->sysid_string, item->gi == 1 ? "PRIVATE" : "GROUP", item->tgt_str, item->src_str);
        FILE* f = fopen(out, "rb");
        if (f) {
            fclose(f);
            return 1;
        }
    }
    out[0] = '\0';
    return 0;
}

static int
find_wav_rename_output_for_numeric_event(char* out, size_t out_size, const char* dir, const Event_History* item) {
    if (!out || out_size == 0 || !dir || !item) {
        return 0;
    }

    char datestr[9];
    char timestr[7];
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_DATE_COMPACT, datestr, sizeof datestr);
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_TIME_COMPACT, timestr, sizeof timestr);

    for (unsigned int n = 0; n <= UINT16_MAX; n++) {
        DSD_SNPRINTF(out, out_size, "%s/%s_%s_%05u_%s_%s_TGT_%u_SRC_%u.wav", dir, datestr, timestr, n,
                     item->sysid_string, item->gi == 1 ? "PRIVATE" : "GROUP", item->target_id, item->source_id);
        FILE* f = fopen(out, "rb");
        if (f) {
            fclose(f);
            return 1;
        }
    }
    out[0] = '\0';
    return 0;
}

static int
make_rdio_sidecar_path(const char* wav_path, char* out, size_t out_size) {
    if (!wav_path || !out || out_size == 0) {
        return 1;
    }
    int written = DSD_SNPRINTF(out, out_size, "%s", wav_path);
    if (written < 0 || (size_t)written >= out_size) {
        return 1;
    }
    char* dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".wav") == 0) {
        *dot = '\0';
    }
    size_t base_len = strlen(out);
    written = DSD_SNPRINTF(out + base_len, out_size - base_len, "%s", ".json");
    return (written < 0 || (size_t)written >= out_size - base_len) ? 1 : 0;
}

static int
write_wav_test_samples(SNDFILE* wav) {
    const short samples[] = {1000, -1000, 500, -500};
    sf_count_t written = sf_write_short(wav, samples, (sf_count_t)(sizeof samples / sizeof samples[0]));
    return written == (sf_count_t)(sizeof samples / sizeof samples[0]);
}

static void
set_bits_from_bytes(char* bits, const unsigned char* bytes, size_t nbytes) {
    size_t k = 0;
    for (size_t i = 0; i < nbytes; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bits[k++] = (char)((bytes[i] >> bit) & 1u);
        }
    }
}

static int
read_bytes(FILE* f, unsigned char* out, size_t want) {
    if (fseek(f, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek failed\n");
        return 1;
    }
    clearerr(f);
    size_t got = fread(out, 1, want, f);
    if (got != want) {
        if (ferror(f)) {
            DSD_FPRINTF(stderr, "fread failed\n");
        } else {
            DSD_FPRINTF(stderr, "fread got %zu want %zu\n", got, want);
        }
        return 1;
    }
    return 0;
}

static int
write_sdrtrunk_json_input(FILE** out, const char* json) {
    if (!out || !json) {
        return 1;
    }
    *out = tmpfile();
    if (!*out) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    const size_t len = strlen(json);
    if (fwrite(json, 1, len, *out) != len) {
        DSD_FPRINTF(stderr, "failed to write SDRTrunk JSON input\n");
        fclose(*out);
        *out = NULL;
        return 1;
    }
    if (fseek(*out, 0L, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "failed to rewind SDRTrunk JSON input: %s\n", strerror(errno));
        fclose(*out);
        *out = NULL;
        return 1;
    }
    return 0;
}

static int
run_sdrtrunk_json(const char* json, dsd_opts* opts, dsd_state* state) {
    FILE* in = NULL;
    if (write_sdrtrunk_json_input(&in, json) != 0) {
        return 1;
    }
    opts->mbe_in_f = in;
    read_sdrtrunk_json_format(opts, state);
    fclose(in);
    opts->mbe_in_f = NULL;
    return 0;
}

static int
test_imbe_save_read_roundtrip(void) {
    int rc = 0;
    static const unsigned char payload[11] = {0x00, 0xFF, 0x81, 0x7E, 0xA5, 0x5A, 0x3C, 0xC3, 0x18, 0xE7, 0x42};
    char imbe_bits[88] = {0};
    char decoded[88] = {0};
    unsigned char bytes[12] = {0};
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.errs2 = 0x35;
    set_bits_from_bytes(imbe_bits, payload, sizeof payload);

    FILE* out = tmpfile();
    if (!out) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.mbe_out_f = out;
    saveImbe4400Data(&opts, &state, imbe_bits);
    fflush(out);
    rc |= read_bytes(out, bytes, sizeof bytes);
    rc |= expect_byte("imbe-err", bytes[0], 0x35);
    for (size_t i = 0; i < sizeof payload; i++) {
        char tag[32];
        DSD_SNPRINTF(tag, sizeof tag, "imbe-byte-%zu", i);
        rc |= expect_byte(tag, bytes[i + 1], payload[i]);
    }

    rewind(out);
    DSD_MEMSET(decoded, 0x55, sizeof decoded);
    state.errs = 0;
    state.errs2 = 0;
    opts.mbe_in_f = out;
    rc |= expect_int("read-imbe", readImbe4400Data(&opts, &state, decoded), 0);
    rc |= expect_int("imbe-state-errs", state.errs, 0x35);
    rc |= expect_int("imbe-state-errs2", state.errs2, 0x35);
    rc |= expect_bits("imbe-roundtrip", decoded, imbe_bits, sizeof decoded);

    fclose(out);
    opts.mbe_in_f = NULL;
    opts.mbe_out_f = NULL;
    return rc;
}

static int
test_ambe_save_read_roundtrip_and_slot2(void) {
    int rc = 0;
    static const unsigned char payload[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    char ambe_bits[49] = {0};
    char decoded[49] = {0};
    unsigned char bytes[8] = {0};
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.errs2 = 0x44;
    state.errs2R = 0x55;
    set_bits_from_bytes(ambe_bits, payload, sizeof payload);
    ambe_bits[48] = 1;

    FILE* out = tmpfile();
    FILE* out_r = tmpfile();
    if (!out || !out_r) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        if (out) {
            fclose(out);
        }
        if (out_r) {
            fclose(out_r);
        }
        return 1;
    }

    opts.mbe_out_f = out;
    saveAmbe2450Data(&opts, &state, ambe_bits);
    fflush(out);
    rc |= read_bytes(out, bytes, sizeof bytes);
    rc |= expect_byte("ambe-err", bytes[0], 0x44);
    for (size_t i = 0; i < sizeof payload; i++) {
        char tag[32];
        DSD_SNPRINTF(tag, sizeof tag, "ambe-byte-%zu", i);
        rc |= expect_byte(tag, bytes[i + 1], payload[i]);
    }
    rc |= expect_byte("ambe-tail", bytes[7], 1);

    rewind(out);
    DSD_MEMSET(decoded, 0x55, sizeof decoded);
    state.errs = 0;
    state.errs2 = 0;
    opts.mbe_in_f = out;
    rc |= expect_int("read-ambe", readAmbe2450Data(&opts, &state, decoded), 0);
    rc |= expect_int("ambe-state-errs", state.errs, 0x44);
    rc |= expect_int("ambe-state-errs2", state.errs2, 0x44);
    rc |= expect_bits("ambe-roundtrip", decoded, ambe_bits, sizeof decoded);

    DSD_MEMSET(bytes, 0, sizeof bytes);
    opts.mbe_out_fR = out_r;
    saveAmbe2450DataR(&opts, &state, ambe_bits);
    fflush(out_r);
    rc |= read_bytes(out_r, bytes, sizeof bytes);
    rc |= expect_byte("ambe-r-err", bytes[0], 0x55);
    rc |= expect_byte("ambe-r-tail", bytes[7], 1);

    fclose(out);
    fclose(out_r);
    opts.mbe_in_f = NULL;
    opts.mbe_out_f = NULL;
    opts.mbe_out_fR = NULL;
    return rc;
}

static int
test_bit_packing_helpers_roundtrip(void) {
    int rc = 0;
    const uint8_t bits[16] = {1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0};
    const uint8_t want_bytes[2] = {0xA5, 0x5A};
    uint8_t packed[2] = {0};
    uint8_t unpacked[16] = {0};

    rc |= expect_u64("convert bits to output", convert_bits_into_output(bits, 16), 0xA55AULL);

    DSD_PACK_ARRAY_TO_BYTES(bits, packed, 2);
    rc |= expect_byte("pack byte 0", packed[0], want_bytes[0]);
    rc |= expect_byte("pack byte 1", packed[1], want_bytes[1]);

    DSD_UNPACK_ARRAY_TO_BITS(packed, unpacked, 2);
    rc |= expect_u8_bits("unpack byte bits", unpacked, bits, sizeof bits);

    rc |= expect_u64("CRC-CCITT bit array", dsd_crc_ccitt16_bits(bits, sizeof bits), 0xE6CBU);
    rc |= expect_u64("CRC-CCITT empty bit array", dsd_crc_ccitt16_bits(bits, 0U), 0xFFFFU);
    rc |= expect_u64("CRC-CCITT null input", dsd_crc_ccitt16_bits(NULL, sizeof bits), 0U);

    return rc;
}

static int
test_ambe_pack_unpack_49_bits_roundtrip(void) {
    int rc = 0;
    char ambe[49] = {0};
    char decoded[49] = {0};
    uint8_t packed[7] = {0};

    for (size_t i = 0; i < sizeof ambe; i++) {
        ambe[i] = (char)(((i * 7u) + 3u) & 1u);
    }
    ambe[48] = 1;

    pack_ambe(ambe, packed, (int)sizeof ambe);
    rc |= expect_true("ambe tail stored in high bit", (packed[6] & 0x80u) != 0);
    rc |= expect_true("ambe tail padding zeroed", (packed[6] & 0x7Fu) == 0);

    unpack_ambe(packed, decoded);
    rc |= expect_bits("ambe pack/unpack", decoded, ambe, sizeof ambe);

    return rc;
}

static int
test_parse_raw_user_string_guards_and_bounds(void) {
    int rc = 0;
    uint8_t out[4] = {0xEE, 0xEE, 0xEE, 0xEE};

    rc |= expect_u16("parse null input", parse_raw_user_string(NULL, out, sizeof out), 0);
    rc |= expect_u16("parse null output", parse_raw_user_string("00", NULL, sizeof out), 0);
    rc |= expect_u16("parse zero cap", parse_raw_user_string("00", out, 0), 0);
    rc |= expect_u16("parse empty", parse_raw_user_string("", out, sizeof out), 0);
    rc |= expect_byte("parse guards preserve byte 0", out[0], 0xEE);

    DSD_MEMSET(out, 0xEE, sizeof out);
    rc |= expect_u16("parse even hex count", parse_raw_user_string("0a1Bff", out, sizeof out), 3);
    rc |= expect_byte("parse hex byte 0", out[0], 0x0A);
    rc |= expect_byte("parse hex byte 1", out[1], 0x1B);
    rc |= expect_byte("parse hex byte 2", out[2], 0xFF);
    rc |= expect_byte("parse keeps spare byte", out[3], 0xEE);

    DSD_MEMSET(out, 0xEE, sizeof out);
    rc |= expect_u16("parse caps output", parse_raw_user_string("abcdef", out, 2), 2);
    rc |= expect_byte("parse capped byte 0", out[0], 0xAB);
    rc |= expect_byte("parse capped byte 1", out[1], 0xCD);
    rc |= expect_byte("parse capped spare byte", out[2], 0xEE);

    DSD_MEMSET(out, 0xEE, sizeof out);
    rc |= expect_u16("parse invalid pair count", parse_raw_user_string("0g", out, sizeof out), 1);
    rc |= expect_byte("parse invalid pair zeroes byte", out[0], 0x00);
    rc |= expect_byte("parse invalid pair preserves spare", out[1], 0xEE);

    DSD_MEMSET(out, 0xEE, sizeof out);
    rc |= expect_u16("parse odd hex count", parse_raw_user_string("a5f", out, sizeof out), 2);
    rc |= expect_byte("parse odd hex first byte", out[0], 0xA5);
    rc |= expect_byte("parse odd hex padded high nibble", out[1], 0xF0);
    rc |= expect_byte("parse odd hex preserves spare", out[2], 0xEE);

    return rc;
}

static int
test_sdrtrunk_json_metadata_protocols_and_time(void) {
    int rc = 0;

    struct {
        const char* protocol;
        int want_synctype;
    } cases[] = {
        {"APCO25-PHASE1", DSD_SYNC_P25P1_POS},
        {"APCO25-PHASE2", DSD_SYNC_P25P2_POS},
        {"DMR", DSD_SYNC_DMR_BS_DATA_POS},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char json[256];
        static dsd_opts opts;
        static dsd_state state;
        static Event_History_I history[2];

        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_MEMSET(history, 0, sizeof history);
        opts.playfiles = 1;
        state.event_history_s = history;
        DSD_SNPRINTF(json, sizeof json,
                     "{\"version\":\"2\",\"protocol\":\"%s\",\"call_type\":\"GROUP\",\"encrypted\":\"false\","
                     "\"to\":\"1234\",\"from\":\"5678\",\"time\":\"1700000000000\"}",
                     cases[i].protocol);

        rc |= run_sdrtrunk_json(json, &opts, &state);
        rc |= expect_int("sdrtrunk protocol synctype", state.synctype, cases[i].want_synctype);
        rc |= expect_int("sdrtrunk protocol lastsynctype", state.lastsynctype, cases[i].want_synctype);
        dsd_call_snapshot call = {0};
        rc |= expect_true("sdrtrunk canonical call available", dsd_call_state_get(&state, 0U, &call) > 0);
        rc |= expect_int("sdrtrunk group call", (int)call.kind, DSD_CALL_KIND_GROUP_VOICE);
        rc |= expect_u64("sdrtrunk target id", call.ota_target_id, 1234U);
        rc |= expect_u64("sdrtrunk policy target id", call.policy_target_id, 1234U);
        rc |= expect_u64("sdrtrunk source id", call.ota_source_id, 5678U);
        rc |= expect_u64("sdrtrunk event time", (uint64_t)history[0].Event_History_Items[0].event_time, 1700000000ULL);
        dsd_state_ext_free_all(&state);
    }

    return rc;
}

static int
test_sdrtrunk_json_encryption_metadata_updates_payload_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] =
        "{\"version\":\"2\",\"protocol\":\"APCO25-PHASE1\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_algorithm\":\"129\",\"encryption_key_id\":\"4660\","
        "\"encryption_mi\":\"001122334455667788\",\"to\":\"55\",\"from\":\"66\",\"time\":\"1700000000000\"}";

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    state.event_history_s = history;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    rc |= expect_int("sdrtrunk encrypted synctype", state.synctype, DSD_SYNC_P25P1_POS);
    rc |= expect_int("sdrtrunk encrypted current slot", state.currentslot, 0);
    rc |= expect_int("sdrtrunk encrypted algid", state.payload_algid, 0x81);
    rc |= expect_u16("sdrtrunk encrypted key id", state.payload_keyid, 4660);
    rc |= expect_u64("sdrtrunk encrypted mi truncates 18-char value", state.payload_mi, 0x0011223344556677ULL);
    dsd_call_snapshot call = {0};
    rc |= expect_true("sdrtrunk encrypted canonical call available", dsd_call_state_get(&state, 0U, &call) > 0);
    rc |= expect_u64("sdrtrunk encrypted target id", call.ota_target_id, 55U);
    rc |= expect_u64("sdrtrunk encrypted source id", call.ota_source_id, 66U);
    rc |= expect_int("sdrtrunk encrypted canonical algid", call.algid, 0x81);
    rc |= expect_u16("sdrtrunk encrypted canonical key id", call.kid, 4660U);
    rc |= expect_u64("sdrtrunk encrypted canonical mi", call.mi, 0x0011223344556677ULL);
    dsd_state_ext_free_all(&state);

    return rc;
}

// --dmr-tg-key-csv was a complete no-op for sdrtrunk JSON replay: that path activated keys
// directly and never reached the voice-frame prep where the map used to be applied.
//
// state->R is the wrong thing to assert on here, and asserting on it is what let the AES sub-case
// stay silently inert: the replay keystream builders index rkey_array by *key id* and consult
// state->R only as a fallback when that index is empty, and sdrtrunk_build_aes_keystream_bytes()
// never reads state->R at all. What actually decrypts is ctx->ks, which ambe2_str_to_decode()
// XORs into each voice frame before saveAmbe2450Data() writes it -- so these cases pin the MBE
// records the replay produced.
//
// Each JSON record is replayed three times: once with the map (signaled key id 0x03, row
// TG 123 -> 0x7B), once with 0x7B signaled directly and no map (the bytes the mapped run must
// reproduce), and once with 0x03 signaled and no map (the bytes it must not). The third run is
// what makes the second load-bearing: a builder that ignored the key id entirely would satisfy
// the equality alone.
//
// JSON object field order is not a contract, and the neighbouring P25 fixtures in this file
// (and the DMR late-entry fixture below) all put "encryption_mi" ahead of "to"/"from" --
// sdrtrunk_json_handle_mi() alone would see ctx->target_id still 0 in that order and build the
// keystream from the signaled key id. sdrtrunk_json_apply_dmr_tg_key_map() runs on every token
// and rebuilds it once target_id is known, the same way sdrtrunk_json_apply_forced_algid()'s own
// fallback already re-activates, so both field orders must reach the same records.
enum {
    SDRTRUNK_MAP_SIGNALED_KID = 0x03,
    SDRTRUNK_MAP_MAPPED_KID = 0x7B,
    SDRTRUNK_MAP_TG = 123,
    SDRTRUNK_MAP_RECORD_CAP = 128,
};

// Distinguishable material at both key ids, on every segment the builders read. rkey_array[kid]
// doubles as the RC4/DES scalar and as the AES A1 segment, exactly as
// keyring_activate_slot_with_kid() treats it; the remaining three offsets are AES-only, and
// AES-256 is the only algorithm that reaches the last two.
static void
seed_sdrtrunk_dmr_replay_keys(dsd_state* state) {
    static const unsigned long long int material[2][4] = {
        {0xA1A2A3A4A5ULL, 0xA6A7A8A9AAABACADULL, 0xAEAFB0B1B2B3B4B5ULL, 0xB6B7B8B9BABBBCBDULL},
        {0xC1C2C3C4C5ULL, 0xC6C7C8C9CACBCCCDULL, 0xCECFD0D1D2D3D4D5ULL, 0xD6D7D8D9DADBDCDDULL},
    };
    static const int segment_offset[4] = {0x000, 0x101, 0x201, 0x301};
    static const int kids[2] = {SDRTRUNK_MAP_SIGNALED_KID, SDRTRUNK_MAP_MAPPED_KID};

    state->keyloader = 1;
    for (size_t k = 0; k < 2U; k++) {
        for (size_t s = 0; s < 4U; s++) {
            const int index = kids[k] + segment_offset[s];
            state->rkey_array[index] = material[k][s];
            state->rkey_array_loaded[index] = 1U;
        }
    }
}

static int
capture_sdrtrunk_replay_records(const char* tag, const char* json, dsd_state* state, unsigned char* out, size_t out_cap,
                                size_t* out_len) {
    int rc = 0;
    static dsd_opts opts;
    static Event_History_I history[2];

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(history, 0, sizeof history);
    DSD_MEMSET(out, 0, out_cap);
    *out_len = 0;
    opts.playfiles = 1;
    opts.floating_point = 1;
    state->event_history_s = history;

    FILE* f = tmpfile();
    if (!f) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.mbe_out_f = f;
    rc |= run_sdrtrunk_json(json, &opts, state);
    rc |= expect_int(tag, fflush(f), 0);
    if (fseek(f, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek(%s) failed\n", tag);
        rc = 1;
    }
    clearerr(f);
    *out_len = fread(out, 1, out_cap, f);
    if (ferror(f)) {
        DSD_FPRINTF(stderr, "fread(%s) failed\n", tag);
        rc = 1;
    }
    fclose(f);
    opts.mbe_out_f = NULL;
    return rc;
}

// Three all-zero AMBE frames follow the metadata: the decoded record body is then purely a
// function of the keystream window each frame consumed, so a wrong key changes every byte after
// the error count. addr_before/addr_after place "to"/"from" on either side of the crypto fields
// without changing anything else about the record.
static void
build_sdrtrunk_map_json(char* out, size_t cap, const char* addr_before, const char* addr_after, unsigned alg_id,
                        unsigned key_id) {
    DSD_SNPRINTF(out, cap,
                 "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",%s\"encrypted\":\"true\","
                 "\"encryption_algorithm\":\"%u\",\"encryption_key_id\":\"%u\","
                 "\"encryption_mi\":\"001122334455667788\",%s\"time\":\"1700000000000\","
                 "\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\"}",
                 addr_before, alg_id, key_id, addr_after);
}

static int
test_sdrtrunk_json_dmr_tg_key_map_overrides_signaled_kid(void) {
    int rc = 0;

    struct {
        const char* label;
        const char* addr_before;
        const char* addr_after;
    } orders[] = {
        {"encryption_mi before to/from", "", "\"to\":\"123\",\"from\":\"456\","},
        {"to/from before encryption_mi", "\"to\":\"123\",\"from\":\"456\",", ""},
    };

    struct {
        const char* name;
        unsigned alg_id;
    } algs[] = {
        {"rc4", 0x21U},
        {"aes128", 0x89U},
        {"aes256", 0x84U},
    };

    for (size_t o = 0; o < sizeof orders / sizeof orders[0]; o++) {
        for (size_t a = 0; a < sizeof algs / sizeof algs[0]; a++) {
            static dsd_state state;
            unsigned char mapped[SDRTRUNK_MAP_RECORD_CAP];
            unsigned char want[SDRTRUNK_MAP_RECORD_CAP];
            unsigned char signaled[SDRTRUNK_MAP_RECORD_CAP];
            size_t mapped_len = 0;
            size_t want_len = 0;
            size_t signaled_len = 0;
            char json[512];
            char tag[128];

            // Mapped: the record signals 0x03, the map points TG 123 at 0x7B.
            build_sdrtrunk_map_json(json, sizeof json, orders[o].addr_before, orders[o].addr_after, algs[a].alg_id,
                                    (unsigned)SDRTRUNK_MAP_SIGNALED_KID);
            DSD_MEMSET(&state, 0, sizeof state);
            seed_sdrtrunk_dmr_replay_keys(&state);
            state.dmr_tg_key_map_tg[0] = SDRTRUNK_MAP_TG;
            state.dmr_tg_key_map_kid[0] = SDRTRUNK_MAP_MAPPED_KID;
            state.dmr_tg_key_map_count = 1;
            DSD_SNPRINTF(tag, sizeof tag, "sdrtrunk map replay %s (%s)", algs[a].name, orders[o].label);
            rc |= capture_sdrtrunk_replay_records(tag, json, &state, mapped, sizeof mapped, &mapped_len);
            // The OTA key id stays the truth in state, as it does on every other path.
            DSD_SNPRINTF(tag, sizeof tag, "sdrtrunk ota key id untouched %s (%s)", algs[a].name, orders[o].label);
            rc |= expect_int(tag, state.payload_keyid, SDRTRUNK_MAP_SIGNALED_KID);
            dsd_state_ext_free_all(&state);

            // Reference: 0x7B signaled directly, no map row.
            build_sdrtrunk_map_json(json, sizeof json, orders[o].addr_before, orders[o].addr_after, algs[a].alg_id,
                                    (unsigned)SDRTRUNK_MAP_MAPPED_KID);
            DSD_MEMSET(&state, 0, sizeof state);
            seed_sdrtrunk_dmr_replay_keys(&state);
            DSD_SNPRINTF(tag, sizeof tag, "sdrtrunk mapped-key reference %s (%s)", algs[a].name, orders[o].label);
            rc |= capture_sdrtrunk_replay_records(tag, json, &state, want, sizeof want, &want_len);
            dsd_state_ext_free_all(&state);

            // Decoy: 0x03 signaled, no map row -- what the map must move the output away from.
            build_sdrtrunk_map_json(json, sizeof json, orders[o].addr_before, orders[o].addr_after, algs[a].alg_id,
                                    (unsigned)SDRTRUNK_MAP_SIGNALED_KID);
            DSD_MEMSET(&state, 0, sizeof state);
            seed_sdrtrunk_dmr_replay_keys(&state);
            DSD_SNPRINTF(tag, sizeof tag, "sdrtrunk signaled-key decoy %s (%s)", algs[a].name, orders[o].label);
            rc |= capture_sdrtrunk_replay_records(tag, json, &state, signaled, sizeof signaled, &signaled_len);
            dsd_state_ext_free_all(&state);

            DSD_SNPRINTF(tag, sizeof tag, "sdrtrunk map replay wrote records %s (%s)", algs[a].name, orders[o].label);
            rc |= expect_true(tag, mapped_len >= 24U && want_len == mapped_len && signaled_len == mapped_len);
            DSD_SNPRINTF(tag, sizeof tag, "sdrtrunk map keyed the keystream %s (%s)", algs[a].name, orders[o].label);
            rc |= expect_u8_bits(tag, mapped, want, mapped_len < want_len ? mapped_len : want_len);
            DSD_SNPRINTF(tag, sizeof tag, "sdrtrunk keys are distinguishable %s (%s)", algs[a].name, orders[o].label);
            rc |= expect_true(tag, mapped_len != signaled_len || memcmp(mapped, signaled, mapped_len) != 0);
        }
    }
    return rc;
}

// Task 3 (commit 17040e67) made sdrtrunk_json_apply_dmr_tg_key_map()'s gate depend on
// dsd_dmr_alg_key_need(ctx->alg_id), so a map row now only satisfies the material check once
// ctx->alg_id is known -- a row can now only apply once the "encryption_algorithm" token has been
// seen. sdrtrunk_json_apply_dmr_tg_key_map() still runs on every token and self-corrects once
// alg_id lands (see its own comment above), and sdrtrunk_json_rekey_slot0() rebuilds the keystream
// whenever ks_built==0 or ks_key_id != kid, so both orderings are expected to settle on the same
// mapped key by the time "hex" is reached.
//
// This pins that seam specifically: the "encryption_mi before/after to from" cases above move the
// whole encrypted/algorithm/key_id/mi block as one unit, which happens to carry
// "encryption_algorithm" along with it -- build_sdrtrunk_map_json()'s "to/from before
// encryption_mi" order IS "encryption_algorithm after to/from", the ordering Task 3 made
// load-bearing, so no new fixture is needed: reusing the helper's existing two orders and
// asserting their MAPPED outputs match each other (not just each its own same-order reference)
// is what the earlier matrix test never checked directly.
static int
test_sdrtrunk_json_dmr_tg_key_map_settles_regardless_of_algorithm_order(void) {
    int rc = 0;
    char algorithm_first[512];
    char addressing_first[512];
    static dsd_state state;
    unsigned char algorithm_first_out[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char addressing_first_out[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char signaled_out[SDRTRUNK_MAP_RECORD_CAP];
    size_t algorithm_first_len = 0;
    size_t addressing_first_len = 0;
    size_t signaled_len = 0;

    // "encryption_algorithm" (bundled with key_id/mi) ahead of "to"/"from".
    build_sdrtrunk_map_json(algorithm_first, sizeof algorithm_first, "", "\"to\":\"123\",\"from\":\"456\",", 0x21U,
                            (unsigned)SDRTRUNK_MAP_SIGNALED_KID);
    // "to"/"from" ahead of "encryption_algorithm" -- ctx->alg_id is still 0 (dsd_dmr_alg_key_need()
    // reads DSD_KEY_NEED_NONE) when target_id lands, so the map cannot apply on that token; it must
    // catch up once "encryption_algorithm" itself is parsed.
    build_sdrtrunk_map_json(addressing_first, sizeof addressing_first, "\"to\":\"123\",\"from\":\"456\",", "", 0x21U,
                            (unsigned)SDRTRUNK_MAP_SIGNALED_KID);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.dmr_tg_key_map_tg[0] = SDRTRUNK_MAP_TG;
    state.dmr_tg_key_map_kid[0] = SDRTRUNK_MAP_MAPPED_KID;
    state.dmr_tg_key_map_count = 1;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk map algorithm before to/from", algorithm_first, &state,
                                          algorithm_first_out, sizeof algorithm_first_out, &algorithm_first_len);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.dmr_tg_key_map_tg[0] = SDRTRUNK_MAP_TG;
    state.dmr_tg_key_map_kid[0] = SDRTRUNK_MAP_MAPPED_KID;
    state.dmr_tg_key_map_count = 1;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk map to/from before algorithm", addressing_first, &state,
                                          addressing_first_out, sizeof addressing_first_out, &addressing_first_len);
    dsd_state_ext_free_all(&state);

    // No map row loaded: the signaled key alone. Proves the equality below is not vacuous -- i.e.
    // that the map genuinely applied in both orders rather than in neither.
    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    rc |= capture_sdrtrunk_replay_records("sdrtrunk map algorithm-order signaled reference", algorithm_first, &state,
                                          signaled_out, sizeof signaled_out, &signaled_len);
    dsd_state_ext_free_all(&state);

    rc |= expect_true("sdrtrunk map algorithm-order records wrote output",
                      algorithm_first_len >= 24U && addressing_first_len == algorithm_first_len
                          && signaled_len == algorithm_first_len);
    rc |= expect_u8_bits("sdrtrunk map settles on the mapped key regardless of algorithm order", addressing_first_out,
                         algorithm_first_out, algorithm_first_len);
    rc |= expect_true("sdrtrunk map algorithm-order still moved the keystream off the signaled key",
                      memcmp(algorithm_first_out, signaled_out, algorithm_first_len) != 0);
    return rc;
}

// A private call's destination is a RADIO ID, and DMR radio ids share the talkgroup's 24-bit
// space, so a map row must never key one. The map is resolved on every token because JSON field
// order is not a contract, which means it can be applied on a partially-parsed record and then
// have to be taken back out again: here "call_type" arrives after the crypto fields, so the map
// applies against the still-default GROUP reading first and the later PRIVATE reading has to undo
// it. The reference run signals the same key id with no map row loaded at all.
static int
test_sdrtrunk_json_private_call_never_keeps_a_map_row(void) {
    int rc = 0;
    // "call_type" last: every earlier token sees the DSD_CALL_KIND_VOICE default, which reads as
    // group. The destination radio id equals the mapped talkgroup number on purpose.
    static const char json[] = "{\"version\":\"2\",\"protocol\":\"DMR\",\"encrypted\":\"true\","
                               "\"encryption_algorithm\":\"33\",\"encryption_key_id\":\"3\","
                               "\"encryption_mi\":\"001122334455667788\",\"to\":\"123\",\"from\":\"456\","
                               "\"call_type\":\"PRIVATE\",\"time\":\"1700000000000\","
                               "\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\","
                               "\"hex\":\"000000000000000000\"}";
    static dsd_state state;
    unsigned char with_map[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char without_map[SDRTRUNK_MAP_RECORD_CAP];
    size_t with_map_len = 0;
    size_t without_map_len = 0;

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.dmr_tg_key_map_tg[0] = SDRTRUNK_MAP_TG;
    state.dmr_tg_key_map_kid[0] = SDRTRUNK_MAP_MAPPED_KID;
    state.dmr_tg_key_map_count = 1;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk private call with map", json, &state, with_map, sizeof with_map,
                                          &with_map_len);
    // The map must leave no trace in the slot key either.
    rc |= expect_true("sdrtrunk private call slot key is the signaled one",
                      state.R == state.rkey_array[SDRTRUNK_MAP_SIGNALED_KID]);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    rc |= capture_sdrtrunk_replay_records("sdrtrunk private call without map", json, &state, without_map,
                                          sizeof without_map, &without_map_len);
    dsd_state_ext_free_all(&state);

    rc |= expect_true("sdrtrunk private call wrote records", with_map_len >= 24U);
    rc |= expect_true("sdrtrunk private call ignores the map row",
                      with_map_len == without_map_len && memcmp(with_map, without_map, with_map_len) == 0);
    return rc;
}

// sdrtrunk_json_apply_forced_algid() builds its own RC4 keystream from a payload_mi-derived IV,
// on every token, and that build wins over the one sdrtrunk_json_handle_mi() made -- so the
// resolved key id has to reach it too. With --dmr-force-algid set (state.M) and no
// "encryption_algorithm" field, this is the only build that runs, which is what keeps this
// fixture pointed at that one call site instead of at handle_mi's.
//
// The talkgroup is 4567 rather than 123 on purpose: 123 == 0x7B is also the mapped key id, so
// this branch's implicit rkey_array[target_id] fallback would collide with the map's own index
// and mask which of the two produced the keystream. rkey_array[4567] is left empty so the
// fallback cannot fire at all here.
static int
test_sdrtrunk_json_dmr_tg_key_map_keys_forced_algid_keystream(void) {
    int rc = 0;
    static const char json_signaled_kid[] =
        "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_key_id\":\"3\",\"encryption_mi\":\"001122334455667788\","
        "\"to\":\"4567\",\"from\":\"456\",\"time\":\"1700000000000\","
        "\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\"}";
    static const char json_mapped_kid[] =
        "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_key_id\":\"123\",\"encryption_mi\":\"001122334455667788\","
        "\"to\":\"4567\",\"from\":\"456\",\"time\":\"1700000000000\","
        "\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\"}";
    static dsd_state state;
    unsigned char mapped[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char want[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char signaled[SDRTRUNK_MAP_RECORD_CAP];
    size_t mapped_len = 0;
    size_t want_len = 0;
    size_t signaled_len = 0;

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21;
    state.dmr_tg_key_map_tg[0] = 4567U;
    state.dmr_tg_key_map_kid[0] = SDRTRUNK_MAP_MAPPED_KID;
    state.dmr_tg_key_map_count = 1;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced-algid map replay", json_signaled_kid, &state, mapped,
                                          sizeof mapped, &mapped_len);
    rc |= expect_int("sdrtrunk forced-algid ota key id untouched", state.payload_keyid, SDRTRUNK_MAP_SIGNALED_KID);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced-algid mapped-key reference", json_mapped_kid, &state, want,
                                          sizeof want, &want_len);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced-algid signaled-key decoy", json_signaled_kid, &state,
                                          signaled, sizeof signaled, &signaled_len);
    dsd_state_ext_free_all(&state);

    rc |= expect_true("sdrtrunk forced-algid replay wrote records",
                      mapped_len >= 24U && want_len == mapped_len && signaled_len == mapped_len);
    rc |= expect_u8_bits("sdrtrunk forced-algid map keyed the keystream", mapped, want,
                         mapped_len < want_len ? mapped_len : want_len);
    rc |= expect_true("sdrtrunk forced-algid keys are distinguishable",
                      mapped_len != signaled_len || memcmp(mapped, signaled, mapped_len) != 0);
    return rc;
}

// sdrtrunk_json_apply_forced_algid() runs on every token, and its RC4 branch only rebuilds a
// keystream when both a key (state->R) and an IV (state->payload_mi) are known. Before this fix,
// when that guard failed, the PREVIOUS token's ctx->ks_available simply stayed in force, so the
// decoder kept reporting a record decryptable using a keystream it no longer had grounds to trust.
//
// A record whose key id is genuinely unseeded self-corrects before any "hex" token can observe a
// stale value: sdrtrunk_json_handle_mi() unconditionally activates the signaled id *and* rebuilds
// ctx->ks_available in the same call, so state->R and ctx->ks_available always land together --
// there is no window between them for a later token to see. The guard's failure survives to a
// later "hex" token only when state->R stays valid (a properly imported key) while
// state->payload_mi alone goes to zero: an all-zero "encryption_mi" is exactly that -- a real,
// importable key with no usable IV. handle_mi()'s own build succeeds anyway (RC4 does not reject a
// zero IV), latching ctx->ks_available=1 from a keystream built with a bogus, all-zero IV; the
// very next token's guard check then sees payload_mi==0 and, pre-fix, left that latch alone
// instead of reporting "no keystream." Both records below signal the SAME key id (3, imported by
// seed_sdrtrunk_dmr_replay_keys()) so state->R is genuinely valid throughout -- the only thing the
// second record lacks is its own IV.
//
// Three all-zero AMBE frames make the decoded body purely a function of the keystream window each
// frame consumed (see build_sdrtrunk_map_json()'s comment); here that means a blocked record must
// write nothing at all rather than merely differing bytes.
static int
test_sdrtrunk_forced_rc4_reports_no_keystream_without_an_iv(void) {
    int rc = 0;
    /* A normal, fully decryptable forced-RC4 record: kid 3, a real MI. */
    static const char json_keyed[] = "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\","
                                     "\"encrypted\":\"true\",\"encryption_key_id\":\"3\","
                                     "\"encryption_mi\":\"001122334455667788\",\"to\":\"4567\","
                                     "\"from\":\"456\",\"time\":\"1700000000000\","
                                     "\"hex\":\"000000000000000000\"}";
    /* SAME key id 3 (still a valid, imported key) but an all-zero MI -- no usable IV. */
    static const char json_no_iv[] = "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\","
                                     "\"encrypted\":\"true\",\"encryption_key_id\":\"3\","
                                     "\"encryption_mi\":\"000000000000000000\",\"to\":\"4567\","
                                     "\"from\":\"456\",\"time\":\"1700000001000\","
                                     "\"hex\":\"000000000000000000\"}";
    char pair[1024];
    static dsd_state state;
    unsigned char keyed[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char both[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char alone[SDRTRUNK_MAP_RECORD_CAP];
    size_t keyed_len = 0;
    size_t both_len = 0;
    size_t alone_len = 0;

    DSD_SNPRINTF(pair, sizeof pair, "%s\n%s", json_keyed, json_no_iv);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21; /* --dmr-force-algid 21 */
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced rc4 keyed reference", json_keyed, &state, keyed,
                                          sizeof keyed, &keyed_len);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced rc4 keyed then no-iv pair", pair, &state, both, sizeof both,
                                          &both_len);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced rc4 no-iv alone", json_no_iv, &state, alone, sizeof alone,
                                          &alone_len);
    dsd_state_ext_free_all(&state);

    rc |= expect_true("sdrtrunk forced rc4 keyed record wrote a record", keyed_len >= 8U);
    rc |= expect_true("sdrtrunk forced rc4 record with no iv writes nothing alone", alone_len == 0U);
    rc |=
        expect_true("sdrtrunk forced rc4 record with no iv writes nothing after a keyed record", both_len == keyed_len);
    rc |= expect_u8_bits("sdrtrunk forced rc4 keyed record unaffected by the trailing no-iv record", both, keyed,
                         both_len < keyed_len ? both_len : keyed_len);
    return rc;
}

// With no matching row, the pre-existing implicit "key indexed by talkgroup" replay behavior
// must be untouched -- replay workflows depend on it. That convention lives only in
// sdrtrunk_json_apply_forced_algid()'s DMRA branch (state.M in 0x21..0x25): handle_mi has no
// rkey_array[target_id] fallback of its own -- an unmapped target there activates the
// *signaled* key id, not the target-keyed one -- so this fixture forces state.M and drops
// encryption_mi from the token stream entirely, keeping the assertion pointed at the one path
// that implements the behavior it names.
static int
test_sdrtrunk_json_without_map_row_keeps_target_keyed_lookup(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] = "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
                               "\"encryption_algorithm\":\"33\",\"encryption_key_id\":\"3\","
                               "\"to\":\"123\",\"from\":\"456\",\"time\":\"1700000000000\"}";

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    state.event_history_s = history;
    state.M = 0x21;

    state.keyloader = 1;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    // No map row for 123; the implicit lookup keys R off the target id itself.
    state.rkey_array[123] = 0xCCCCCULL;
    state.rkey_array_loaded[123] = 1U;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    rc |= expect_u64("sdrtrunk implicit lookup preserved", state.R, 0xCCCCCULL);
    dsd_state_ext_free_all(&state);
    return rc;
}

// sdrtrunk_json_handle_mi() also serves P25 replay, where ctx->key_id (a uint16_t; P25 signals a
// full 16-bit KID) must reach keyring_activate_slot_with_kid() at full width, not narrowed to
// the DMR resolver's uint8_t. Neither P25 fixture above sets state.keyloader = 1, so nothing
// else in this file exercises this activation block on the P25 path. rkey_array is seeded at
// both the real key id (4660) and the 8-bit-truncated one (4660 & 0xFF == 0x34) with
// distinguishable material so a reintroduced narrowing cast lands on the wrong value instead of
// silently matching.
static int
test_sdrtrunk_json_p25_replay_keyloader_uses_full_width_key_id(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] =
        "{\"version\":\"2\",\"protocol\":\"APCO25-PHASE1\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_algorithm\":\"129\",\"encryption_key_id\":\"4660\","
        "\"encryption_mi\":\"001122334455667788\",\"to\":\"55\",\"from\":\"66\",\"time\":\"1700000000000\"}";

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    state.event_history_s = history;

    state.keyloader = 1;
    state.rkey_array[4660] = 0xDDDDDULL;
    state.rkey_array_loaded[4660] = 1U;
    // Decoy at the 8-bit-truncated index: a reintroduced (uint8_t) cast would land here instead.
    state.rkey_array[0x34] = 0xEEEEEULL;
    state.rkey_array_loaded[0x34] = 1U;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    rc |= expect_u64("sdrtrunk p25 keyloader full-width key id", state.R, 0xDDDDDULL);
    dsd_state_ext_free_all(&state);
    return rc;
}

// The DMR branch of that same block narrows the key id to the resolver's uint8_t, so a record
// whose "encryption_key_id" does not fit a byte has to skip the resolver rather than be truncated
// into rkey_array[id & 0xFF]. DMR signals a byte-wide KEY ID, so only a malformed record gets
// here -- and a malformed record the map does not cover has to behave exactly as it did before
// the map existed. The reference run signals 52 (== 4660 & 0xFF) against the same seeded keyring:
// under a reintroduced narrowing the two runs would decrypt identically instead of differing.
static int
test_sdrtrunk_json_dmr_replay_oversized_key_id_keeps_full_width(void) {
    int rc = 0;
    static dsd_state state;
    static const char json_wide[] =
        "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_algorithm\":\"33\",\"encryption_key_id\":\"4660\","
        "\"encryption_mi\":\"001122334455667788\",\"to\":\"123\",\"from\":\"456\",\"time\":\"1700000000000\","
        "\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\"}";
    static const char json_truncated[] =
        "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_algorithm\":\"33\",\"encryption_key_id\":\"52\","
        "\"encryption_mi\":\"001122334455667788\",\"to\":\"123\",\"from\":\"456\",\"time\":\"1700000000000\","
        "\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\",\"hex\":\"000000000000000000\"}";
    unsigned char wide[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char truncated[SDRTRUNK_MAP_RECORD_CAP];
    size_t wide_len = 0;
    size_t truncated_len = 0;

    DSD_MEMSET(&state, 0, sizeof state);
    state.keyloader = 1;
    state.rkey_array[4660] = 0xD1D2D3D4D5ULL;
    state.rkey_array_loaded[4660] = 1U;
    // Decoy at the 8-bit-truncated index (4660 & 0xFF == 0x34 == 52).
    state.rkey_array[0x34] = 0xE1E2E3E4E5ULL;
    state.rkey_array_loaded[0x34] = 1U;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk dmr oversized key id", json_wide, &state, wide, sizeof wide,
                                          &wide_len);
    rc |= expect_u64("sdrtrunk dmr oversized key id activates full width", state.R, 0xD1D2D3D4D5ULL);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    state.keyloader = 1;
    state.rkey_array[4660] = 0xD1D2D3D4D5ULL;
    state.rkey_array_loaded[4660] = 1U;
    state.rkey_array[0x34] = 0xE1E2E3E4E5ULL;
    state.rkey_array_loaded[0x34] = 1U;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk dmr truncated key id", json_truncated, &state, truncated,
                                          sizeof truncated, &truncated_len);
    dsd_state_ext_free_all(&state);

    rc |= expect_true("sdrtrunk dmr oversized key id wrote records", wide_len >= 24U && truncated_len == wide_len);
    rc |= expect_true("sdrtrunk dmr oversized key id is not the truncated one",
                      wide_len != truncated_len || memcmp(wide, truncated, wide_len) != 0);
    return rc;
}

static int
test_sdrtrunk_json_p25p2_encryption_metadata_updates_event(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] =
        "{\"version\":\"2\",\"protocol\":\"APCO25-PHASE2\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_algorithm\":\"132\",\"encryption_key_id\":\"8738\","
        "\"encryption_mi\":\"0011223344556677\",\"to\":\"55\",\"from\":\"66\",\"time\":\"1700000000000\"}";

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    state.event_history_s = history;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    const Event_History* item = &history[0].Event_History_Items[0];
    rc |= expect_int("sdrtrunk p25p2 crypto state", state.p25_crypto_state[0], DSD_P25_CRYPTO_BLOCKED);
    rc |= expect_int("sdrtrunk p25p2 event encrypted", item->enc, 1);
    rc |= expect_int("sdrtrunk p25p2 event algid", item->enc_alg, 0x84);
    rc |= expect_u16("sdrtrunk p25p2 event key id", item->enc_key, 0x2222);
    rc |= expect_u64("sdrtrunk p25p2 event mi", item->mi, 0x0011223344556677ULL);

    return rc;
}

static int
test_sdrtrunk_json_invalid_numeric_fields_reset_to_zero(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] =
        "{\"protocol\":\"DMR\",\"call_type\":\"PRIVATE\",\"encrypted\":\"false\",\"to\":\"notnum\","
        "\"from\":\"bad\",\"time\":\"bad0000000\"}";

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    state.event_history_s = history;

    const dsd_call_observation seeded = {
        .protocol = DSD_SYNC_DMR_BS_DATA_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 111U,
        .policy_target_id = 111U,
        .ota_source_id = 222U,
    };
    rc |= expect_true("sdrtrunk seed prior canonical call",
                      dsd_call_state_observe(&state, &seeded, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_call_snapshot prior = {0};
    rc |= expect_true("sdrtrunk prior canonical call available", dsd_call_state_get(&state, 0U, &prior) > 0);

    rc |= run_sdrtrunk_json(json, &opts, &state);
    dsd_call_snapshot call = {0};
    rc |= expect_true("sdrtrunk invalid canonical call available", dsd_call_state_get(&state, 0U, &call) > 0);
    rc |= expect_true("sdrtrunk invalid fields begin fresh epoch", call.epoch != prior.epoch);
    rc |= expect_u64("sdrtrunk invalid target zero", call.ota_target_id, 0U);
    rc |= expect_u64("sdrtrunk invalid source zero", call.ota_source_id, 0U);
    rc |= expect_int("sdrtrunk private call", (int)call.kind, DSD_CALL_KIND_PRIVATE_VOICE);
    rc |= expect_u64("sdrtrunk invalid time zero", (uint64_t)history[0].Event_History_Items[0].event_time, 0ULL);
    dsd_state_ext_free_all(&state);

    return rc;
}

static int
test_sdrtrunk_json_protocol_opens_and_closes_mbe_out_file(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] = "{\"protocol\":\"DMR\",\"encrypted\":\"false\"}";

    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_sdrtrunk_mbe")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof opts.mbe_out_dir, "%s%c", dir, dsd_test_path_sep());
    opts.playfiles = 1;
    state.tgcount = 3;
    state.tg[2][1] = 4;
    state.event_history_s = history;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    rc |= expect_int("sdrtrunk mbe close clears flag", opts.mbe_out, 0);
    rc |= expect_true("sdrtrunk mbe close clears handle", opts.mbe_out_f == NULL);
    rc |= expect_true("sdrtrunk mbe filename suffix", has_suffix(opts.mbe_out_file, "_S1.amb"));
    rc |= expect_int("sdrtrunk mbe resets tgcount", state.tgcount, 0);
    rc |= expect_int("sdrtrunk mbe resets tg table", state.tg[2][1], 0);

    char header[8];
    if (read_file_prefix(opts.mbe_out_path, header, sizeof header) != 0) {
        rc = 1;
    } else {
        rc |= expect_true("sdrtrunk mbe header", strcmp(header, ".amb") == 0);
    }

    (void)remove(opts.mbe_out_path);
    (void)remove_dir(dir);
    return rc;
}

static int
test_sdrtrunk_json_hex_voice_writes_unencrypted_mbe_records(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] =
        "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\",\"encrypted\":\"false\","
        "\"hex\":\"000000000000000000\"}";
    unsigned char bytes[8] = {0};
    size_t got = 0;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    opts.floating_point = 1;
    state.event_history_s = history;

    FILE* out = tmpfile();
    if (!out) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.mbe_out_f = out;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    fflush(out);
    if (fseek(out, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek(sdrtrunk hex output) failed\n");
        rc = 1;
    }
    clearerr(out);
    got = fread(bytes, 1, sizeof bytes, out);
    if (ferror(out)) {
        DSD_FPRINTF(stderr, "fread(sdrtrunk hex output) failed\n");
        rc = 1;
    }
    rc |= expect_u16("sdrtrunk hex ambe record size", (uint16_t)got, 8);
    rc |= expect_int("sdrtrunk hex dmr synctype", state.synctype, DSD_SYNC_DMR_BS_DATA_POS);
    rc |= expect_int("sdrtrunk hex dmr slot", state.currentslot, 0);

    fclose(out);
    opts.mbe_out_f = NULL;
    return rc;
}

static int
test_sdrtrunk_json_hex_voice_blocks_encrypted_without_keystream(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    static const char json[] =
        "{\"version\":\"2\",\"protocol\":\"APCO25-PHASE1\",\"call_type\":\"GROUP\",\"encrypted\":\"true\","
        "\"encryption_algorithm\":\"129\",\"encryption_key_id\":\"7\",\"encryption_mi\":\"0011223344556677\","
        "\"hex\":\"000000000000000000000000000000000000\"}";
    unsigned char byte = 0xFF;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    opts.floating_point = 1;
    state.event_history_s = history;

    FILE* out = tmpfile();
    if (!out) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.mbe_out_f = out;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    fflush(out);
    if (fseek(out, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek(sdrtrunk encrypted output) failed\n");
        rc = 1;
    }
    clearerr(out);
    int empty_check = fgetc(out);
    if (empty_check == EOF && ferror(out)) {
        DSD_FPRINTF(stderr, "fgetc(sdrtrunk encrypted output) failed\n");
        rc = 1;
    }
    rc |= expect_int("sdrtrunk encrypted hex writes no mbe record", empty_check, EOF);
    rc |= expect_byte("sdrtrunk encrypted hex sentinel unchanged", byte, 0xFF);
    rc |= expect_int("sdrtrunk encrypted hex p25 synctype", state.synctype, DSD_SYNC_P25P1_POS);
    rc |= expect_int("sdrtrunk encrypted hex algid", state.payload_algid, 0x81);
    rc |= expect_u16("sdrtrunk encrypted hex key id", state.payload_keyid, 7);
    rc |= expect_u64("sdrtrunk encrypted hex mi", state.payload_mi, 0x0011223344556677ULL);

    fclose(out);
    opts.mbe_out_f = NULL;
    return rc;
}

static int
run_sdrtrunk_voice_record_case(const char* tag, const char* json, dsd_state* state, size_t min_record_bytes) {
    int rc = 0;
    static dsd_opts opts;
    static Event_History_I history[2];

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(history, 0, sizeof history);
    opts.playfiles = 1;
    opts.floating_point = 1;
    opts.slot1_on = 1;
    state->event_history_s = history;

    FILE* out = tmpfile();
    if (!out) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.mbe_out_f = out;

    rc |= run_sdrtrunk_json(json, &opts, state);
    rc |= expect_int(tag, fflush(out), 0);
    if (fseek(out, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek(%s) failed\n", tag);
        rc = 1;
    }
    clearerr(out);
    int first = fgetc(out);
    if (first == EOF && ferror(out)) {
        DSD_FPRINTF(stderr, "fgetc(%s) failed\n", tag);
        rc = 1;
    }
    rc |= expect_true(tag, first != EOF);
    if (first != EOF) {
        size_t bytes = 1;
        while (fgetc(out) != EOF) {
            bytes++;
        }
        if (ferror(out)) {
            DSD_FPRINTF(stderr, "fgetc(%s) failed\n", tag);
            rc = 1;
        }
        rc |= expect_true(tag, bytes >= min_record_bytes);
    }

    fclose(out);
    opts.mbe_out_f = NULL;
    return rc;
}

static int
test_sdrtrunk_json_encrypted_keystreams_write_voice_records(void) {
    int rc = 0;

    struct {
        const char* tag;
        const char* json;
        unsigned long long r;
        unsigned long long k1;
        unsigned long long k2;
        unsigned long long k3;
        unsigned long long k4;
        int forced_m;
        unsigned long long forced_k;
        int want_algid;
        size_t min_record_bytes;
    } cases[] = {
        {"sdrtrunk p25 rc4 record",
         "{\"version\":\"1\",\"protocol\":\"APCO25-PHASE1\",\"encrypted\":\"true\","
         "\"encryption_algorithm\":\"170\",\"encryption_key_id\":\"7\",\"encryption_mi\":\"0011223344556677\","
         "\"hex\":\"000000000000000000000000000000000000\"}",
         0x0102030405ULL, 0, 0, 0, 0, 0, 0, 0xAA, 12},
        {"sdrtrunk p25 des record",
         "{\"version\":\"1\",\"protocol\":\"APCO25-PHASE1\",\"encrypted\":\"true\","
         "\"encryption_algorithm\":\"129\",\"encryption_key_id\":\"7\",\"encryption_mi\":\"0011223344556677\","
         "\"hex\":\"000000000000000000000000000000000000\"}",
         0x0102030405ULL, 0, 0, 0, 0, 0, 0, 0x81, 12},
        {"sdrtrunk p25 aes record",
         "{\"version\":\"1\",\"protocol\":\"APCO25-PHASE1\",\"encrypted\":\"true\","
         "\"encryption_algorithm\":\"137\",\"encryption_key_id\":\"7\",\"encryption_mi\":\"0011223344556677\","
         "\"hex\":\"000000000000000000000000000000000000\"}",
         0, 0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL, 0x1021324354657687ULL, 0x98A9BACBDCEDFE0FULL, 0, 0, 0x89, 12},
        {"sdrtrunk dmr forced bp record",
         "{\"version\":\"2\",\"protocol\":\"DMR\",\"encrypted\":\"false\",\"hex\":\"000000000000000000\"}", 0, 0, 0, 0,
         0, 1, 42, 0, 8},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static dsd_state state;
        DSD_MEMSET(&state, 0, sizeof state);
        state.R = cases[i].r;
        state.K1 = cases[i].k1;
        state.K2 = cases[i].k2;
        state.K3 = cases[i].k3;
        state.K4 = cases[i].k4;
        state.M = cases[i].forced_m;
        state.K = cases[i].forced_k;

        rc |= run_sdrtrunk_voice_record_case(cases[i].tag, cases[i].json, &state, cases[i].min_record_bytes);
        if (cases[i].want_algid != 0) {
            rc |= expect_int(cases[i].tag, state.payload_algid, cases[i].want_algid);
        }
        dsd_state_ext_free_all(&state);
    }

    return rc;
}

static uint8_t
bits_to_u4_msb(const unsigned char bits4[4]) {
    uint8_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (uint8_t)((v << 1U) | (bits4[i] & 1U));
    }
    return (uint8_t)(v & 0xFU);
}

static void
fill_sdrtrunk_dmr_le_fragments(dsd_state* state, uint32_t mi32) {
    uint8_t mi_bits32[32];
    for (int i = 0; i < 32; i++) {
        mi_bits32[i] = (uint8_t)((mi32 >> (31 - i)) & 1U);
    }
    const uint8_t crc = dsd_dmr_crc4(mi_bits32, 32);

    unsigned char msg36[36];
    unsigned char go36[36];
    DSD_MEMSET(msg36, 0, sizeof msg36);
    DSD_MEMSET(go36, 0, sizeof go36);
    for (int i = 0; i < 32; i++) {
        msg36[i] = (unsigned char)(mi_bits32[i] & 1U);
    }
    for (int i = 0; i < 4; i++) {
        msg36[32 + i] = (unsigned char)((crc >> (3 - i)) & 1U);
    }

    for (int chunk = 0; chunk < 3; chunk++) {
        unsigned char orig[12];
        unsigned char enc[24];
        DSD_MEMSET(orig, 0, sizeof orig);
        DSD_MEMSET(enc, 0, sizeof enc);
        for (int i = 0; i < 12; i++) {
            orig[i] = (unsigned char)(msg36[chunk * 12 + i] & 1U);
        }
        Golay_24_12_encode(orig, enc);
        for (int i = 0; i < 12; i++) {
            go36[chunk * 12 + i] = (unsigned char)(enc[12 + i] & 1U);
        }
    }

    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            const int bit_base = col * 12 + row * 4;
            state->late_entry_mi_fragment[0][1 + row][col] = bits_to_u4_msb(&msg36[bit_base]);
            state->late_entry_mi_fragment[0][4 + row][col] = bits_to_u4_msb(&go36[bit_base]);
        }
    }
}

static uint8_t
hex_char_to_nibble(char c) {
    const int parsed = dsd_hex_nibble_value((unsigned char)c);
    return (parsed < 0) ? 0U : (uint8_t)parsed;
}

static void
set_sdrtrunk_dmr_hex_nibble_bit(char hex[19], int row, int col, uint8_t bit) {
    if ((bit & 1U) == 0U) {
        return;
    }
    for (int map_idx = 0; map_idx < DSD_AMBE_2450_DIBITS; map_idx++) {
        const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[map_idx];
        if (map->low_row == row && map->low_col == col) {
            const int hex_idx = map_idx / 2;
            const uint8_t mask = (map_idx % 2 == 0) ? 0x4U : 0x1U;
            const uint8_t nibble = hex_char_to_nibble(hex[hex_idx]);
            static const char lut[] = "0123456789ABCDEF";
            hex[hex_idx] = lut[(nibble | mask) & 0xFU];
            return;
        }
    }
}

static void
build_sdrtrunk_dmr_late_entry_hex(uint8_t nibble, char hex[19]) {
    DSD_MEMSET(hex, '0', 18);
    hex[18] = '\0';
    for (int bit_idx = 0; bit_idx < 4; bit_idx++) {
        set_sdrtrunk_dmr_hex_nibble_bit(hex, 3, bit_idx, (uint8_t)((nibble >> (3 - bit_idx)) & 1U));
    }
}

static int
test_sdrtrunk_json_dmr_late_entry_updates_mi(void) {
    int rc = 0;
    const uint32_t mi = 0xA1B2C3D4U;
    static dsd_state encoded;
    static dsd_state state;
    static dsd_opts opts;
    static Event_History_I history[2];

    DSD_MEMSET(&encoded, 0, sizeof encoded);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(history, 0, sizeof history);
    fill_sdrtrunk_dmr_le_fragments(&encoded, mi);

    char json[768];
    size_t used = 0;
    used += (size_t)DSD_SNPRINTF(json + used, sizeof(json) - used,
                                 "{\"version\":\"2\",\"protocol\":\"DMR\",\"encrypted\":\"true\",");
    for (uint8_t vc = 1; vc <= 6; vc++) {
        for (uint8_t col = 0; col < 3; col++) {
            char hex[19];
            build_sdrtrunk_dmr_late_entry_hex((uint8_t)encoded.late_entry_mi_fragment[0][vc][col], hex);
            used += (size_t)DSD_SNPRINTF(json + used, sizeof(json) - used, "\"hex\":\"%s\",", hex);
        }
    }
    rc |= expect_true("sdrtrunk dmr late entry json buffer", used + 2 < sizeof json);
    json[used - 1] = '}';
    json[used] = '\0';

    opts.playfiles = 1;
    opts.floating_point = 1;
    state.event_history_s = history;
    state.M = 0x21;
    state.R = 0x0102030405ULL;

    rc |= run_sdrtrunk_json(json, &opts, &state);
    rc |= expect_int("sdrtrunk dmr late entry synctype", state.synctype, DSD_SYNC_DMR_BS_DATA_POS);
    rc |= expect_int("sdrtrunk dmr late entry algid", state.payload_algid, 0x21);
    rc |= expect_u64("sdrtrunk dmr late entry mi", state.payload_mi, 0xDAB4A1A7ULL);
    return rc;
}

static int
test_open_mbe_out_file_creates_slot_files_and_closes(void) {
    int rc = 0;

    struct {
        int synctype;
        const char* suffix;
        const char* header;
    } slot1_cases[] = {
        {DSD_SYNC_P25P1_POS, "_S1.imb", ".imb"},
        {DSD_SYNC_DSTAR_VOICE_POS, "_S1.dmb", ".dmb"},
        {DSD_SYNC_DMR_BS_VOICE_POS, "_S1.amb", ".amb"},
    };

    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_mbe_out")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    for (size_t i = 0; i < sizeof slot1_cases / sizeof slot1_cases[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);
        DSD_SNPRINTF(opts.mbe_out_dir, sizeof opts.mbe_out_dir, "%s%c", dir, dsd_test_path_sep());
        state.synctype = slot1_cases[i].synctype;
        state.tgcount = 7;
        state.tg[3][2] = 9;

        openMbeOutFile(&opts, &state);
        rc |= expect_int("slot1 open flag", opts.mbe_out, 1);
        rc |= expect_true("slot1 file handle opened", opts.mbe_out_f != NULL);
        rc |= expect_true("slot1 filename suffix", has_suffix(opts.mbe_out_file, slot1_cases[i].suffix));
        rc |= expect_true("slot1 path contains filename", strstr(opts.mbe_out_path, opts.mbe_out_file) != NULL);
        rc |= expect_int("slot1 tgcount reset", state.tgcount, 0);
        rc |= expect_int("slot1 tg table reset", state.tg[3][2], 0);

        closeMbeOutFile(&opts, &state);
        rc |= expect_int("slot1 close clears flag", opts.mbe_out, 0);
        rc |= expect_true("slot1 close clears handle", opts.mbe_out_f == NULL);

        char header[8];
        if (read_file_prefix(opts.mbe_out_path, header, sizeof header) != 0) {
            rc = 1;
        } else {
            rc |= expect_true("slot1 header", strcmp(header, slot1_cases[i].header) == 0);
        }
        (void)remove(opts.mbe_out_path);
    }

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof opts.mbe_out_dir, "%s%c", dir, dsd_test_path_sep());
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.tgcount = 5;
    state.tg[4][3] = 8;

    openMbeOutFileR(&opts, &state);
    rc |= expect_int("slot2 open flag", opts.mbe_outR, 1);
    rc |= expect_true("slot2 file handle opened", opts.mbe_out_fR != NULL);
    rc |= expect_true("slot2 filename suffix", has_suffix(opts.mbe_out_fileR, "_S2.amb"));
    rc |= expect_true("slot2 path contains filename", strstr(opts.mbe_out_path, opts.mbe_out_fileR) != NULL);
    rc |= expect_int("slot2 tgcount reset", state.tgcount, 0);
    rc |= expect_int("slot2 tg table reset", state.tg[4][3], 0);

    closeMbeOutFileR(&opts, &state);
    rc |= expect_int("slot2 close clears flag", opts.mbe_outR, 0);
    rc |= expect_true("slot2 close clears handle", opts.mbe_out_fR == NULL);

    char header[8];
    if (read_file_prefix(opts.mbe_out_path, header, sizeof header) != 0) {
        rc = 1;
    } else {
        rc |= expect_true("slot2 header", strcmp(header, ".amb") == 0);
    }
    (void)remove(opts.mbe_out_path);
    (void)remove_dir(dir);
    return rc;
}

static int
test_truncated_reads_fail(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    char bits[88] = {0};

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    FILE* empty = tmpfile();
    FILE* short_imbe = tmpfile();
    FILE* short_ambe = tmpfile();
    if (!empty || !short_imbe || !short_ambe) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        if (empty) {
            fclose(empty);
        }
        if (short_imbe) {
            fclose(short_imbe);
        }
        if (short_ambe) {
            fclose(short_ambe);
        }
        return 1;
    }

    opts.mbe_in_f = empty;
    rc |= expect_int("empty-imbe", readImbe4400Data(&opts, &state, bits), 1);

    fputc(0x22, short_imbe);
    fputc(0xAA, short_imbe);
    rewind(short_imbe);
    opts.mbe_in_f = short_imbe;
    rc |= expect_int("short-imbe", readImbe4400Data(&opts, &state, bits), 1);

    fputc(0x33, short_ambe);
    for (int i = 0; i < 6; i++) {
        fputc(0xAA, short_ambe);
    }
    rewind(short_ambe);
    opts.mbe_in_f = short_ambe;
    rc |= expect_int("short-ambe-tail", readAmbe2450Data(&opts, &state, bits), 1);

    fclose(empty);
    fclose(short_imbe);
    fclose(short_ambe);
    opts.mbe_in_f = NULL;
    return rc;
}

static int
write_cookie_file(char* path, size_t path_size, const char* prefix, const char cookie[4]) {
    int fd = dsd_test_mkstemp(path, path_size, prefix);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    FILE* f = fdopen(fd, "wb");
    if (!f) {
        DSD_FPRINTF(stderr, "fdopen failed: %s\n", strerror(errno));
        (void)dsd_close(fd);
        (void)remove(path);
        return 1;
    }
    if (fwrite(cookie, 1, 4, f) != 4) {
        DSD_FPRINTF(stderr, "fwrite(%s) failed\n", path);
        fclose(f);
        (void)remove(path);
        return 1;
    }
    fclose(f);
    return 0;
}

static int
write_short_cookie_file(char* path, size_t path_size, const char* prefix) {
    int fd = dsd_test_mkstemp(path, path_size, prefix);
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    FILE* f = fdopen(fd, "wb");
    if (!f) {
        DSD_FPRINTF(stderr, "fdopen failed: %s\n", strerror(errno));
        (void)dsd_close(fd);
        (void)remove(path);
        return 1;
    }
    if (fwrite(".a", 1, 2, f) != 2) {
        DSD_FPRINTF(stderr, "fwrite(%s) failed\n", path);
        fclose(f);
        (void)remove(path);
        return 1;
    }
    fclose(f);
    return 0;
}

static int
test_open_mbe_in_file_classifies_cookies(void) {
    int rc = 0;

    struct {
        const char* prefix;
        char cookie[4];
        int want_type;
    } cases[] = {
        {"mbe_in_amb", {'.', 'a', 'm', 'b'}, 1},
        {"mbe_in_imb", {'.', 'i', 'm', 'b'}, 0},
        {"mbe_in_dmb", {'.', 'd', 'm', 'b'}, 2},
        {"mbe_in_bad", {'n', 'o', 'p', 'e'}, -1},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char path[DSD_TEST_PATH_MAX];
        static dsd_opts opts;
        static dsd_state state;

        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&state, 0, sizeof state);

        if (write_cookie_file(path, sizeof path, cases[i].prefix, cases[i].cookie) != 0) {
            return 1;
        }
        DSD_SNPRINTF(opts.mbe_in_file, sizeof opts.mbe_in_file, "%s", path);

        openMbeInFile(&opts, &state);
        rc |= expect_int(cases[i].prefix, state.mbe_file_type, cases[i].want_type);
        if (cases[i].want_type < 0) {
            rc |= expect_true("mbe bad cookie closes handle", opts.mbe_in_f == NULL);
        }

        if (opts.mbe_in_f) {
            fclose(opts.mbe_in_f);
            opts.mbe_in_f = NULL;
        }
        (void)remove(path);
    }

    return rc;
}

static int
test_open_mbe_in_file_accepts_sdrtrunk_extension(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    char path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;

    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_sdrtrunk_input")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    if (dsd_test_path_join(path, sizeof path, dir, "call.mbe") != 0) {
        (void)remove_dir(dir);
        return 1;
    }
    FILE* f = dsd_fopen_private(path, "wb");
    if (!f) {
        DSD_FPRINTF(stderr, "dsd_fopen_private(%s) failed: %s\n", path, strerror(errno));
        (void)remove_dir(dir);
        return 1;
    }
    if (fwrite("{}\n", 1, 3, f) != 3) {
        DSD_FPRINTF(stderr, "fwrite(%s) failed\n", path);
        fclose(f);
        (void)remove(path);
        (void)remove_dir(dir);
        return 1;
    }
    fclose(f);

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.mbe_in_file, sizeof opts.mbe_in_file, "%s", path);

    openMbeInFile(&opts, &state);
    rc |= expect_int("sdrtrunk mbe extension", state.mbe_file_type, 3);
    rc |= expect_true("sdrtrunk mbe handle remains open", opts.mbe_in_f != NULL);

    if (opts.mbe_in_f) {
        fclose(opts.mbe_in_f);
        opts.mbe_in_f = NULL;
    }
    (void)remove(path);
    (void)remove_dir(dir);
    return rc;
}

static int
test_open_mbe_in_file_rejects_short_cookie_without_handle(void) {
    int rc = 0;
    char path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    state.mbe_file_type = 99;

    if (write_short_cookie_file(path, sizeof path, "mbe_in_short") != 0) {
        return 1;
    }
    DSD_SNPRINTF(opts.mbe_in_file, sizeof opts.mbe_in_file, "%s", path);

    openMbeInFile(&opts, &state);
    rc |= expect_int("mbe short cookie rejected", state.mbe_file_type, -1);
    rc |= expect_true("mbe short cookie closes handle", opts.mbe_in_f == NULL);

    if (opts.mbe_in_f) {
        fclose(opts.mbe_in_f);
        opts.mbe_in_f = NULL;
    }
    (void)remove(path);
    return rc;
}

static int
test_symbol_capture_open_writes_expected_headers(void) {
    static const unsigned char soft_header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE] = {
        'D', 'S', 'D', 'N', 'S', 'Y', 'M', '2', 2, DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE, 0, 0, 0, 0, 0, 0,
    };
    int rc = 0;

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "symbol_soft");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(path);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.symbol_out_file, sizeof opts.symbol_out_file, "%s", path);

    openSymbolOutFile(&opts, &state);
    rc |= expect_true("symbol file handle opened", opts.symbol_out_f != NULL);
    closeSymbolOutFile(&opts, &state);
    rc |= expect_true("symbol close clears handle", opts.symbol_out_f == NULL);

    unsigned char bytes[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE];
    size_t got = 0;
    if (read_file_exact(path, bytes, sizeof bytes, &got) != 0) {
        rc = 1;
    } else {
        rc |= expect_int("symbol file size", (int)got, (int)sizeof soft_header);
        rc |= expect_true("symbol soft header bytes", memcmp(bytes, soft_header, sizeof soft_header) == 0);
    }
    (void)remove(path);

    return rc;
}

static int
test_symbol_capture_auto_rotation_reopens_and_logs_event(void) {
    static const unsigned char soft_header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE] = {
        'D', 'S', 'D', 'N', 'S', 'Y', 'M', '2', 2, DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE, 0, 0, 0, 0, 0, 0,
    };
    int rc = 0;
    char cwd[DSD_TEST_PATH_MAX];
    char dir[DSD_TEST_PATH_MAX];
    char old_path[DSD_TEST_PATH_MAX];
    char new_path[DSD_TEST_PATH_MAX];

    if (!test_getcwd(cwd, sizeof cwd)) {
        DSD_FPRINTF(stderr, "getcwd failed: %s\n", strerror(errno));
        return 1;
    }
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_symbol_rotate")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    if (dsd_test_path_join(old_path, sizeof old_path, dir, "old_symbol_capture.bin") != 0) {
        (void)remove_dir(dir);
        return 1;
    }

    // Seed an auto-rotated soft symbol capture with a known old filename and event history.
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I history[2];
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(history, 0, sizeof history);
    state.event_history_s = history;
    DSD_SNPRINTF(opts.symbol_out_file, sizeof opts.symbol_out_file, "%s", old_path);
    opts.symbol_out_file_is_auto = 1;
    opts.symbol_out_file_creation_time = time(NULL) - 4000;
    const dsd_call_observation active_observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 4321U,
        .policy_target_id = 4321U,
        .ota_source_id = 1234U,
    };
    rc |= expect_true("rotation seeds active canonical call",
                      dsd_call_state_observe(&state, &active_observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_call_snapshot active_before = {0};
    rc |= expect_true("rotation canonical call available before notice",
                      dsd_call_state_get(&state, 0U, &active_before) > 0);

    openSymbolOutFile(&opts, &state);
    rc |= expect_true("rotation source handle opened", opts.symbol_out_f != NULL);
    if (opts.symbol_out_f) {
        rc |= expect_true("rotation old file marker written", fputc(0xA5, opts.symbol_out_f) != EOF);
        rc |= expect_int("rotation old file flushed", fflush(opts.symbol_out_f), 0);
    }

    // Rotate from inside the capture directory so generated relative paths can be checked.
    time_t before = time(NULL);
    if (test_chdir(dir) != 0) {
        DSD_FPRINTF(stderr, "chdir(%s) failed: %s\n", dir, strerror(errno));
        closeSymbolOutFile(&opts, &state);
        (void)remove(old_path);
        (void)remove_dir(dir);
        return 1;
    }
    rotate_symbol_out_file(&opts, &state);
    time_t after = time(NULL);
    if (test_chdir(cwd) != 0) {
        DSD_FPRINTF(stderr, "chdir(%s) restore failed: %s\n", cwd, strerror(errno));
        closeSymbolOutFile(&opts, &state);
        return 1;
    }

    rc |= expect_true("rotation keeps handle open", opts.symbol_out_f != NULL);
    rc |= expect_true("rotation generated capture name", has_suffix(opts.symbol_out_file, "_dibit_capture.bin"));
    rc |= expect_true("rotation updates creation time", opts.symbol_out_file_creation_time >= before);
    rc |= expect_true("rotation creation time bounded", opts.symbol_out_file_creation_time <= after + 1);
    dsd_call_snapshot active_after = {0};
    rc |= expect_true("rotation canonical call available after notice",
                      dsd_call_state_get(&state, 0U, &active_after) > 0);
    rc |= expect_int("rotation preserves active call phase", (int)active_after.phase, DSD_CALL_PHASE_ACTIVE);
    rc |= expect_u64("rotation preserves active call epoch", active_after.epoch, active_before.epoch);
    rc |= expect_u64("rotation preserves active call source", active_after.ota_source_id, 1234U);

    // The user-visible event should name the generated capture without being attributed to radio data.
    Event_History* rotated = &state.event_history_s[0].Event_History_Items[1];
    rc |= expect_int("rotation event category", rotated->category, DSD_EVENT_CATEGORY_SYSTEM);
    rc |= expect_int("rotation event source", (int)rotated->source_id, 0);
    rc |= expect_int("rotation event target", (int)rotated->target_id, 0);
    rc |= expect_true("rotation event string", strstr(rotated->event_string, "Dibit Capture File Rotated") != NULL);
    rc |= expect_true("rotation event names capture", strstr(rotated->event_string, opts.symbol_out_file) != NULL);

    closeSymbolOutFile(&opts, &state);
    rc |= expect_true("rotation close clears handle", opts.symbol_out_f == NULL);

    // Both old and new files should retain soft-capture headers; the old file keeps its marker.
    unsigned char old_bytes[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE + 1];
    size_t got = 0;
    if (read_file_exact(old_path, old_bytes, sizeof old_bytes, &got) != 0) {
        rc = 1;
    } else {
        rc |= expect_int("rotation old file size", (int)got, (int)sizeof old_bytes);
        rc |= expect_true("rotation old file header", memcmp(old_bytes, soft_header, sizeof soft_header) == 0);
        rc |= expect_byte("rotation old file marker", old_bytes[sizeof soft_header], 0xA5);
    }

    if (dsd_test_path_join(new_path, sizeof new_path, dir, opts.symbol_out_file) == 0) {
        unsigned char new_bytes[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE];
        got = 0;
        if (read_file_exact(new_path, new_bytes, sizeof new_bytes, &got) != 0) {
            rc = 1;
        } else {
            rc |= expect_int("rotation new file size", (int)got, (int)sizeof new_bytes);
            rc |= expect_true("rotation new file header", memcmp(new_bytes, soft_header, sizeof soft_header) == 0);
        }
        (void)remove(new_path);
    } else {
        rc = 1;
    }

    (void)remove(old_path);
    (void)remove_dir(dir);
    dsd_state_ext_free_all(&state);
    return rc;
}

static int
test_wav_output_helpers_create_temp_and_raw_files(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_wav_out")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    rc |= expect_true("open wav null filename rejects", open_wav_file(dir, NULL, 64, 8000, 0) == NULL);
    char too_small[8] = {0};
    rc |= expect_true("open wav short filename rejects",
                      open_wav_file(dir, too_small, sizeof too_small, 8000, 0) == NULL);

    struct {
        uint8_t ext;
        int wants_suffix;
    } cases[] = {
        {0, 0},
        {1, 1},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char path[DSD_TEST_PATH_MAX];
        DSD_MEMSET(path, 0, sizeof path);
        SNDFILE* wav = open_wav_file(dir, path, sizeof path, 8000, cases[i].ext);
        rc |= expect_true("open wav returns handle", wav != NULL);
        rc |= expect_true("open wav writes temp path", strstr(path, "TEMP_") != NULL);
        rc |= expect_true("open wav path in dir", strstr(path, dir) == path);
        rc |= expect_int("open wav suffix policy", has_suffix(path, ".wav"), cases[i].wants_suffix);
        if (wav) {
            wav = close_wav_file(wav);
            rc |= expect_true("close wav returns null", wav == NULL);
            rc |= expect_wav_header("open wav RIFF/WAVE header", path);
            rc |= expect_true("open wav header-only size", file_size_or_negative(path) >= 44);
        }
        (void)remove(path);
    }

    char raw_path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(raw_path, sizeof raw_path, "raw_wav");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        (void)remove_dir(dir);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(raw_path);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.wav_out_file_raw, sizeof opts.wav_out_file_raw, "%s", raw_path);

    openWavOutFileRaw(&opts, &state);
    rc |= expect_true("raw wav handle opened", opts.wav_out_raw != NULL);
    if (opts.wav_out_raw) {
        opts.wav_out_raw = close_wav_file(opts.wav_out_raw);
        rc |= expect_true("raw wav close clears handle", opts.wav_out_raw == NULL);
        rc |= expect_wav_header("raw wav RIFF/WAVE header", raw_path);
        rc |= expect_true("raw wav header-only size", file_size_or_negative(raw_path) >= 44);
    }
    (void)remove(raw_path);
    (void)remove_dir(dir);
    return rc;
}

static int
test_close_and_rename_wav_removes_header_only_files(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_wav_close")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    rc |= expect_true("close rename null filename rejects",
                      close_and_rename_wav_file(NULL, NULL, NULL, dir, NULL) == NULL);
    rc |= expect_true("close rename empty filename rejects",
                      close_and_rename_wav_file(NULL, NULL, "", dir, NULL) == NULL);

    char path[DSD_TEST_PATH_MAX];
    DSD_MEMSET(path, 0, sizeof path);
    SNDFILE* wav = open_wav_file(dir, path, sizeof path, 8000, 1);
    rc |= expect_true("close rename source wav opened", wav != NULL);
    if (wav) {
        rc |= expect_true("close rename header-only returns null",
                          close_and_rename_wav_file(wav, NULL, path, dir, NULL) == NULL);
        rc |= expect_true("close rename header-only removed source", file_size_or_negative(path) < 0);
    }

    (void)remove(path);
    (void)remove_dir(dir);
    return rc;
}

static int
test_close_and_rename_wav_preserves_nonempty_event_file(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_wav_rename")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char path[DSD_TEST_PATH_MAX];
    DSD_MEMSET(path, 0, sizeof path);
    SNDFILE* wav = open_wav_file(dir, path, sizeof path, 8000, 0);
    rc |= expect_true("close rename nonempty source wav opened", wav != NULL);
    if (wav) {
        rc |= expect_true("close rename wrote pcm samples", write_wav_test_samples(wav));

        Event_History_I history;
        DSD_MEMSET(&history, 0, sizeof history);
        Event_History* item = &history.Event_History_Items[0];
        item->event_time = (time_t)1700000000;
        item->gi = 1;
        item->source_id = 98765U;
        item->target_id = 12345U;
        DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", "SYS-A");
        DSD_SNPRINTF(item->src_str, sizeof item->src_str, "%s", "SRCUNIT");
        DSD_SNPRINTF(item->tgt_str, sizeof item->tgt_str, "%s", "TGTUNIT");

        rc |= expect_true("close rename nonempty returns null",
                          close_and_rename_wav_file(wav, NULL, path, dir, &history) == NULL);
        rc |= expect_true("close rename nonempty removed temp source", file_size_or_negative(path) < 0);

        char renamed[DSD_TEST_PATH_MAX];
        rc |= expect_true("close rename nonempty creates event filename",
                          find_wav_rename_output_for_string_event(renamed, sizeof renamed, dir, item));
        if (renamed[0] != '\0') {
            rc |= expect_true("close rename filename has system", strstr(renamed, "SYS-A") != NULL);
            rc |= expect_true("close rename filename has private tag", strstr(renamed, "_PRIVATE_") != NULL);
            rc |= expect_true("close rename filename has target string", strstr(renamed, "TGT_TGTUNIT") != NULL);
            rc |= expect_true("close rename filename has source string", strstr(renamed, "SRC_SRCUNIT") != NULL);
            rc |= expect_wav_header("close rename final RIFF/WAVE header", renamed);
            rc |= expect_true("close rename final has audio data", file_size_or_negative(renamed) > 44);
            (void)remove(renamed);
        }
    }

    (void)remove(path);
    (void)remove_dir(dir);
    return rc;
}

static int
test_close_and_rename_wav_numeric_and_failure_paths(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_wav_numeric")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char path[DSD_TEST_PATH_MAX];
    DSD_MEMSET(path, 0, sizeof path);
    SNDFILE* wav = open_wav_file(dir, path, sizeof path, 8000, 0);
    rc |= expect_true("numeric rename source wav opened", wav != NULL);
    if (wav) {
        rc |= expect_true("numeric rename wrote pcm samples", write_wav_test_samples(wav));

        Event_History_I history;
        DSD_MEMSET(&history, 0, sizeof history);
        Event_History* item = &history.Event_History_Items[0];
        item->event_time = (time_t)1700001000;
        item->gi = 0;
        item->source_id = 222U;
        item->target_id = 333U;
        DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", "SYS-N");

        rc |= expect_true("numeric rename returns null",
                          close_and_rename_wav_file(wav, NULL, path, dir, &history) == NULL);
        rc |= expect_true("numeric rename removed temp source", file_size_or_negative(path) < 0);

        char renamed[DSD_TEST_PATH_MAX];
        rc |= expect_true("numeric rename creates event filename",
                          find_wav_rename_output_for_numeric_event(renamed, sizeof renamed, dir, item));
        if (renamed[0] != '\0') {
            rc |= expect_true("numeric rename filename has system", strstr(renamed, "SYS-N") != NULL);
            rc |= expect_true("numeric rename filename has group tag", strstr(renamed, "_GROUP_") != NULL);
            rc |= expect_true("numeric rename filename has target id", strstr(renamed, "TGT_333") != NULL);
            rc |= expect_true("numeric rename filename has source id", strstr(renamed, "SRC_222") != NULL);
            rc |= expect_wav_header("numeric rename final RIFF/WAVE header", renamed);
            rc |= expect_true("numeric rename final has audio data", file_size_or_negative(renamed) > 44);
            (void)remove(renamed);
        }
    }

    char fail_path[DSD_TEST_PATH_MAX];
    DSD_MEMSET(fail_path, 0, sizeof fail_path);
    wav = open_wav_file(dir, fail_path, sizeof fail_path, 8000, 0);
    rc |= expect_true("rename failure source wav opened", wav != NULL);
    if (wav) {
        rc |= expect_true("rename failure wrote pcm samples", write_wav_test_samples(wav));
        char missing_dir[DSD_TEST_PATH_MAX];
        DSD_SNPRINTF(missing_dir, sizeof missing_dir, "%s%cmissing", dir, dsd_test_path_sep());
        rc |= expect_true("rename failure returns null",
                          close_and_rename_wav_file(wav, NULL, fail_path, missing_dir, NULL) == NULL);
        rc |= expect_true("rename failure keeps original temp wav", file_size_or_negative(fail_path) > 44);
    }

    (void)remove(path);
    (void)remove(fail_path);
    (void)remove_dir(dir);
    return rc;
}

static int
test_close_and_rename_wav_exports_rdio_sidecar(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_wav_rdio")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char path[DSD_TEST_PATH_MAX];
    DSD_MEMSET(path, 0, sizeof path);
    SNDFILE* wav = open_wav_file(dir, path, sizeof path, 8000, 0);
    rc |= expect_true("rdio rename source wav opened", wav != NULL);
    if (wav) {
        rc |= expect_true("rdio rename wrote pcm samples", write_wav_test_samples(wav));

        static dsd_opts opts;
        Event_History_I history;
        DSD_MEMSET(&opts, 0, sizeof opts);
        DSD_MEMSET(&history, 0, sizeof history);
        opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        opts.rdio_system_id = 48;
        opts.rdio_upload_timeout_ms = 5000;
        opts.rdio_upload_retries = 1;

        Event_History* item = &history.Event_History_Items[0];
        item->event_time = (time_t)1700002000;
        item->gi = 0;
        item->source_id = 660045U;
        item->target_id = 1201U;
        item->channel = 851012500U;
        item->enc = 1;
        DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", "P25_TEST");
        DSD_SNPRINTF(item->t_name, sizeof item->t_name, "%s", "FIRE DISP");

        rc |=
            expect_true("rdio rename returns null", close_and_rename_wav_file(wav, &opts, path, dir, &history) == NULL);
        rc |= expect_true("rdio rename removed temp source", file_size_or_negative(path) < 0);

        char renamed[DSD_TEST_PATH_MAX];
        rc |= expect_true("rdio rename creates event filename",
                          find_wav_rename_output_for_numeric_event(renamed, sizeof renamed, dir, item));
        if (renamed[0] != '\0') {
            char sidecar[DSD_TEST_PATH_MAX];
            char body[4096];
            if (make_rdio_sidecar_path(renamed, sidecar, sizeof sidecar) != 0
                || read_file_prefix(sidecar, body, sizeof body) != 0) {
                rc = 1;
            } else {
                rc |= expect_true("rdio sidecar start time", strstr(body, "\"start_time\": 1700002000") != NULL);
                rc |= expect_true("rdio sidecar talkgroup", strstr(body, "\"talkgroup\": 1201") != NULL);
                rc |=
                    expect_true("rdio sidecar talkgroup tag", strstr(body, "\"talkgroup_tag\": \"FIRE DISP\"") != NULL);
                rc |= expect_true("rdio sidecar source",
                                  strstr(body, "\"srcList\": [{\"pos\":0,\"src\":660045}]") != NULL);
                rc |= expect_true("rdio sidecar frequency", strstr(body, "\"freq\": 851012500") != NULL);
                rc |= expect_true("rdio sidecar system", strstr(body, "\"system\": 48") != NULL);
                rc |= expect_true("rdio sidecar short name", strstr(body, "\"short_name\": \"P25_TEST\"") != NULL);
                rc |= expect_true("rdio sidecar encrypted", strstr(body, "\"encrypted\": true") != NULL);
            }
            (void)remove(sidecar);
            (void)remove(renamed);
        }
    }

    (void)remove(path);
    (void)remove_dir(dir);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_imbe_save_read_roundtrip();
    rc |= test_ambe_save_read_roundtrip_and_slot2();
    rc |= test_bit_packing_helpers_roundtrip();
    rc |= test_ambe_pack_unpack_49_bits_roundtrip();
    rc |= test_parse_raw_user_string_guards_and_bounds();
    rc |= test_sdrtrunk_json_metadata_protocols_and_time();
    rc |= test_sdrtrunk_json_encryption_metadata_updates_payload_state();
    rc |= test_sdrtrunk_json_dmr_tg_key_map_overrides_signaled_kid();
    rc |= test_sdrtrunk_json_dmr_tg_key_map_settles_regardless_of_algorithm_order();
    rc |= test_sdrtrunk_json_dmr_tg_key_map_keys_forced_algid_keystream();
    rc |= test_sdrtrunk_forced_rc4_reports_no_keystream_without_an_iv();
    rc |= test_sdrtrunk_json_private_call_never_keeps_a_map_row();
    rc |= test_sdrtrunk_json_without_map_row_keeps_target_keyed_lookup();
    rc |= test_sdrtrunk_json_p25_replay_keyloader_uses_full_width_key_id();
    rc |= test_sdrtrunk_json_dmr_replay_oversized_key_id_keeps_full_width();
    rc |= test_sdrtrunk_json_p25p2_encryption_metadata_updates_event();
    rc |= test_sdrtrunk_json_invalid_numeric_fields_reset_to_zero();
    rc |= test_sdrtrunk_json_protocol_opens_and_closes_mbe_out_file();
    rc |= test_sdrtrunk_json_hex_voice_writes_unencrypted_mbe_records();
    rc |= test_sdrtrunk_json_hex_voice_blocks_encrypted_without_keystream();
    rc |= test_sdrtrunk_json_encrypted_keystreams_write_voice_records();
    rc |= test_sdrtrunk_json_dmr_late_entry_updates_mi();
    rc |= test_open_mbe_out_file_creates_slot_files_and_closes();
    rc |= test_truncated_reads_fail();
    rc |= test_open_mbe_in_file_classifies_cookies();
    rc |= test_open_mbe_in_file_accepts_sdrtrunk_extension();
    rc |= test_open_mbe_in_file_rejects_short_cookie_without_handle();
    rc |= test_symbol_capture_open_writes_expected_headers();
    rc |= test_symbol_capture_auto_rotation_reopens_and_logs_event();
    rc |= test_wav_output_helpers_create_temp_and_raw_files();
    rc |= test_close_and_rename_wav_removes_header_only_files();
    rc |= test_close_and_rename_wav_preserves_nonempty_event_file();
    rc |= test_close_and_rename_wav_numeric_and_failure_paths();
    rc |= test_close_and_rename_wav_exports_rdio_sidecar();

    if (rc == 0) {
        printf("CORE_MBE_FILE_IO: OK\n");
    }
    return rc;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c,clang-analyzer-optin.performance.Padding,clang-analyzer-unix.Errno,clang-analyzer-unix.Stream)
