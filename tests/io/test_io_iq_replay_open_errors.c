// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Why an I/Q replay path cannot be opened. The replay reads a path's first bytes to tell a WAV recording from a raw
 * capture, and a sidecar's data_file the same way. A path it cannot open is reported with the reason (a symbolic link,
 * a missing file, no permission), not as a missing sidecar; a read that fails is an I/O error, distinct from a file too
 * short to hold a header; and a path whose sidecar exists still resolves through it.
 *
 * Built with DSD_NEO_TEST_HOOKS around iq_replay.c, for the seam that fails a sniff read.
 */

#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/file_compat.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "iq_replay_test_hooks.h"
#include "test_support.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* label, const char* text, const char* want) {
    if (!text || !strstr(text, want)) {
        DSD_FPRINTF(stderr, "FAIL: %s: missing \"%s\" in:\n  %s\n", label, want, text ? text : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_lacks(const char* label, const char* text, const char* unwanted) {
    if (text && strstr(text, unwanted)) {
        DSD_FPRINTF(stderr, "FAIL: %s: unexpected \"%s\" in:\n  %s\n", label, unwanted, text);
        return 1;
    }
    return 0;
}

/* ---- Files ------------------------------------------------------------------------------------------------------ */

static char g_dir[DSD_TEST_PATH_MAX];
static char g_made[32][64];
static size_t g_made_count = 0;

/* A path for @p name in the test's directory, recorded for removal. */
static const char*
tpath(const char* name) {
    static char paths[8][DSD_TEST_PATH_MAX];
    static unsigned next = 0;
    char* out = paths[next++ % 8U];
    (void)dsd_test_path_join(out, DSD_TEST_PATH_MAX, g_dir, name);
    for (size_t i = 0; i < g_made_count; i++) {
        if (strcmp(g_made[i], name) == 0) {
            return out;
        }
    }
    if (g_made_count < sizeof(g_made) / sizeof(g_made[0])) {
        DSD_SNPRINTF(g_made[g_made_count], sizeof(g_made[0]), "%s", name);
        g_made_count++;
    }
    return out;
}

static int
write_file(const char* name, const void* data, size_t len) {
    FILE* fp = dsd_fopen_private(tpath(name), "wb");
    if (!fp) {
        return -1;
    }
    if (len > 0U && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    return fclose(fp);
}

/* A WAV recording of 4 frames of 2-channel 16-bit PCM at 48 kHz. */
static int
write_wav(const char* name) {
    static const uint8_t wav[] = {
        'R', 'I', 'F', 'F', 52,   0,    0, 0, 'W',  'A',  'V',  'E', 'f', 'm', 't', ' ', 16,  0,   0,   0,
        1,   0,   2,   0,   0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0,   4,   0,   16,  0,   'd', 'a', 't', 'a',
        16,  0,   0,   0,   1,    0,    2, 0, 3,    0,    4,    0,   5,   0,   6,   0,   7,   0,   8,   0,
    };
    return write_file(name, wav, sizeof(wav));
}

/* A raw cu8 capture named @p data_name with its sidecar @p data_name ".json", written by the capture writer. */
static int
write_capture(const char* data_name) {
    char json_name[64];
    DSD_SNPRINTF(json_name, sizeof(json_name), "%s.json", data_name);
    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", tpath(data_name));
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", tpath(json_name));
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = 1536000U;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851375000ULL;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = 48000U;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");

    dsd_iq_capture_writer* writer = NULL;
    char err[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: capture writer for %s: %s\n", data_name, err);
        return -1;
    }
    uint8_t payload[4096];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFFU);
    }
    if (dsd_iq_capture_submit(writer, payload, sizeof(payload)) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return -1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    return 0;
}

/* dsd_iq_replay_read_metadata() and dsd_iq_replay_open() on @p path: both fail with @p want_rc, and their messages
 * contain @p want and do not contain @p unwanted (NULL: no check). */
static int
expect_refused(const char* label, const char* path, int want_rc, const char* want, const char* unwanted) {
    int rc = 0;
    char err[2048] = {0};
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char what[256];
    DSD_SNPRINTF(what, sizeof(what), "%s: metadata", label);
    rc |= expect_int(what, dsd_iq_replay_read_metadata(path, &cfg, err, sizeof(err)), want_rc);
    rc |= expect_contains(what, err, want);
    if (unwanted) {
        rc |= expect_lacks(what, err, unwanted);
    }
    dsd_iq_replay_config_clear(&cfg);

    err[0] = '\0';
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_source* src = NULL;
    DSD_SNPRINTF(what, sizeof(what), "%s: open", label);
    rc |= expect_int(what, dsd_iq_replay_open(path, &cfg, &src, err, sizeof(err)), want_rc);
    rc |= expect_contains(what, err, want);
    if (unwanted) {
        rc |= expect_lacks(what, err, unwanted);
    }
    if (src) {
        dsd_iq_replay_close(src);
    }
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

/* dsd_iq_replay_open() on @p path succeeds, replaying @p want_data. */
static int
expect_opens(const char* label, const char* path, const char* want_data) {
    char err[2048] = {0};
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_source* src = NULL;
    int rc = expect_int(label, dsd_iq_replay_open(path, &cfg, &src, err, sizeof(err)), DSD_IQ_OK);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  error: %s\n", err);
    } else {
        rc |= expect_contains(label, cfg.data_path, want_data);
    }
    if (src) {
        dsd_iq_replay_close(src);
    }
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

/* ---- Cases ------------------------------------------------------------------------------------------------------ */

static int
test_missing_wav(void) {
    return expect_refused("missing .wav", tpath("missing.wav"), DSD_IQ_ERR_IO, "No such file", "sidecar not found");
}

/* A path that is neither there nor a sidecar's name keeps the sidecar names it tried in its message. */
static int
test_missing_bare_name(void) {
    int rc = expect_refused("missing bare name", tpath("nothing"), DSD_IQ_ERR_IO, "No such file", NULL);
    char err[2048] = {0};
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    (void)dsd_iq_replay_read_metadata(tpath("nothing"), &cfg, err, sizeof(err));
    rc |= expect_contains("missing bare name: sidecars tried", err, "nothing.iq.json");
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

static int
test_short_and_truncated(void) {
    int rc = 0;
    /* Fewer bytes than a WAV header: the end of the file, not a read error, so not a WAV. */
    rc |= expect_int("short write", write_file("short.wav", "RIF", 3), 0);
    rc |= expect_refused("3-byte .wav", tpath("short.wav"), DSD_IQ_ERR_INVALID_META, "is not a WAV file",
                         "failed to read");
    /* A WAV header cut one byte short of its WAVE tag. */
    rc |= expect_int("truncated write", write_file("cut.wav", "RIFF\x24\0\0\0WAV", 11), 0);
    rc |= expect_refused("truncated WAV header", tpath("cut.wav"), DSD_IQ_ERR_INVALID_META, "is not a WAV file",
                         "failed to read");
    rc |= expect_int("empty write", write_file("empty.wav", "", 0), 0);
    rc |= expect_refused("empty .wav", tpath("empty.wav"), DSD_IQ_ERR_INVALID_META, "is not a WAV file",
                         "failed to read");
    return rc;
}

/* A sniff read that fails is a read error, through either sniff: the given path's, and a sidecar's data_file. */
static int
test_injected_read_errors(void) {
    int rc = 0;
    rc |= expect_int("read-error wav", write_wav("readerr.wav"), 0);
    rc |= expect_int("read-error capture", write_capture("readerr.iq"), 0);
    rc |= expect_opens("read-error wav, unarmed", tpath("readerr.wav"), "readerr.wav");
    rc |= expect_opens("read-error capture, unarmed", tpath("readerr.iq"), "readerr.iq");

    char err[2048];
    dsd_iq_replay_config cfg;

    /* The WAV path's own sniff. */
    err[0] = '\0';
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_test_fail_sniff_read(0);
    rc |= expect_int("wav sniff read error", dsd_iq_replay_read_metadata(tpath("readerr.wav"), &cfg, err, sizeof(err)),
                     DSD_IQ_ERR_IO);
    rc |= expect_contains("wav sniff read error", err, "failed to read");
    rc |= expect_contains("wav sniff read error names the file", err, "readerr.wav");
    dsd_iq_replay_config_clear(&cfg);

    /* The sidecar path: only its data_file is sniffed. */
    err[0] = '\0';
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_test_fail_sniff_read(0);
    rc |= expect_int("data_file sniff read error",
                     dsd_iq_replay_read_metadata(tpath("readerr.iq.json"), &cfg, err, sizeof(err)), DSD_IQ_ERR_IO);
    rc |= expect_contains("data_file sniff read error", err, "failed to read");
    rc |= expect_contains("data_file sniff read error names the data file", err, "readerr.iq'");
    dsd_iq_replay_config_clear(&cfg);

    /* The data path: its own sniff reads, then the data_file sniff of the sidecar it found fails. */
    err[0] = '\0';
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_test_fail_sniff_read(1);
    rc |= expect_int("second sniff read error",
                     dsd_iq_replay_read_metadata(tpath("readerr.iq"), &cfg, err, sizeof(err)), DSD_IQ_ERR_IO);
    rc |= expect_contains("second sniff read error", err, "failed to read");
    dsd_iq_replay_config_clear(&cfg);

    /* ... and the data path's own sniff failing is reported, not passed over for the sidecar. */
    err[0] = '\0';
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_test_fail_sniff_read(0);
    rc |= expect_int("data path sniff read error",
                     dsd_iq_replay_read_metadata(tpath("readerr.iq"), &cfg, err, sizeof(err)), DSD_IQ_ERR_IO);
    rc |= expect_contains("data path sniff read error", err, "failed to read");
    dsd_iq_replay_config_clear(&cfg);

    dsd_iq_replay_test_fail_sniff_read(-1);
    rc |= expect_opens("read-error capture, disarmed", tpath("readerr.iq"), "readerr.iq");
    return rc;
}

/* A raw capture beside its sidecar, named by its data path or by the bare name given to --iq-capture, still
 * resolves. */
static int
test_sidecar_fallback(void) {
    int rc = 0;
    rc |= expect_int("fallback capture", write_capture("plain.iq"), 0);
    rc |= expect_opens("data path beside its sidecar", tpath("plain.iq"), "plain.iq");
    rc |= expect_opens("bare capture name", tpath("plain"), "plain.iq");
    rc |= expect_opens("sidecar path", tpath("plain.iq.json"), "plain.iq");
    return rc;
}

#ifndef _WIN32
static const char kSymlinkWording[] =
    "is a symbolic link, or a path the system would not follow; replay opens regular files only";

static int
test_symlinked_wav(void) {
    int rc = 0;
    rc |= expect_int("symlink target wav", write_wav("target.wav"), 0);
    rc |= expect_int("symlink wav", symlink("target.wav", tpath("link.wav")), 0);
    rc |= expect_refused("symlinked WAV", tpath("link.wav"), DSD_IQ_ERR_IO, kSymlinkWording, "sidecar not found");
    rc |= expect_opens("symlink target WAV itself", tpath("target.wav"), "target.wav");
    return rc;
}

/* A sidecar whose data_file is a symbolic link, named by the sidecar or by the link. */
static int
test_symlinked_data_file(void) {
    int rc = 0;
    rc |= expect_int("linked capture", write_capture("linked.iq"), 0);
    rc |= expect_int("move the data aside", rename(tpath("linked.iq"), tpath("linked_real.iq")), 0);
    rc |= expect_int("data symlink", symlink("linked_real.iq", tpath("linked.iq")), 0);
    rc |=
        expect_refused("symlinked data_file by sidecar", tpath("linked.iq.json"), DSD_IQ_ERR_IO, kSymlinkWording, NULL);
    rc |= expect_refused("symlinked data_file by data path", tpath("linked.iq"), DSD_IQ_ERR_IO, kSymlinkWording,
                         "sidecar not found");
    return rc;
}

static int
test_unreadable_wav(void) {
    if (geteuid() == 0) {
        DSD_FPRINTF(stderr, "SKIP: unreadable file case (running as root)\n");
        return 0;
    }
    int rc = expect_int("unreadable wav", write_wav("locked.wav"), 0);
    rc |= expect_int("chmod 000", chmod(tpath("locked.wav"), 0), 0);
    rc |= expect_refused("mode-000 .wav", tpath("locked.wav"), DSD_IQ_ERR_IO, "Permission denied", "sidecar not found");
    (void)chmod(tpath("locked.wav"), S_IRUSR | S_IWUSR);
    return rc;
}
#endif

int
main(void) {
    if (!dsd_test_mkdtemp(g_dir, sizeof(g_dir), "dsdneo_replay_open")) {
        DSD_FPRINTF(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    int rc = 0;
    rc |= test_missing_wav();
    rc |= test_missing_bare_name();
    rc |= test_short_and_truncated();
    rc |= test_injected_read_errors();
    rc |= test_sidecar_fallback();
#ifndef _WIN32
    rc |= test_symlinked_wav();
    rc |= test_symlinked_data_file();
    rc |= test_unreadable_wav();
#endif
    dsd_iq_replay_test_fail_sniff_read(-1);

    const char* files[sizeof(g_made) / sizeof(g_made[0]) + 1U];
    for (size_t i = 0; i < g_made_count; i++) {
        files[i] = g_made[i];
    }
    files[g_made_count] = NULL;
    if (dsd_test_remove_temp_dir(g_dir, files) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not remove %s: %s\n", g_dir, strerror(errno));
        rc = 1;
    }
    if (rc == 0) {
        DSD_FPRINTF(stdout, "IO_IQ_REPLAY_OPEN_ERRORS: OK\n");
    }
    return rc;
}
