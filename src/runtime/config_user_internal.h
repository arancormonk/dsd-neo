// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Private helpers shared by runtime config implementation units.
 */

#ifndef DSD_NEO_RUNTIME_CONFIG_USER_INTERNAL_H
#define DSD_NEO_RUNTIME_CONFIG_USER_INTERNAL_H

#include <dsd-neo/runtime/config.h>
#include <stddef.h>
#include <stdio.h>

static constexpr int DSD_USER_CONFIG_MAX_INCLUDE_DEPTH = 3;

void user_cfg_reset(dsdneoUserConfig* cfg);
void user_config_strip_inline_comment(char* line);
int user_config_parse_include_directive_line(char* line, char* out_include_path, size_t out_include_path_size);
int user_config_include_stack_contains_path(const char** include_stack, int include_stack_size, const char* path);
int user_config_parse_decode_mode_value(const char* val, dsdneoUserDecodeMode* out_mode);
int user_config_parse_bool_value(const char* val, int* out_value);
int user_config_parse_int_value(const char* val, int* out_value);
int user_config_parse_double_value(const char* val, double* out_value);
char* user_config_trim_ascii_whitespace(char* text);
/* One physical line of an INI stream into @p line (newline kept): 1 while there is one, 0 at the end. A line that does
   not fit @p line_size is read to its end and dropped whole, @p line left empty and *@p oversized (when given) set, so
   no part of it, a credential's tail included, is read as a setting or quoted (issue #931). */
int user_config_read_line(FILE* stream, char* line, size_t line_size, int* oversized);
void user_config_lowercase_ascii(char* text);
void user_config_strip_wrapping_quotes(char* val);
size_t user_config_path_capacity(const char* section, const char* key);

#endif /* DSD_NEO_RUNTIME_CONFIG_USER_INTERNAL_H */
