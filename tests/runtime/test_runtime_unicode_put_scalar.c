// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_unicode_fput_scalar() is the one way radio-sourced text reaches stderr (issue #358).
 * It encodes the scalar itself in UTF-8 mode instead of handing it to fprintf("%lc"),
 * and keeps the historical best-effort ASCII fallback otherwise.
 */

#include <assert.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

static void
capture_put_scalars(const uint32_t* scalars, size_t count, char* buf, size_t buf_size) {
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "unicode_put_scalar") == 0);
    for (size_t i = 0; i < count; i++) {
        dsd_unicode_fput_scalar(scalars[i], stderr);
    }
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, buf, buf_size) == 0);
}

static void
test_utf8_mode_writes_utf8_bytes(void) {
    static const uint32_t scalars[] = {0x1F600U, 0x4739U, 0x41U, DSD_UNICODE_REPLACEMENT};
    char buf[64];
    assert(dsd_test_setenv("DSD_FORCE_UTF8", "1", 1) == 0);
    assert(dsd_test_unsetenv("DSD_FORCE_ASCII") == 0);
    capture_put_scalars(scalars, sizeof scalars / sizeof scalars[0], buf, sizeof buf);
    assert(strcmp(buf, "\xF0\x9F\x98\x80"
                       "\xE4\x9C\xB9"
                       "A"
                       "\xEF\xBF\xBD")
           == 0);
}

static void
test_utf8_mode_writes_nul_scalar(void) {
    /* U+0000 is a scalar value: it must reach the stream as one NUL byte rather than vanish
     * because the encoded form looks like an empty C string. */
    static const uint32_t scalars[] = {0x41U, 0x00U, 0x42U};
    char buf[64];
    assert(dsd_test_setenv("DSD_FORCE_UTF8", "1", 1) == 0);
    assert(dsd_test_unsetenv("DSD_FORCE_ASCII") == 0);
    capture_put_scalars(scalars, sizeof scalars / sizeof scalars[0], buf, sizeof buf);
    assert(memcmp(buf, "A\0B", 4U) == 0);
}

static void
test_ascii_mode_prints_printable_low_byte_or_question_mark(void) {
    static const uint32_t scalars[] = {0x41U, 0x4739U, 0x1F600U, DSD_UNICODE_REPLACEMENT, 0x0AU};
    char buf[64];
    assert(dsd_test_setenv("DSD_FORCE_ASCII", "1", 1) == 0);
    assert(dsd_test_unsetenv("DSD_FORCE_UTF8") == 0);
    capture_put_scalars(scalars, sizeof scalars / sizeof scalars[0], buf, sizeof buf);
    /* 0x4739 keeps its printable low byte '9'; the rest have none. */
    assert(strcmp(buf, "A9???") == 0);
}

int
main(void) {
    test_utf8_mode_writes_utf8_bytes();
    test_utf8_mode_writes_nul_scalar();
    test_ascii_mode_prints_printable_low_byte_or_question_mark();
    assert(dsd_test_unsetenv("DSD_FORCE_ASCII") == 0);
    DSD_FPRINTF(stderr, "RUNTIME_UNICODE_PUT_SCALAR: PASS\n");
    return 0;
}
