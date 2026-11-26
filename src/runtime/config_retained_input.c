// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The [input] of the config a session loaded, which a session running an I/Q replay saves in place of its own input
 * (issue #743): a replay has no INI form, and a save that wrote it as a file input left a config the next start
 * refuses. See <dsd-neo/runtime/config.h> for who sets, replaces and drops it.
 */

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/config.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"

/* The [input] block of dsdneoUserConfig, has_input through input_warn_db_is_set. */
#define RETAINED_INPUT_BEGIN offsetof(dsdneoUserConfig, has_input)
#define RETAINED_INPUT_END                                                                                             \
    (offsetof(dsdneoUserConfig, input_warn_db_is_set) + sizeof(((dsdneoUserConfig*)0)->input_warn_db_is_set))

typedef struct {
    /* Which retain the copy came from, unique for the process, so a snapshot that holds it copies nothing again; a
       per-state count would repeat in the next session's state, which may land at the freed one's address. */
    uint64_t revision;
    unsigned char input[RETAINED_INPUT_END - RETAINED_INPUT_BEGIN];
} retained_input;

static dsd_atomic_u64 g_retained_input_revision = {0U};

static retained_input*
retained_input_of(const dsd_state* state) {
    return (retained_input*)dsd_state_ext_get(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT);
}

/* The state's copy, allocated empty when it has none; NULL when that allocation fails. */
static retained_input*
retained_input_slot(dsd_state* state) {
    retained_input* kept = retained_input_of(state);
    if (kept != NULL) {
        return kept;
    }
    kept = (retained_input*)calloc(1, sizeof *kept);
    if (kept != NULL) {
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT, kept, free);
    }
    return kept;
}

/* An [input] no config can start: a file input whose file_path is an input spec (an I/Q replay). A config saved by
   the bug this copy fixes has one; retaining it would write it back. */
static int
input_is_unstartable(const dsdneoUserConfig* cfg) {
    return cfg->has_input && cfg->input_source == DSDCFG_INPUT_FILE
           && dsd_user_config_file_path_is_input_spec(cfg->file_path);
}

int
dsd_user_config_retain_input(dsd_state* state, const dsdneoUserConfig* cfg) {
    if (state == NULL) {
        return -1;
    }
    if (cfg == NULL || !cfg->has_input) {
        dsd_user_config_forget_retained_input(state);
        return 0;
    }
    if (input_is_unstartable(cfg)) {
        return 0;
    }
    retained_input* kept = retained_input_slot(state);
    if (kept == NULL) {
        return -1;
    }
    DSD_MEMCPY(kept->input, (const unsigned char*)cfg + RETAINED_INPUT_BEGIN, sizeof kept->input);
    kept->revision = dsd_atomic_u64_fetch_add_relaxed(&g_retained_input_revision, 1U) + 1U;
    return 0;
}

/* One [input] key of the merge: its field, and the field of its `_is_set` flag (0 for none: has_input, at offset 0, is
   no key's flag). A flagged key counts as set when its flag is; any other when it differs from the default. */
typedef struct {
    size_t offset;
    size_t size;
    size_t flag_offset;
} input_key;

#define INPUT_KEY(field) {offsetof(dsdneoUserConfig, field), sizeof(((dsdneoUserConfig*)0)->field), 0U}
#define INPUT_FLAGGED_KEY(field, flag)                                                                                 \
    {offsetof(dsdneoUserConfig, field), sizeof(((dsdneoUserConfig*)0)->field), offsetof(dsdneoUserConfig, flag)}

/* Every [input] field but has_input, the flags (merged with their keys), the RTL-SDR selector and airspy (each merged
   whole, below). */
