// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Linked into every MSVC test executable. A Debug-CRT assertion, invalid-parameter report or abort() opens a modal
 * dialog, and a test that hits one sits waiting for a click until CTest's timeout instead of failing. Before main()
 * runs, this routes those reports to stderr, where CTest captures them, and keeps abort() and crashes from raising
 * any dialog, so the test exits with a failure code at once.
 */

#if defined(_MSC_VER)

#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>

static void __cdecl
dsd_test_win_crt_report_init(void) {
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    (void)_set_error_mode(_OUT_TO_STDERR);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

/* .CRT$XCU holds the C++ dynamic initializers, which the CRT runs before main() in C programs as well. The pointer has
 * external linkage and is named in a /include directive so the linker keeps it even though nothing references it. */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void(__cdecl* const dsd_test_win_crt_report_init_ptr)(void) =
    dsd_test_win_crt_report_init;
#if defined(_M_IX86)
#pragma comment(linker, "/include:_dsd_test_win_crt_report_init_ptr")
#else
#pragma comment(linker, "/include:dsd_test_win_crt_report_init_ptr")
#endif

#endif /* _MSC_VER */
