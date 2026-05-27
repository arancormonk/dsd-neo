// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dsd-neo/core/safe_api.h"

#include <stddef.h>

static int
test_unknown_size_sprintf_fails(void) {
    char buf[8] = {0};
    char* dst = buf;
    int rc = dsd_safe_sprintf_impl(dst, (size_t)-1, "%s", "abc");
    return rc == -1 ? 0 : 1;
}

int
main(void) {
    int rc = 0;
    rc |= test_unknown_size_sprintf_fails();
    return rc;
}
