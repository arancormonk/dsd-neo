// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Basic tests for INI-based user configuration.
 *
 * Exercises load/apply/snapshot behavior for a representative config
 * without touching CLI or environment precedence.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "dsd-neo/runtime/call_alert.h"
#include "dsd-neo/runtime/config_schema.h"
#include "test_support.h"

static int
write_temp_config(const char* contents, char* out_path, size_t out_sz) {
    char tmpl[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(tmpl, sizeof(tmpl), "dsdneo_config_user");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    size_t len = strlen(contents);
    ssize_t wr = dsd_write(fd, contents, len);
    if (wr < 0 || (size_t)wr != len) {
        DSD_FPRINTF(stderr, "write failed: %s\n", strerror(errno));
        (void)dsd_close(fd);
        (void)remove(tmpl);
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(out_path, out_sz, "%s", tmpl);
    out_path[out_sz - 1] = '\0';
    return 0;
}

static void
reset_opts_and_state(dsd_opts& opts, dsd_state& state) {
    static const dsd_opts k_zero_opts = {};
    static const dsd_state k_zero_state = {};
    opts = k_zero_opts;
    state = k_zero_state;
}

static int
render_config_to_buffer(const dsdneoUserConfig* cfg, char* out, size_t out_sz) {
    if (!cfg || !out || out_sz == 0) {
        return 1;
    }
    FILE* tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    dsd_user_config_render_ini(cfg, tmp);
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek failed\n");
        fclose(tmp);
        return 1;
    }
    size_t n = fread(out, 1, out_sz - 1, tmp);
    if (n == 0 && ferror(tmp)) {
        DSD_FPRINTF(stderr, "fread failed\n");
        fclose(tmp);
        return 1;
    }
    out[n] = '\0';
    fclose(tmp);
    return 0;
}

static int
expect_contains(const char* label, const char* haystack, const char* needle) {
    if (!haystack || !needle || !strstr(haystack, needle)) {
        DSD_FPRINTF(stderr, "FAIL: %s missing \"%s\" in:\n%s\n", label, needle ? needle : "(null)",
                    haystack ? haystack : "(null)");
        return 1;
    }
    return 0;
}

static int
test_apply_file_input_rescales_symbol_timing(void) {
    dsdneoUserConfig cfg = {};
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.input_source = DSDCFG_INPUT_FILE;
    cfg.file_sample_rate = 44100;
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", "/tmp/input.wav");
    cfg.file_path[sizeof cfg.file_path - 1] = '\0';

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.wav_decimator = 48000;
    opts.wav_sample_rate = 48000;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.jitter = 3;

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (strcmp(opts.audio_in_dev, "/tmp/input.wav") != 0) {
        DSD_FPRINTF(stderr, "file input path not applied correctly: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (opts.wav_sample_rate != 44100) {
        DSD_FPRINTF(stderr, "file input sample rate not applied correctly: %d\n", opts.wav_sample_rate);
        rc |= 1;
    }
    if (opts.staged_file_sample_rate != 44100) {
        DSD_FPRINTF(stderr, "staged file sample rate not applied correctly: %d\n", opts.staged_file_sample_rate);
        rc |= 1;
    }
    if (dsd_opts_effective_input_rate(&opts) != 44100) {
        DSD_FPRINTF(stderr, "effective input rate mismatch after file config: %d\n",
                    dsd_opts_effective_input_rate(&opts));
        rc |= 1;
    }
    if (state.samplesPerSymbol != 9 || state.symbolCenter != 4 || state.jitter != -1) {
        DSD_FPRINTF(stderr, "file input timing rescale mismatch: sps=%d center=%d jitter=%d\n", state.samplesPerSymbol,
                    state.symbolCenter, state.jitter);
        rc |= 1;
    }

    return rc;
}

static int
test_decode_mode_aliases_and_guards(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"provoice\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for alias config %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_EDACS_PV) {
        DSD_FPRINTF(stderr, "decode alias provoice not parsed as EDACS/PV, mode=%d\n", (int)cfg.decode_mode);
        rc |= 1;
    }

    dsdneoUserConfig bad_cfg;
    bad_cfg.version = 99;
    if (dsd_user_config_load(NULL, &bad_cfg) == 0 || bad_cfg.version != 1) {
        DSD_FPRINTF(stderr, "load NULL path should fail and reset config version=%d\n", bad_cfg.version);
        rc |= 1;
    }
    if (dsd_user_config_load(path, NULL) == 0) {
        DSD_FPRINTF(stderr, "load NULL config should fail\n");
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_unknown_section_warnings_do_not_mutate_loaded_config(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[unexpected]\n"
                             "source = \"rtl\"\n"
                             "rtl_device = 9\n"
                             "rtl_freq = \"851.0125M\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"nxdn48\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));
    int validate_rc = dsd_user_config_validate(path, &diags);

    int rc = 0;
    if (validate_rc != 0 || diags.error_count != 0 || diags.warning_count == 0) {
        DSD_FPRINTF(stderr, "expected unknown section to validate with warning only, rc=%d errors=%d warnings=%d\n",
                    validate_rc, diags.error_count, diags.warning_count);
        rc |= 1;
    }
    int found_unknown_section = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].level == DSDCFG_DIAG_WARNING && strcmp(diags.items[i].section, "unexpected") == 0
            && strstr(diags.items[i].message, "Unknown section")) {
            found_unknown_section = 1;
            break;
        }
    }
    if (!found_unknown_section) {
        DSD_FPRINTF(stderr, "missing unknown-section warning diagnostic\n");
        rc |= 1;
    }
    dsd_user_config_diags_free(&diags);

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for warning-only config %s\n", path);
        (void)remove(path);
        return 1;
    }
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_PULSE) {
        DSD_FPRINTF(stderr, "unknown section mutated input source=%d has_input=%d\n", (int)cfg.input_source,
                    cfg.has_input);
        rc |= 1;
    }
    if (cfg.rtl_device == 9 || cfg.rtl_freq[0] != '\0') {
        DSD_FPRINTF(stderr, "unknown section leaked RTL fields device=%d freq=%s\n", cfg.rtl_device, cfg.rtl_freq);
        rc |= 1;
    }
    if (!cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_NXDN48) {
        DSD_FPRINTF(stderr, "known mode section after unknown section did not load, mode=%d has=%d\n",
                    (int)cfg.decode_mode, cfg.has_mode);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.audio_in_type == AUDIO_IN_RTL || strncmp(opts.audio_in_dev, "rtl:", 4) == 0) {
        DSD_FPRINTF(stderr, "unknown section applied RTL input to live opts: type=%d dev=%s\n", opts.audio_in_type,
                    opts.audio_in_dev);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_render_input_variants_and_save_atomic(void) {
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.version = 1;
    cfg.has_input = 1;
    cfg.has_output = 1;
    cfg.output_backend = DSDCFG_OUTPUT_PULSE;
    DSD_SNPRINTF(cfg.pulse_output, sizeof cfg.pulse_output, "%s", "speaker");
    cfg.frontend_kind = DSD_FRONTEND_TERMINAL;
    cfg.frontend_kind_is_set = 1;
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_M17;
    cfg.has_demod = 1;
    cfg.demod_path = DSDCFG_DEMOD_AUTO;
    cfg.has_logging = 1;
    DSD_SNPRINTF(cfg.event_log, sizeof cfg.event_log, "%s", "/tmp/events.log");
    DSD_SNPRINTF(cfg.frame_log, sizeof cfg.frame_log, "%s", "/tmp/frames.log");
    DSD_SNPRINTF(cfg.p25_sm_log, sizeof cfg.p25_sm_log, "%s", "/tmp/p25-sm.log");
    cfg.has_recording = 1;
    cfg.per_call_wav = 0;
    DSD_SNPRINTF(cfg.per_call_wav_dir, sizeof cfg.per_call_wav_dir, "%s", "/tmp/wav");
    DSD_SNPRINTF(cfg.static_wav_path, sizeof cfg.static_wav_path, "%s", "/tmp/static.wav");
    DSD_SNPRINTF(cfg.raw_wav_path, sizeof cfg.raw_wav_path, "%s", "/tmp/raw.wav");
    cfg.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    cfg.rdio_system_id = 12;
    DSD_SNPRINTF(cfg.rdio_api_url, sizeof cfg.rdio_api_url, "%s", "http://rdio.local");
    DSD_SNPRINTF(cfg.rdio_api_key, sizeof cfg.rdio_api_key, "%s", "secret");
    cfg.rdio_upload_timeout_ms = 6000;
    cfg.rdio_upload_retries = 2;
    cfg.rdio_api_delete_after_upload = 1;
    cfg.has_dsp = 1;
    cfg.iq_balance = 1;
    cfg.iq_dc_block = 1;

    char rendered[8192];
    int rc = 0;

    cfg.input_source = DSDCFG_INPUT_RTL;
    cfg.rtl_device = 3;
    DSD_SNPRINTF(cfg.rtl_freq, sizeof cfg.rtl_freq, "%s", "851.375M");
    cfg.rtl_gain = 29;
    cfg.rtl_ppm = 0;
    cfg.rtl_ppm_is_set = 1;
    cfg.rtl_bw_khz = 16;
    cfg.rtl_sql = -45;
    cfg.rtl_volume = 4;
    cfg.rtl_auto_ppm = 1;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render rtl source", rendered, "source = \"rtl\"\n");
    rc |= expect_contains("render rtl device", rendered, "rtl_device = 3\n");
    rc |= expect_contains("render rtl explicit zero ppm", rendered, "rtl_ppm = 0\n");
    rc |= expect_contains("render auto demod", rendered, "demod = \"auto\"\n");
    rc |= expect_contains("render pulse sink", rendered, "pulse_sink = \"speaker\"\n");
    rc |= expect_contains("render event log", rendered, "event_log = \"/tmp/events.log\"\n");
    rc |= expect_contains("render static wav", rendered, "static_wav = \"/tmp/static.wav\"\n");
    rc |= expect_contains("render raw wav", rendered, "raw_wav = \"/tmp/raw.wav\"\n");
    rc |= expect_contains("render rdio api key", rendered, "rdio_api_key = \"secret\"\n");
    rc |= expect_contains("render dsp balance", rendered, "iq_balance = true\n");

    cfg.input_source = DSDCFG_INPUT_RTLTCP;
    DSD_SNPRINTF(cfg.rtltcp_host, sizeof cfg.rtltcp_host, "%s", "127.0.0.1");
    cfg.rtltcp_port = 1234;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render rtltcp source", rendered, "source = \"rtltcp\"\n");
    rc |= expect_contains("render rtltcp host", rendered, "rtltcp_host = \"127.0.0.1\"\n");
    rc |= expect_contains("render rtltcp port", rendered, "rtltcp_port = 1234\n");

    cfg.input_source = DSDCFG_INPUT_FILE;
    DSD_SNPRINTF(cfg.file_path, sizeof cfg.file_path, "%s", "/tmp/input.wav");
    cfg.file_sample_rate = 96000;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render file source", rendered, "source = \"file\"\n");
    rc |= expect_contains("render file path", rendered, "file_path = \"/tmp/input.wav\"\n");
    rc |= expect_contains("render file sample rate", rendered, "file_sample_rate = 96000\n");

    cfg.input_source = DSDCFG_INPUT_TCP;
    DSD_SNPRINTF(cfg.tcp_host, sizeof cfg.tcp_host, "%s", "localhost");
    cfg.tcp_port = 7355;
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        return 1;
    }
    rc |= expect_contains("render tcp source", rendered, "source = \"tcp\"\n");
    rc |= expect_contains("render tcp host", rendered, "tcp_host = \"localhost\"\n");
    rc |= expect_contains("render tcp port", rendered, "tcp_port = 7355\n");

    char base_path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(base_path, sizeof base_path, "dsdneo_config_save");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed for save path\n");
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(base_path);

    char save_dir[DSD_TEST_PATH_MAX];
    char save_subdir[DSD_TEST_PATH_MAX];
    char save_path[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(save_dir, sizeof save_dir, "%s.d", base_path);
    DSD_SNPRINTF(save_subdir, sizeof save_subdir, "%s/sub", save_dir);
    DSD_SNPRINTF(save_path, sizeof save_path, "%s/config.ini", save_subdir);

    if (dsd_user_config_save_atomic(NULL, &cfg) == 0 || dsd_user_config_save_atomic("", &cfg) == 0
        || dsd_user_config_save_atomic(save_path, NULL) == 0) {
        DSD_FPRINTF(stderr, "save_atomic guard should reject NULL/empty inputs\n");
        rc |= 1;
    }
    if (dsd_user_config_save_atomic(save_path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "save_atomic failed for %s: %s\n", save_path, strerror(errno));
        rc |= 1;
    } else {
        dsdneoUserConfig loaded;
        if (dsd_user_config_load(save_path, &loaded) != 0) {
            DSD_FPRINTF(stderr, "load saved config failed for %s\n", save_path);
            rc |= 1;
        } else {
            rc |= (loaded.input_source == DSDCFG_INPUT_TCP) ? 0 : 1;
            if (loaded.input_source != DSDCFG_INPUT_TCP || strcmp(loaded.tcp_host, "localhost") != 0
                || loaded.tcp_port != 7355 || loaded.decode_mode != DSDCFG_MODE_M17
                || loaded.demod_path != DSDCFG_DEMOD_AUTO || !loaded.has_dsp || !loaded.iq_balance
                || !loaded.iq_dc_block) {
                DSD_FPRINTF(stderr,
                            "saved config reload mismatch source=%d host=%s port=%d mode=%d demod=%d dsp=%d/%d\n",
                            (int)loaded.input_source, loaded.tcp_host, loaded.tcp_port, (int)loaded.decode_mode,
                            (int)loaded.demod_path, loaded.iq_balance, loaded.iq_dc_block);
                rc |= 1;
            }
        }
    }

    (void)remove(save_path);
    (void)remove(save_subdir);
    (void)remove(save_dir);
    return rc;
}

