// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_socket_connect.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "services.h"

#ifdef USE_RADIO

static int
svc_radio_source_is_soapy(const dsd_opts* opts) {
    const char* dev = opts ? opts->audio_in_dev : NULL;
    if (!dev) {
        return 0;
    }
    return (strcmp(dev, "soapy") == 0) || (strncmp(dev, "soapy:", 6) == 0);
}
#endif

int
svc_toggle_all_mutes(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    opts->unmute_encrypted_p25 = !opts->unmute_encrypted_p25;
    opts->dmr_mute_encL = !opts->dmr_mute_encL;
    opts->dmr_mute_encR = !opts->dmr_mute_encR;
    return 0;
}

int
svc_enable_per_call_wav(dsd_opts* opts, dsd_state* state) {
    (void)state;
    if (!opts) {
        return -1;
    }
    if (opts->dmr_stereo_wav == 1 && (opts->wav_out_f != NULL || opts->wav_out_fR != NULL)) {
        return (opts->wav_out_f != NULL && opts->wav_out_fR != NULL) ? 0 : -1;
    }
    char wav_file_directory[1024];
    DSD_SNPRINTF(wav_file_directory, sizeof wav_file_directory, "%s", opts->wav_out_dir);
    dsd_stat_t st;
    if (dsd_stat_path(wav_file_directory, &st) == -1) {
        LOG_INFO("NOTICE: %s wav file directory does not exist\n", wav_file_directory);
        LOG_INFO("NOTICE: Creating directory %s to save decoded wav files\n", wav_file_directory);
        dsd_mkdir(wav_file_directory, 0700);
    }
    DSD_FPRINTF(stderr, "\n Per Call Wav File Enabled to Directory: %s;.\n", opts->wav_out_dir);
    opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, 8000, 0);
    opts->wav_out_fR = open_wav_file(opts->wav_out_dir, opts->wav_out_fileR, sizeof opts->wav_out_fileR, 8000, 0);
    opts->dmr_stereo_wav = 1;
    return (opts->wav_out_f && opts->wav_out_fR) ? 0 : -1;
}

int
svc_open_symbol_out(dsd_opts* opts, dsd_state* state, const char* filename) {
    if (!opts || !state || !filename || !*filename) {
        return -1;
    }
    DSD_SNPRINTF(opts->symbol_out_file, sizeof opts->symbol_out_file, "%s", filename);
    openSymbolOutFile(opts, state);
    return (opts->symbol_out_f != NULL) ? 0 : -1;
}

