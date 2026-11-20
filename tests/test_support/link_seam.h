// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* GNU ld --wrap seams with ordinary C names. `-Wl,--wrap=f` sends every call of f that another object makes to the
   linker symbol __wrap_f, and makes __real_f the real f. A test names its wrapper and the real function as it likes
   and gives each the linker name it needs with an asm label (GCC and Clang only, as --wrap itself is):

       long seam_readable_bytes(int fd) DSD_TEST_LINK_NAME(__wrap_dsd_fd_readable_bytes);

   The label carries the target's user label prefix (none on ELF), so the name matches the one the linker looks for. */
#ifndef DSD_NEO_TESTS_TEST_SUPPORT_LINK_SEAM_H
#define DSD_NEO_TESTS_TEST_SUPPORT_LINK_SEAM_H

#define DSD_TEST_LINK_NAME_TEXT(prefix, name)   #prefix #name
#define DSD_TEST_LINK_NAME_PREFIX(prefix, name) DSD_TEST_LINK_NAME_TEXT(prefix, name)
/** The asm label that gives a declaration the linker name @p name. */
#define DSD_TEST_LINK_NAME(name)                __asm__(DSD_TEST_LINK_NAME_PREFIX(__USER_LABEL_PREFIX__, name))

#endif
