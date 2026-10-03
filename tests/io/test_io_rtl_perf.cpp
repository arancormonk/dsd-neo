// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: opt-in RTL performance logging must gate on the environment,
 * aggregate counters over an interval, write CSV rows, and reset on shutdown.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cstdio>
#include <cstring>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdint.h>
#include <string>

#include "rtl_perf.h"
#include "test_support.h"

/* The logger's stream is a real file the test reads back by name, without
 * flushing it for the logger: the header and every row must reach the file as
 * they are written. The logger closes it on shutdown, and the final row is only
 * checked after that. Each open
 * gets a fresh private temp file, created owner-only by dsd_test_mkstemp() and
 * opened through that descriptor, never by name, so nothing here creates a file
 * with default permissions. reset_fixture() and main() remove it. */
static char g_csv_capture_path[DSD_TEST_PATH_MAX];

static void
remove_csv_capture(void) {
    if (g_csv_capture_path[0] != '\0') {
        (void)std::remove(g_csv_capture_path);
        g_csv_capture_path[0] = '\0';
    }
}

static FILE*
open_fresh_csv_capture(void) {
    remove_csv_capture();
    const int fd = dsd_test_mkstemp(g_csv_capture_path, sizeof g_csv_capture_path, "dsd_neo_test_rtl_perf");
    assert(fd >= 0);
    FILE* fp = dsd_test_fdopen(fd, "w+b");
    assert(fp != nullptr);
    return fp;
}

static uint64_t g_now_ns = 1000;
static FILE* g_csv_file = nullptr;
static int g_open_count = 0;
static decltype(dsd_stat_t{}.st_size) g_stub_size = 0;

extern "C" uint64_t
test_dsd_realtime_mono_ns(void) {
    return g_now_ns;
}

extern "C" FILE*
test_dsd_fopen_private(const char* path, const char* mode) {
    assert(std::strcmp(path, "dsd-neo-rtl-perf.csv") == 0);
    assert(std::strcmp(mode, "a") == 0);
    g_open_count++;
    g_csv_file = open_fresh_csv_capture();
    return g_csv_file;
}

static std::string
read_csv_capture(void) {
    std::string text;
    if (g_csv_capture_path[0] == '\0') {
        return text;
    }
    FILE* in = std::fopen(g_csv_capture_path, "rb");
    if (!in) {
        return text;
    }
    char buf[512];
    /* A short read is end of file or a read error; either way the stream has nothing more to give. */
    for (;;) {
        const size_t n = std::fread(buf, 1, sizeof buf, in);
        text.append(buf, n);
        if (n < sizeof buf) {
            break;
        }
    }
    (void)std::fclose(in);
    return text;
}

extern "C" int
test_dsd_fileno(FILE* stream) {
    assert(stream == g_csv_file);
    return 123;
}

extern "C" int
test_dsd_fstat(int fd, dsd_stat_t* st) {
    assert(fd == 123);
    if (!st) {
        return -1;
    }
    std::memset(st, 0, sizeof(*st));
    st->st_size = g_stub_size;
    return 0;
}

static void
reset_fixture(void) {
    rtl_perf_shutdown();
    (void)dsd_unsetenv("DSD_NEO_RTL_PERF_CSV");
    (void)dsd_unsetenv("DSD_NEO_RTL_PERF_INTERVAL_MS");
    remove_csv_capture();
    g_csv_file = nullptr;
    g_open_count = 0;
    g_stub_size = 0;
    g_now_ns = 1000;
}

static void
test_disabled_without_env(void) {
    reset_fixture();
    assert(rtl_perf_enabled() == 0);
    rtl_perf_record_ingest(10, 20, 3);
    rtl_perf_record_demod_block(1, 2, 3, 4, 5);
    rtl_perf_record_consumer_read(6, 7);
    rtl_perf_log_snapshot snapshot{};
    rtl_perf_maybe_log(&snapshot);
    assert(g_open_count == 0);
    rtl_perf_shutdown();
    assert(rtl_perf_enabled() == 0);
}

static void
test_csv_logging_aggregates_and_resets(void) {
    reset_fixture();
    (void)dsd_setenv("DSD_NEO_RTL_PERF_CSV", "1", 1);
    (void)dsd_setenv("DSD_NEO_RTL_PERF_INTERVAL_MS", "1", 1);

    assert(rtl_perf_enabled() == 1);
    assert(g_open_count == 1);
    rtl_perf_record_ingest(11, 22, 3);
    rtl_perf_record_ingest(13, 5, 1);
    rtl_perf_record_demod_block(17, 19, 23, 29, 31);
    rtl_perf_record_consumer_read(37, 41);

    rtl_perf_log_snapshot snapshot{};
    snapshot.source = "rtltcp";
    snapshot.sample_rate_hz = 48000;
    snapshot.output_kind = 2;
    snapshot.input_used = 10;
    snapshot.input_capacity = 20;
    snapshot.input_drops = 30;
    snapshot.output_used = 40;
    snapshot.output_capacity = 50;
    snapshot.symbol_cache_pending = 6;
    snapshot.snr_db = 7.25;
    snapshot.cfo_hz = -12.5;
    snapshot.carrier_lock = 1;

    rtl_perf_maybe_log(&snapshot);
    std::string before_interval = read_csv_capture();
    assert(before_interval.find("rtltcp") == std::string::npos);
    assert(before_interval.find("time_ms,source,rate_hz") != std::string::npos);

    g_now_ns = 100001000ULL;
    rtl_perf_maybe_log(&snapshot);
    std::string first_log = read_csv_capture();
    assert(first_log.find(",rtltcp,48000,2,10,20,30,40,50,6,2,27,4,24,1,29,31,17,19,23,1,41,37,7.250,-12.500,1\n")
           != std::string::npos);

    g_now_ns = 200001000ULL;
    snapshot.source = nullptr;
    snapshot.sample_rate_hz = 24000;
    snapshot.output_kind = 9;
    rtl_perf_maybe_log(&snapshot);
    rtl_perf_shutdown();
    std::string final_log = read_csv_capture();
    assert(final_log.find(",unknown,24000,9,10,20,30,40,50,6,0,0,0,0,0,0,0,0,0,0,0,0,0,7.250,-12.500,1\n")
           != std::string::npos);
}

static void
test_existing_file_skips_header(void) {
    reset_fixture();
    g_stub_size = 99;
    (void)dsd_setenv("DSD_NEO_RTL_PERF_CSV", "1", 1);
    (void)dsd_setenv("DSD_NEO_RTL_PERF_INTERVAL_MS", "60001", 1);
    assert(rtl_perf_enabled() == 1);
    std::string output = read_csv_capture();
    assert(output.find("time_ms,source") == std::string::npos);
}

int
main(void) {
    test_disabled_without_env();
    test_csv_logging_aggregates_and_resets();
    test_existing_file_skips_header();
    reset_fixture();
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
