// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_unicode_init_locale() switches a Windows console to UTF-8, and the console keeps its code pages after the
 * process exits. dsd_unicode_restore_console() (also registered with atexit()) must give the shell its own back.
 * This runs in its own process because the switch happens only on the first initialization.
 */

#include <assert.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/unicode.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <stdio.h>
#include <windows.h>
#endif

int
main(void) {
    assert(dsd_unsetenv("DSD_FORCE_ASCII") == 0);
    assert(dsd_unsetenv("DSD_FORCE_UTF8") == 0);

#if DSD_PLATFORM_WIN_NATIVE
    const UINT original_output = GetConsoleOutputCP();
    const UINT original_input = GetConsoleCP();
    if (original_output != 0 && original_input != 0) {
        /* Start from the OEM code page a fresh cmd.exe has. */
        assert(SetConsoleOutputCP(437) && SetConsoleCP(437));
        dsd_unicode_init_locale();
        assert(GetConsoleOutputCP() == CP_UTF8);
        assert(GetConsoleCP() == CP_UTF8);

        dsd_unicode_restore_console();
        assert(GetConsoleOutputCP() == 437);
        assert(GetConsoleCP() == 437);

        /* Restoring twice leaves the console as the first restore left it. */
        dsd_unicode_restore_console();
        assert(GetConsoleOutputCP() == 437);
        assert(GetConsoleCP() == 437);

        (void)SetConsoleOutputCP(original_output);
        (void)SetConsoleCP(original_input);
        return 0;
    }
    printf("no console attached: nothing to switch or restore\n");
#endif

    /* Elsewhere, and without a console, restoring is a no-op. */
    dsd_unicode_init_locale();
    dsd_unicode_restore_console();
    dsd_unicode_restore_console();
    return 0;
}
