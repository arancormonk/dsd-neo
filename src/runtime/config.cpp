// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime configuration parser for environment-derived settings.
 *
 * Parses environment variables into a typed `dsd-neoRuntimeConfig` and exposes
 * an immutable accessor. Intended to be called early during application init.
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <errno.h>
#include <limits.h>
#include <limits>
#include <mutex>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

namespace {

struct config_snapshot_node {
    dsdneoRuntimeConfig cfg;
    struct config_snapshot_node* next;
};

} // namespace

/* Publish-side lock and append-only snapshot list.
 * Snapshots are intentionally retained for process lifetime because the API
 * returns raw pointers that callers may cache and share across threads.
 * To avoid growth on repeated republishes of identical values, publishing
 * reuses any matching existing snapshot. */
static std::mutex g_config_mu;
static struct config_snapshot_node* g_config_head = NULL;
static struct config_snapshot_node* g_config_tail = NULL;
static std::atomic<const dsdneoRuntimeConfig*> g_config_active{NULL};

template <typename T>
static inline bool
config_scalar_equals(const T& lhs, const T& rhs) {
    return lhs == rhs;
}

static inline bool
config_scalar_equals(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= std::numeric_limits<float>::epsilon() || (std::isnan(lhs) && std::isnan(rhs));
}

static inline bool
config_scalar_equals(double lhs, double rhs) {
    return std::fabs(lhs - rhs) <= std::numeric_limits<double>::epsilon() || (std::isnan(lhs) && std::isnan(rhs));
}

/* Keep this in sync with dsdneoRuntimeConfig in config.h.
 * Explicit field comparison avoids struct-wide memcmp on padded objects. */
