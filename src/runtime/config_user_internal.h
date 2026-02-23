// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Private helpers shared by runtime config implementation units.
 */

#pragma once

#include <dsd-neo/runtime/config.h>

int user_config_parse_decode_mode_value(const char* val, dsdneoUserDecodeMode* out_mode, int* used_compat_alias);
int user_config_is_mode_decode_key(const char* section, const char* key);
