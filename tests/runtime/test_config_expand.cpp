// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for config path expansion (~, $VAR, ${VAR}).
 */

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/config.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

#define setenv   dsd_test_setenv
#define unsetenv dsd_test_unsetenv

static int
test_tilde_expansion(void) {
    char buf[512];
    const char* home = dsd_test_home_dir();
    if (!home || !*home) {
        DSD_FPRINTF(stderr, "SKIP: home dir not set\n");
        return 0;
    }

    // Test ~/path expansion
    int rc = dsd_config_expand_path("~/foo/bar", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d for ~/foo/bar\n", rc);
        return 1;
    }

    char expected[512];
    DSD_SNPRINTF(expected, sizeof(expected), "%s/foo/bar", home);
    if (strcmp(buf, expected) != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected '%s', got '%s'\n", expected, buf);
        return 1;
    }

    // Test ~ alone
    rc = dsd_config_expand_path("~", buf, sizeof(buf));
    if (rc != 0 || strcmp(buf, home) != 0) {
        DSD_FPRINTF(stderr, "FAIL: ~ alone should expand to HOME\n");
        return 1;
    }

    return 0;
}

static int
test_env_var_expansion(void) {
    char buf[512];

    // Set a test variable
    setenv("DSD_TEST_VAR", "test_value", 1);

    // Test $VAR form
    int rc = dsd_config_expand_path("/path/$DSD_TEST_VAR/file", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d for $VAR\n", rc);
        return 1;
    }
    if (strcmp(buf, "/path/test_value/file") != 0) {
        DSD_FPRINTF(stderr, "FAIL: $VAR expansion failed, got '%s'\n", buf);
        return 1;
    }

    // Test ${VAR} form
    rc = dsd_config_expand_path("/path/${DSD_TEST_VAR}/file", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d for ${VAR}\n", rc);
        return 1;
    }
    if (strcmp(buf, "/path/test_value/file") != 0) {
        DSD_FPRINTF(stderr, "FAIL: ${VAR} expansion failed, got '%s'\n", buf);
        return 1;
    }

    unsetenv("DSD_TEST_VAR");
    return 0;
}

static int
test_missing_var_expansion(void) {
    char buf[512];

    // Unset any existing variable
    unsetenv("DSD_NONEXISTENT_VAR");

    // Missing variable should expand to empty string
    int rc = dsd_config_expand_path("/path/$DSD_NONEXISTENT_VAR/file", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d for missing var\n", rc);
        return 1;
    }
    if (strcmp(buf, "/path//file") != 0) {
        DSD_FPRINTF(stderr, "FAIL: missing var should expand to empty, got '%s'\n", buf);
        return 1;
    }

    return 0;
}

static int
test_literal_dollar_sign(void) {
    char buf[512];

    // $ followed by non-identifier character should be literal
    int rc = dsd_config_expand_path("/path/$/file", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d\n", rc);
        return 1;
    }
    if (strcmp(buf, "/path/$/file") != 0) {
        DSD_FPRINTF(stderr, "FAIL: literal $ not preserved, got '%s'\n", buf);
        return 1;
    }

    // Malformed ${...  (no closing brace) should preserve $
    rc = dsd_config_expand_path("/path/${INCOMPLETE", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d for malformed\n", rc);
        return 1;
    }
    if (strcmp(buf, "/path/${INCOMPLETE") != 0) {
        DSD_FPRINTF(stderr, "FAIL: malformed ${... not preserved, got '%s'\n", buf);
        return 1;
    }

    return 0;
}

static int
test_no_expansion(void) {
    char buf[512];

    // Path without special characters should pass through
    int rc = dsd_config_expand_path("/usr/local/etc/config.ini", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d\n", rc);
        return 1;
    }
    if (strcmp(buf, "/usr/local/etc/config.ini") != 0) {
        DSD_FPRINTF(stderr, "FAIL: plain path not preserved, got '%s'\n", buf);
        return 1;
    }

    return 0;
}