#define CONFIG_EQ_FIELD(name)                                                                                          \
    do {                                                                                                               \
        if (!config_scalar_equals(lhs.name, rhs.name)) {                                                               \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)
#define CONFIG_EQ_ARRAY(name)                                                                                          \
    do {                                                                                                               \
        if (memcmp(lhs.name, rhs.name, sizeof(lhs.name)) != 0) {                                                       \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

static inline bool
config_snapshot_equals_block_a(const dsdneoRuntimeConfig& lhs, const dsdneoRuntimeConfig& rhs) {
    CONFIG_EQ_FIELD(dmr_hangtime_s);
    CONFIG_EQ_FIELD(dmr_grant_timeout_s);
    CONFIG_EQ_FIELD(p25_hangtime_s);
    CONFIG_EQ_FIELD(p25_grant_timeout_s);
    CONFIG_EQ_FIELD(p25_cc_grace_s);
    CONFIG_EQ_FIELD(p25_vc_grace_s);
    CONFIG_EQ_FIELD(p25_mac_hold_s);
    CONFIG_EQ_FIELD(p25_voice_hold_s);
    CONFIG_EQ_FIELD(p25_min_follow_dwell_s);
    CONFIG_EQ_FIELD(p25_grant_voice_to_s);
    CONFIG_EQ_FIELD(p25_force_release_extra_s);
    CONFIG_EQ_FIELD(p25_force_release_margin_s);
    CONFIG_EQ_FIELD(p25p1_err_hold_pct);
    CONFIG_EQ_FIELD(p25p1_err_hold_s);
    CONFIG_EQ_FIELD(p25_afc_status_gate_is_set);
    CONFIG_EQ_FIELD(p25_afc_status_gate_enable);
    CONFIG_EQ_FIELD(input_warn_db);
    CONFIG_EQ_FIELD(tuner_autogain_seed_db);
    CONFIG_EQ_FIELD(tuner_autogain_spec_snr_db);
    CONFIG_EQ_FIELD(tuner_autogain_inband_ratio);
    CONFIG_EQ_FIELD(tuner_autogain_up_step_db);
    CONFIG_EQ_FIELD(auto_ppm_snr_db);
    CONFIG_EQ_FIELD(auto_ppm_pwr_db);
    CONFIG_EQ_FIELD(auto_ppm_zerolock_ppm);
    CONFIG_EQ_FIELD(costas_loop_bw);
    CONFIG_EQ_FIELD(costas_damping);
    CONFIG_EQ_FIELD(dmr_t3_step_hz);
    CONFIG_EQ_FIELD(dmr_t3_cc_freq_hz);
    CONFIG_EQ_FIELD(dmr_t3_cc_lcn);
    CONFIG_EQ_FIELD(dmr_t3_start_lcn);
    CONFIG_EQ_FIELD(rt_sched_is_set);
    CONFIG_EQ_FIELD(rt_sched_enable);
    CONFIG_EQ_FIELD(rt_prio_usb_is_set);
    CONFIG_EQ_FIELD(rt_prio_usb);
    CONFIG_EQ_FIELD(rt_prio_dongle_is_set);
    CONFIG_EQ_FIELD(rt_prio_dongle);
    CONFIG_EQ_FIELD(rt_prio_demod_is_set);
    CONFIG_EQ_FIELD(rt_prio_demod);
    CONFIG_EQ_FIELD(cpu_usb_is_set);
    CONFIG_EQ_FIELD(cpu_usb);
    CONFIG_EQ_FIELD(cpu_dongle_is_set);
    CONFIG_EQ_FIELD(cpu_dongle);
    CONFIG_EQ_FIELD(cpu_demod_is_set);
    CONFIG_EQ_FIELD(cpu_demod);
    return true;
}

static inline bool
config_snapshot_equals_block_b(const dsdneoRuntimeConfig& lhs, const dsdneoRuntimeConfig& rhs) {
    CONFIG_EQ_FIELD(ftz_daz_is_set);
    CONFIG_EQ_FIELD(ftz_daz_enable);
    CONFIG_EQ_FIELD(no_bootstrap_is_set);
    CONFIG_EQ_FIELD(no_bootstrap_enable);
    CONFIG_EQ_FIELD(no_signal_handlers_is_set);
    CONFIG_EQ_FIELD(no_signal_handlers_enable);
    CONFIG_EQ_FIELD(debug_sync_is_set);
    CONFIG_EQ_FIELD(debug_sync_enable);
    CONFIG_EQ_FIELD(debug_cqpsk_is_set);
    CONFIG_EQ_FIELD(debug_cqpsk_enable);
    CONFIG_EQ_FIELD(debug_symbol_timing_is_set);
    CONFIG_EQ_FIELD(debug_symbol_timing);
    CONFIG_EQ_FIELD(cqpsk_is_set);
    CONFIG_EQ_FIELD(cqpsk_enable);
    CONFIG_EQ_FIELD(cqpsk_sync_inv_is_set);
    CONFIG_EQ_FIELD(cqpsk_sync_inv);
    CONFIG_EQ_FIELD(cqpsk_sync_neg_is_set);
    CONFIG_EQ_FIELD(cqpsk_sync_neg);
    CONFIG_EQ_FIELD(sync_warmstart_is_set);
    CONFIG_EQ_FIELD(sync_warmstart_enable);
    CONFIG_EQ_FIELD(dmr_hangtime_is_set);
    CONFIG_EQ_FIELD(dmr_grant_timeout_is_set);
    CONFIG_EQ_FIELD(p25_hangtime_is_set);
    CONFIG_EQ_FIELD(p25_grant_timeout_is_set);
    CONFIG_EQ_FIELD(p25_cc_grace_is_set);
    CONFIG_EQ_FIELD(p25_vc_grace_is_set);
    CONFIG_EQ_FIELD(p25_mac_hold_is_set);
    CONFIG_EQ_FIELD(p25_voice_hold_is_set);
    CONFIG_EQ_FIELD(p25_wd_ms_is_set);
    CONFIG_EQ_FIELD(p25_wd_ms);
    CONFIG_EQ_FIELD(p25_min_follow_dwell_is_set);
    CONFIG_EQ_FIELD(p25_grant_voice_to_is_set);
    CONFIG_EQ_FIELD(p25_force_release_extra_is_set);
    CONFIG_EQ_FIELD(p25_force_release_margin_is_set);
    CONFIG_EQ_FIELD(p25p1_err_hold_pct_is_set);
    CONFIG_EQ_FIELD(p25p1_err_hold_s_is_set);
    CONFIG_EQ_FIELD(p25_afc_status_gate_is_set);
    CONFIG_EQ_FIELD(p25_afc_status_gate_enable);
    CONFIG_EQ_FIELD(p25_soft_erasure_threshold_is_set);
    CONFIG_EQ_FIELD(p25_soft_erasure_threshold);
    CONFIG_EQ_FIELD(p25p1_soft_erasure_threshold_is_set);
    CONFIG_EQ_FIELD(p25p1_soft_erasure_threshold);
    CONFIG_EQ_FIELD(p25p2_soft_erasure_threshold_is_set);
    CONFIG_EQ_FIELD(p25p2_soft_erasure_threshold);
    CONFIG_EQ_FIELD(p25_soft_hard_override_is_set);
    CONFIG_EQ_FIELD(p25_soft_hard_override_enable);
    CONFIG_EQ_FIELD(input_volume_is_set);
    CONFIG_EQ_FIELD(input_volume_multiplier);
    CONFIG_EQ_FIELD(input_warn_db_is_set);
    CONFIG_EQ_FIELD(dmr_t3_calc_csv_is_set);
    CONFIG_EQ_FIELD(dmr_t3_step_hz_is_set);
    CONFIG_EQ_FIELD(dmr_t3_cc_freq_is_set);
    CONFIG_EQ_FIELD(dmr_t3_cc_lcn_is_set);
    CONFIG_EQ_FIELD(dmr_t3_start_lcn_is_set);
    return true;
}

static inline bool
config_snapshot_equals_block_c(const dsdneoRuntimeConfig& lhs, const dsdneoRuntimeConfig& rhs) {
    CONFIG_EQ_FIELD(dmr_t3_heur_is_set);
    CONFIG_EQ_FIELD(dmr_t3_heur_enable);
    CONFIG_EQ_FIELD(config_path_is_set);
    CONFIG_EQ_FIELD(cache_dir_is_set);
    CONFIG_EQ_FIELD(cc_cache_is_set);
    CONFIG_EQ_FIELD(cc_cache_enable);
    CONFIG_EQ_FIELD(tcp_bufsz_is_set);
    CONFIG_EQ_FIELD(tcp_bufsz_bytes);
    CONFIG_EQ_FIELD(tcp_waitall_is_set);
    CONFIG_EQ_FIELD(tcp_waitall_enable);
    CONFIG_EQ_FIELD(tcp_autotune_is_set);
    CONFIG_EQ_FIELD(tcp_autotune_enable);
    CONFIG_EQ_FIELD(tcp_stats_is_set);
    CONFIG_EQ_FIELD(tcp_stats_enable);
    CONFIG_EQ_FIELD(tcp_max_timeouts_is_set);
    CONFIG_EQ_FIELD(tcp_max_timeouts);
    CONFIG_EQ_FIELD(tcp_rcvbuf_is_set);
    CONFIG_EQ_FIELD(tcp_rcvbuf_bytes);
    CONFIG_EQ_FIELD(tcp_rcvtimeo_is_set);
    CONFIG_EQ_FIELD(tcp_rcvtimeo_ms);
    CONFIG_EQ_FIELD(rigctl_rcvtimeo_is_set);
    CONFIG_EQ_FIELD(rigctl_rcvtimeo_ms);
    CONFIG_EQ_FIELD(tcp_prebuf_ms_is_set);
    CONFIG_EQ_FIELD(tcp_prebuf_ms);
    CONFIG_EQ_FIELD(rtl_agc_is_set);
    CONFIG_EQ_FIELD(rtl_agc_enable);
    CONFIG_EQ_FIELD(rtl_direct_is_set);
    CONFIG_EQ_FIELD(rtl_direct_mode);
    CONFIG_EQ_FIELD(rtl_offset_tuning_is_set);
    CONFIG_EQ_FIELD(rtl_offset_tuning_enable);
    CONFIG_EQ_FIELD(rtl_xtal_hz_is_set);
    CONFIG_EQ_FIELD(rtl_xtal_hz);
    CONFIG_EQ_FIELD(tuner_xtal_hz_is_set);
    CONFIG_EQ_FIELD(tuner_xtal_hz);
    CONFIG_EQ_FIELD(rtl_testmode_is_set);
    CONFIG_EQ_FIELD(rtl_testmode_enable);
    CONFIG_EQ_FIELD(rtl_if_gains_is_set);
    CONFIG_EQ_FIELD(rtl_verify_is_set);
    CONFIG_EQ_FIELD(rtl_verify_enable);
    CONFIG_EQ_FIELD(rtl_verify_attempts_is_set);
    CONFIG_EQ_FIELD(rtl_verify_attempts);
    CONFIG_EQ_FIELD(tuner_bw_hz_is_set);
    CONFIG_EQ_FIELD(tuner_bw_hz);
    CONFIG_EQ_FIELD(tuner_autogain_is_set);
    CONFIG_EQ_FIELD(tuner_autogain_enable);
    CONFIG_EQ_FIELD(tuner_autogain_probe_ms_is_set);
    CONFIG_EQ_FIELD(tuner_autogain_probe_ms);
    CONFIG_EQ_FIELD(tuner_autogain_seed_db_is_set);
    CONFIG_EQ_FIELD(tuner_autogain_spec_snr_db_is_set);
    CONFIG_EQ_FIELD(tuner_autogain_inband_ratio_is_set);
    CONFIG_EQ_FIELD(tuner_autogain_up_step_db_is_set);
    CONFIG_EQ_FIELD(tuner_autogain_up_persist_is_set);
    CONFIG_EQ_FIELD(tuner_autogain_up_persist);
    CONFIG_EQ_FIELD(auto_ppm_is_set);
    CONFIG_EQ_FIELD(auto_ppm_enable);
    CONFIG_EQ_FIELD(auto_ppm_snr_db_is_set);
    CONFIG_EQ_FIELD(auto_ppm_pwr_db_is_set);
    CONFIG_EQ_FIELD(auto_ppm_zerolock_ppm_is_set);
    CONFIG_EQ_FIELD(auto_ppm_zerolock_hz_is_set);
    CONFIG_EQ_FIELD(auto_ppm_zerolock_hz);
    CONFIG_EQ_FIELD(auto_ppm_freeze_is_set);
    CONFIG_EQ_FIELD(auto_ppm_freeze_enable);
    return true;
}

static inline bool
config_snapshot_equals_block_d(const dsdneoRuntimeConfig& lhs, const dsdneoRuntimeConfig& rhs) {
    CONFIG_EQ_FIELD(resamp_is_set);
    CONFIG_EQ_FIELD(resamp_disable);
    CONFIG_EQ_FIELD(resamp_target_hz);
    CONFIG_EQ_FIELD(costas_bw_is_set);
    CONFIG_EQ_FIELD(costas_damping_is_set);
    CONFIG_EQ_FIELD(ted_gain_is_set);
    CONFIG_EQ_FIELD(ted_gain);
    CONFIG_EQ_FIELD(deemph_is_set);
    CONFIG_EQ_FIELD(deemph_mode);
    CONFIG_EQ_FIELD(audio_lpf_is_set);
    CONFIG_EQ_FIELD(audio_lpf_disable);
    CONFIG_EQ_FIELD(audio_lpf_cutoff_hz);
    CONFIG_EQ_FIELD(mt_is_set);
    CONFIG_EQ_FIELD(mt_enable);
    CONFIG_EQ_FIELD(combine_rot_is_set);
    CONFIG_EQ_FIELD(combine_rot);
    CONFIG_EQ_FIELD(fs4_shift_disable_is_set);
    CONFIG_EQ_FIELD(fs4_shift_disable);
    CONFIG_EQ_FIELD(output_clear_on_retune_is_set);
    CONFIG_EQ_FIELD(output_clear_on_retune);
    CONFIG_EQ_FIELD(retune_drain_ms_is_set);
    CONFIG_EQ_FIELD(retune_drain_ms);
    CONFIG_EQ_FIELD(retune_mute_ms_is_set);
    CONFIG_EQ_FIELD(retune_mute_ms);
    CONFIG_EQ_FIELD(tcpin_backoff_ms_is_set);
    CONFIG_EQ_FIELD(tcpin_backoff_ms);
    CONFIG_EQ_FIELD(window_freeze_is_set);
    CONFIG_EQ_FIELD(window_freeze);
    CONFIG_EQ_FIELD(pdu_json_is_set);
    CONFIG_EQ_FIELD(pdu_json_enable);
    CONFIG_EQ_FIELD(snr_sql_is_set);
    CONFIG_EQ_FIELD(snr_sql_db);
    CONFIG_EQ_FIELD(iq_dc_block_is_set);
    CONFIG_EQ_FIELD(iq_dc_block_enable);
    CONFIG_EQ_FIELD(iq_dc_shift_is_set);
    CONFIG_EQ_FIELD(iq_dc_shift);
    CONFIG_EQ_FIELD(channel_lpf_is_set);
    CONFIG_EQ_FIELD(channel_lpf_enable);
    CONFIG_EQ_ARRAY(dmr_t3_calc_csv);
    CONFIG_EQ_ARRAY(config_path);
    CONFIG_EQ_ARRAY(cache_dir);
    CONFIG_EQ_ARRAY(rtl_if_gains);
    return true;
}

static inline bool
config_snapshot_equals(const dsdneoRuntimeConfig& lhs, const dsdneoRuntimeConfig& rhs) {
    return config_snapshot_equals_block_a(lhs, rhs) && config_snapshot_equals_block_b(lhs, rhs)
           && config_snapshot_equals_block_c(lhs, rhs) && config_snapshot_equals_block_d(lhs, rhs);
}

#undef CONFIG_EQ_FIELD
#undef CONFIG_EQ_ARRAY

static inline struct config_snapshot_node*
find_matching_snapshot_locked(const dsdneoRuntimeConfig& cfg) {
    for (struct config_snapshot_node* node = g_config_head; node; node = node->next) {
        if (config_snapshot_equals(node->cfg, cfg)) {
            return node;
        }
    }
    return NULL;
}

static inline void
publish_config_locked(const dsdneoRuntimeConfig& cfg) {
    const dsdneoRuntimeConfig* active = g_config_active.load(std::memory_order_acquire);
    if (active && config_snapshot_equals(*active, cfg)) {
        return;
    }

    struct config_snapshot_node* matched = find_matching_snapshot_locked(cfg);
    if (matched) {
        g_config_active.store(&matched->cfg, std::memory_order_release);
        return;
    }

    struct config_snapshot_node* node = static_cast<struct config_snapshot_node*>(malloc(sizeof(*node)));
    if (!node) {
        return;
    }
    node->cfg = cfg;
    node->next = NULL;

    if (g_config_tail) {
        g_config_tail->next = node;
    } else {
        g_config_head = node;
    }
    g_config_tail = node;
    g_config_active.store(&node->cfg, std::memory_order_release);
}

/**
 * @brief Check whether an environment string is set and non-empty.
 *
 * @param v Environment value string pointer (may be NULL).
 * @return 1 if set and non-empty; otherwise 0.
 */
static int
env_is_set(const char* v) {
    return v && v[0] != '\0';
}

/* A whole boolean word (on/off, 1/0, true/false, yes/no) decides first; otherwise the first character does, as it
   always has (`y...`, `t...`, `n...`, `f...`). The first character alone cannot tell "on" from "off". */
static int
env_is_truthy(const char* v) {
    if (!env_is_set(v)) {
        return 0;
    }
    int word = 0;
    if (dsd_parse_bool_strict(v, &word) == 0) {
        return word == 1;
    }
    return (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T');
}

static int
env_is_falsey(const char* v) {
    if (!env_is_set(v)) {
        return 0;
    }
    int word = 0;
    if (dsd_parse_bool_strict(v, &word) == 0) {
        return word == 0;
    }
    return (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

static int
env_parse_int_range(const char* v, int min_val, int max_val, int* out) {
    if (!env_is_set(v) || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || (end && *end != '\0') || errno == ERANGE) {
        return 0;
    }
    if (x < (long)min_val || x > (long)max_val) {
        return 0;
    }
    *out = (int)x;
    return 1;
}

static int
env_parse_int_strict(const char* v, int* out) {
    if (!env_is_set(v) || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || (end && *end != '\0') || errno == ERANGE || x < INT_MIN || x > INT_MAX) {
        return 0;
    }
    *out = (int)x;
    return 1;
}

static int
env_parse_long_strict(const char* v, long* out) {
    if (!env_is_set(v) || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || (end && *end != '\0') || errno == ERANGE) {
        return 0;
    }
    *out = x;
    return 1;
}

static int
env_parse_long_range(const char* v, long min_val, long max_val, long* out) {
    if (!env_is_set(v) || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || (end && *end != '\0') || errno == ERANGE) {
        return 0;
    }
    if (x < min_val || x > max_val) {
        return 0;
    }
    *out = x;
    return 1;
}

static int
env_parse_double_strict(const char* v, double* out) {
    if (!env_is_set(v) || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    double x = strtod(v, &end);
    if (end == v || (end && *end != '\0') || errno == ERANGE) {
        return 0;
    }
    *out = x;
    return 1;
}

static int
env_parse_double_range(const char* v, double min_val, double max_val, double* out) {
    if (!env_is_set(v) || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    double x = strtod(v, &end);
    if (end == v || (end && *end != '\0') || errno == ERANGE) {
        return 0;
    }
    if (x < min_val || x > max_val) {
        return 0;
    }
    *out = x;
    return 1;
}

static void
env_copy_str(char* dst, size_t dst_size, const char* v) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!env_is_set(v)) {
        dst[0] = '\0';
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", v);
    dst[dst_size - 1] = '\0';
}

static void
default_cache_dir(char* dst, size_t dst_size) {
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';

    const dsd_env_value home("HOME");
    const char* base = home.c_str();
#if defined(_WIN32)
    const dsd_env_value local_app_data("LOCALAPPDATA");
    if (!env_is_set(base)) {
        base = local_app_data.c_str();
    }
#endif
    if (env_is_set(base)) {
        DSD_SNPRINTF(dst, dst_size, "%s/.cache/dsd-neo", base);
        dst[dst_size - 1] = '\0';
        return;
    }

    DSD_SNPRINTF(dst, dst_size, "%s", ".dsdneo_cache");
    dst[dst_size - 1] = '\0';
}

static void
config_init_defaults(dsdneoRuntimeConfig& c) {
    /* Defaults for centralized knobs (may be overridden by env parsing below). */
    c.sync_warmstart_enable = 1;
    c.combine_rot = 1;

    c.dmr_hangtime_s = 2.0;
    c.dmr_grant_timeout_s = 4.0;

    c.p25_hangtime_s = 2.0;
    c.p25_grant_timeout_s = 3.0;
    c.p25_cc_grace_s = 5.0;
    c.p25_vc_grace_s = 0.75;
    c.p25_mac_hold_s = 0.75;
    c.p25_voice_hold_s = 0.75;
    c.p25_wd_ms = 0; /* 0 => dynamic default selected by caller */

    c.input_volume_multiplier = 1;
    c.input_warn_db = -40.0;

    c.dmr_t3_step_hz = 0;
    c.dmr_t3_cc_freq_hz = 0;
    c.dmr_t3_cc_lcn = 0;
    c.dmr_t3_start_lcn = 1;

    c.cc_cache_enable = 1;

    c.tcp_max_timeouts = 3;
    c.tcp_rcvbuf_bytes = 4 * 1024 * 1024;
    c.tcp_rcvtimeo_ms = 2000;
    c.rigctl_rcvtimeo_ms = 1500;
    c.tcp_prebuf_ms = 1000;

    c.rtl_agc_enable = 1;
    c.rtl_direct_mode = 0;
    c.rtl_verify_enable = 1;
    c.rtl_verify_attempts = 10;

    c.tuner_autogain_probe_ms = 3000;
    c.tuner_autogain_seed_db = 30.0;
    c.tuner_autogain_spec_snr_db = 6.0;
    c.tuner_autogain_inband_ratio = 0.60;
    c.tuner_autogain_up_step_db = 3.0;
    c.tuner_autogain_up_persist = 2;

    c.auto_ppm_snr_db = 6.0;
    c.auto_ppm_pwr_db = -80.0;
    c.auto_ppm_zerolock_ppm = 0.6;
    c.auto_ppm_zerolock_hz = 60;
    c.auto_ppm_freeze_enable = 1;
}

static void
config_init_paths_and_cache(dsdneoRuntimeConfig& c) {
    /* User config discovery */
    const dsd_env_value cfgp_value("DSD_NEO_CONFIG");
    const char* cfgp = cfgp_value.c_str();
    c.config_path_is_set = env_is_set(cfgp);
    env_copy_str(c.config_path, sizeof c.config_path, cfgp);

    /* Cache/path knobs */
    const dsd_env_value cache_dir_value("DSD_NEO_CACHE_DIR");
    const char* cache_dir = cache_dir_value.c_str();
    c.cache_dir_is_set = env_is_set(cache_dir);
    if (c.cache_dir_is_set) {
        env_copy_str(c.cache_dir, sizeof c.cache_dir, cache_dir);
    } else {
        default_cache_dir(c.cache_dir, sizeof c.cache_dir);
    }

    const dsd_env_value cc_cache_value("DSD_NEO_CC_CACHE");
    const char* cc_cache = cc_cache_value.c_str();
    c.cc_cache_is_set = env_is_set(cc_cache);
    if (c.cc_cache_is_set) {
        c.cc_cache_enable = env_is_falsey(cc_cache) ? 0 : 1;
    }
}

static void
config_init_realtime_and_cpu(dsdneoRuntimeConfig& c) {
    /* Realtime scheduling and CPU affinity */
    const dsd_env_value rt_value("DSD_NEO_RT_SCHED");
    const char* rt = rt_value.c_str();
    c.rt_sched_is_set = env_is_set(rt);
    c.rt_sched_enable = c.rt_sched_is_set ? (env_is_falsey(rt) ? 0 : 1) : 0;

    const dsd_env_value rpu_value("DSD_NEO_RT_PRIO_USB");
    const char* rpu = rpu_value.c_str();
    c.rt_prio_usb_is_set = env_parse_int_range(rpu, 1, 99, &c.rt_prio_usb);

    const dsd_env_value rpd_value("DSD_NEO_RT_PRIO_DONGLE");
    const char* rpd = rpd_value.c_str();
    c.rt_prio_dongle_is_set = env_parse_int_range(rpd, 1, 99, &c.rt_prio_dongle);

    const dsd_env_value rpm_value("DSD_NEO_RT_PRIO_DEMOD");
    const char* rpm = rpm_value.c_str();
    c.rt_prio_demod_is_set = env_parse_int_range(rpm, 1, 99, &c.rt_prio_demod);

    const dsd_env_value cpuu_value("DSD_NEO_CPU_USB");
    const char* cpuu = cpuu_value.c_str();
    c.cpu_usb_is_set = env_parse_int_range(cpuu, 0, 4096, &c.cpu_usb);

    const dsd_env_value cpud_value("DSD_NEO_CPU_DONGLE");
    const char* cpud = cpud_value.c_str();
    c.cpu_dongle_is_set = env_parse_int_range(cpud, 0, 4096, &c.cpu_dongle);

    const dsd_env_value cpum_value("DSD_NEO_CPU_DEMOD");
    const char* cpum = cpum_value.c_str();
    c.cpu_demod_is_set = env_parse_int_range(cpum, 0, 4096, &c.cpu_demod);
}

static void
config_init_bootstrap_and_debug(dsdneoRuntimeConfig& c) {
    /* Bootstrap/system toggles */
    const dsd_env_value ftz_value("DSD_NEO_FTZ_DAZ");
    const char* ftz = ftz_value.c_str();
    c.ftz_daz_is_set = env_is_set(ftz);
    c.ftz_daz_enable = c.ftz_daz_is_set ? (env_is_falsey(ftz) ? 0 : 1) : 0;

    const dsd_env_value nb_value("DSD_NEO_NO_BOOTSTRAP");
    const char* nb = nb_value.c_str();
    c.no_bootstrap_is_set = env_is_set(nb);
    c.no_bootstrap_enable = c.no_bootstrap_is_set ? (env_is_falsey(nb) ? 0 : 1) : 0;

    const dsd_env_value nsh_value("DSD_NEO_NO_SIGNAL_HANDLERS");
    const char* nsh = nsh_value.c_str();
    c.no_signal_handlers_is_set = env_is_set(nsh);
    c.no_signal_handlers_enable = c.no_signal_handlers_is_set ? (env_is_falsey(nsh) ? 0 : 1) : 0;

    /* Debug/tuning toggles */
    const dsd_env_value ds_value("DSD_NEO_DEBUG_SYNC");
    const char* ds = ds_value.c_str();
    c.debug_sync_is_set = env_is_set(ds);
    c.debug_sync_enable = c.debug_sync_is_set ? (env_is_falsey(ds) ? 0 : 1) : 0;

    const dsd_env_value dcq_value("DSD_NEO_DEBUG_CQPSK");
    const char* dcq = dcq_value.c_str();
    c.debug_cqpsk_is_set = env_is_set(dcq);
    c.debug_cqpsk_enable = c.debug_cqpsk_is_set ? (env_is_falsey(dcq) ? 0 : 1) : 0;

    /* A level, so "2" selects the per-sample trace rather than reading as a plain truthy 1. */
    const dsd_env_value dst_value("DSD_NEO_DEBUG_SYMBOL_TIMING");
    const char* dst = dst_value.c_str();
    int symbol_timing_level = 0;
    if (env_parse_int_range(dst, 0, DSD_NEO_SYMBOL_TIMING_TRACE, &symbol_timing_level)) {
        c.debug_symbol_timing_is_set = 1;
        c.debug_symbol_timing = symbol_timing_level;
    } else if (env_is_truthy(dst)) {
        c.debug_symbol_timing_is_set = 1;
        c.debug_symbol_timing = DSD_NEO_SYMBOL_TIMING_SYNC_LINE;
    } else if (env_is_falsey(dst)) {
        c.debug_symbol_timing_is_set = 1;
        c.debug_symbol_timing = DSD_NEO_SYMBOL_TIMING_OFF;
    }
}

static void
config_init_cqpsk_toggle(dsdneoRuntimeConfig& c) {
    const dsd_env_value cq_value("DSD_NEO_CQPSK");
    const char* cq = cq_value.c_str();
    c.cqpsk_is_set = env_is_set(cq);
    if (c.cqpsk_is_set) {
        if (env_is_truthy(cq)) {
            c.cqpsk_enable = 1;
        } else if (env_is_falsey(cq)) {
            c.cqpsk_enable = 0;
        } else {
            c.cqpsk_is_set = 0; /* ignore unrecognized values */
        }
    }
}

static void
config_init_cqpsk_sync(dsdneoRuntimeConfig& c) {
    const dsd_env_value cqi_value("DSD_NEO_CQPSK_SYNC_INV");
    const char* cqi = cqi_value.c_str();
    c.cqpsk_sync_inv_is_set = env_is_set(cqi);
    c.cqpsk_sync_inv = c.cqpsk_sync_inv_is_set ? (env_is_falsey(cqi) ? 0 : 1) : 0;

    const dsd_env_value cqn_value("DSD_NEO_CQPSK_SYNC_NEG");
    const char* cqn = cqn_value.c_str();
    c.cqpsk_sync_neg_is_set = env_is_set(cqn);
    c.cqpsk_sync_neg = c.cqpsk_sync_neg_is_set ? (env_is_falsey(cqn) ? 0 : 1) : 0;
}

static void
config_init_cqpsk(dsdneoRuntimeConfig& c) {
    /* CQPSK runtime toggles */
    config_init_cqpsk_toggle(c);
    config_init_cqpsk_sync(c);

    /* Keep the warm-start safety switch at the canonical calibration boundary. */
    const dsd_env_value warmstart_value("DSD_NEO_SYNC_WARMSTART");
    const char* warmstart = warmstart_value.c_str();
    c.sync_warmstart_is_set = env_is_set(warmstart);
    if (c.sync_warmstart_is_set && strcmp(warmstart, "0") == 0) {
        c.sync_warmstart_enable = 0;
    }
}

static void
config_init_dmr_and_p25(dsdneoRuntimeConfig& c) {
    /* DMR timers */
    const dsd_env_value dmr_hang_value("DSD_NEO_DMR_HANGTIME");
    const char* dmr_hang = dmr_hang_value.c_str();
    c.dmr_hangtime_is_set = env_parse_double_range(dmr_hang, 0.0, 10.0, &c.dmr_hangtime_s);

    const dsd_env_value dmr_gt_value("DSD_NEO_DMR_GRANT_TIMEOUT");
    const char* dmr_gt = dmr_gt_value.c_str();
    c.dmr_grant_timeout_is_set = env_parse_double_range(dmr_gt, 0.0, 30.0, &c.dmr_grant_timeout_s);

    /* P25 timers/holds */
    const dsd_env_value p25_hang_value("DSD_NEO_P25_HANGTIME");
    const char* p25_hang = p25_hang_value.c_str();
    c.p25_hangtime_is_set = env_parse_double_range(p25_hang, 0.0, 10.0, &c.p25_hangtime_s);

    const dsd_env_value p25_gt_value("DSD_NEO_P25_GRANT_TIMEOUT");
    const char* p25_gt = p25_gt_value.c_str();
    c.p25_grant_timeout_is_set = env_parse_double_range(p25_gt, 0.0, 30.0, &c.p25_grant_timeout_s);

    const dsd_env_value p25_cc_value("DSD_NEO_P25_CC_GRACE");
    const char* p25_cc = p25_cc_value.c_str();
    c.p25_cc_grace_is_set = env_parse_double_range(p25_cc, 0.0, 30.0, &c.p25_cc_grace_s);

    const dsd_env_value p25_vcg_value("DSD_NEO_P25_VC_GRACE");
    const char* p25_vcg = p25_vcg_value.c_str();
    c.p25_vc_grace_is_set = env_parse_double_range(p25_vcg, 0.0, 10.0, &c.p25_vc_grace_s);

    const dsd_env_value p25_mh_value("DSD_NEO_P25_MAC_HOLD");
    const char* p25_mh = p25_mh_value.c_str();
    c.p25_mac_hold_is_set = env_parse_double_range(p25_mh, 0.0, 10.0, &c.p25_mac_hold_s);

    const dsd_env_value p25_vh_value("DSD_NEO_P25_VOICE_HOLD");
    const char* p25_vh = p25_vh_value.c_str();
    c.p25_voice_hold_is_set = env_parse_double_range(p25_vh, 0.0, 5.0, &c.p25_voice_hold_s);

    const dsd_env_value p25_wd_value("DSD_NEO_P25_WD_MS");
    const char* p25_wd = p25_wd_value.c_str();
    c.p25_wd_ms_is_set = env_parse_int_range(p25_wd, 20, 2000, &c.p25_wd_ms);
}

static void
config_init_p25_follower_and_soft(dsdneoRuntimeConfig& c) {
    /* P25 follower (UI-exposed) knobs */
    const dsd_env_value p25_mfd_value("DSD_NEO_P25_MIN_FOLLOW_DWELL");
    const char* p25_mfd = p25_mfd_value.c_str();
    c.p25_min_follow_dwell_is_set = env_parse_double_range(p25_mfd, 0.0, 120.0, &c.p25_min_follow_dwell_s);

    const dsd_env_value p25_gvt_value("DSD_NEO_P25_GRANT_VOICE_TO");
    const char* p25_gvt = p25_gvt_value.c_str();
    c.p25_grant_voice_to_is_set = env_parse_double_range(p25_gvt, 0.0, 120.0, &c.p25_grant_voice_to_s);

    const dsd_env_value p25_fe_value("DSD_NEO_P25_FORCE_RELEASE_EXTRA");
    const char* p25_fe = p25_fe_value.c_str();
    c.p25_force_release_extra_is_set = env_parse_double_range(p25_fe, 0.0, 120.0, &c.p25_force_release_extra_s);

    const dsd_env_value p25_fm_value("DSD_NEO_P25_FORCE_RELEASE_MARGIN");
    const char* p25_fm = p25_fm_value.c_str();
    c.p25_force_release_margin_is_set = env_parse_double_range(p25_fm, 0.0, 120.0, &c.p25_force_release_margin_s);

    const dsd_env_value p25_ehp_value("DSD_NEO_P25P1_ERR_HOLD_PCT");
    const char* p25_ehp = p25_ehp_value.c_str();
    c.p25p1_err_hold_pct_is_set = env_parse_double_range(p25_ehp, 0.0, 100.0, &c.p25p1_err_hold_pct);

    const dsd_env_value p25_ehs_value("DSD_NEO_P25P1_ERR_HOLD_S");
    const char* p25_ehs = p25_ehs_value.c_str();
    c.p25p1_err_hold_s_is_set = env_parse_double_range(p25_ehs, 0.0, 120.0, &c.p25p1_err_hold_s);

    const dsd_env_value p25_afc_status_gate_value("DSD_NEO_P25_AFC_STATUS_GATE");
    const char* p25_afc_status_gate = p25_afc_status_gate_value.c_str();
    c.p25_afc_status_gate_is_set = env_is_set(p25_afc_status_gate);
    c.p25_afc_status_gate_enable = c.p25_afc_status_gate_is_set ? (env_is_truthy(p25_afc_status_gate) ? 1 : 0) : 0;

    const dsd_env_value p25_soft_thr_value("DSD_NEO_P25_SOFT_ERASURE_THRESHOLD");
    const char* p25_soft_thr = p25_soft_thr_value.c_str();
    c.p25_soft_erasure_threshold_is_set = env_parse_int_range(p25_soft_thr, 0, 255, &c.p25_soft_erasure_threshold);

    const dsd_env_value p25p1_soft_thr_value("DSD_NEO_P25P1_SOFT_ERASURE_THRESHOLD");
    const char* p25p1_soft_thr = p25p1_soft_thr_value.c_str();
    c.p25p1_soft_erasure_threshold_is_set =
        env_parse_int_range(p25p1_soft_thr, 0, 255, &c.p25p1_soft_erasure_threshold);

    const dsd_env_value p25p2_soft_thr_value("DSD_NEO_P25P2_SOFT_ERASURE_THRESHOLD");
    const char* p25p2_soft_thr = p25p2_soft_thr_value.c_str();
    c.p25p2_soft_erasure_threshold_is_set =
        env_parse_int_range(p25p2_soft_thr, 0, 255, &c.p25p2_soft_erasure_threshold);

    const dsd_env_value p25_soft_override_value("DSD_NEO_P25_SOFT_HARD_OVERRIDE");
    const char* p25_soft_override = p25_soft_override_value.c_str();
    c.p25_soft_hard_override_is_set = env_is_set(p25_soft_override);
    c.p25_soft_hard_override_enable = c.p25_soft_hard_override_is_set ? (env_is_falsey(p25_soft_override) ? 0 : 1) : 1;
}

static void
config_init_input_and_dmr_t3(dsdneoRuntimeConfig& c) {
    /* Input processing knobs */
    const dsd_env_value iv_value("DSD_NEO_INPUT_VOLUME");
    const char* iv = iv_value.c_str();
    c.input_volume_is_set = env_parse_int_range(iv, 1, 16, &c.input_volume_multiplier);

    const dsd_env_value iw_value("DSD_NEO_INPUT_WARN_DB");
    const char* iw = iw_value.c_str();
    c.input_warn_db_is_set = env_parse_double_range(iw, -200.0, 0.0, &c.input_warn_db);

    /* DMR TIII tools (one-shot LCN calculator) */
    const dsd_env_value t3csv_value("DSD_NEO_DMR_T3_CALC_CSV");
    const char* t3csv = t3csv_value.c_str();
    c.dmr_t3_calc_csv_is_set = env_is_set(t3csv);
    env_copy_str(c.dmr_t3_calc_csv, sizeof c.dmr_t3_calc_csv, t3csv);

    const dsd_env_value t3step_value("DSD_NEO_DMR_T3_STEP_HZ");
    const char* t3step = t3step_value.c_str();
    c.dmr_t3_step_hz_is_set = env_parse_long_range(t3step, 1, 1000000000L, &c.dmr_t3_step_hz);

    const dsd_env_value t3ccl_value("DSD_NEO_DMR_T3_CC_LCN");
    const char* t3ccl = t3ccl_value.c_str();
    c.dmr_t3_cc_lcn_is_set = env_parse_long_range(t3ccl, 1, 1000000000L, &c.dmr_t3_cc_lcn);

    const dsd_env_value t3start_value("DSD_NEO_DMR_T3_START_LCN");
    const char* t3start = t3start_value.c_str();
    c.dmr_t3_start_lcn_is_set = env_parse_long_range(t3start, 1, 1000000000L, &c.dmr_t3_start_lcn);

    const dsd_env_value t3ccf_value("DSD_NEO_DMR_T3_CC_FREQ");
    const char* t3ccf = t3ccf_value.c_str();
    c.dmr_t3_cc_freq_is_set = 0;
    if (env_is_set(t3ccf)) {
        double v = 0.0;
        if (env_parse_double_strict(t3ccf, &v)) {
            /* Values under 1e5 are MHz. Bounded before rounding: llround() of a value past long long's range is
               unspecified, and the result is a trunk frequency, so it must also fit DSD_TRUNK_FREQ_MAX_HZ. */
            const double hz = (v < 1e5) ? v * 1000000.0 : v;
            if (hz >= 0.5 && hz <= (double)DSD_TRUNK_FREQ_MAX_HZ) {
                c.dmr_t3_cc_freq_hz = (long)std::llround(hz);
                c.dmr_t3_cc_freq_is_set = 1;
            }
        }
    }

    /* DMR TIII heuristic fill */
    const dsd_env_value t3h_value("DSD_NEO_DMR_T3_HEUR");
    const char* t3h = t3h_value.c_str();
    c.dmr_t3_heur_is_set = env_is_set(t3h);
    c.dmr_t3_heur_enable = c.dmr_t3_heur_is_set ? (env_is_falsey(t3h) ? 0 : 1) : 0;
}

static void
config_init_tcp_and_rigctl(dsdneoRuntimeConfig& c) {
    /* TCP/rigctl knobs */
    const dsd_env_value tbs_value("DSD_NEO_TCP_BUFSZ");
    const char* tbs = tbs_value.c_str();
    c.tcp_bufsz_is_set = 0;
    if (env_is_set(tbs)) {
        long v = 0;
        if (env_parse_long_strict(tbs, &v) && v > 4096 && v < (32L * 1024L * 1024L)) {
            c.tcp_bufsz_bytes = (int)v;
            c.tcp_bufsz_is_set = 1;
        }
    }

    const dsd_env_value twa_value("DSD_NEO_TCP_WAITALL");
    const char* twa = twa_value.c_str();
    c.tcp_waitall_is_set = env_is_set(twa);
    if (c.tcp_waitall_is_set) {
        c.tcp_waitall_enable = env_is_falsey(twa) ? 0 : 1;
    }

    const dsd_env_value tau_value("DSD_NEO_TCP_AUTOTUNE");
    const char* tau = tau_value.c_str();
    c.tcp_autotune_is_set = env_is_set(tau);
    if (c.tcp_autotune_is_set) {
        c.tcp_autotune_enable = env_is_falsey(tau) ? 0 : 1;
    }

    const dsd_env_value tstats_value("DSD_NEO_TCP_STATS");
    const char* tstats = tstats_value.c_str();
    c.tcp_stats_is_set = env_is_set(tstats);
    if (c.tcp_stats_is_set) {
        c.tcp_stats_enable = env_is_falsey(tstats) ? 0 : 1;
    }

    const dsd_env_value tmt_value("DSD_NEO_TCP_MAX_TIMEOUTS");
    const char* tmt = tmt_value.c_str();
    c.tcp_max_timeouts_is_set = env_parse_int_range(tmt, 1, 100, &c.tcp_max_timeouts);

    const dsd_env_value trb_value("DSD_NEO_TCP_RCVBUF");
    const char* trb = trb_value.c_str();
    c.tcp_rcvbuf_is_set = 0;
    if (env_is_set(trb)) {
        long v = 0;
        if (env_parse_long_strict(trb, &v) && v > 0 && v <= INT_MAX) {
            c.tcp_rcvbuf_bytes = (int)v;
            c.tcp_rcvbuf_is_set = 1;
        }
    }

    const dsd_env_value trt_value("DSD_NEO_TCP_RCVTIMEO");
    const char* trt = trt_value.c_str();
    c.tcp_rcvtimeo_is_set = env_parse_int_range(trt, 100, 60000, &c.tcp_rcvtimeo_ms);

    const dsd_env_value rrt_value("DSD_NEO_RIGCTL_RCVTIMEO");
    const char* rrt = rrt_value.c_str();
    c.rigctl_rcvtimeo_is_set = env_parse_int_range(rrt, 100, 60000, &c.rigctl_rcvtimeo_ms);

    const dsd_env_value tpb_value("DSD_NEO_TCP_PREBUF_MS");
    const char* tpb = tpb_value.c_str();
    c.tcp_prebuf_ms_is_set = env_parse_int_range(tpb, 5, 1000, &c.tcp_prebuf_ms);
}

static int
config_parse_rtl_direct_mode(const char* rdir) {
    if (!rdir || rdir[0] == '\0') {
        return 0;
    }
    if (rdir[0] == '1' || rdir[0] == 'I' || rdir[0] == 'i') {
        return 1;
    }
    if (rdir[0] == '2' || rdir[0] == 'Q' || rdir[0] == 'q') {
        return 2;
    }
    if (rdir[0] == '0') {
        return 0;
    }
    int v = 0;
    if (env_parse_int_strict(rdir, &v) && v >= 0 && v <= 2) {
        return v;
    }
    return 0;
}

static void
config_init_rtl_agc_and_direct(dsdneoRuntimeConfig& c) {
    const dsd_env_value ragc_value("DSD_NEO_RTL_AGC");
    const char* ragc = ragc_value.c_str();
    c.rtl_agc_is_set = env_is_set(ragc);
    if (c.rtl_agc_is_set) {
        c.rtl_agc_enable = env_is_falsey(ragc) ? 0 : 1;
    }

    const dsd_env_value rdir_value("DSD_NEO_RTL_DIRECT");
    const char* rdir = rdir_value.c_str();
    c.rtl_direct_is_set = env_is_set(rdir);
    if (c.rtl_direct_is_set) {
        c.rtl_direct_mode = config_parse_rtl_direct_mode(rdir);
    }
}

static void
config_init_rtl_offset_xtal_testmode(dsdneoRuntimeConfig& c) {
    const dsd_env_value rot_value("DSD_NEO_RTL_OFFSET_TUNING");
    const char* rot = rot_value.c_str();
    c.rtl_offset_tuning_is_set = env_is_set(rot);
    if (c.rtl_offset_tuning_is_set) {
        c.rtl_offset_tuning_enable = env_is_falsey(rot) ? 0 : 1;
    }

    const dsd_env_value rxt_value("DSD_NEO_RTL_XTAL_HZ");
    const char* rxt = rxt_value.c_str();
    c.rtl_xtal_hz_is_set = env_parse_int_range(rxt, 1, 1000000000, &c.rtl_xtal_hz);

    const dsd_env_value txt_value("DSD_NEO_TUNER_XTAL_HZ");
    const char* txt = txt_value.c_str();
    c.tuner_xtal_hz_is_set = env_parse_int_range(txt, 1, 1000000000, &c.tuner_xtal_hz);

    const dsd_env_value rtm_value("DSD_NEO_RTL_TESTMODE");
    const char* rtm = rtm_value.c_str();
    c.rtl_testmode_is_set = env_is_set(rtm);
    if (c.rtl_testmode_is_set) {
        c.rtl_testmode_enable = env_is_falsey(rtm) ? 0 : 1;
    }
}

static void
config_init_rtl_if_gains_and_tuner_bw(dsdneoRuntimeConfig& c) {
    const dsd_env_value rig_value("DSD_NEO_RTL_IF_GAINS");
    const char* rig = rig_value.c_str();
    c.rtl_if_gains_is_set = env_is_set(rig);
    env_copy_str(c.rtl_if_gains, sizeof c.rtl_if_gains, rig);

    const dsd_env_value rv_value("DSD_NEO_RTL_VERIFY");
    const char* rv = rv_value.c_str();
    c.rtl_verify_is_set = env_is_set(rv);
    if (c.rtl_verify_is_set) {
        c.rtl_verify_enable = env_is_falsey(rv) ? 0 : 1;
    }

    const dsd_env_value rva_value("DSD_NEO_RTL_VERIFY_ATTEMPTS");
    const char* rva = rva_value.c_str();
    c.rtl_verify_attempts_is_set = env_parse_int_range(rva, 1, 10, &c.rtl_verify_attempts);

    const dsd_env_value tbw_value("DSD_NEO_TUNER_BW_HZ");
    const char* tbw = tbw_value.c_str();
    c.tuner_bw_hz_is_set = 0;
    if (env_is_set(tbw)) {
        if (dsd_strcasecmp(tbw, "auto") == 0) {
            c.tuner_bw_hz = 0;
            c.tuner_bw_hz_is_set = 1;
        } else {
            int v = 0;
            if (env_parse_int_range(tbw, 0, 20000000, &v)) {
                c.tuner_bw_hz = v;
                c.tuner_bw_hz_is_set = 1;
            }
        }
    }
}

static void
config_init_rtl_and_tuner(dsdneoRuntimeConfig& c) {
    /* RTL device/tuner knobs */
    config_init_rtl_agc_and_direct(c);
    config_init_rtl_offset_xtal_testmode(c);
    config_init_rtl_if_gains_and_tuner_bw(c);
}

static void
config_init_tuner_autogain(dsdneoRuntimeConfig& c) {
    /* Supervisory tuner autogain knobs */
    const dsd_env_value tag_value("DSD_NEO_TUNER_AUTOGAIN");
    const char* tag = tag_value.c_str();
    c.tuner_autogain_is_set = env_is_set(tag);
    c.tuner_autogain_enable = c.tuner_autogain_is_set ? (env_is_truthy(tag) ? 1 : 0) : 0;

    const dsd_env_value tap_value("DSD_NEO_TUNER_AUTOGAIN_PROBE_MS");
    const char* tap = tap_value.c_str();
    c.tuner_autogain_probe_ms_is_set = env_parse_int_range(tap, 0, 20000, &c.tuner_autogain_probe_ms);

    const dsd_env_value tas_value("DSD_NEO_TUNER_AUTOGAIN_SEED_DB");
    const char* tas = tas_value.c_str();
    c.tuner_autogain_seed_db_is_set = env_parse_double_range(tas, 0.0, 60.0, &c.tuner_autogain_seed_db);

    const dsd_env_value tss_value("DSD_NEO_TUNER_AUTOGAIN_SPEC_SNR_DB");
    const char* tss = tss_value.c_str();
    c.tuner_autogain_spec_snr_db_is_set = env_parse_double_range(tss, 0.0, 60.0, &c.tuner_autogain_spec_snr_db);

    const dsd_env_value tir_value("DSD_NEO_TUNER_AUTOGAIN_INBAND_RATIO");
    const char* tir = tir_value.c_str();
    c.tuner_autogain_inband_ratio_is_set = env_parse_double_range(tir, 0.10, 0.95, &c.tuner_autogain_inband_ratio);

    const dsd_env_value tus_value("DSD_NEO_TUNER_AUTOGAIN_UP_STEP_DB");
    const char* tus = tus_value.c_str();
    c.tuner_autogain_up_step_db_is_set = env_parse_double_range(tus, 1.0, 10.0, &c.tuner_autogain_up_step_db);

    const dsd_env_value tup_value("DSD_NEO_TUNER_AUTOGAIN_UP_PERSIST");
    const char* tup = tup_value.c_str();
    c.tuner_autogain_up_persist_is_set = env_parse_int_range(tup, 1, 5, &c.tuner_autogain_up_persist);
}

static void
config_init_auto_ppm(dsdneoRuntimeConfig& c) {
    /* Auto-PPM knobs */
    const dsd_env_value appm_value("DSD_NEO_AUTO_PPM");
    const char* appm = appm_value.c_str();
    c.auto_ppm_is_set = env_is_set(appm);
    c.auto_ppm_enable = c.auto_ppm_is_set ? (env_is_truthy(appm) ? 1 : 0) : 0;

    const dsd_env_value apsnr_value("DSD_NEO_AUTO_PPM_SNR_DB");
    const char* apsnr = apsnr_value.c_str();
    c.auto_ppm_snr_db_is_set = env_parse_double_range(apsnr, 0.0, 40.0, &c.auto_ppm_snr_db);

    const dsd_env_value apwr_value("DSD_NEO_AUTO_PPM_PWR_DB");
    const char* apwr = apwr_value.c_str();
    c.auto_ppm_pwr_db_is_set = 0;
    if (env_is_set(apwr)) {
        double v = 0.0;
        if (env_parse_double_strict(apwr, &v) && v <= 0.0) {
            c.auto_ppm_pwr_db = v;
            c.auto_ppm_pwr_db_is_set = 1;
        }
    }

    const dsd_env_value azppm_value("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM");
    const char* azppm = azppm_value.c_str();
    c.auto_ppm_zerolock_ppm_is_set = env_parse_double_range(azppm, 0.0, 2.0, &c.auto_ppm_zerolock_ppm);

    const dsd_env_value azhz_value("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ");
    const char* azhz = azhz_value.c_str();
    c.auto_ppm_zerolock_hz_is_set = env_parse_int_range(azhz, 10, 500, &c.auto_ppm_zerolock_hz);

    const dsd_env_value afrz_value("DSD_NEO_AUTO_PPM_FREEZE");
    const char* afrz = afrz_value.c_str();
    c.auto_ppm_freeze_is_set = env_is_set(afrz);
    if (c.auto_ppm_freeze_is_set) {
        c.auto_ppm_freeze_enable = env_is_falsey(afrz) ? 0 : 1;
    }
}

static void
config_init_resamp(dsdneoRuntimeConfig& c) {
    /* RESAMP */
    const dsd_env_value rs_value("DSD_NEO_RESAMP");
    const char* rs = rs_value.c_str();
    c.resamp_is_set = env_is_set(rs);
    c.resamp_disable = 0;
    c.resamp_target_hz = 48000;
    if (c.resamp_is_set) {
        if (dsd_strcasecmp(rs, "off") == 0 || strcmp(rs, "0") == 0) {
            c.resamp_disable = 1;
        } else {
            int v = 0;
            if (env_parse_int_strict(rs, &v) && v > 0) {
                c.resamp_target_hz = v;
            }
        }
    }
}

static void
config_init_costas(dsdneoRuntimeConfig& c) {
    /* CQPSK Costas loop (carrier recovery) using GNU Radio control loop */
    const dsd_env_value cbw_value("DSD_NEO_COSTAS_BW");
    const char* cbw = cbw_value.c_str();
    const dsd_env_value cdp_value("DSD_NEO_COSTAS_DAMPING");
    const char* cdp = cdp_value.c_str();
    c.costas_bw_is_set = env_is_set(cbw);
    c.costas_damping_is_set = env_is_set(cdp);
    if (c.costas_bw_is_set) {
        double v = 0.0;
        c.costas_loop_bw = env_parse_double_strict(cbw, &v) ? v : 0.0;
    } else {
        c.costas_loop_bw = dsd_neo_costas_default_loop_bw();
    }
    if (c.costas_damping_is_set) {
        double v = 0.0;
        c.costas_damping = env_parse_double_strict(cdp, &v) ? v : 0.0;
    } else {
        c.costas_damping = dsd_neo_costas_default_damping();
    }
}

static void
config_init_ted(dsdneoRuntimeConfig& c) {
    /* CQPSK/OP25 Gardner timing gain, in the range the setter allows (rtl_stream_set_ted_gain(): 0.01..0.5), so a
       startup value can neither freeze the loop nor drive it past what any control sets. A value that is not a finite
       number is no setting: the default stands (issue #669 B11). */
    const dsd_env_value tg_value("DSD_NEO_TED_GAIN");
    const char* tg = tg_value.c_str();
    double v = 0.0;
    c.ted_gain_is_set = env_is_set(tg) && env_parse_double_strict(tg, &v) && std::isfinite(v);
    if (c.ted_gain_is_set) {
        c.ted_gain = (float)((v < 0.01) ? 0.01 : ((v > 0.5) ? 0.5 : v));
    } else {
        c.ted_gain = 0.05f;
    }
}

static void
config_init_costas_ted(dsdneoRuntimeConfig& c) {
    config_init_costas(c);
    config_init_ted(c);
}

static void
config_init_deemph(dsdneoRuntimeConfig& c) {
    /* Deemphasis */
    const dsd_env_value deemph_value("DSD_NEO_DEEMPH");
    const char* deemph = deemph_value.c_str();
    c.deemph_is_set = env_is_set(deemph);
    c.deemph_mode = DSD_NEO_DEEMPH_UNSET;
    if (c.deemph_is_set) {
        if (dsd_strcasecmp(deemph, "off") == 0 || strcmp(deemph, "0") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_OFF;
        } else if (strcmp(deemph, "50") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_50;
        } else if (strcmp(deemph, "75") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_75;
        } else if (dsd_strcasecmp(deemph, "nfm") == 0) {
            c.deemph_mode = DSD_NEO_DEEMPH_NFM;
        }
    }
}

static void
config_init_audio_lpf(dsdneoRuntimeConfig& c) {
    /* Audio LPF */
    const dsd_env_value alpf_value("DSD_NEO_AUDIO_LPF");
    const char* alpf = alpf_value.c_str();
    c.audio_lpf_is_set = env_is_set(alpf);
    c.audio_lpf_disable = 0;
    c.audio_lpf_cutoff_hz = 0;
    if (c.audio_lpf_is_set) {
        if (dsd_strcasecmp(alpf, "off") == 0 || strcmp(alpf, "0") == 0) {
            c.audio_lpf_disable = 1;
        } else {
            int cutoff = 0;
            if (env_parse_int_strict(alpf, &cutoff) && cutoff > 0) {
                c.audio_lpf_cutoff_hz = cutoff;
            }
        }
    }
}

static void
config_init_deemph_audio(dsdneoRuntimeConfig& c) {
    config_init_deemph(c);
    config_init_audio_lpf(c);
}

static void
config_init_mt_and_retune(dsdneoRuntimeConfig& c) {
    /* MT (intra-block worker pool) */
    const dsd_env_value mt_value("DSD_NEO_MT");
    const char* mt = mt_value.c_str();
    c.mt_is_set = env_is_set(mt);
    c.mt_enable = (c.mt_is_set && mt[0] == '1') ? 1 : 0;

    /* Select the current combined CU8 transform or its supported two-pass equivalent. */
    const dsd_env_value combine_rot_value("DSD_NEO_COMBINE_ROT");
    const char* combine_rot = combine_rot_value.c_str();
    c.combine_rot_is_set = env_is_set(combine_rot);
    if (c.combine_rot_is_set) {
        int value = 0;
        c.combine_rot = env_parse_int_strict(combine_rot, &value) ? (value != 0) : 0;
    }

    /* Disable fs/4 capture shift */
    const dsd_env_value dfs4_value("DSD_NEO_DISABLE_FS4_SHIFT");
    const char* dfs4 = dfs4_value.c_str();
    c.fs4_shift_disable_is_set = env_is_set(dfs4);
    c.fs4_shift_disable = (c.fs4_shift_disable_is_set && dfs4[0] == '1') ? 1 : 0;

    /* Output clear/drain on retune */
    const dsd_env_value clr_value("DSD_NEO_OUTPUT_CLEAR_ON_RETUNE");
    const char* clr = clr_value.c_str();
    const dsd_env_value dms_value("DSD_NEO_RETUNE_DRAIN_MS");
    const char* dms = dms_value.c_str();
    const dsd_env_value mms_value("DSD_NEO_RETUNE_MUTE_MS");
    const char* mms = mms_value.c_str();
    c.output_clear_on_retune_is_set = env_is_set(clr);
    if (c.output_clear_on_retune_is_set) {
        int v = 0;
        c.output_clear_on_retune = env_parse_int_strict(clr, &v) ? (v != 0) : 0;
    } else {
        c.output_clear_on_retune = 0;
    }
    c.retune_drain_ms_is_set = env_is_set(dms);
    if (c.retune_drain_ms_is_set) {
        int v = 0;
        c.retune_drain_ms = env_parse_int_strict(dms, &v) ? v : 0;
    } else {
        c.retune_drain_ms = 50;
    }
    c.retune_mute_ms_is_set = 0;
    c.retune_mute_ms = 120;
    if (env_is_set(mms)) {
        int v = 0;
        if (env_parse_int_strict(mms, &v) && v >= 10 && v <= 1000) {
            c.retune_mute_ms_is_set = 1;
            c.retune_mute_ms = v;
        }
    }
}

static void
config_init_tcpin_window_pdu_snr(dsdneoRuntimeConfig& c) {
    /* TCP input reconnect backoff (ms) */
    const dsd_env_value tb_value("DSD_NEO_TCPIN_BACKOFF_MS");
    const char* tb = tb_value.c_str();
    c.tcpin_backoff_ms_is_set = 0;
    c.tcpin_backoff_ms = 300;
    if (env_is_set(tb)) {
        int v = 0;
        if (env_parse_int_strict(tb, &v) && v >= 50 && v <= 5000) {
            c.tcpin_backoff_ms_is_set = 1;
            c.tcpin_backoff_ms = v;
        }
    }

    /* Symbol window freeze for A/B testing */
    const dsd_env_value wf_value("DSD_NEO_WINDOW_FREEZE");
    const char* wf = wf_value.c_str();
    c.window_freeze_is_set = env_is_set(wf);
    if (c.window_freeze_is_set) {
        int v = 0;
        c.window_freeze = env_parse_int_strict(wf, &v) ? (v != 0) : 0;
    } else {
        c.window_freeze = 0;
    }

    /* Optional JSON emitter for P25 PDUs */
    const dsd_env_value pj_value("DSD_NEO_PDU_JSON");
    const char* pj = pj_value.c_str();
    c.pdu_json_is_set = env_is_set(pj);
    if (c.pdu_json_is_set) {
        int v = 0;
        c.pdu_json_enable = env_parse_int_strict(pj, &v) ? (v != 0) : 0;
    } else {
        c.pdu_json_enable = 0;
    }

    /* Optional SNR-based digital squelch threshold (dB) */
    const dsd_env_value snrsql_value("DSD_NEO_SNR_SQL_DB");
    const char* snrsql = snrsql_value.c_str();
    c.snr_sql_is_set = env_is_set(snrsql);
    if (c.snr_sql_is_set) {
        int v = 0;
        c.snr_sql_db = env_parse_int_strict(snrsql, &v) ? v : 0;
    } else {
        c.snr_sql_db = 0;
    }
}

static void
config_init_iq_and_channel_lpf(dsdneoRuntimeConfig& c) {
    /* Complex DC blocker */
    const dsd_env_value dcb_value("DSD_NEO_IQ_DC_BLOCK");
    const char* dcb = dcb_value.c_str();
    const dsd_env_value dck_value("DSD_NEO_IQ_DC_SHIFT");
    const char* dck = dck_value.c_str();
    c.iq_dc_block_is_set = env_is_set(dcb);
    if (c.iq_dc_block_is_set) {
        int v = 0;
        c.iq_dc_block_enable = env_parse_int_strict(dcb, &v) ? (v != 0) : 0;
    } else {
        c.iq_dc_block_enable = 0;
    }
    c.iq_dc_shift_is_set = env_is_set(dck);
    if (c.iq_dc_shift_is_set) {
        int v = 0;
        c.iq_dc_shift = env_parse_int_strict(dck, &v) ? v : 0;
    } else {
        c.iq_dc_shift = 11;
    }

    /* Channel complex low-pass on RTL baseband (post-HB).
       Default: off for digital voice modes at 24 kHz; may be enabled via env. */
    const dsd_env_value clpf_value("DSD_NEO_CHANNEL_LPF");
    const char* clpf = clpf_value.c_str();
    c.channel_lpf_is_set = env_is_set(clpf);
    if (c.channel_lpf_is_set) {
        int v = 0;
        c.channel_lpf_enable = env_parse_int_strict(clpf, &v) ? v : 0;
    } else {
        c.channel_lpf_enable = 0;
    }
}

/**
 * @brief Parse environment variables and initialize the runtime configuration.
 *
 * @note Safe to call multiple times; the most recent call wins.
 */
void
dsd_neo_config_init(void) {
    dsdneoRuntimeConfig c{};
    config_init_defaults(c);
    config_init_paths_and_cache(c);
    config_init_realtime_and_cpu(c);
    config_init_bootstrap_and_debug(c);
    config_init_cqpsk(c);
    config_init_dmr_and_p25(c);
    config_init_p25_follower_and_soft(c);
    config_init_input_and_dmr_t3(c);
    config_init_tcp_and_rigctl(c);
    config_init_rtl_and_tuner(c);
    config_init_tuner_autogain(c);
    config_init_auto_ppm(c);
    config_init_resamp(c);
    config_init_costas_ted(c);
    config_init_deemph_audio(c);
    config_init_mt_and_retune(c);
    config_init_tcpin_window_pdu_snr(c);
    config_init_iq_and_channel_lpf(c);

    {
        std::lock_guard<std::mutex> lk(g_config_mu);
        publish_config_locked(c);
    }
}

/**
 * @brief Get immutable pointer to the current runtime configuration, or NULL if
 * initialization has not been performed.
 *
 * @return Pointer to config or NULL.
 */
const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_config_active.load(std::memory_order_acquire);
}

extern "C" void
dsd_apply_runtime_config_to_opts(const dsdneoRuntimeConfig* cfg, dsd_opts* opts, dsd_state* state) {
    (void)state;
    if (!cfg || !opts) {
        return;
    }

    if (cfg->dmr_t3_heur_is_set) {
        opts->dmr_t3_heuristic_fill = (uint8_t)(cfg->dmr_t3_heur_enable ? 1 : 0);
    }
    if (cfg->p25_afc_status_gate_is_set) {
        opts->p25_afc_status_gate_enable = cfg->p25_afc_status_gate_enable ? 1 : 0;
    }
}