static const input_key k_input_keys[] = {
    INPUT_KEY(input_source),
    INPUT_KEY(pulse_input),
    INPUT_KEY(rtl_freq),
    INPUT_FLAGGED_KEY(rtl_gain, rtl_gain_is_set),
    INPUT_FLAGGED_KEY(rtl_ppm, rtl_ppm_is_set),
    INPUT_KEY(rtl_bw_khz),
    INPUT_FLAGGED_KEY(rtl_sql, rtl_sql_is_set),
    INPUT_FLAGGED_KEY(rtl_sql_mode, rtl_sql_is_set),
    INPUT_FLAGGED_KEY(rtl_sql_margin_db, rtl_sql_is_set),
    /* The note that rtl_sql was rounded (issue #899 B15) goes with rtl_sql: a merged whole dB clears it. */
    INPUT_FLAGGED_KEY(rtl_sql_rounded, rtl_sql_is_set),
    INPUT_FLAGGED_KEY(rtl_sql_unrounded_db, rtl_sql_is_set),
    /* So does the note that the analog default was in force: a patch naming a squelch key clears it. */
    INPUT_FLAGGED_KEY(rtl_sql_analog_default, rtl_sql_is_set),
    INPUT_KEY(rtl_volume),
    INPUT_KEY(rtl_auto_ppm),
    INPUT_KEY(rtltcp_host),
    INPUT_KEY(rtltcp_port),
    INPUT_KEY(airspy_invalid),
    INPUT_KEY(soapy_args),
    INPUT_KEY(soapy_profile),
    INPUT_KEY(soapy_stream_format),
    INPUT_KEY(soapy_antenna),
    INPUT_KEY(soapy_clock),
    INPUT_KEY(soapy_settings),
    INPUT_KEY(soapy_gains),
    INPUT_FLAGGED_KEY(soapy_bandwidth_hz, soapy_bandwidth_hz_is_set),
    INPUT_KEY(digital_resample),
    INPUT_KEY(file_path),
    INPUT_KEY(file_sample_rate),
    INPUT_KEY(tcp_host),
    INPUT_KEY(tcp_port),
    INPUT_KEY(udp_addr),
    INPUT_KEY(udp_port),
    INPUT_KEY(iq_host),
    INPUT_KEY(iq_port),
    INPUT_KEY(iq_rate),
    INPUT_KEY(iq_format),
    INPUT_KEY(iq_freq),
    INPUT_FLAGGED_KEY(input_warn_db, input_warn_db_is_set),
};

static int
input_key_is_set(const input_key* key, const unsigned char* patch, const unsigned char* defaults) {
    if (key->flag_offset != 0U) {
        int flag = 0;
        DSD_MEMCPY(&flag, patch + key->flag_offset, sizeof flag);
        return flag != 0;
    }
    return memcmp(patch + key->offset, defaults + key->offset, key->size) != 0;
}

/* Field by field: the struct has padding after the serial. */
static int
airspy_config_differs(const dsd_airspy_config* a, const dsd_airspy_config* b) {
    return memcmp(a->serial, b->serial, sizeof a->serial) != 0 || a->sample_rate != b->sample_rate
           || a->gain_mode != b->gain_mode || a->sensitivity_gain != b->sensitivity_gain
           || a->linearity_gain != b->linearity_gain || a->lna_gain != b->lna_gain || a->mixer_gain != b->mixer_gain
           || a->vga_gain != b->vga_gain || a->lna_agc != b->lna_agc || a->mixer_agc != b->mixer_agc
           || a->bias_tee != b->bias_tee;
}

/* The Airspy keys travel as one struct: set when it differs from both the defaults and an all-zero one (a payload
   built from zero rather than from dsd_user_config_init()). */
static int
airspy_keys_are_set(const dsdneoUserConfig* patch, const dsdneoUserConfig* defaults) {
    static const dsd_airspy_config k_zero_airspy = {0};
    return airspy_config_differs(&patch->airspy, &defaults->airspy)
           && airspy_config_differs(&patch->airspy, &k_zero_airspy);
}

/* The RTL-SDR a patch names is one choice, rtl_serial over rtl_device, by the keys it names (index 0 and an empty serial
   are the defaults): a serial when it names rtl_serial (an empty one selects by index, a refused one stays refused),
   else the index when it names rtl_device, which drops the serial the retained [input] held. A patch naming neither
   (tuning alone) keeps the retained choice. The index a patch does not name is kept, for a serial selection to fall
   back on once cleared. A key counts as named when its value is not the default, as every other [input] key does, so a
   config a C caller filled in (dsd_app_command_apply_config()) names its index without the loader's flags; the flags
   say a default value was named (`rtl_device = 0`, `rtl_serial = ""`). */