static int
test_combined_expansion(void) {
    char buf[512];
    const char* home = dsd_test_home_dir();
    if (!home || !*home) {
        DSD_FPRINTF(stderr, "SKIP: home dir not set\n");
        return 0;
    }

    char home_copy[512];
    DSD_SNPRINTF(home_copy, sizeof(home_copy), "%s", home);

    setenv("DSD_TEST_DIR", "configs", 1);

    // Combine ~ and $VAR
    int rc = dsd_config_expand_path("~/$DSD_TEST_DIR/test.ini", buf, sizeof(buf));
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: dsd_config_expand_path returned %d\n", rc);
        return 1;
    }

    char expected[512];
    DSD_SNPRINTF(expected, sizeof(expected), "%s/configs/test.ini", home_copy);
    if (strcmp(buf, expected) != 0) {
        DSD_FPRINTF(stderr, "FAIL: combined expansion failed, expected '%s', got '%s'\n", expected, buf);
        return 1;
    }

    unsetenv("DSD_TEST_DIR");
    return 0;
}

static int
test_buffer_overflow_protection(void) {
    char small_buf[16];

    // Set a long value that would overflow the buffer
    setenv("DSD_LONG_VAR", "this_is_a_very_long_value_that_will_overflow", 1);

    int rc = dsd_config_expand_path("$DSD_LONG_VAR", small_buf, sizeof(small_buf));
    // Should return error due to truncation
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: should have returned error for overflow\n");
        unsetenv("DSD_LONG_VAR");
        return 1;
    }

    unsetenv("DSD_LONG_VAR");
    return 0;
}

#if DSD_PLATFORM_WIN_NATIVE
#define TEST_HOME_VAR "USERPROFILE"
#else
#define TEST_HOME_VAR "HOME"
#endif

/* The home directory is read at every expansion, not cached from the first: the control API applies configuration
   from several session threads at once, and a cache filled by one while another reads it is a data race (the
   concurrent part is what the thread sanitizer build checks). */
static int
test_home_follows_environment_and_threads(void) {
    const char* original = dsd_test_home_dir();
    const std::string saved = original != NULL ? original : "";
    int failed = 0;
    char buf[512];

    setenv(TEST_HOME_VAR, "/dsd-home-one", 1);
    if (dsd_config_expand_path("~/a", buf, sizeof(buf)) != 0 || strcmp(buf, "/dsd-home-one/a") != 0) {
        DSD_FPRINTF(stderr, "FAIL: ~/a with the first home gave '%s'\n", buf);
        failed = 1;
    }
    setenv(TEST_HOME_VAR, "/dsd-home-two", 1);
    if (dsd_config_expand_path("~/a", buf, sizeof(buf)) != 0 || strcmp(buf, "/dsd-home-two/a") != 0) {
        DSD_FPRINTF(stderr, "FAIL: ~/a after the home changed gave '%s'\n", buf);
        failed = 1;
    }

    /* A home directory too long for the expansion fails it instead of cutting the path. */
    const std::string long_home = "/" + std::string(2000U, 'h');
    setenv(TEST_HOME_VAR, long_home.c_str(), 1);
    std::vector<char> big(4096U);
    if (dsd_config_expand_path("~/a", big.data(), big.size()) == 0) {
        DSD_FPRINTF(stderr, "FAIL: an oversized home directory was accepted\n");
        failed = 1;
    }

    setenv(TEST_HOME_VAR, "/dsd-home-threads", 1);
    std::vector<int> ok(8U, 0);
    std::vector<std::thread> threads;
    threads.reserve(ok.size());
    for (size_t t = 0; t < ok.size(); t++) {
        threads.emplace_back([&ok, t]() {
            char out[256];
            int good = 1;
            for (int i = 0; i < 200; i++) {
                good &= dsd_config_expand_path("~/capture.wav", out, sizeof(out)) == 0
                        && strcmp(out, "/dsd-home-threads/capture.wav") == 0;
            }
            ok[t] = good;
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    for (size_t t = 0; t < ok.size(); t++) {
        if (!ok[t]) {
            DSD_FPRINTF(stderr, "FAIL: thread %zu expanded ~ wrongly\n", t);
            failed = 1;
        }
    }

    if (!saved.empty()) {
        setenv(TEST_HOME_VAR, saved.c_str(), 1);
    } else {
        unsetenv(TEST_HOME_VAR);
    }
    return failed;
}

int
main(void) {
    int rc = 0;

    rc |= test_tilde_expansion();
    rc |= test_env_var_expansion();
    rc |= test_missing_var_expansion();
    rc |= test_literal_dollar_sign();
    rc |= test_no_expansion();
    rc |= test_combined_expansion();
    rc |= test_buffer_overflow_protection();
    rc |= test_home_follows_environment_and_threads();

    if (rc == 0) {
        printf("All config_expand tests passed\n");
    }

    return rc;
}
