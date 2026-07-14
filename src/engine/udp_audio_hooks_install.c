// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/udp_audio.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include "engine_hooks_install.h"

void
dsd_engine_udp_audio_hooks_install(void) {
    dsd_udp_audio_hooks hooks = {0};
    hooks.blast = udp_socket_blaster;
    hooks.blast_analog = udp_socket_blasterA;
    dsd_udp_audio_hooks_set(hooks);
}