int
svc_open_symbol_in(dsd_opts* opts, dsd_state* state, const char* filename) {
    (void)state;
    if (!opts || !filename || !*filename) {
        return -1;
    }
    opts->symbolfile = dsd_fopen_existing_regular_file(filename, "rb");
    if (!opts->symbolfile) {
        LOG_ERROR("Error, couldn't open %s\n", filename);
        return -1;
    }
    dsd_stat_t sb;
    if (dsd_fstat(dsd_fileno(opts->symbolfile), &sb) != 0) {
        LOG_ERROR("Error, couldn't stat %s\n", filename);
        fclose(opts->symbolfile);
        opts->symbolfile = NULL;
        return -1;
    }
    if (!dsd_stat_is_regular(&sb)) {
        LOG_ERROR("Error, %s is not a regular file\n", filename);
        fclose(opts->symbolfile);
        opts->symbolfile = NULL;
        return -1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", filename);
    opts->audio_in_type = AUDIO_IN_SYMBOL_BIN; // symbol capture bin
    if (state) {
        state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
        state->symbol_replay_header_checked = 0;
        state->symbol_replay_has_soft = 0;
    }
    return 0;
}

int
svc_rigctl_connect(dsd_opts* opts, const char* host, int port) {
    if (!opts || !host || port <= 0) {
        return -1;
    }
    DSD_SNPRINTF(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", host);
    opts->rigctlportno = port;
    opts->rigctl_sockfd = Connect(opts->rigctlhostname, opts->rigctlportno);
    if (opts->rigctl_sockfd != DSD_INVALID_SOCKET) {
        opts->use_rigctl = 1;
        return 0;
    }
    opts->use_rigctl = 0;
    return -1;
}

int
svc_lrrp_set_home(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    char path[1024];
    if (dsd_config_expand_path("~/lrrp.txt", path, sizeof path) != 0 || !path[0]) {
        return -1;
    }
    DSD_SNPRINTF(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", path);
    opts->lrrp_file_output = 1;
    return 0;
}

int
svc_lrrp_set_dsdp(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    DSD_SNPRINTF(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", "DSDPlus.LRRP");
    opts->lrrp_file_output = 1;
    return 0;
}

int
svc_lrrp_set_custom(dsd_opts* opts, const char* filename) {
    if (!opts || !filename || !*filename) {
        return -1;
    }
    DSD_SNPRINTF(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", filename);
    opts->lrrp_file_output = 1;
    return 0;
}

void
svc_lrrp_disable(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    opts->lrrp_file_output = 0;
    opts->lrrp_out_file[0] = 0;
}

void
svc_reset_event_history(dsd_state* state) {
    dsd_event_history_reset(state);
}

void
svc_set_p2_params(dsd_state* state, unsigned long long wacn, unsigned long long sysid, unsigned long long cc) {
    if (!state) {
        return;
    }
    state->p2_wacn = (wacn > 0xFFFFF) ? 0xFFFFF : wacn;
    state->p2_sysid = (sysid > 0xFFF) ? 0xFFF : sysid;
    state->p2_cc = (cc > 0xFFF) ? 0xFFF : cc;
    state->p2_hardset = (state->p2_wacn != 0 && state->p2_sysid != 0 && state->p2_cc != 0) ? 1 : 0;
}

// Logging & file outputs ----------------------------------------------------
int
svc_set_event_log(dsd_opts* opts, const char* path) {
    if (!opts || !path || !*path) {
        return -1;
    }
    DSD_STRNCPY(opts->event_out_file, path, sizeof opts->event_out_file - 1);
    opts->event_out_file[sizeof opts->event_out_file - 1] = '\0';
    return 0;
}

void
svc_disable_event_log(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    opts->event_out_file[0] = '\0';
}

int
svc_open_static_wav(dsd_opts* opts, dsd_state* state, const char* path) {
    if (!opts || !state || !path || !*path) {
        return -1;
    }
    DSD_STRNCPY(opts->wav_out_file, path, sizeof opts->wav_out_file - 1);
    opts->wav_out_file[sizeof opts->wav_out_file - 1] = '\0';
    opts->dmr_stereo_wav = 0;
    opts->static_wav_file = 1;
    openWavOutFileLR(opts, state);
    return (opts->wav_out_f != NULL) ? 0 : -1;
}

int
svc_open_raw_wav(dsd_opts* opts, dsd_state* state, const char* path) {
    if (!opts || !state || !path || !*path) {
        return -1;
    }
    DSD_STRNCPY(opts->wav_out_file_raw, path, sizeof opts->wav_out_file_raw - 1);
    opts->wav_out_file_raw[sizeof opts->wav_out_file_raw - 1] = '\0';
    openWavOutFileRaw(opts, state);
    return (opts->wav_out_raw != NULL) ? 0 : -1;
}

int
svc_set_dsp_output_file(dsd_opts* opts, const char* filename) {
    if (!opts || !filename || !*filename) {
        return -1;
    }
    char dir[1024];
    DSD_SNPRINTF(dir, sizeof dir, "./DSP");
    dsd_stat_t st;
    if (dsd_stat_path(dir, &st) == -1) {
        dsd_mkdir(dir, 0700);
    }
    DSD_SNPRINTF(opts->dsp_out_file, sizeof opts->dsp_out_file, "%s/%s", dir, filename);
    opts->use_dsp_output = 1;
    return 0;
}

// Pulse/UDP helpers ---------------------------------------------------------
int
svc_set_pulse_output(dsd_opts* opts, const char* index) {
    if (!opts || !index) {
        return -1;
    }
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse");
    opts->audio_out_type = 0;
    // supply only the part after 'pulse:' to parser
    char tmp[128];
    DSD_SNPRINTF(tmp, sizeof tmp, "%s", index);
    parse_audio_output_string(opts, tmp);
    return 0;
}

int
svc_set_pulse_input(dsd_opts* opts, const char* index) {
    if (!opts || !index) {
        return -1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
    char tmp[128];
    DSD_SNPRINTF(tmp, sizeof tmp, "%s", index);
    parse_audio_input_string(opts, tmp);
    return 0;
}

int
svc_udp_output_config(dsd_opts* opts, dsd_state* state, const char* host, int port) {
    if (!opts || !state || !host || port <= 0) {
        return -1;
    }
    DSD_STRNCPY(opts->udp_hostname, host, sizeof opts->udp_hostname - 1);
    opts->udp_hostname[sizeof opts->udp_hostname - 1] = '\0';
    opts->udp_portno = port;
    int err = udp_socket_connect(opts, state);
    if (err < 0) {
        return -1;
    }
    opts->audio_out_type = 8;
    if (opts->monitor_input_audio == 1 || opts->frame_provoice == 1) {
        if (udp_socket_connectA(opts, state) < 0) {
            opts->udp_sockfdA = 0;
        }
    }
    return 0;
}

// Trunking & control --------------------------------------------------------

/*
 * Adopt an imported channel map wholesale. The importer is additive
 * (lcn_freq_count only grows, stale trunk_chan_map slots survive), so a
 * runtime re-import must replace the previous map rather than append to it.
 *
 * Replace, deliberately, and not a merge: what the protocol layer learned on
 * the air (dmr_csbk, p25_frequency, edacs-fme) is discarded along with the old
 * CSV's entries, so a grant for an LCN the new file omits resolves to 0 Hz
 * until the site re-announces it. A merge was considered and rejected -- "apply
 * this file" would stop meaning the map is what the file says, a re-import
 * could no longer shrink a map, and correcting a wrong frequency in the CSV
 * would not take on a site that had already announced it. Emptying the map is
 * svc_clear_channel_map()'s job, so nothing needs the merge to express it.
 *
 * dmr_lcn_trust is provenance for the map, not a separate table: leaving it
 * behind would let a stale "learned on the control channel" byte authorize an
 * off-CC tune to a frequency that only the new CSV asserts. Clearing it matches
 * what -C produces at startup, and dmr_learn_chan_map() re-earns trust for any
 * LCN the new map leaves empty. lcn_freq_roll indexes trunk_lcn_freq, so a
 * shorter list has to restart the hunt rather than resume mid-way.
 */
static void
chan_map_adopt(dsd_state* dst, const dsd_state* src) {
    DSD_MEMCPY(dst->trunk_chan_map, src->trunk_chan_map, sizeof dst->trunk_chan_map);
    DSD_MEMCPY(dst->trunk_chan_map_used, src->trunk_chan_map_used, sizeof dst->trunk_chan_map_used);
    dst->trunk_chan_map_used_count = src->trunk_chan_map_used_count;
    DSD_MEMCPY(dst->trunk_lcn_freq, src->trunk_lcn_freq, sizeof dst->trunk_lcn_freq);
    dst->lcn_freq_count = src->lcn_freq_count;
    dst->lcn_freq_roll = 0;
    DSD_MEMSET(dst->dmr_lcn_trust, 0, sizeof dst->dmr_lcn_trust);
    dst->trunk_chan_map_seq++;
}

int
svc_import_channel_map(dsd_opts* opts, dsd_state* state, const char* path) {
    if (!opts || !state || !path || !*path) {
        return -1;
    }
    // Same invariant the CLI enforces for -C: a trunk-scan run gets its channel
    // maps per target, and adopting a global one here would wipe the target's.
    if (opts->trunk_scan_enabled == 1) {
        return -1;
    }
    DSD_STRNCPY(opts->chan_in_file, path, sizeof opts->chan_in_file - 1);
    opts->chan_in_file[sizeof opts->chan_in_file - 1] = '\0';

    // Import into throwaway heap state (dsd_state is multi-megabyte) so a
    // failed import leaves the live map untouched. The allocation is large and
    // this runs on the decoder thread, but it happens once per user import
    // rather than per frame, and it is the same validate-then-swap shape
    // dsd_tg_policy_reload_group_file() uses for the same reason.
    dsd_state* imported = (dsd_state*)calloc(1, sizeof(*imported));
    if (!imported) {
        return -1;
    }
    const int import_rc = csvChanImport(opts, imported);
    // The importer reports success for any file it could open, so a mispicked
    // CSV (a talkgroup list, a header-only file) parses to an empty map. Adopting
    // that would replace the live map with zeros; refuse instead, which is what
    // the additive import used to do by doing nothing.
    const int mapped_any = (imported->trunk_chan_map_used_count > 0);
    if (import_rc == 0 && mapped_any) {
        chan_map_adopt(state, imported);
    }
    dsd_state_ext_free_all(imported);
    free(imported);
    return (import_rc == 0 && mapped_any) ? 0 : -1;
}

int
svc_clear_channel_map(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    // The same invariant svc_import_channel_map() enforces: under trunk scan the
    // per-target maps are not this frontend's to empty.
    if (opts->trunk_scan_enabled == 1) {
        return -1;
    }
    opts->chan_in_file[0] = '\0';
    DSD_MEMSET(state->trunk_chan_map, 0, sizeof state->trunk_chan_map);
    DSD_MEMSET(state->trunk_chan_map_used, 0, sizeof state->trunk_chan_map_used);
    state->trunk_chan_map_used_count = 0;
    DSD_MEMSET(state->trunk_lcn_freq, 0, sizeof state->trunk_lcn_freq);
    state->lcn_freq_count = 0;
    state->lcn_freq_roll = 0;
    // Provenance goes with the map, exactly as in chan_map_adopt(): a surviving
    // "learned on the control channel" byte would authorize an off-CC tune to a
    // frequency no longer in the map.
    DSD_MEMSET(state->dmr_lcn_trust, 0, sizeof state->dmr_lcn_trust);
    state->trunk_chan_map_seq++;
    return 0;
}

int
svc_clear_group_list(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    opts->group_in_file[0] = '\0';
    return dsd_tg_policy_clear(state);
}

int
svc_clear_keys(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    // One keyring behind both CSV kinds, so one clear covers dec and hex.
    opts->key_in_file[0] = '\0';
    DSD_MEMSET(state->rkey_array, 0, sizeof state->rkey_array);
    DSD_MEMSET(state->rkey_array_loaded, 0, sizeof state->rkey_array_loaded);
    // The TG->key ID override map indexes into the keyring, so it goes with it. Spelled out rather
    // than routed through keyring_dmr_tg_map_reset(): this translation unit is compiled standalone
    // by tests/ui/test_ui_menu_services.c and does not link the keyring.
    DSD_MEMSET(state->dmr_tg_key_map_tg, 0, sizeof state->dmr_tg_key_map_tg);
    DSD_MEMSET(state->dmr_tg_key_map_kid, 0, sizeof state->dmr_tg_key_map_kid);
    state->dmr_tg_key_map_count = 0;
    state->dmr_tg_key_note_epoch[0] = state->dmr_tg_key_note_epoch[1] = 0U;
    // Disarm the keyring the way svc_import_keys_dec() arms it. Leaving it set
    // would keep every consumer treating the now-zeroed array as loaded keys.
    state->keyloader = 0;
    return 0;
}

int
svc_import_group_list(dsd_opts* opts, dsd_state* state, const char* path) {
    if (!opts || !state || !path || !*path) {
        return -1;
    }
    DSD_STRNCPY(opts->group_in_file, path, sizeof opts->group_in_file - 1);
    opts->group_in_file[sizeof opts->group_in_file - 1] = '\0';
    return dsd_tg_policy_reload_group_file(opts, state);
}

int
svc_import_keys_dec(dsd_opts* opts, dsd_state* state, const char* path) {
    if (!opts || !state || !path || !*path) {
        return -1;
    }
    DSD_STRNCPY(opts->key_in_file, path, sizeof opts->key_in_file - 1);
    opts->key_in_file[sizeof opts->key_in_file - 1] = '\0';
    const int rc = csvKeyImportDec(opts, state);
    if (rc == 0) {
        // Arm the keyring, exactly as -k does. Without this the rows land in
        // rkey_array but every consumer (dsd_mbe, P25/NXDN crypto) gates on
        // keyloader, so a session that started without a key file would report
        // the import as applied and keep failing to decrypt.
        state->keyloader = 1;
    }
    return rc;
}

int
svc_import_keys_hex(dsd_opts* opts, dsd_state* state, const char* path) {
    if (!opts || !state || !path || !*path) {
        return -1;
    }
    DSD_STRNCPY(opts->key_in_file, path, sizeof opts->key_in_file - 1);
    opts->key_in_file[sizeof opts->key_in_file - 1] = '\0';
    const int rc = csvKeyImportHex(opts, state);
    if (rc == 0) {
        state->keyloader = 1; // see svc_import_keys_dec
    }
    return rc;
}

void
svc_set_tg_hold(dsd_state* state, unsigned tg) {
    if (!state) {
        return;
    }
    state->tg_hold = tg;
}

void
svc_set_hangtime(dsd_opts* opts, double seconds) {
    if (!opts) {
        return;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    opts->trunk_hangtime = seconds;
}

void
svc_set_rigctl_setmod_bw(dsd_opts* opts, int hz) {
    if (!opts) {
        return;
    }
    if (hz < 0) {
        hz = 0;
    }
    if (hz > 25000) {
        hz = 25000;
    }
    opts->setmod_bw = hz;
}

void
svc_toggle_reverse_mute(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    opts->reverse_mute = !opts->reverse_mute;
}

void
svc_toggle_lcw_retune(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    opts->p25_lcw_retune = !opts->p25_lcw_retune;
}

void
svc_toggle_dmr_le(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    opts->dmr_le = !opts->dmr_le;
}

void
svc_set_slot_pref(dsd_opts* opts, int pref) {
    if (!opts) {
        return;
    }
    if (pref < 0) {
        pref = 0;
    }
    if (pref > 2) {
        pref = 2;
    }
    opts->slot_preference = pref;
}

void
svc_set_slots_onoff(dsd_opts* opts, int mask) {
    if (!opts) {
        return;
    }
    opts->slot1_on = (mask & 1) ? 1 : 0;
    opts->slot2_on = (mask & 2) ? 1 : 0;
}

// Inversion toggles ---------------------------------------------------------
void
svc_toggle_inv_x2(dsd_opts* opts) {
    if (opts) {
        opts->inverted_x2tdma = !opts->inverted_x2tdma;
    }
}

void
svc_toggle_inv_dmr(dsd_opts* opts) {
    if (opts) {
        opts->inverted_dmr = !opts->inverted_dmr;
    }
}

void
svc_toggle_inv_dpmr(dsd_opts* opts) {
    if (opts) {
        opts->inverted_dpmr = !opts->inverted_dpmr;
    }
}

void
svc_toggle_inv_m17(dsd_opts* opts) {
    if (opts) {
        opts->inverted_m17 = !opts->inverted_m17;
    }
}

#ifdef USE_RADIO

int
svc_rtl_enable_input(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    opts->audio_in_type = AUDIO_IN_RTL;
    /* Ensure an RTL stream is ready immediately when switching inputs. */
    return svc_rtl_restart(opts, state);
}

int
svc_rtl_restart(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    int result = 0;

    /* P25 retunes hold this guard through their synchronous wait and
     * orchestrator bookkeeping. Quiesce them before destroying the stream's
     * wait primitives or replacing the context they use. */
    p25_sm_tick_guard_enter();

    /* Stop and destroy any existing stream context. */
    if (state->rtl_ctx) {
        rtl_stream_stop(state->rtl_ctx);
        rtl_stream_destroy(state->rtl_ctx);
        state->rtl_ctx = NULL;
    }
    opts->rtl_started = 0;
    opts->rtl_needs_restart = 0;

    /* If the radio pipeline is the active input, immediately recreate and start the stream
       so changes take effect as soon as the user confirms the setting. */
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        if (rtl_stream_create(opts, &state->rtl_ctx) < 0) {
            result = -1;
            goto done;
        }
        if (rtl_stream_start(state->rtl_ctx) < 0) {
            rtl_stream_destroy(state->rtl_ctx);
            state->rtl_ctx = NULL;
            result = -1;
            goto done;
        }
        opts->rtl_started = 1;
        opts->rtl_needs_restart = 0;
    }

done:
    p25_sm_tick_guard_leave();
    return result;
}

int
svc_rtl_set_dev_index(dsd_opts* opts, dsd_state* state, int index) {
    if (!opts || !state) {
        return -1;
    }
    if (svc_radio_source_is_soapy(opts)) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    if (index < 0) {
        index = 0;
    }
    opts->rtl_dev_index = index;
    /* Changing device requires reopen */
    opts->rtl_needs_restart = 1;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return svc_rtl_restart(opts, state);
    }
    return 0;
}

int
svc_rtl_set_freq(dsd_opts* opts, dsd_state* state, uint32_t hz) {
    if (!opts) {
        return -1;
    }
    // Use centralized io/control tuning API for both RTL and rigctl
    return io_control_set_freq(opts, state, (long int)hz);
}

int
svc_rtl_set_gain(dsd_opts* opts, dsd_state* state, int value) {
    if (!opts || !state) {
        return -1;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 49) {
        value = 49;
    }
    opts->rtl_gain_value = value;
    /* Manual gain change requires reopen to apply */
    opts->rtl_needs_restart = 1;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return svc_rtl_restart(opts, state);
    }
    return 0;
}

int
svc_rtl_set_bandwidth(dsd_opts* opts, dsd_state* state, int khz) {
    if (!opts || !state) {
        return -1;
    }
    if (khz != 4 && khz != 6 && khz != 8 && khz != 12 && khz != 16 && khz != 24 && khz != 48) {
        khz = 48;
    }
    opts->rtl_dsp_bw_khz = khz;
    /* Tuner bandwidth change requires reopen */
    opts->rtl_needs_restart = 1;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return svc_rtl_restart(opts, state);
    }
    return 0;
}

int
svc_rtl_set_sql_db(dsd_opts* opts, double dB) {
    if (!opts) {
        return -1;
    }
    opts->rtl_squelch_level = dB_to_pwr(dB);
    /* Sync the demod state for channel-based squelching */
    rtl_stream_set_channel_squelch((float)opts->rtl_squelch_level);
    return 0;
}

int
svc_rtl_set_volume_mult(dsd_opts* opts, int mult) {
    if (!opts) {
        return -1;
    }
    if (mult < 0 || mult > 3) {
        mult = 1;
    }
    opts->rtl_volume_multiplier = mult;
    return 0;
}

int
svc_rtl_set_bias_tee(dsd_opts* opts, const dsd_state* state, int on) {
    if (!opts) {
        return -1;
    }
    opts->rtl_bias_tee = on ? 1 : 0;
    if (state && state->rtl_ctx) {
        /* Apply live when RTL stream is active */
        return rtl_stream_set_bias_tee(opts->rtl_bias_tee);
    }
    return 0;
}

int
svc_rtltcp_set_autotune(dsd_opts* opts, const dsd_state* state, int on) {
    if (!opts) {
        return -1;
    }
    const int requested = on ? 1 : 0;
    if (state && state->rtl_ctx) {
        /* Apply live when RTL stream is active. */
        const int rc = rtl_stream_set_rtltcp_autotune(requested);
        if (rc != 0) {
            return rc;
        }
    }
    opts->rtltcp_autotune = requested;
    /* Update env so future restarts inherit */
    dsd_setenv("DSD_NEO_TCP_AUTOTUNE", on ? "1" : "0", 1);
    return 0;
}

int
svc_rtl_set_auto_ppm(dsd_opts* opts, const dsd_state* state, int on) {
    if (!opts) {
        return -1;
    }
    opts->rtl_auto_ppm = on ? 1 : 0;
    /* Update env for persistence */
    dsd_setenv("DSD_NEO_AUTO_PPM", on ? "1" : "0", 1);
    if (state && state->rtl_ctx) {
        rtl_stream_set_auto_ppm(on ? 1 : 0);
    }
    return 0;
}
#endif
