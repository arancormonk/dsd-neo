// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validation and diagnostics for INI-based user configuration.
 */

#include <ctype.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/config_schema.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "config_user_internal.h"

static int
validate_bool_value(const char* val) {
    if (!val || !*val) {
        return -1;
    }
    if (dsd_strcasecmp(val, "1") == 0 || dsd_strcasecmp(val, "true") == 0 || dsd_strcasecmp(val, "yes") == 0
        || dsd_strcasecmp(val, "on") == 0 || dsd_strcasecmp(val, "0") == 0 || dsd_strcasecmp(val, "false") == 0
        || dsd_strcasecmp(val, "no") == 0 || dsd_strcasecmp(val, "off") == 0) {
        return 0;
    }
    return -1;
}

static int
validate_int_value(const char* val, int* out_val) {
    if (!val || !*val) {
        return -1;
    }
    char* end = NULL;
    long x = strtol(val, &end, 10);
    if (end == val || *end != '\0') {
        return -1;
    }
    if (out_val) {
        *out_val = (int)x;
    }
    return 0;
}

static int
validate_enum_value(const char* val, const char* allowed) {
    if (!val || !allowed) {
        return -1;
    }

    char buf[256];
    snprintf(buf, sizeof buf, "%s", allowed);
    buf[sizeof buf - 1] = '\0';

    char* save = NULL;
    char* tok = dsd_strtok_r(buf, "|", &save);
    while (tok) {
        if (dsd_strcasecmp(val, tok) == 0) {
            return 0;
        }
        tok = dsd_strtok_r(NULL, "|", &save);
    }
    return -1;
}

static void
add_decode_mode_validation_error(dsdcfg_diagnostics_t* diags, int line_num, const char* section, const char* key,
                                 const char* val, const char* allowed) {
    if (!diags || !val) {
        return;
    }
    char msg[256];
    snprintf(msg, sizeof msg,
             "Invalid value '%s' (allowed: %s, aliases: p25p1_only|p25p2_only|edacs|provoice|analog_monitor)", val,
             allowed ? allowed : "auto|p25p1|p25p2|dmr|nxdn48|nxdn96|x2tdma|ysf|dstar|edacs_pv|dpmr|m17|tdma|analog");
    dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, section ? section : "", key ? key : "", msg);
}

static int
validate_enum_with_compat_aliases(const char* section, const char* key, const char* val, const char* allowed,
                                  dsdcfg_diagnostics_t* diags, int line_num, const char* diag_section,
                                  const char* diag_key) {
    if (!val) {
        return -1;
    }
    if (user_config_is_mode_decode_key(section, key)) {
        dsdneoUserDecodeMode mode = DSDCFG_MODE_UNSET;
        if (user_config_parse_decode_mode_value(val, &mode, NULL) == 0) {
            return 0;
        }
        add_decode_mode_validation_error(diags, line_num, diag_section, diag_key, val, allowed);
        return -1;
    }

    if (allowed && validate_enum_value(val, allowed) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "Invalid value '%s' (allowed: %s)", val, allowed);
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, diag_section ? diag_section : "", diag_key ? diag_key : "",
                         msg);
        return -1;
    }

    return 0;
}

static void
validate_entry_value(const dsdcfg_schema_entry_t* entry, const char* schema_section, const char* schema_key,
                     const char* val, dsdcfg_diagnostics_t* diags, int line_num, const char* diag_section,
                     const char* diag_key) {
    if (!entry || !val || !diags) {
        return;
    }

    switch (entry->type) {
        case DSDCFG_TYPE_BOOL:
            if (validate_bool_value(val) != 0) {
                char msg[128];
                snprintf(msg, sizeof msg, "Invalid boolean value '%s' (use true/false/yes/no/1/0)", val);
                dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, diag_section, diag_key, msg);
            }
            break;

        case DSDCFG_TYPE_INT: {
            int int_val = 0;
            if (validate_int_value(val, &int_val) != 0) {
                char msg[128];
                snprintf(msg, sizeof msg, "Invalid integer value '%s'", val);
                dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, diag_section, diag_key, msg);
            } else if ((entry->min_val != 0 || entry->max_val != 0)
                       && (int_val < entry->min_val || int_val > entry->max_val)) {
                char msg[128];
                snprintf(msg, sizeof msg, "Value %d is out of range [%d, %d]", int_val, entry->min_val, entry->max_val);
                dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, diag_section, diag_key, msg);
            }
            break;
        }

        case DSDCFG_TYPE_ENUM:
            (void)validate_enum_with_compat_aliases(schema_section, schema_key, val, entry->allowed, diags, line_num,
                                                    diag_section, diag_key);
            break;

        default: break;
    }
}