static int
test_load_and_apply_basic(void) {
    /*
     * Load one representative user config that touches every public section.
     * The first half verifies parsed config fields; the second half applies the
     * snapshot to opts/state and checks the runtime-facing values.
     */
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_device = 1\n"
                             "rtl_freq = \"851.375M\"\n"
                             "rtl_gain = 30\n"
                             "rtl_ppm = 5\n"
                             "rtl_bw_khz = 16\n"
                             "rtl_sql = -50\n"
                             "rtl_volume = 2\n"
                             "\n"
                             "[output]\n"
                             "backend = \"null\"\n"
                             "frontend = \"terminal\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"dmr\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = true\n"
                             "chan_csv = \"/tmp/chan.csv\"\n"
                             "group_csv = \"/tmp/group.csv\"\n"
                             "allow_list = true\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"/tmp/targets.csv\"\n"
                             "idle_dwell_ms = 5000\n"
                             "activity_hold_ms = 2000\n"
                             "\n"
                             "[logging]\n"
                             "event_log = \"/tmp/events.log\"\n"
                             "frame_log = \"/tmp/frames.log\"\n"
                             "p25_sm_log = \"/tmp/p25-sm.log\"\n"
                             "\n"
                             "[alerts]\n"
                             "enabled = true\n"
                             "voice_start = true\n"
                             "voice_end = false\n"
                             "data = true\n"
                             "\n"
                             "[recording]\n"
                             "per_call_wav = true\n"
                             "per_call_wav_dir = \"/tmp/wav\"\n"
                             "rdio_mode = \"both\"\n"
                             "rdio_system_id = 77\n"
                             "rdio_api_url = \"http://127.0.0.1:3000\"\n"
                             "rdio_api_key = \"apikey\"\n"
                             "rdio_upload_timeout_ms = 2500\n"
                             "rdio_upload_retries = 3\n"
                             "rdio_api_delete_after_upload = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "input section not parsed as RTL\n");
        rc |= 1;
    }
    if (!cfg.has_output || cfg.output_backend != DSDCFG_OUTPUT_NULL || cfg.frontend_kind != DSD_FRONTEND_TERMINAL
        || !cfg.frontend_kind_is_set) {
        DSD_FPRINTF(stderr, "output section not parsed correctly\n");
        rc |= 1;
    }
    if (!cfg.has_mode || cfg.decode_mode != DSDCFG_MODE_DMR) {
        DSD_FPRINTF(stderr, "mode section not parsed as DMR\n");
        rc |= 1;
    }
    if (!cfg.has_trunking || !cfg.trunk_enabled || !cfg.trunk_use_allow_list) {
        DSD_FPRINTF(stderr, "trunking section not parsed correctly\n");
        rc |= 1;
    }
    if (!cfg.has_trunk_scan || !cfg.trunk_scan_enabled || strcmp(cfg.trunk_scan_targets_csv, "/tmp/targets.csv") != 0
        || cfg.trunk_scan_idle_dwell_ms != 5000 || cfg.trunk_scan_activity_hold_ms != 2000) {
        DSD_FPRINTF(stderr, "trunk_scan section not parsed correctly\n");
        rc |= 1;
    }
    if (!cfg.has_alerts || !cfg.call_alert_enabled
        || cfg.call_alert_events != (DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_DATA)) {
        DSD_FPRINTF(stderr, "alerts section not parsed correctly enabled=%d events=%d\n", cfg.call_alert_enabled,
                    cfg.call_alert_events);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    // Input, output, and mode fields are applied before trunking and logging.
    if (strcmp(opts.audio_in_dev, "rtl:1:851.375M:30:5:16:-50:2") != 0) {
        DSD_FPRINTF(stderr, "audio_in_dev mismatch: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (strcmp(opts.audio_out_dev, "null") != 0) {
        DSD_FPRINTF(stderr, "audio_out_dev mismatch: \"%s\"\n", opts.audio_out_dev);
        rc |= 1;
    }
    if (opts.frontend_kind != DSD_FRONTEND_TERMINAL) {
        DSD_FPRINTF(stderr, "terminal frontend not enabled\n");
        rc |= 1;
    }
    if (!(opts.frame_dmr == 1 && opts.frame_p25p1 == 0 && opts.frame_p25p2 == 0 && opts.frame_ysf == 0)) {
        DSD_FPRINTF(stderr, "DMR mode flags not applied as expected\n");
        rc |= 1;
    }
    if (!(opts.p25_trunk == 1 && opts.trunk_enable == 1)) {
        DSD_FPRINTF(stderr, "trunking not enabled in opts\n");
        rc |= 1;
    }
    if (opts.call_alert != 1 || opts.call_alert_events != (DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_DATA)
        || dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_END)) {
        DSD_FPRINTF(stderr, "call alert config not applied enabled=%d events=%u\n", opts.call_alert,
                    (unsigned)opts.call_alert_events);
        rc |= 1;
    }
    if (strcmp(opts.chan_in_file, "/tmp/chan.csv") != 0 || strcmp(opts.group_in_file, "/tmp/group.csv") != 0) {
        DSD_FPRINTF(stderr, "trunk CSV paths not applied correctly\n");
        rc |= 1;
    }
    if (opts.trunk_use_allow_list != 1) {
        DSD_FPRINTF(stderr, "trunk_use_allow_list not set\n");
        rc |= 1;
    }
    if (opts.trunk_scan_enabled != 1 || strcmp(opts.trunk_scan_targets_csv, "/tmp/targets.csv") != 0
        || opts.trunk_scan_idle_dwell_ms != 5000 || opts.trunk_scan_activity_hold_ms != 2000) {
        DSD_FPRINTF(stderr, "trunk scan config not applied correctly enabled=%d targets=%s dwell=%d hold=%d\n",
                    opts.trunk_scan_enabled, opts.trunk_scan_targets_csv, opts.trunk_scan_idle_dwell_ms,
                    opts.trunk_scan_activity_hold_ms);
        rc |= 1;
    }
    if (strcmp(opts.event_out_file, "/tmp/events.log") != 0 || strcmp(opts.frame_log_file, "/tmp/frames.log") != 0
        || strcmp(opts.p25_sm_log_file, "/tmp/p25-sm.log") != 0) {
        DSD_FPRINTF(stderr, "logging paths not applied correctly event=%s frame=%s p25_sm=%s\n", opts.event_out_file,
                    opts.frame_log_file, opts.p25_sm_log_file);
        rc |= 1;
    }
    if (opts.dmr_stereo_wav != 1 || strcmp(opts.wav_out_dir, "/tmp/wav") != 0) {
        DSD_FPRINTF(stderr, "recording per-call WAV settings not applied\n");
        rc |= 1;
    }
    if (opts.rdio_mode != DSD_RDIO_MODE_BOTH || opts.rdio_system_id != 77) {
        DSD_FPRINTF(stderr, "rdio mode/system_id not applied (%d/%d)\n", opts.rdio_mode, opts.rdio_system_id);
        rc |= 1;
    }
    if (strcmp(opts.rdio_api_url, "http://127.0.0.1:3000") != 0 || strcmp(opts.rdio_api_key, "apikey") != 0) {
        DSD_FPRINTF(stderr, "rdio API settings not applied\n");
        rc |= 1;
    }
    if (opts.rdio_upload_timeout_ms != 2500 || opts.rdio_upload_retries != 3) {
        DSD_FPRINTF(stderr, "rdio upload timeout/retries not applied (%d/%d)\n", opts.rdio_upload_timeout_ms,
                    opts.rdio_upload_retries);
        rc |= 1;
    }
    if (opts.rdio_api_delete_after_upload != 1) {
        DSD_FPRINTF(stderr, "rdio API delete-after-upload not applied (%d)\n", opts.rdio_api_delete_after_upload);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_legacy_ncurses_ui_alias(void) {
    static const char* true_ini = "version = 1\n"
                                  "\n"
                                  "[output]\n"
                                  "ncurses_ui = true\n";

    char true_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(true_ini, true_path, sizeof true_path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));
    int validate_rc = dsd_user_config_validate(true_path, &diags);

    int rc = 0;
    if (validate_rc != 0 || diags.error_count != 0) {
        DSD_FPRINTF(stderr, "legacy ncurses_ui alias should validate without errors, rc=%d errors=%d warnings=%d\n",
                    validate_rc, diags.error_count, diags.warning_count);
        rc |= 1;
    }
    int found_deprecated_alias = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].level == DSDCFG_DIAG_INFO && strcmp(diags.items[i].section, "output") == 0
            && strcmp(diags.items[i].key, "ncurses_ui") == 0 && strstr(diags.items[i].message, "deprecated")) {
            found_deprecated_alias = 1;
            break;
        }
    }
    if (!found_deprecated_alias) {
        DSD_FPRINTF(stderr, "missing deprecated diagnostic for output.ncurses_ui alias\n");
        rc |= 1;
    }
    dsd_user_config_diags_free(&diags);

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(true_path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for legacy ncurses_ui=true config %s\n", true_path);
        (void)remove(true_path);
        return rc | 1;
    }
    if (!cfg.has_output || !cfg.frontend_kind_is_set || cfg.frontend_kind != DSD_FRONTEND_TERMINAL) {
        DSD_FPRINTF(stderr, "legacy ncurses_ui=true did not enable terminal frontend, has=%d set=%d kind=%d\n",
                    cfg.has_output, cfg.frontend_kind_is_set, (int)cfg.frontend_kind);
        rc |= 1;
    }
    (void)remove(true_path);

    static const char* false_ini = "version = 1\n"
                                   "\n"
                                   "[output]\n"
                                   "ncurses_ui = false\n";

    char false_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(false_ini, false_path, sizeof false_path) != 0) {
        return rc | 1;
    }
    if (dsd_user_config_load(false_path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for legacy ncurses_ui=false config %s\n", false_path);
        (void)remove(false_path);
        return rc | 1;
    }
    if (!cfg.has_output || !cfg.frontend_kind_is_set || cfg.frontend_kind != DSD_FRONTEND_NONE) {
        DSD_FPRINTF(stderr, "legacy ncurses_ui=false did not disable frontend, has=%d set=%d kind=%d\n", cfg.has_output,
                    cfg.frontend_kind_is_set, (int)cfg.frontend_kind);
        rc |= 1;
    }
    (void)remove(false_path);
    return rc;
}

