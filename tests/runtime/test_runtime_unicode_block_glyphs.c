// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdio.h>

static void
set_env_flag(const char* name, const char* value) {
    int rc = value ? dsd_setenv(name, value, 1) : dsd_unsetenv(name);
    assert(rc == 0);
}

int
main(void) {
    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", NULL);

    set_env_flag("DSD_FORCE_ASCII", "1");
    set_env_flag("DSD_FORCE_UTF8", "1");
    assert(dsd_unicode_block_glyphs_supported() == 0);

    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", "1");
    assert(dsd_unicode_block_glyphs_supported() == 1);

    set_env_flag("DSD_FORCE_ASCII", NULL);
    set_env_flag("DSD_FORCE_UTF8", NULL);

    printf("RUNTIME_UNICODE_BLOCK_GLYPHS: OK\n");
    return 0;
}