int
dsd_user_config_validate(const char* path, dsdcfg_diagnostics_t* diags) {
    if (!diags) {
        return -1;
    }

    dsdcfg_diags_init(diags);

    if (!path || !*path) {
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "", "No config path provided");
        return -1;
    }

    FILE* fp = fopen(path, "r");
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof msg, "Cannot open file: %s", strerror(errno));
        dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, 0, "", "", msg);
        return -1;
    }

    char line[1024];
    char current_section[64];
    current_section[0] = '\0';
    int line_num = 0;
    int is_profile_section = 0;

    while (fgets(line, sizeof line, fp)) {
        line_num++;

        char* p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        size_t n = strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) {
            p[--n] = '\0';
        }

        if (p[0] == '\0' || p[0] == '#' || p[0] == ';') {
            continue;
        }

        if (p[0] == '[') {
            char* end = strchr(p, ']');
            if (!end) {
                dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, "", "", "Malformed section header");
                continue;
            }
            *end = '\0';
            snprintf(current_section, sizeof current_section, "%s", p + 1);

            for (char* c = current_section; *c; ++c) {
                *c = (char)tolower((unsigned char)*c);
            }

            is_profile_section = (strncmp(current_section, "profile.", 8) == 0);
            if (!is_profile_section) {
                const char* known_sections[] = {"input",   "output",    "mode", "trunking",
                                                "logging", "recording", "dsp",  NULL};
                int found = 0;
                for (int i = 0; known_sections[i]; i++) {
                    if (strcmp(current_section, known_sections[i]) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "Unknown section [%s]", current_section);
                    dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, "", msg);
                }
            }
            continue;
        }

        char* eq = strchr(p, '=');
        if (!eq) {
            dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, current_section, "",
                             "Line is not a comment, section, or key=value");
            continue;
        }

        *eq = '\0';
        char* key = p;
        char* val = eq + 1;

        while (*key && isspace((unsigned char)*key)) {
            key++;
        }
        n = strlen(key);
        while (n > 0 && isspace((unsigned char)key[n - 1])) {
            key[--n] = '\0';
        }
        while (*val && isspace((unsigned char)*val)) {
            val++;
        }
        n = strlen(val);
        while (n > 0 && isspace((unsigned char)val[n - 1])) {
            val[--n] = '\0';
        }

        size_t val_len = strlen(val);
        if (val_len >= 2 && val[0] == '"' && val[val_len - 1] == '"') {
            memmove(val, val + 1, val_len - 2);
            val[val_len - 2] = '\0';
        }

        if (current_section[0] == '\0') {
            if (dsd_strcasecmp(key, "version") == 0) {
                int ver = 0;
                if (validate_int_value(val, &ver) != 0) {
                    dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, "", key, "version must be an integer");
                }
            } else if (dsd_strcasecmp(key, "include") == 0) {
                if (!val[0]) {
                    dsdcfg_diags_add(diags, DSDCFG_DIAG_ERROR, line_num, "", key, "include path is empty");
                }
            } else {
                std::string msg = std::string("Unknown top-level key '") + key + "'";
                dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, "", key, msg.c_str());
            }
            continue;
        }

        if (is_profile_section) {
            char* dot = strchr(key, '.');
            if (!dot) {
                std::string msg = std::string("Profile key '") + key + "' should use section.key format";
                dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, key, msg.c_str());
            } else {
                *dot = '\0';
                const char* target_sec = key;
                const char* target_key = dot + 1;
                const dsdcfg_schema_entry_t* entry = dsdcfg_schema_find(target_sec, target_key);
                if (!entry) {
                    std::string msg = std::string("Unknown key '") + target_sec + "." + target_key + "' in profile";
                    dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, key, msg.c_str());
                } else {
                    validate_entry_value(entry, target_sec, target_key, val, diags, line_num, current_section, key);
                }
                *dot = '.';
            }
            continue;
        }

        std::string key_lc = key;
        for (char& c : key_lc) {
            c = (char)tolower((unsigned char)c);
        }

        const dsdcfg_schema_entry_t* entry = dsdcfg_schema_find(current_section, key_lc.c_str());
        if (!entry) {
            std::string msg = std::string("Unknown key '") + key + "' in section [" + current_section + "]";
            dsdcfg_diags_add(diags, DSDCFG_DIAG_WARNING, line_num, current_section, key, msg.c_str());
            continue;
        }

        if (entry->deprecated) {
            std::string msg = std::string("Key '") + key + "' is deprecated";
            dsdcfg_diags_add(diags, DSDCFG_DIAG_INFO, line_num, current_section, key, msg.c_str());
        }

        validate_entry_value(entry, current_section, key, val, diags, line_num, current_section, key);
    }

    fclose(fp);
    return diags->error_count > 0 ? -1 : 0;
}

void
dsd_user_config_diags_free(dsdcfg_diagnostics_t* diags) {
    dsdcfg_diags_free(diags);
}