static int
test_load_apply_and_snapshot_native_frontend(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[output]\n"
                             "frontend = \"native\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for native frontend config %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_output || !cfg.frontend_kind_is_set || cfg.frontend_kind != DSD_FRONTEND_NATIVE) {
        DSD_FPRINTF(stderr, "native frontend config not parsed, has=%d set=%d kind=%d\n", cfg.has_output,
                    cfg.frontend_kind_is_set, (int)cfg.frontend_kind);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    if (opts.frontend_kind != DSD_FRONTEND_NATIVE) {
        DSD_FPRINTF(stderr, "native frontend config not applied, kind=%d\n", (int)opts.frontend_kind);
        rc |= 1;
    }

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.frontend_kind_is_set || snap.frontend_kind != DSD_FRONTEND_NATIVE) {
        DSD_FPRINTF(stderr, "native frontend snapshot mismatch, set=%d kind=%d\n", snap.frontend_kind_is_set,
                    (int)snap.frontend_kind);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_and_apply_alerts_empty_event_mask(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[alerts]\n"
                             "enabled = true\n"
                             "voice_start = false\n"
                             "voice_end = false\n"
                             "data = false\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_alerts || !cfg.call_alert_enabled || cfg.call_alert_events != 0) {
        DSD_FPRINTF(stderr, "empty alert mask not parsed correctly enabled=%d events=%d\n", cfg.call_alert_enabled,
                    cfg.call_alert_events);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (opts.call_alert != 0 || opts.call_alert_events != 0) {
        DSD_FPRINTF(stderr, "empty alert mask should disable runtime alerts enabled=%d events=%u\n", opts.call_alert,
                    (unsigned)opts.call_alert_events);
        rc |= 1;
    }
    if (dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_START)
        || dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_END)
        || dsd_call_alert_event_enabled(opts.call_alert, opts.call_alert_events, DSD_CALL_ALERT_EVENT_DATA)) {
        DSD_FPRINTF(stderr, "empty alert mask should suppress every event\n");
        rc |= 1;
    }

    char rendered[1024];
    if (render_config_to_buffer(&cfg, rendered, sizeof rendered) != 0) {
        rc |= 1;
    } else if (!strstr(rendered, "enabled = true\n") || !strstr(rendered, "voice_start = false\n")
               || !strstr(rendered, "voice_end = false\n") || !strstr(rendered, "data = false\n")) {
        DSD_FPRINTF(stderr, "empty alert mask not preserved in rendered config:\n%s\n", rendered);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_and_apply_soapy_input_no_args(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"soapy\"\n"
                             "rtl_freq = \"155.340M\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_SOAPY) {
        DSD_FPRINTF(stderr, "input section not parsed as Soapy source\n");
        rc |= 1;
    }
    if (cfg.soapy_args[0] != '\0') {
        DSD_FPRINTF(stderr, "soapy_args expected empty for plain soapy source, got \"%s\"\n", cfg.soapy_args);
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);
    opts.rtl_gain_value = 77;
    opts.rtl_dsp_bw_khz = 16;
    opts.rtl_volume_multiplier = 9;

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (strcmp(opts.audio_in_dev, "soapy") != 0) {
        DSD_FPRINTF(stderr, "audio_in_dev mismatch for plain soapy: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (opts.rtlsdr_center_freq != 155340000U) {
        DSD_FPRINTF(stderr, "rtlsdr_center_freq mismatch for soapy source: %u\n", opts.rtlsdr_center_freq);
        rc |= 1;
    }
    if (opts.rtl_gain_value != 77 || opts.rtl_dsp_bw_khz != 16 || opts.rtl_volume_multiplier != 9) {
        DSD_FPRINTF(stderr, "soapy defaults should preserve existing rtl tuning values gain=%d bw=%d vol=%d\n",
                    opts.rtl_gain_value, opts.rtl_dsp_bw_khz, opts.rtl_volume_multiplier);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_load_and_apply_soapy_input_with_args(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"soapy\"\n"
                             "soapy_args = \"driver=airspy,serial=ABC123\"\n"
                             "soapy_profile = \"airspy\"\n"
                             "soapy_stream_format = \"cf32\"\n"
                             "soapy_antenna = \"RX\"\n"
                             "soapy_clock = \"external\"\n"
                             "soapy_settings = \"rfnotch_ctrl=true,rx:agc_setpoint=-30\"\n"
                             "soapy_gains = \"LNA:12,MIX:8\"\n"
                             "soapy_bandwidth_hz = 250000\n"
                             "rtl_freq = \"851.375M\"\n"
                             "rtl_gain = 30\n"
                             "rtl_ppm = 5\n"
                             "rtl_bw_khz = 16\n"
                             "rtl_sql = -50\n"
                             "rtl_volume = 2\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_SOAPY) {
        DSD_FPRINTF(stderr, "input section not parsed as Soapy source\n");
        rc |= 1;
    }
    if (strcmp(cfg.soapy_args, "driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "soapy_args parse mismatch: \"%s\"\n", cfg.soapy_args);
        rc |= 1;
    }
    if (strcmp(cfg.soapy_profile, "airspy") != 0 || strcmp(cfg.soapy_stream_format, "cf32") != 0
        || strcmp(cfg.soapy_antenna, "RX") != 0 || strcmp(cfg.soapy_clock, "external") != 0
        || strcmp(cfg.soapy_settings, "rfnotch_ctrl=true,rx:agc_setpoint=-30") != 0
        || strcmp(cfg.soapy_gains, "LNA:12,MIX:8") != 0 || cfg.soapy_bandwidth_hz != 250000
        || !cfg.soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(stderr, "soapy extended fields not parsed correctly\n");
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (strcmp(opts.audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "audio_in_dev mismatch for soapy args: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }
    if (opts.rtlsdr_center_freq != 851375000U) {
        DSD_FPRINTF(stderr, "rtlsdr_center_freq mismatch for soapy args: %u\n", opts.rtlsdr_center_freq);
        rc |= 1;
    }
    if (opts.rtl_gain_value != 30 || opts.rtlsdr_ppm_error != 5 || opts.rtl_dsp_bw_khz != 16
        || opts.rtl_volume_multiplier != 2) {
        DSD_FPRINTF(stderr, "shared rtl tuning values not applied for soapy gain=%d ppm=%d bw=%d vol=%d\n",
                    opts.rtl_gain_value, opts.rtlsdr_ppm_error, opts.rtl_dsp_bw_khz, opts.rtl_volume_multiplier);
        rc |= 1;
    }
    if (strcmp(opts.soapy_profile, "airspy") != 0 || strcmp(opts.soapy_stream_format, "cf32") != 0
        || strcmp(opts.soapy_antenna, "RX") != 0 || strcmp(opts.soapy_clock, "external") != 0
        || strcmp(opts.soapy_settings, "rfnotch_ctrl=true,rx:agc_setpoint=-30") != 0
        || strcmp(opts.soapy_gains, "LNA:12,MIX:8") != 0 || opts.soapy_bandwidth_hz != 250000) {
        DSD_FPRINTF(stderr, "soapy extended fields not applied correctly\n");
        rc |= 1;
    }
    if (!(opts.rtl_squelch_level > 0.0 && opts.rtl_squelch_level < 1.0)) {
        DSD_FPRINTF(stderr, "rtl_squelch_level should be mapped from dB for soapy input, got %.9f\n",
                    opts.rtl_squelch_level);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_roundtrip_soapy_args(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    /*
     * Snapshot a configured Soapy source, render it to INI, reload it, and apply
     * it back to opts. This protects both shared RTL tuning fields and the
     * Soapy-only extended fields.
     */
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=sdrplay,serial=RSP1A");
    opts.audio_in_dev[sizeof opts.audio_in_dev - 1] = '\0';
    opts.rtlsdr_center_freq = 935012500U;
    opts.rtl_gain_value = 44;
    opts.rtlsdr_ppm_error = -3;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_squelch_level = 1.0;
    opts.rtl_volume_multiplier = 5;
    DSD_SNPRINTF(opts.soapy_profile, sizeof opts.soapy_profile, "%s", "sdrplay");
    DSD_SNPRINTF(opts.soapy_stream_format, sizeof opts.soapy_stream_format, "%s", "cs16");
    DSD_SNPRINTF(opts.soapy_antenna, sizeof opts.soapy_antenna, "%s", "A");
    DSD_SNPRINTF(opts.soapy_clock, sizeof opts.soapy_clock, "%s", "internal");
    DSD_SNPRINTF(opts.soapy_settings, sizeof opts.soapy_settings, "%s",
                 "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4");
    DSD_SNPRINTF(opts.soapy_gains, sizeof opts.soapy_gains, "%s", "IFGR:35");
    opts.soapy_bandwidth_hz = 200000;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    // Snapshot fields should preserve the Soapy args without the input prefix.
    int rc = 0;
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_SOAPY) {
        DSD_FPRINTF(stderr, "snapshot input_source mismatch for soapy\n");
        rc |= 1;
    }
    if (strcmp(snap.soapy_args, "driver=sdrplay,serial=RSP1A") != 0) {
        DSD_FPRINTF(stderr, "snapshot soapy_args mismatch: \"%s\"\n", snap.soapy_args);
        rc |= 1;
    }
    if (strcmp(snap.rtl_freq, "935012500") != 0 || snap.rtl_gain != 44 || snap.rtl_ppm != -3 || snap.rtl_bw_khz != 24
        || snap.rtl_volume != 5) {
        DSD_FPRINTF(stderr, "snapshot shared tuning mismatch freq=%s gain=%d ppm=%d bw=%d vol=%d\n", snap.rtl_freq,
                    snap.rtl_gain, snap.rtl_ppm, snap.rtl_bw_khz, snap.rtl_volume);
        rc |= 1;
    }
    if (snap.rtl_sql != 0) {
        DSD_FPRINTF(stderr, "snapshot rtl_sql expected 0 from unit squelch power, got %d\n", snap.rtl_sql);
        rc |= 1;
    }
    if (strcmp(snap.soapy_profile, "sdrplay") != 0 || strcmp(snap.soapy_stream_format, "cs16") != 0
        || strcmp(snap.soapy_antenna, "A") != 0 || strcmp(snap.soapy_clock, "internal") != 0
        || strcmp(snap.soapy_settings,
                  "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4")
               != 0
        || strcmp(snap.soapy_gains, "IFGR:35") != 0 || snap.soapy_bandwidth_hz != 200000
        || !snap.soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(stderr, "snapshot soapy extended fields mismatch\n");
        rc |= 1;
    }

    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (!strstr(rendered, "source = \"soapy\"") || !strstr(rendered, "soapy_args = \"driver=sdrplay,serial=RSP1A\"")) {
        DSD_FPRINTF(stderr, "rendered Soapy config missing source/args:\n%s\n", rendered);
        rc |= 1;
    }
    if (!strstr(rendered, "soapy_profile = \"sdrplay\"") || !strstr(rendered, "soapy_stream_format = \"cs16\"")
        || !strstr(rendered,
                   "soapy_settings = \"rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,"
                   "rfgain_sel=4\"")
        || !strstr(rendered, "soapy_bandwidth_hz = 200000")) {
        DSD_FPRINTF(stderr, "rendered Soapy config missing extended fields:\n%s\n", rendered);
        rc |= 1;
    }

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg_reload;
    if (dsd_user_config_load(path, &cfg_reload) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for rendered soapy config %s\n", path);
        (void)remove(path);
        return 1;
    }

    if (cfg_reload.input_source != DSDCFG_INPUT_SOAPY
        || strcmp(cfg_reload.soapy_args, "driver=sdrplay,serial=RSP1A") != 0) {
        DSD_FPRINTF(stderr, "reloaded soapy config mismatch source=%d args=%s\n", cfg_reload.input_source,
                    cfg_reload.soapy_args);
        rc |= 1;
    }
    if (strcmp(cfg_reload.soapy_profile, "sdrplay") != 0 || strcmp(cfg_reload.soapy_stream_format, "cs16") != 0
        || strcmp(cfg_reload.soapy_antenna, "A") != 0 || strcmp(cfg_reload.soapy_clock, "internal") != 0
        || strcmp(cfg_reload.soapy_settings,
                  "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4")
               != 0
        || strcmp(cfg_reload.soapy_gains, "IFGR:35") != 0 || cfg_reload.soapy_bandwidth_hz != 200000
        || !cfg_reload.soapy_bandwidth_hz_is_set) {
        DSD_FPRINTF(stderr, "reloaded soapy extended fields mismatch\n");
        rc |= 1;
    }

    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (strcmp(opts_reload.audio_in_dev, "soapy:driver=sdrplay,serial=RSP1A") != 0) {
        DSD_FPRINTF(stderr, "reloaded audio_in_dev mismatch: \"%s\"\n", opts_reload.audio_in_dev);
        rc |= 1;
    }
    if (strcmp(opts_reload.soapy_profile, "sdrplay") != 0 || strcmp(opts_reload.soapy_stream_format, "cs16") != 0
        || strcmp(opts_reload.soapy_antenna, "A") != 0 || strcmp(opts_reload.soapy_clock, "internal") != 0
        || strcmp(opts_reload.soapy_settings,
                  "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4")
               != 0
        || strcmp(opts_reload.soapy_gains, "IFGR:35") != 0 || opts_reload.soapy_bandwidth_hz != 200000) {
        DSD_FPRINTF(stderr, "reloaded opts soapy extended fields mismatch\n");
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_roundtrip_zero_rtl_ppm(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=airspy");
    opts.audio_in_dev[sizeof opts.audio_in_dev - 1] = '\0';
    opts.rtlsdr_center_freq = 155340000U;
    opts.rtl_gain_value = 22;
    opts.rtlsdr_ppm_error = 0;
    opts.rtl_dsp_bw_khz = 12;
    opts.rtl_squelch_level = 1.0;
    opts.rtl_volume_multiplier = 3;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (!snap.rtl_ppm_is_set || snap.rtl_ppm != 0) {
        DSD_FPRINTF(stderr, "snapshot zero rtl_ppm should be explicit, got is_set=%d ppm=%d\n", snap.rtl_ppm_is_set,
                    snap.rtl_ppm);
        rc |= 1;
    }

    char rendered[4096];
    if (render_config_to_buffer(&snap, rendered, sizeof rendered) != 0) {
        return 1;
    }
    if (!strstr(rendered, "rtl_ppm = 0\n")) {
        DSD_FPRINTF(stderr, "rendered config should keep explicit zero rtl_ppm:\n%s\n", rendered);
        rc |= 1;
    }

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(rendered, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg_reload;
    if (dsd_user_config_load(path, &cfg_reload) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for rendered zero-ppm config %s\n", path);
        (void)remove(path);
        return 1;
    }

    if (!cfg_reload.rtl_ppm_is_set || cfg_reload.rtl_ppm != 0) {
        DSD_FPRINTF(stderr, "reloaded zero rtl_ppm should stay explicit, got is_set=%d ppm=%d\n",
                    cfg_reload.rtl_ppm_is_set, cfg_reload.rtl_ppm);
        rc |= 1;
    }

    static dsd_opts opts_reload;
    static dsd_state state_reload;
    reset_opts_and_state(opts_reload, state_reload);
    opts_reload.rtlsdr_ppm_error = 17;
    dsd_apply_user_config_to_opts(&cfg_reload, &opts_reload, &state_reload);
    if (opts_reload.rtlsdr_ppm_error != 0) {
        DSD_FPRINTF(stderr, "reloaded zero rtl_ppm should override existing ppm, got %d\n",
                    opts_reload.rtlsdr_ppm_error);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_rtl_and_rtltcp_device_specs(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsdneoUserConfig snap;
    int rc = 0;

    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:7:451.125M:19:-2:12:-55:6");
    opts.rtlsdr_center_freq = 451125000U;
    opts.rtl_gain_value = 21;
    opts.rtlsdr_ppm_error = -4;
    opts.rtl_dsp_bw_khz = 12;
    opts.rtl_squelch_level = 2.0;
    opts.rtl_volume_multiplier = 6;
    opts.rtl_auto_ppm = 1;

    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_RTL || snap.rtl_device != 7) {
        DSD_FPRINTF(stderr, "snapshot RTL source/device mismatch source=%d device=%d\n", (int)snap.input_source,
                    snap.rtl_device);
        rc |= 1;
    }
    if (strcmp(snap.rtl_freq, "451125000") != 0 || snap.rtl_gain != 21 || snap.rtl_ppm != -4 || snap.rtl_bw_khz != 12
        || snap.rtl_sql != 0 || snap.rtl_volume != 6 || !snap.rtl_ppm_is_set || snap.rtl_auto_ppm != 1) {
        DSD_FPRINTF(stderr, "snapshot RTL tuning mismatch freq=%s gain=%d ppm=%d bw=%d sql=%d vol=%d auto=%d\n",
                    snap.rtl_freq, snap.rtl_gain, snap.rtl_ppm, snap.rtl_bw_khz, snap.rtl_sql, snap.rtl_volume,
                    snap.rtl_auto_ppm);
        rc |= 1;
    }

    reset_opts_and_state(opts, state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtltcp:radio.local:1234:769.00625M:28:3:24:-47:5");
    opts.rtlsdr_center_freq = 769006250U;
    opts.rtl_gain_value = 28;
    opts.rtlsdr_ppm_error = 3;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtl_squelch_level = 1e-30;
    opts.rtl_volume_multiplier = 5;

    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_RTLTCP || strcmp(snap.rtltcp_host, "radio.local") != 0
        || snap.rtltcp_port != 1234) {
        DSD_FPRINTF(stderr, "snapshot RTLTCP source mismatch source=%d host=%s port=%d\n", (int)snap.input_source,
                    snap.rtltcp_host, snap.rtltcp_port);
        rc |= 1;
    }
    if (strcmp(snap.rtl_freq, "769006250") != 0 || snap.rtl_gain != 28 || snap.rtl_ppm != 3 || snap.rtl_bw_khz != 24
        || snap.rtl_sql != -120 || snap.rtl_volume != 5 || !snap.rtl_ppm_is_set) {
        DSD_FPRINTF(stderr, "snapshot RTLTCP tuning mismatch freq=%s gain=%d ppm=%d bw=%d sql=%d vol=%d\n",
                    snap.rtl_freq, snap.rtl_gain, snap.rtl_ppm, snap.rtl_bw_khz, snap.rtl_sql, snap.rtl_volume);
        rc |= 1;
    }

    return rc;
}

static int
test_load_and_apply_rtltcp_regression(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"rtltcp\"\n"
                             "rtltcp_host = \"127.0.0.1\"\n"
                             "rtltcp_port = 1234\n"
                             "rtl_freq = \"851.375M\"\n"
                             "rtl_gain = 30\n"
                             "rtl_ppm = 5\n"
                             "rtl_bw_khz = 16\n"
                             "rtl_sql = -50\n"
                             "rtl_volume = 2\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    int rc = 0;
    if (!cfg.has_input || cfg.input_source != DSDCFG_INPUT_RTLTCP) {
        DSD_FPRINTF(stderr, "input section not parsed as RTLTCP source\n");
        rc |= 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    if (strcmp(opts.audio_in_dev, "rtltcp:127.0.0.1:1234:851.375M:30:5:16:-50:2") != 0) {
        DSD_FPRINTF(stderr, "rtltcp audio_in_dev regression: \"%s\"\n", opts.audio_in_dev);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_roundtrip(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"udp\"\n"
                             "udp_addr = \"127.0.0.1\"\n"
                             "udp_port = 9000\n"
                             "\n"
                             "[output]\n"
                             "backend = \"pulse\"\n"
                             "frontend = \"none\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"analog\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = false\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);
    opts.call_alert = 1;
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_END;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_UDP) {
        DSD_FPRINTF(stderr, "snapshot input_source mismatch\n");
        rc |= 1;
    }
    if (strcmp(snap.udp_addr, "127.0.0.1") != 0 || snap.udp_port != 9000) {
        DSD_FPRINTF(stderr, "snapshot udp_addr/udp_port mismatch: %s:%d\n", snap.udp_addr, snap.udp_port);
        rc |= 1;
    }
    if (!snap.has_output || snap.output_backend != DSDCFG_OUTPUT_PULSE) {
        DSD_FPRINTF(stderr, "snapshot output_backend mismatch\n");
        rc |= 1;
    }
    if (!snap.has_mode || snap.decode_mode != DSDCFG_MODE_ANALOG) {
        DSD_FPRINTF(stderr, "snapshot decode_mode mismatch\n");
        rc |= 1;
    }
    if (!snap.has_trunking) {
        DSD_FPRINTF(stderr, "snapshot missing trunking section\n");
        rc |= 1;
    }
    if (snap.trunk_enabled != 0) {
        DSD_FPRINTF(stderr, "snapshot trunk_enabled should be false for this config\n");
        rc |= 1;
    }
    if (!snap.has_alerts || !snap.call_alert_enabled || snap.call_alert_events != DSD_CALL_ALERT_EVENT_VOICE_END) {
        DSD_FPRINTF(stderr, "snapshot alerts mismatch enabled=%d events=%d\n", snap.call_alert_enabled,
                    snap.call_alert_events);
        rc |= 1;
    }

    // Render snapshot to an in-memory file to ensure INI output does not crash.
    FILE* tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        (void)remove(path);
        return 1;
    }
    dsd_user_config_render_ini(&snap, tmp);
    fclose(tmp);

    (void)remove(path);
    return rc;
}

static int
test_apply_demod_lock(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"auto\"\n"
                             "demod = \"qpsk\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    if (dsd_user_config_load(path, &cfg) != 0) {
        DSD_FPRINTF(stderr, "dsd_user_config_load failed for %s\n", path);
        (void)remove(path);
        return 1;
    }

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (!(opts.mod_cli_lock == 1 && opts.mod_qpsk == 1 && opts.mod_c4fm == 0 && opts.mod_gfsk == 0)) {
        DSD_FPRINTF(stderr, "demod lock not applied correctly (c4fm=%d qpsk=%d gfsk=%d lock=%d)\n", opts.mod_c4fm,
                    opts.mod_qpsk, opts.mod_gfsk, opts.mod_cli_lock);
        rc |= 1;
    }
    if (state.rf_mod != 1) {
        DSD_FPRINTF(stderr, "rf_mod should be 1 for QPSK lock, got %d\n", state.rf_mod);
        rc |= 1;
    }

    (void)remove(path);
    return rc;
}

static int
test_snapshot_persists_demod_lock(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "pulse");
    DSD_SNPRINTF(opts.audio_out_dev, sizeof opts.audio_out_dev, "null");
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    opts.mod_c4fm = 0;
    opts.mod_gfsk = 0;
    state.rf_mod = 1;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (!snap.has_demod || snap.demod_path != DSDCFG_DEMOD_QPSK) {
        DSD_FPRINTF(stderr, "snapshot missing demod lock (has_demod=%d demod_path=%d)\n", snap.has_demod,
                    snap.demod_path);
        rc |= 1;
    }

    FILE* tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    dsd_user_config_render_ini(&snap, tmp);
    char buf[512];
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "fseek failed\n");
        fclose(tmp);
        return 1;
    }
    size_t n = fread(buf, 1, sizeof buf - 1, tmp);
    if (n == 0 && ferror(tmp)) {
        DSD_FPRINTF(stderr, "fread failed\n");
        fclose(tmp);
        return 1;
    }
    buf[n] = '\0';
    fclose(tmp);

    if (!strstr(buf, "demod = \"qpsk\"")) {
        DSD_FPRINTF(stderr, "rendered INI missing demod line:\n%s\n", buf);
        rc |= 1;
    }

    return rc;
}

static int
test_apply_logging_retargets_frame_log_file(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    FILE* first_handle = tmpfile();
    if (!first_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.frame_log_f = first_handle;
    opts.frame_log_open_error_reported = 1;
    opts.frame_log_write_error_reported = 1;
    DSD_SNPRINTF(opts.frame_log_file, sizeof opts.frame_log_file, "%s", "/tmp/frames-old.log");
    opts.frame_log_file[sizeof opts.frame_log_file - 1] = '\0';
    FILE* first_p25_handle = tmpfile();
    if (!first_p25_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.p25_sm_log_f = first_p25_handle;
    opts.p25_sm_log_open_error_reported = 1;
    opts.p25_sm_log_write_error_reported = 1;
    DSD_SNPRINTF(opts.p25_sm_log_file, sizeof opts.p25_sm_log_file, "%s", "/tmp/p25-sm-old.log");
    opts.p25_sm_log_file[sizeof opts.p25_sm_log_file - 1] = '\0';

    dsdneoUserConfig cfg = {};
    cfg.version = 1;
    cfg.has_logging = 1;
    DSD_SNPRINTF(cfg.frame_log, sizeof cfg.frame_log, "%s", "/tmp/frames-new.log");
    cfg.frame_log[sizeof cfg.frame_log - 1] = '\0';
    DSD_SNPRINTF(cfg.p25_sm_log, sizeof cfg.p25_sm_log, "%s", "/tmp/p25-sm-new.log");
    cfg.p25_sm_log[sizeof cfg.p25_sm_log - 1] = '\0';

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    /*
     * Retargeting to new frame-log paths must close existing handles, replace the
     * remembered path strings, and clear sticky open/write error state.
     */
    int rc = 0;
    if (opts.frame_log_f != NULL) {
        DSD_FPRINTF(stderr, "frame log handle should be closed after retarget\n");
        rc |= 1;
    }
    if (strcmp(opts.frame_log_file, "/tmp/frames-new.log") != 0) {
        DSD_FPRINTF(stderr, "frame log path not updated after retarget: %s\n", opts.frame_log_file);
        rc |= 1;
    }
    if (opts.frame_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log open error state should reset after retarget\n");
        rc |= 1;
    }
    if (opts.frame_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log write error state should reset after retarget\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_f != NULL) {
        DSD_FPRINTF(stderr, "P25 SM log handle should be closed after retarget\n");
        rc |= 1;
    }
    if (strcmp(opts.p25_sm_log_file, "/tmp/p25-sm-new.log") != 0) {
        DSD_FPRINTF(stderr, "P25 SM log path not updated after retarget: %s\n", opts.p25_sm_log_file);
        rc |= 1;
    }
    if (opts.p25_sm_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log open error state should reset after retarget\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log write error state should reset after retarget\n");
        rc |= 1;
    }

    FILE* second_handle = tmpfile();
    if (!second_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.frame_log_f = second_handle;
    opts.frame_log_open_error_reported = 1;
    opts.frame_log_write_error_reported = 1;
    FILE* second_p25_handle = tmpfile();
    if (!second_p25_handle) {
        DSD_FPRINTF(stderr, "tmpfile failed: %s\n", strerror(errno));
        return 1;
    }
    opts.p25_sm_log_f = second_p25_handle;
    opts.p25_sm_log_open_error_reported = 1;
    opts.p25_sm_log_write_error_reported = 1;
    cfg.frame_log[0] = '\0';
    cfg.p25_sm_log[0] = '\0';

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    /*
     * Clearing configured paths is the disable case: both handles close, paths
     * become empty, and the same sticky error flags reset for future attempts.
     */
    if (opts.frame_log_f != NULL) {
        DSD_FPRINTF(stderr, "frame log handle should be closed when disabling logging path\n");
        rc |= 1;
    }
    if (opts.frame_log_file[0] != '\0') {
        DSD_FPRINTF(stderr, "frame log path should be cleared when disabling logging path\n");
        rc |= 1;
    }
    if (opts.frame_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log open error state should reset when disabling logging path\n");
        rc |= 1;
    }
    if (opts.frame_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "frame log write error state should reset when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_f != NULL) {
        DSD_FPRINTF(stderr, "P25 SM log handle should be closed when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_file[0] != '\0') {
        DSD_FPRINTF(stderr, "P25 SM log path should be cleared when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_open_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log open error state should reset when disabling logging path\n");
        rc |= 1;
    }
    if (opts.p25_sm_log_write_error_reported != 0) {
        DSD_FPRINTF(stderr, "P25 SM log write error state should reset when disabling logging path\n");
        rc |= 1;
    }

    return rc;
}

static int
test_apply_mode_ysf_uses_config_profile_behavior(void) {
    dsdneoUserConfig cfg = {};
    cfg.version = 1;
    cfg.has_mode = 1;
    cfg.decode_mode = DSDCFG_MODE_YSF;

    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    dsd_apply_user_config_to_opts(&cfg, &opts, &state);

    int rc = 0;
    if (!(opts.frame_ysf == 1 && opts.frame_dstar == 0 && opts.frame_dmr == 0)) {
        DSD_FPRINTF(stderr, "YSF config mode flags not applied as expected\n");
        rc |= 1;
    }
    if (opts.pulse_digi_out_channels != 2 || opts.dmr_stereo != 1 || opts.dmr_mono != 0) {
        DSD_FPRINTF(stderr, "YSF config profile audio mismatch channels=%d stereo=%d mono=%d\n",
                    opts.pulse_digi_out_channels, opts.dmr_stereo, opts.dmr_mono);
        rc |= 1;
    }
    if (strcmp(opts.output_name, "YSF") != 0) {
        DSD_FPRINTF(stderr, "YSF output_name mismatch: %s\n", opts.output_name);
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_staged_file_rate_uses_requested_rate(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_opts_and_state(opts, state);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "/tmp/staged-input.wav");
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 48000;
    opts.staged_file_sample_rate = 96000;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);

    int rc = 0;
    if (!snap.has_input || snap.input_source != DSDCFG_INPUT_FILE) {
        DSD_FPRINTF(stderr, "staged file snapshot input_source mismatch\n");
        rc |= 1;
    }
    if (snap.file_sample_rate != 96000) {
        DSD_FPRINTF(stderr, "staged file snapshot sample rate mismatch: %d\n", snap.file_sample_rate);
        rc |= 1;
    }
    return rc;
}

static int
test_snapshot_mode_inference_tdma_and_auto(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset_opts_and_state(opts, state);
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 0;
    opts.frame_ysf = 0;
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 0;
    opts.frame_provoice = 0;
    opts.frame_m17 = 0;

    dsdneoUserConfig snap;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    int rc = 0;
    if (snap.decode_mode != DSDCFG_MODE_TDMA) {
        DSD_FPRINTF(stderr, "expected TDMA mode inference, got %d\n", (int)snap.decode_mode);
        rc |= 1;
    }

    reset_opts_and_state(opts, state);
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    dsd_snapshot_opts_to_user_config(&opts, &state, &snap);
    if (snap.decode_mode != DSDCFG_MODE_AUTO) {
        DSD_FPRINTF(stderr, "expected AUTO fallback mode inference, got %d\n", (int)snap.decode_mode);
        rc |= 1;
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_apply_file_input_rescales_symbol_timing();
    rc |= test_decode_mode_aliases_and_guards();
    rc |= test_unknown_section_warnings_do_not_mutate_loaded_config();
    rc |= test_render_input_variants_and_save_atomic();
    rc |= test_load_and_apply_basic();
    rc |= test_load_legacy_ncurses_ui_alias();
    rc |= test_load_apply_and_snapshot_native_frontend();
    rc |= test_load_and_apply_alerts_empty_event_mask();
    rc |= test_load_and_apply_soapy_input_no_args();
    rc |= test_load_and_apply_soapy_input_with_args();
    rc |= test_snapshot_roundtrip_soapy_args();
    rc |= test_snapshot_roundtrip_zero_rtl_ppm();
    rc |= test_snapshot_rtl_and_rtltcp_device_specs();
    rc |= test_load_and_apply_rtltcp_regression();
    rc |= test_snapshot_roundtrip();
    rc |= test_apply_demod_lock();
    rc |= test_snapshot_persists_demod_lock();
    rc |= test_apply_logging_retargets_frame_log_file();
    rc |= test_apply_mode_ysf_uses_config_profile_behavior();
    rc |= test_snapshot_staged_file_rate_uses_requested_rate();
    rc |= test_snapshot_mode_inference_tdma_and_auto();
    return rc;
}