static void
merge_rtl_selector(dsdneoUserConfig* merged, const dsdneoUserConfig* patch, const dsdneoUserConfig* defaults) {
    const int names_device = patch->rtl_device_is_set || patch->rtl_device != defaults->rtl_device;
    const int names_serial = patch->rtl_serial_is_set || patch->rtl_serial[0] != '\0' || patch->rtl_serial_invalid;
    if (!names_serial && !names_device) {
        return;
    }
    if (names_device) {
        merged->rtl_device = patch->rtl_device;
        merged->rtl_device_is_set = 1;
    }
    if (names_serial) {
        DSD_MEMCPY(merged->rtl_serial, patch->rtl_serial, sizeof merged->rtl_serial);
        merged->rtl_serial_invalid = patch->rtl_serial_invalid;
    } else {
        DSD_MEMSET(merged->rtl_serial, 0, sizeof merged->rtl_serial);
        merged->rtl_serial_invalid = 0;
    }
    merged->rtl_serial_is_set = 1;
}

/* @p merged (holding the retained [input]) takes every key @p patch sets. */
static void
merge_input_keys(dsdneoUserConfig* merged, const dsdneoUserConfig* patch, const dsdneoUserConfig* defaults) {
    unsigned char* to = (unsigned char*)merged;
    const unsigned char* from = (const unsigned char*)patch;
    for (size_t i = 0; i < sizeof k_input_keys / sizeof k_input_keys[0]; i++) {
        const input_key* key = &k_input_keys[i];
        if (!input_key_is_set(key, from, (const unsigned char*)defaults)) {
            continue;
        }
        DSD_MEMCPY(to + key->offset, from + key->offset, key->size);
        if (key->flag_offset != 0U) {
            DSD_MEMCPY(to + key->flag_offset, from + key->flag_offset, sizeof(int));
        }
    }
    merge_rtl_selector(merged, patch, defaults);
    if (airspy_keys_are_set(patch, defaults)) {
        merged->airspy = patch->airspy;
    }
    merged->has_input = 1;
}

int
dsd_user_config_retained_input_patched(const dsd_state* state, const dsdneoUserConfig* patch, dsdneoUserConfig* out) {
    if (state == NULL || patch == NULL || out == NULL || !patch->has_input || retained_input_of(state) == NULL) {
        return 0;
    }
    /* The defaults a patch is built on. */
    dsdneoUserConfig* defaults = (dsdneoUserConfig*)malloc(sizeof *defaults);
    if (defaults == NULL) {
        return -1;
    }
    dsd_user_config_init(defaults);
    DSD_MEMSET(out, 0, sizeof *out);
    (void)dsd_user_config_retained_input(state, out);
    merge_input_keys(out, patch, defaults);
    free(defaults);
    return 1;
}

int
dsd_user_config_merge_retained_input(dsd_state* state, const dsdneoUserConfig* patch) {
    if (state == NULL) {
        return -1;
    }
    if (patch == NULL || !patch->has_input) {
        return 0;
    }
    if (retained_input_of(state) == NULL) {
        return dsd_user_config_retain_input(state, patch);
    }
    dsdneoUserConfig* merged = (dsdneoUserConfig*)malloc(sizeof *merged);
    if (merged == NULL) {
        return -1;
    }
    int rc = dsd_user_config_retained_input_patched(state, patch, merged);
    if (rc == 1) {
        rc = dsd_user_config_retain_input(state, merged);
    }
    DSD_SECURE_ZERO(merged, sizeof *merged);
    free(merged);
    return rc;
}

void
dsd_user_config_forget_retained_input(dsd_state* state) {
    if (retained_input_of(state) != NULL) {
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT, NULL, NULL);
    }
}

int
dsd_user_config_retained_input(const dsd_state* state, dsdneoUserConfig* cfg) {
    const retained_input* kept =
        (const retained_input*)dsd_state_ext_get_const(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT);
    if (kept == NULL || cfg == NULL) {
        return 0;
    }
    DSD_MEMCPY((unsigned char*)cfg + RETAINED_INPUT_BEGIN, kept->input, sizeof kept->input);
    return 1;
}

void
dsd_user_config_copy_retained_input(dsd_state* dst, const dsd_state* src) {
    if (dst == NULL || dst == src) {
        return;
    }
    const retained_input* from = retained_input_of(src);
    if (from == NULL) {
        dsd_user_config_forget_retained_input(dst);
        return;
    }
    const retained_input* held = retained_input_of(dst);
    if (held != NULL && held->revision == from->revision) {
        return;
    }
    retained_input* to = retained_input_slot(dst);
    if (to != NULL) {
        DSD_MEMCPY(to, from, sizeof *to);
    }
}
