// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/shutdown.h>

#include <stddef.h>

int
main(void) {
    exitflag = 0;
    dsd_request_shutdown(NULL, NULL);
    return (exitflag == 1) ? 0 : 1;
}
