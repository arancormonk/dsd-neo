// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief JSON command table covering every DSD_APP_CMD_* control.
 *
 * Each entry maps a stable snake_case name to a command id and an argument
 * kind. The generic dispatcher turns the request's "params" object into the
 * exact by-value payload the app-command queue expects, so the JSON boundary
 * stays a thin, auditable layer over dsd_app_command_submit() and friends.
 */

#include <dsd-neo/api/json.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_internal.h"

typedef enum {
    API_K_ACTION = 0,
    API_K_I32,
    API_K_U8,
    API_K_U32,
    API_K_U64,
    API_K_DOUBLE,
    API_K_FLOAT,
    API_K_STRING,
    API_K_ENDPOINT,
    API_K_UDP_INPUT,
    API_K_P25P2,
    API_K_TG_LISTEN,
    API_K_TG_LISTEN_ALL,
    API_K_TG_ROW,
    API_K_TG_RANGE,
    API_K_TG_EXPORT,
    API_K_TG_SELECTION,
    API_K_TONE_FILTER,
    API_K_AIRSPY,
    API_K_DSP,
    API_K_HYTERA,
    API_K_AES,
    API_K_RR_APPLY,
    API_K_RR_ACCOUNT,
    API_K_DECRYPTION,
    API_K_KEY_DIRECT,
    API_K_CONFIG,
    API_K_CONFIG_META,
} api_arg_kind;

typedef struct {
    const char* name;
    int cmd_id;
    api_arg_kind kind;
    const char* params;
} api_command_desc;

#define API_CMD(name, id, kind, params) {name, id, kind, params}

/* Ordered to follow include/dsd-neo/app_control/commands.h. */
static const api_command_desc k_commands[] = {
    API_CMD("toggle_mute", DSD_APP_CMD_TOGGLE_MUTE, API_K_ACTION, ""),
    API_CMD("toggle_compact", DSD_APP_CMD_TOGGLE_COMPACT, API_K_ACTION, ""),
    API_CMD("history_cycle", DSD_APP_CMD_HISTORY_CYCLE, API_K_ACTION, ""),

    API_CMD("slot1_toggle", DSD_APP_CMD_SLOT1_TOGGLE, API_K_ACTION, ""),
    API_CMD("slot2_toggle", DSD_APP_CMD_SLOT2_TOGGLE, API_K_ACTION, ""),
    API_CMD("slot_pref_cycle", DSD_APP_CMD_SLOT_PREF_CYCLE, API_K_ACTION, ""),

    API_CMD("gain_delta", DSD_APP_CMD_GAIN_DELTA, API_K_I32, "{value:int}"),
    API_CMD("again_delta", DSD_APP_CMD_AGAIN_DELTA, API_K_I32, "{value:int}"),

    API_CMD("trunk_toggle", DSD_APP_CMD_TRUNK_TOGGLE, API_K_ACTION, ""),
    API_CMD("scanner_toggle", DSD_APP_CMD_SCANNER_TOGGLE, API_K_ACTION, ""),

    API_CMD("payload_toggle", DSD_APP_CMD_PAYLOAD_TOGGLE, API_K_ACTION, ""),

    API_CMD("p25_ga_toggle", DSD_APP_CMD_P25_GA_TOGGLE, API_K_ACTION, ""),
    API_CMD("tg_hold_toggle", DSD_APP_CMD_TG_HOLD_TOGGLE, API_K_U8, "{value:0|1 slot}"),
    API_CMD("lpf_toggle", DSD_APP_CMD_LPF_TOGGLE, API_K_ACTION, ""),
    API_CMD("hpf_toggle", DSD_APP_CMD_HPF_TOGGLE, API_K_ACTION, ""),
    API_CMD("pbf_toggle", DSD_APP_CMD_PBF_TOGGLE, API_K_ACTION, ""),
    API_CMD("hpf_d_toggle", DSD_APP_CMD_HPF_D_TOGGLE, API_K_ACTION, ""),
    API_CMD("aggr_sync_toggle", DSD_APP_CMD_AGGR_SYNC_TOGGLE, API_K_ACTION, ""),
    API_CMD("call_alert_toggle", DSD_APP_CMD_CALL_ALERT_TOGGLE, API_K_ACTION, ""),
    API_CMD("call_alert_events_set", DSD_APP_CMD_CALL_ALERT_EVENTS_SET, API_K_U8, "{value:uint8 mask}"),

    API_CMD("const_toggle", DSD_APP_CMD_CONST_TOGGLE, API_K_ACTION, ""),
    API_CMD("const_norm_toggle", DSD_APP_CMD_CONST_NORM_TOGGLE, API_K_ACTION, ""),
    API_CMD("const_gate_delta", DSD_APP_CMD_CONST_GATE_DELTA, API_K_FLOAT, "{value:float}"),
    API_CMD("eye_toggle", DSD_APP_CMD_EYE_TOGGLE, API_K_ACTION, ""),
    API_CMD("eye_unicode_toggle", DSD_APP_CMD_EYE_UNICODE_TOGGLE, API_K_ACTION, ""),
    API_CMD("eye_color_toggle", DSD_APP_CMD_EYE_COLOR_TOGGLE, API_K_ACTION, ""),
    API_CMD("fsk_hist_toggle", DSD_APP_CMD_FSK_HIST_TOGGLE, API_K_ACTION, ""),
    API_CMD("spectrum_toggle", DSD_APP_CMD_SPECTRUM_TOGGLE, API_K_ACTION, ""),
    API_CMD("spec_size_delta", DSD_APP_CMD_SPEC_SIZE_DELTA, API_K_I32, "{value:int}"),
    API_CMD("input_vol_cycle", DSD_APP_CMD_INPUT_VOL_CYCLE, API_K_ACTION, ""),

    API_CMD("eh_next", DSD_APP_CMD_EH_NEXT, API_K_ACTION, ""),
    API_CMD("eh_prev", DSD_APP_CMD_EH_PREV, API_K_ACTION, ""),
    API_CMD("eh_toggle_slot", DSD_APP_CMD_EH_TOGGLE_SLOT, API_K_ACTION, ""),

    API_CMD("ppm_delta", DSD_APP_CMD_PPM_DELTA, API_K_I32, "{value:int}"),
    API_CMD("invert_toggle", DSD_APP_CMD_INVERT_TOGGLE, API_K_ACTION, ""),
    API_CMD("mod_toggle", DSD_APP_CMD_MOD_TOGGLE, API_K_ACTION, ""),
    API_CMD("dmr_reset", DSD_APP_CMD_DMR_RESET, API_K_ACTION, ""),
    API_CMD("gain_set", DSD_APP_CMD_GAIN_SET, API_K_I32, "{value:int}"),
    API_CMD("again_set", DSD_APP_CMD_AGAIN_SET, API_K_I32, "{value:int}"),
    API_CMD("input_warn_db_set", DSD_APP_CMD_INPUT_WARN_DB_SET, API_K_DOUBLE, "{value:double}"),
    API_CMD("input_monitor_toggle", DSD_APP_CMD_INPUT_MONITOR_TOGGLE, API_K_ACTION, ""),
    API_CMD("cosine_filter_toggle", DSD_APP_CMD_COSINE_FILTER_TOGGLE, API_K_ACTION, ""),

    API_CMD("tcp_connect_audio", DSD_APP_CMD_TCP_CONNECT_AUDIO, API_K_ACTION, ""),
    API_CMD("rigctl_connect", DSD_APP_CMD_RIGCTL_CONNECT, API_K_ACTION, ""),
    API_CMD("return_cc", DSD_APP_CMD_RETURN_CC, API_K_ACTION, ""),
    API_CMD("channel_cycle", DSD_APP_CMD_CHANNEL_CYCLE, API_K_ACTION, ""),
    API_CMD("symcap_save", DSD_APP_CMD_SYMCAP_SAVE, API_K_ACTION, ""),
    API_CMD("symcap_stop", DSD_APP_CMD_SYMCAP_STOP, API_K_ACTION, ""),
    API_CMD("replay_last", DSD_APP_CMD_REPLAY_LAST, API_K_ACTION, ""),
    API_CMD("wav_start", DSD_APP_CMD_WAV_START, API_K_ACTION, ""),
    API_CMD("wav_stop", DSD_APP_CMD_WAV_STOP, API_K_ACTION, ""),
    API_CMD("stop_playback", DSD_APP_CMD_STOP_PLAYBACK, API_K_ACTION, ""),

    API_CMD("trunk_wlist_toggle", DSD_APP_CMD_TRUNK_WLIST_TOGGLE, API_K_ACTION, ""),
    API_CMD("trunk_priv_toggle", DSD_APP_CMD_TRUNK_PRIV_TOGGLE, API_K_ACTION, ""),
    API_CMD("trunk_data_toggle", DSD_APP_CMD_TRUNK_DATA_TOGGLE, API_K_ACTION, ""),
    API_CMD("trunk_enc_toggle", DSD_APP_CMD_TRUNK_ENC_TOGGLE, API_K_ACTION, ""),
    API_CMD("wav_toggle", DSD_APP_CMD_WAV_TOGGLE, API_K_ACTION, ""),
    API_CMD("enc_lockout_clear", DSD_APP_CMD_ENC_LOCKOUT_CLEAR, API_K_ACTION, ""),
    API_CMD("scan_hold_toggle", DSD_APP_CMD_SCAN_HOLD_TOGGLE, API_K_ACTION, ""),
    API_CMD("scan_avoid", DSD_APP_CMD_SCAN_AVOID, API_K_ACTION, ""),
    API_CMD("scan_avoid_clear", DSD_APP_CMD_SCAN_AVOID_CLEAR, API_K_ACTION, ""),

    API_CMD("quit", DSD_APP_CMD_QUIT, API_K_ACTION, ""),
    API_CMD("force_priv_toggle", DSD_APP_CMD_FORCE_PRIV_TOGGLE, API_K_ACTION, ""),
    API_CMD("force_rc4_toggle", DSD_APP_CMD_FORCE_RC4_TOGGLE, API_K_ACTION, ""),
    API_CMD("trunk_group_toggle", DSD_APP_CMD_TRUNK_GROUP_TOGGLE, API_K_ACTION, ""),
    API_CMD("sim_nocar", DSD_APP_CMD_SIM_NOCAR, API_K_ACTION, ""),
    API_CMD("mod_p2_toggle", DSD_APP_CMD_MOD_P2_TOGGLE, API_K_ACTION, ""),
    API_CMD("lockout_slot", DSD_APP_CMD_LOCKOUT_SLOT, API_K_U8, "{value:0|1 slot}"),
    API_CMD("m17_tx_toggle", DSD_APP_CMD_M17_TX_TOGGLE, API_K_ACTION, ""),
    API_CMD("provoice_esk_toggle", DSD_APP_CMD_PROVOICE_ESK_TOGGLE, API_K_ACTION, ""),
    API_CMD("provoice_mode_toggle", DSD_APP_CMD_PROVOICE_MODE_TOGGLE, API_K_ACTION, ""),
    API_CMD("skip_slot", DSD_APP_CMD_SKIP_SLOT, API_K_U8, "{value:0|1 slot}"),

    API_CMD("ui_msg_clear", DSD_APP_CMD_UI_MSG_CLEAR, API_K_ACTION, ""),
    API_CMD("eh_reset", DSD_APP_CMD_EH_RESET, API_K_ACTION, ""),
    API_CMD("event_log_disable", DSD_APP_CMD_EVENT_LOG_DISABLE, API_K_ACTION, ""),
    API_CMD("event_log_set", DSD_APP_CMD_EVENT_LOG_SET, API_K_STRING, "{value:path}"),

    API_CMD("lcw_retune_toggle", DSD_APP_CMD_LCW_RETUNE_TOGGLE, API_K_ACTION, ""),
    API_CMD("p25_cc_cand_toggle", DSD_APP_CMD_P25_CC_CAND_TOGGLE, API_K_ACTION, ""),
    API_CMD("reverse_mute_toggle", DSD_APP_CMD_REVERSE_MUTE_TOGGLE, API_K_ACTION, ""),
    API_CMD("dmr_le_toggle", DSD_APP_CMD_DMR_LE_TOGGLE, API_K_ACTION, ""),
    API_CMD("all_mutes_toggle", DSD_APP_CMD_ALL_MUTES_TOGGLE, API_K_ACTION, ""),
    API_CMD("inv_x2_toggle", DSD_APP_CMD_INV_X2_TOGGLE, API_K_ACTION, ""),
    API_CMD("inv_dmr_toggle", DSD_APP_CMD_INV_DMR_TOGGLE, API_K_ACTION, ""),
    API_CMD("inv_dpmr_toggle", DSD_APP_CMD_INV_DPMR_TOGGLE, API_K_ACTION, ""),
    API_CMD("inv_m17_toggle", DSD_APP_CMD_INV_M17_TOGGLE, API_K_ACTION, ""),

    API_CMD("wav_static_open", DSD_APP_CMD_WAV_STATIC_OPEN, API_K_STRING, "{value:path}"),
    API_CMD("wav_raw_open", DSD_APP_CMD_WAV_RAW_OPEN, API_K_STRING, "{value:path}"),
    API_CMD("dsp_out_set", DSD_APP_CMD_DSP_OUT_SET, API_K_STRING, "{value:filename}"),
    API_CMD("symcap_open", DSD_APP_CMD_SYMCAP_OPEN, API_K_STRING, "{value:path}"),
    API_CMD("symbol_in_open", DSD_APP_CMD_SYMBOL_IN_OPEN, API_K_STRING, "{value:path}"),
    API_CMD("input_wav_set", DSD_APP_CMD_INPUT_WAV_SET, API_K_STRING, "{value:path}"),
    API_CMD("input_sym_stream_set", DSD_APP_CMD_INPUT_SYM_STREAM_SET, API_K_STRING, "{value:path}"),
    API_CMD("input_set_pulse", DSD_APP_CMD_INPUT_SET_PULSE, API_K_ACTION, ""),

    API_CMD("udp_out_cfg", DSD_APP_CMD_UDP_OUT_CFG, API_K_ENDPOINT, "{host,port}"),
    API_CMD("tcp_connect_audio_cfg", DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG, API_K_ENDPOINT, "{host,port}"),
    API_CMD("rigctl_connect_cfg", DSD_APP_CMD_RIGCTL_CONNECT_CFG, API_K_ENDPOINT, "{host,port}"),
    API_CMD("udp_input_cfg", DSD_APP_CMD_UDP_INPUT_CFG, API_K_UDP_INPUT, "{bind,port}"),

    API_CMD("rtl_enable_input", DSD_APP_CMD_RTL_ENABLE_INPUT, API_K_ACTION, ""),
    API_CMD("rtl_restart", DSD_APP_CMD_RTL_RESTART, API_K_ACTION, ""),
    API_CMD("rtl_set_dev", DSD_APP_CMD_RTL_SET_DEV, API_K_I32, "{value:int}"),
    API_CMD("rtl_set_freq", DSD_APP_CMD_RTL_SET_FREQ, API_K_U32, "{value:hz}"),
    API_CMD("rtl_set_gain", DSD_APP_CMD_RTL_SET_GAIN, API_K_I32, "{value:int}"),
    API_CMD("rtl_set_ppm", DSD_APP_CMD_RTL_SET_PPM, API_K_I32, "{value:int}"),
    API_CMD("rtl_set_bw", DSD_APP_CMD_RTL_SET_BW, API_K_I32, "{value:khz}"),
    API_CMD("rtl_set_sql_db", DSD_APP_CMD_RTL_SET_SQL_DB, API_K_DOUBLE, "{value:db}"),
    API_CMD("rtl_set_vol_mult", DSD_APP_CMD_RTL_SET_VOL_MULT, API_K_I32, "{value:int}"),
    API_CMD("rtl_set_bias_tee", DSD_APP_CMD_RTL_SET_BIAS_TEE, API_K_I32, "{value:0|1}"),
    API_CMD("rtltcp_set_autotune", DSD_APP_CMD_RTLTCP_SET_AUTOTUNE, API_K_I32, "{value:0|1}"),
    API_CMD("rtl_set_auto_ppm", DSD_APP_CMD_RTL_SET_AUTO_PPM, API_K_I32, "{value:0|1}"),
    API_CMD("manual_tune", DSD_APP_CMD_MANUAL_TUNE, API_K_U32, "{value:hz}"),
    API_CMD("tuner_release", DSD_APP_CMD_TUNER_RELEASE, API_K_ACTION, ""),
    API_CMD("mod_set", DSD_APP_CMD_MOD_SET, API_K_I32, "{value:0|1|2}"),
    API_CMD("decode_mode_set", DSD_APP_CMD_DECODE_MODE_SET, API_K_I32, "{value:dsdneoUserDecodeMode}"),
    API_CMD("trunk_set", DSD_APP_CMD_TRUNK_SET, API_K_I32, "{value:0|1}"),
    API_CMD("airspy_set", DSD_APP_CMD_AIRSPY_SET, API_K_AIRSPY, "{key,value}"),
    API_CMD("airspy_enable_input", DSD_APP_CMD_AIRSPY_ENABLE_INPUT, API_K_ACTION, ""),

    API_CMD("rigctl_set_mod_bw", DSD_APP_CMD_RIGCTL_SET_MOD_BW, API_K_I32, "{value:hz}"),
    API_CMD("tg_hold_set", DSD_APP_CMD_TG_HOLD_SET, API_K_U32, "{value:talkgroup}"),
    API_CMD("hangtime_set", DSD_APP_CMD_HANGTIME_SET, API_K_DOUBLE, "{value:seconds}"),
    API_CMD("slot_pref_set", DSD_APP_CMD_SLOT_PREF_SET, API_K_I32, "{value:0|1|2}"),
    API_CMD("slots_onoff_set", DSD_APP_CMD_SLOTS_ONOFF_SET, API_K_I32, "{value:mask}"),
    API_CMD("scan_voice_only_set", DSD_APP_CMD_SCAN_VOICE_ONLY_SET, API_K_I32, "{value:0|1}"),
    API_CMD("scan_voice_qualify_ms_set", DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET, API_K_I32, "{value:ms}"),
    API_CMD("scan_voice_hold_ms_set", DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET, API_K_I32, "{value:ms}"),
    API_CMD("nfm_bandwidth_set", DSD_APP_CMD_NFM_BANDWIDTH_SET, API_K_I32, "{value:hz,0=default}"),
    API_CMD("am_bandwidth_set", DSD_APP_CMD_AM_BANDWIDTH_SET, API_K_I32, "{value:hz,0=default}"),
    API_CMD("tone_filter_set", DSD_APP_CMD_TONE_FILTER_SET, API_K_TONE_FILTER, "{mode,list,keep_list}"),

    API_CMD("pulse_out_set", DSD_APP_CMD_PULSE_OUT_SET, API_K_STRING, "{value:name}"),
    API_CMD("pulse_in_set", DSD_APP_CMD_PULSE_IN_SET, API_K_STRING, "{value:name}"),

    API_CMD("input_vol_set", DSD_APP_CMD_INPUT_VOL_SET, API_K_I32, "{value:mult}"),

    API_CMD("lrrp_set_home", DSD_APP_CMD_LRRP_SET_HOME, API_K_ACTION, ""),
    API_CMD("lrrp_set_dsdp", DSD_APP_CMD_LRRP_SET_DSDP, API_K_ACTION, ""),
    API_CMD("lrrp_set_custom", DSD_APP_CMD_LRRP_SET_CUSTOM, API_K_STRING, "{value:path}"),
    API_CMD("lrrp_disable", DSD_APP_CMD_LRRP_DISABLE, API_K_ACTION, ""),

    API_CMD("import_channel_map", DSD_APP_CMD_IMPORT_CHANNEL_MAP, API_K_STRING, "{value:path}"),
    API_CMD("import_group_list", DSD_APP_CMD_IMPORT_GROUP_LIST, API_K_STRING, "{value:path}"),
    API_CMD("import_keys_dec", DSD_APP_CMD_IMPORT_KEYS_DEC, API_K_STRING, "{value:path}"),
    API_CMD("import_keys_hex", DSD_APP_CMD_IMPORT_KEYS_HEX, API_K_STRING, "{value:path}"),
    API_CMD("import_channel_map_clear", DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR, API_K_ACTION, ""),
    API_CMD("import_group_list_clear", DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR, API_K_ACTION, ""),
    API_CMD("import_keys_clear", DSD_APP_CMD_IMPORT_KEYS_CLEAR, API_K_ACTION, ""),
    API_CMD("import_p25_bandplan", DSD_APP_CMD_IMPORT_P25_BANDPLAN, API_K_STRING, "{value:path}"),
    API_CMD("export_p25_bandplan", DSD_APP_CMD_EXPORT_P25_BANDPLAN, API_K_STRING, "{value:path}"),

    API_CMD("rr_apply_import", DSD_APP_CMD_RR_APPLY_IMPORT, API_K_RR_APPLY,
            "{decode_mode,edacs_ea,edacs_esk,simulcast_qpsk,p25_prefer_candidates,trunking,scanner,chan_path,"
            "group_path,tune_hz}"),
    API_CMD("rr_account_set", DSD_APP_CMD_RR_ACCOUNT_SET, API_K_RR_ACCOUNT, "{username,app_key}"),

    API_CMD("import_src_list", DSD_APP_CMD_IMPORT_SRC_LIST, API_K_STRING, "{value:path}"),
    API_CMD("import_src_list_clear", DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR, API_K_ACTION, ""),

    API_CMD("p25_p2_params_set", DSD_APP_CMD_P25_P2_PARAMS_SET, API_K_P25P2, "{wacn,sysid,cc}"),

    API_CMD("tg_listen_set", DSD_APP_CMD_TG_LISTEN_SET, API_K_TG_LISTEN, "{id_start,id_end,listen}"),
    API_CMD("tg_listen_set_all", DSD_APP_CMD_TG_LISTEN_SET_ALL, API_K_TG_LISTEN_ALL, "{listen,tags}"),
    API_CMD("tg_row_set", DSD_APP_CMD_TG_ROW_SET, API_K_TG_ROW,
            "{id_start,id_end,fields,listen,priority,preempt,name,tags,policy_context,policy_generation}"),
    API_CMD("tg_row_remove", DSD_APP_CMD_TG_ROW_REMOVE, API_K_TG_RANGE,
            "{id_start,id_end,policy_context,"
            "policy_generation}"),
    API_CMD("tg_list_export", DSD_APP_CMD_TG_LIST_EXPORT, API_K_TG_EXPORT, "{policy_context,policy_generation,path}"),
    API_CMD("tg_selection_set", DSD_APP_CMD_TG_SELECTION_SET, API_K_TG_SELECTION,
            "{policy_context,policy_generation,count,listening,path}"),
    API_CMD("tg_lockout_persist_set", DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET, API_K_I32, "{value:0|1}"),
    API_CMD("tg_session_avoid_clear", DSD_APP_CMD_TG_SESSION_AVOID_CLEAR, API_K_U64, "{value:context}"),

    API_CMD("ui_show_dsp_panel_toggle", DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE, API_K_ACTION, ""),
    API_CMD("ui_show_p25_metrics_toggle", DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE, API_K_ACTION, ""),
    API_CMD("ui_show_p25_affil_toggle", DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE, API_K_ACTION, ""),
    API_CMD("ui_show_p25_neighbors_toggle", DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE, API_K_ACTION, ""),
    API_CMD("ui_show_p25_iden_toggle", DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE, API_K_ACTION, ""),
    API_CMD("ui_show_p25_ccc_toggle", DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE, API_K_ACTION, ""),
    API_CMD("ui_show_channels_toggle", DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE, API_K_ACTION, ""),
    API_CMD("ui_show_p25_callsign_toggle", DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE, API_K_ACTION, ""),

    API_CMD("key_basic_set", DSD_APP_CMD_KEY_BASIC_SET, API_K_U32, "{value:uint32}"),
    API_CMD("key_scrambler_set", DSD_APP_CMD_KEY_SCRAMBLER_SET, API_K_U32, "{value:uint32}"),
    API_CMD("key_rc4des_set", DSD_APP_CMD_KEY_RC4DES_SET, API_K_U64, "{value:uint64}"),
    API_CMD("key_hytera_set", DSD_APP_CMD_KEY_HYTERA_SET, API_K_HYTERA, "{H,K1,K2,K3,K4}"),
    API_CMD("key_aes_set", DSD_APP_CMD_KEY_AES_SET, API_K_AES, "{K1,K2,K3,K4}"),
    API_CMD("key_tyt_ap_set", DSD_APP_CMD_KEY_TYT_AP_SET, API_K_STRING, "{value:hex}"),
    API_CMD("key_retevis_rc2_set", DSD_APP_CMD_KEY_RETEVIS_RC2_SET, API_K_STRING, "{value:hex}"),
    API_CMD("key_tyt_ep_set", DSD_APP_CMD_KEY_TYT_EP_SET, API_K_STRING, "{value:hex}"),
    API_CMD("key_ken_scr_set", DSD_APP_CMD_KEY_KEN_SCR_SET, API_K_STRING, "{value:decimal}"),
    API_CMD("key_anytone_bp_set", DSD_APP_CMD_KEY_ANYTONE_BP_SET, API_K_STRING, "{value:hex16}"),
    API_CMD("key_xor_set", DSD_APP_CMD_KEY_XOR_SET, API_K_STRING, "{value:len:hex}"),
    API_CMD("m17_user_data_set", DSD_APP_CMD_M17_USER_DATA_SET, API_K_STRING, "{value:string}"),
    API_CMD("key_direct_set", DSD_APP_CMD_KEY_DIRECT_SET, API_K_KEY_DIRECT, "{key_type,value}"),
    API_CMD("force_key_set", DSD_APP_CMD_FORCE_KEY_SET, API_K_I32, "{value:0|1|2}"),
    API_CMD("decryption_apply", DSD_APP_CMD_DECRYPTION_APPLY, API_K_DECRYPTION, "see docs/api.md"),

    API_CMD("dsp_op", DSD_APP_CMD_DSP_OP, API_K_DSP, "{op,a,b,c,d}"),
    API_CMD("config_apply", DSD_APP_CMD_CONFIG_APPLY, API_K_CONFIG, "{sections:{section:{key:value}}}"),
    API_CMD("config_metadata_set", DSD_APP_CMD_CONFIG_METADATA_SET, API_K_CONFIG_META, "{autosave_enabled,path}"),
};

enum { API_COMMAND_COUNT = (int)(sizeof k_commands / sizeof k_commands[0]) };

static const api_command_desc*
find_command(const char* name) {
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < API_COMMAND_COUNT; i++) {
        if (strcmp(k_commands[i].name, name) == 0) {
            return &k_commands[i];
        }
    }
    return NULL;
}

int
dsd_api_command_count_total(void) {
    return API_COMMAND_COUNT;
}

/*============================================================================
 * Param helpers
 *============================================================================*/

static int
get_i64(const dsd_json_node* params, int64_t* out) {
    return dsd_json_as_i64(dsd_json_obj_get(params, "value"), out);
}

static int
get_u64(const dsd_json_node* params, uint64_t* out) {
    return dsd_json_as_u64(dsd_json_obj_get(params, "value"), out);
}

static int
get_double(const dsd_json_node* params, double* out) {
    return dsd_json_as_double(dsd_json_obj_get(params, "value"), out);
}

static void
copy_string(char* dst, size_t cap, const dsd_json_node* params, const char* key) {
    const char* s = dsd_json_as_str(dsd_json_obj_get(params, key));
    DSD_SNPRINTF(dst, cap, "%s", s ? s : "");
}

static int
get_u64_key(const dsd_json_node* params, const char* key, uint64_t* out) {
    return dsd_json_as_u64(dsd_json_obj_get(params, key), out);
}

static int
get_i64_key(const dsd_json_node* params, const char* key, int64_t* out) {
    return dsd_json_as_i64(dsd_json_obj_get(params, key), out);
}

static int
get_bool_key(const dsd_json_node* params, const char* key, int* out) {
    return dsd_json_as_bool(dsd_json_obj_get(params, key), out);
}

/*============================================================================
 * Dispatch
 *============================================================================*/

static int
submit_action(int cmd_id) {
    return dsd_app_command_submit(cmd_id, NULL, 0U);
}

static int
submit_i32(int cmd_id, const dsd_json_node* params) {
    int64_t v = 0;
    if (get_i64(params, &v) != 0 || v < INT32_MIN || v > INT32_MAX) {
        return -2;
    }
    const int32_t out = (int32_t)v;
    return dsd_app_command_submit(cmd_id, &out, sizeof out);
}

static int
submit_u8(int cmd_id, const dsd_json_node* params) {
    int64_t v = 0;
    if (get_i64(params, &v) != 0 || v < 0 || v > 255) {
        return -2;
    }
    const uint8_t out = (uint8_t)v;
    return dsd_app_command_submit(cmd_id, &out, sizeof out);
}

static int
submit_u32(int cmd_id, const dsd_json_node* params) {
    int64_t v = 0;
    if (get_i64(params, &v) != 0 || v < 0 || v > (int64_t)UINT32_MAX) {
        return -2;
    }
    const uint32_t out = (uint32_t)v;
    return dsd_app_command_submit(cmd_id, &out, sizeof out);
}

static int
submit_u64(int cmd_id, const dsd_json_node* params) {
    uint64_t v = 0;
    if (get_u64(params, &v) != 0) {
        return -2;
    }
    return dsd_app_command_submit(cmd_id, &v, sizeof v);
}

static int
submit_double(int cmd_id, const dsd_json_node* params) {
    double v = 0.0;
    if (get_double(params, &v) != 0) {
        return -2;
    }
    return dsd_app_command_submit(cmd_id, &v, sizeof v);
}

static int
submit_float(int cmd_id, const dsd_json_node* params) {
    double v = 0.0;
    if (get_double(params, &v) != 0) {
        return -2;
    }
    const float out = (float)v;
    return dsd_app_command_submit(cmd_id, &out, sizeof out);
}

static int
submit_string(int cmd_id, const dsd_json_node* params) {
    const char* s = dsd_json_as_str(dsd_json_obj_get(params, "value"));
    if (s == NULL) {
        return -2;
    }
    return dsd_app_command_set_string(cmd_id, s);
}

static int
submit_endpoint(int cmd_id, const dsd_json_node* params) {
    dsd_app_endpoint_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    copy_string(p.host, sizeof p.host, params, "host");
    int64_t port = 0;
    if (dsd_json_as_i64(dsd_json_obj_get(params, "port"), &port) != 0 || port < 0 || port > 65535
        || p.host[0] == '\0') {
        return -2;
    }
    p.port = (int32_t)port;
    return dsd_app_command_submit(cmd_id, &p, sizeof p);
}

static int
submit_udp_input(int cmd_id, const dsd_json_node* params) {
    dsd_app_udp_input_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    copy_string(p.bind, sizeof p.bind, params, "bind");
    int64_t port = 0;
    if (dsd_json_as_i64(dsd_json_obj_get(params, "port"), &port) != 0 || port < 0 || port > 65535) {
        return -2;
    }
    p.port = (int32_t)port;
    return dsd_app_command_submit(cmd_id, &p, sizeof p);
}

static int
submit_p25p2(int cmd_id, const dsd_json_node* params) {
    dsd_app_p25_p2_params_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    if (get_u64_key(params, "wacn", &p.wacn) != 0 || get_u64_key(params, "sysid", &p.sysid) != 0
        || get_u64_key(params, "cc", &p.cc) != 0) {
        return -2;
    }
    (void)cmd_id;
    return dsd_app_command_set_p25_p2_params(&p);
}

static int
submit_tg_listen(int cmd_id, const dsd_json_node* params) {
    dsd_app_tg_listen_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t start = 0;
    int64_t end = 0;
    int listen = 0;
    if (get_i64_key(params, "id_start", &start) != 0 || get_i64_key(params, "id_end", &end) != 0
        || get_bool_key(params, "listen", &listen) != 0 || start < 0 || end < start || start > (int64_t)UINT32_MAX
        || end > (int64_t)UINT32_MAX) {
        return -2;
    }
    p.id_start = (uint32_t)start;
    p.id_end = (uint32_t)end;
    p.listen = listen;
    (void)cmd_id;
    return dsd_app_command_set_tg_listen(&p);
}

static int
submit_tg_listen_all(int cmd_id, const dsd_json_node* params) {
    dsd_app_tg_listen_all_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int listen = 0;
    if (get_bool_key(params, "listen", &listen) != 0) {
        return -2;
    }
    p.listen = listen;
    copy_string(p.tags, sizeof p.tags, params, "tags");
    (void)cmd_id;
    return dsd_app_command_set_tg_listen_all(&p);
}

static int
submit_tg_row(int cmd_id, const dsd_json_node* params) {
    dsd_app_tg_row_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t start = 0, end = 0, fields = 0, priority = 0;
    int listen = 0;
    int preempt = 0;
    uint64_t context = 0;
    if (get_i64_key(params, "id_start", &start) != 0 || get_i64_key(params, "id_end", &end) != 0
        || get_i64_key(params, "fields", &fields) != 0 || start < 0 || end < start || start > (int64_t)UINT32_MAX
        || end > (int64_t)UINT32_MAX || fields < 0 || fields > 0x1F) {
        return -2;
    }
    (void)get_bool_key(params, "listen", &listen);
    (void)get_i64_key(params, "priority", &priority);
    (void)get_bool_key(params, "preempt", &preempt);
    (void)get_u64_key(params, "policy_context", &context);
    uint64_t generation = 0;
    (void)get_u64_key(params, "policy_generation", &generation);
    p.id_start = (uint32_t)start;
    p.id_end = (uint32_t)end;
    p.fields = (uint32_t)fields;
    p.listen = listen;
    p.priority = (int32_t)priority;
    p.preempt = preempt;
    p.policy_context = context;
    p.policy_generation = (unsigned int)generation;
    copy_string(p.name, sizeof p.name, params, "name");
    copy_string(p.tags, sizeof p.tags, params, "tags");
    (void)cmd_id;
    return dsd_app_command_submit(DSD_APP_CMD_TG_ROW_SET, &p, sizeof p);
}

static int
submit_tg_range(int cmd_id, const dsd_json_node* params) {
    dsd_app_tg_range_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t start = 0, end = 0;
    if (get_i64_key(params, "id_start", &start) != 0 || get_i64_key(params, "id_end", &end) != 0 || start < 0
        || end < start || start > (int64_t)UINT32_MAX || end > (int64_t)UINT32_MAX) {
        return -2;
    }
    uint64_t context = 0, generation = 0;
    (void)get_u64_key(params, "policy_context", &context);
    (void)get_u64_key(params, "policy_generation", &generation);
    p.id_start = (uint32_t)start;
    p.id_end = (uint32_t)end;
    p.policy_context = context;
    p.policy_generation = (unsigned int)generation;
    (void)cmd_id;
    return dsd_app_command_submit(DSD_APP_CMD_TG_ROW_REMOVE, &p, sizeof p);
}

static int
submit_tg_export(int cmd_id, const dsd_json_node* params) {
    const char* path = dsd_json_as_str(dsd_json_obj_get(params, "path"));
    if (path == NULL || path[0] == '\0') {
        return -2;
    }
    const size_t path_len = strlen(path);
    if (path_len >= 1024U) {
        return -2;
    }
    uint64_t context = 0, generation = 0;
    (void)get_u64_key(params, "policy_context", &context);
    (void)get_u64_key(params, "policy_generation", &generation);
    const size_t base = offsetof(dsd_app_tg_export_payload, path);
    const size_t total = base + path_len + 1U;
    dsd_app_tg_export_payload* p = (dsd_app_tg_export_payload*)malloc(total);
    if (p == NULL) {
        return -1;
    }
    DSD_MEMSET(p, 0, total);
    p->policy_context = context;
    p->policy_generation = (unsigned int)generation;
    memcpy(p->path, path, path_len + 1U);
    (void)cmd_id;
    const int rc = dsd_app_command_submit(DSD_APP_CMD_TG_LIST_EXPORT, p, total);
    free(p);
    return rc;
}

static int
submit_tg_selection(int cmd_id, const dsd_json_node* params) {
    dsd_app_tg_selection_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    uint64_t context = 0, generation = 0;
    (void)get_u64_key(params, "policy_context", &context);
    (void)get_u64_key(params, "policy_generation", &generation);
    int64_t count = 0;
    int listen = 0;
    (void)get_i64_key(params, "count", &count);
    (void)get_bool_key(params, "listening", &listen);
    const char* path = dsd_json_as_str(dsd_json_obj_get(params, "path"));
    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof p.selection_path) {
        return -2;
    }
    p.policy_context = context;
    p.policy_generation = (uint32_t)generation;
    p.count = (count > 0) ? (uint32_t)count : 0U;
    p.listening = listen;
    DSD_SNPRINTF(p.selection_path, sizeof p.selection_path, "%s", path);
    (void)cmd_id;
    return dsd_app_command_submit(DSD_APP_CMD_TG_SELECTION_SET, &p, sizeof p);
}

static int
submit_tone_filter(int cmd_id, const dsd_json_node* params) {
    (void)cmd_id;
    int64_t mode = 0;
    if (get_i64_key(params, "mode", &mode) != 0 || mode < 0 || mode > 2) {
        return -2;
    }
    int keep = 0;
    (void)get_bool_key(params, "keep_list", &keep);
    const char* list = dsd_json_as_str(dsd_json_obj_get(params, "list"));
    if (keep) {
        return dsd_app_command_set_tone_filter_mode((int32_t)mode);
    }
    return dsd_app_command_set_tone_filter((int32_t)mode, list ? list : "");
}

static int
submit_airspy(int cmd_id, const dsd_json_node* params) {
    dsd_app_airspy_setting_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    const char* key = dsd_json_as_str(dsd_json_obj_get(params, "key"));
    const char* value = dsd_json_as_str(dsd_json_obj_get(params, "value"));
    if (key == NULL || value == NULL || strlen(key) >= sizeof p.key || strlen(value) >= sizeof p.value) {
        return -2;
    }
    DSD_SNPRINTF(p.key, sizeof p.key, "%s", key);
    DSD_SNPRINTF(p.value, sizeof p.value, "%s", value);
    (void)cmd_id;
    return dsd_app_command_submit(DSD_APP_CMD_AIRSPY_SET, &p, sizeof p);
}

static int
submit_dsp(int cmd_id, const dsd_json_node* params) {
    dsd_app_dsp_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t op = 0, a = 0, b = 0, c = 0, d = 0;
    if (get_i64_key(params, "op", &op) != 0 || get_i64_key(params, "a", &a) != 0 || get_i64_key(params, "b", &b) != 0
        || get_i64_key(params, "c", &c) != 0 || get_i64_key(params, "d", &d) != 0) {
        return -2;
    }
    p.op = (int)op;
    p.a = (int)a;
    p.b = (int)b;
    p.c = (int)c;
    p.d = (int)d;
    (void)cmd_id;
    return dsd_app_command_dsp_op(&p);
}

static int
submit_hytera(int cmd_id, const dsd_json_node* params) {
    dsd_app_hytera_key_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    if (get_u64_key(params, "H", &p.H) != 0 || get_u64_key(params, "K1", &p.K1) != 0
        || get_u64_key(params, "K2", &p.K2) != 0 || get_u64_key(params, "K3", &p.K3) != 0
        || get_u64_key(params, "K4", &p.K4) != 0) {
        return -2;
    }
    (void)cmd_id;
    const int rc = dsd_app_command_set_hytera_key(&p);
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static int
submit_aes(int cmd_id, const dsd_json_node* params) {
    dsd_app_aes_key_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    if (get_u64_key(params, "K1", &p.K1) != 0 || get_u64_key(params, "K2", &p.K2) != 0
        || get_u64_key(params, "K3", &p.K3) != 0 || get_u64_key(params, "K4", &p.K4) != 0) {
        return -2;
    }
    (void)cmd_id;
    const int rc = dsd_app_command_set_aes_key(&p);
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static int
submit_rr_apply(int cmd_id, const dsd_json_node* params) {
    dsd_app_rr_apply_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int64_t mode = 0;
    (void)get_i64_key(params, "decode_mode", &mode);
    p.decode_mode = (int32_t)mode;
    int flag = 0;
    if (get_bool_key(params, "edacs_ea", &flag) == 0) {
        p.edacs_ea = (uint8_t)flag;
    }
    if (get_bool_key(params, "edacs_esk", &flag) == 0) {
        p.edacs_esk = (uint8_t)flag;
    }
    if (get_bool_key(params, "simulcast_qpsk", &flag) == 0) {
        p.simulcast_qpsk = (uint8_t)flag;
    }
    if (get_bool_key(params, "p25_prefer_candidates", &flag) == 0) {
        p.p25_prefer_candidates = (uint8_t)flag;
    }
    if (get_bool_key(params, "trunking", &flag) == 0) {
        p.trunking = (uint8_t)flag;
    }
    if (get_bool_key(params, "scanner", &flag) == 0) {
        p.scanner = (uint8_t)flag;
    }
    int64_t tune = 0;
    if (get_i64_key(params, "tune_hz", &tune) == 0 && tune >= 0 && tune <= (int64_t)UINT32_MAX) {
        p.tune_hz = (uint32_t)tune;
    }
    copy_string(p.chan_path, sizeof p.chan_path, params, "chan_path");
    copy_string(p.group_path, sizeof p.group_path, params, "group_path");
    p.has_chan = (p.chan_path[0] != '\0') ? 1U : 0U;
    p.has_group = (p.group_path[0] != '\0') ? 1U : 0U;
    (void)cmd_id;
    return dsd_app_command_set_rr_apply(&p);
}

static int
submit_rr_account(int cmd_id, const dsd_json_node* params) {
    dsd_app_rr_account_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    copy_string(p.username, sizeof p.username, params, "username");
    copy_string(p.app_key, sizeof p.app_key, params, "app_key");
    (void)cmd_id;
    const int rc = dsd_app_command_set_rr_account(&p);
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static int
submit_key_direct(int cmd_id, const dsd_json_node* params) {
    dsd_app_key_direct_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    const char* type = dsd_json_as_str(dsd_json_obj_get(params, "key_type"));
    const char* value = dsd_json_as_str(dsd_json_obj_get(params, "value"));
    if (type == NULL || value == NULL || strlen(value) >= sizeof p.value) {
        return -2;
    }
    if (strcmp(type, "basic") == 0) {
        p.key_type = DSD_APP_KEY_TYPE_BASIC;
    } else if (strcmp(type, "hex") == 0) {
        p.key_type = DSD_APP_KEY_TYPE_HEX;
    } else if (strcmp(type, "rc4") == 0) {
        p.key_type = DSD_APP_KEY_TYPE_RC4;
    } else if (strcmp(type, "scrambler") == 0) {
        p.key_type = DSD_APP_KEY_TYPE_SCRAMBLER;
    } else if (strcmp(type, "m17_scrambler") == 0) {
        p.key_type = DSD_APP_KEY_TYPE_M17_SCRAMBLER;
    } else if (strcmp(type, "m17_aes") == 0) {
        p.key_type = DSD_APP_KEY_TYPE_M17_AES;
    } else {
        return -2;
    }
    DSD_SNPRINTF(p.value, sizeof p.value, "%s", value);
    (void)cmd_id;
    const int rc = dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &p, sizeof p);
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static uint64_t
next_request_id(void) {
    static uint64_t counter = 0;
    counter++;
    if (counter == 0U) {
        counter = 1U;
    }
    return counter;
}

static int
submit_decryption(int cmd_id, const dsd_json_node* params) {
    dsd_app_decryption_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    p.request_id = next_request_id();
    p.session_generation = dsd_app_command_session_generation();
    int64_t v = 0;
    if (get_i64_key(params, "tune_generation", &v) == 0 && v >= 0) {
        p.tune_generation = (uint64_t)v;
    }
    if (get_i64_key(params, "key_epoch", &v) == 0 && v >= 0) {
        p.key_epoch = (uint64_t)v;
    }
    if (get_i64_key(params, "scope", &v) == 0) {
        p.scope = (int32_t)v;
    }
    if (get_i64_key(params, "source", &v) == 0) {
        p.source = (int32_t)v;
    }
    if (get_i64_key(params, "force", &v) == 0) {
        p.force = (int32_t)v;
    }
    const char* type = dsd_json_as_str(dsd_json_obj_get(params, "type"));
    if (type != NULL) {
        if (strcmp(type, "basic") == 0) {
            p.key_type = DSD_APP_KEY_TYPE_BASIC;
        } else if (strcmp(type, "hex") == 0) {
            p.key_type = DSD_APP_KEY_TYPE_HEX;
        } else if (strcmp(type, "rc4") == 0) {
            p.key_type = DSD_APP_KEY_TYPE_RC4;
        } else if (strcmp(type, "scrambler") == 0) {
            p.key_type = DSD_APP_KEY_TYPE_SCRAMBLER;
        } else if (strcmp(type, "m17_scrambler") == 0) {
            p.key_type = DSD_APP_KEY_TYPE_M17_SCRAMBLER;
        } else if (strcmp(type, "m17_aes") == 0) {
            p.key_type = DSD_APP_KEY_TYPE_M17_AES;
        }
    }
    copy_string(p.target_id, sizeof p.target_id, params, "target");
    copy_string(p.profile_ref, sizeof p.profile_ref, params, "profile");
    copy_string(p.value, sizeof p.value, params, "value");
    copy_string(p.keys_hex, sizeof p.keys_hex, params, "hex");
    copy_string(p.keys_dec, sizeof p.keys_dec, params, "dec");
    copy_string(p.map_file, sizeof p.map_file, params, "map");

    int64_t fields = 0;
    if (get_i64_key(params, "fields", &fields) == 0 && fields > 0) {
        p.fields = (uint32_t)fields;
    } else {
        uint32_t f = 0;
        if (p.value[0] != '\0' || p.keys_hex[0] != '\0' || p.keys_dec[0] != '\0') {
            f |= DSD_APP_DECRYPTION_MATERIAL;
        }
        if (p.map_file[0] != '\0') {
            f |= DSD_APP_DECRYPTION_MAP;
        }
        if (dsd_json_obj_get(params, "force") != NULL) {
            f |= DSD_APP_DECRYPTION_FORCE;
        }
        p.fields = f;
    }
    (void)cmd_id;
    const int rc = dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &p, sizeof p);
    DSD_SECURE_ZERO(&p, sizeof p);
    return rc;
}

static int
submit_config(int cmd_id, const dsd_json_node* params) {
    const dsd_json_node* sections = dsd_json_obj_get(params, "sections");
    if (sections == NULL || sections->type != DSD_JSON_OBJECT) {
        return -2;
    }
    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    for (size_t i = 0; i < sections->count; i++) {
        const char* section = sections->keys[i];
        const dsd_json_node* kv = sections->items[i];
        if (section == NULL || kv == NULL || kv->type != DSD_JSON_OBJECT) {
            continue;
        }
        for (size_t j = 0; j < kv->count; j++) {
            const char* key = kv->keys[j];
            const dsd_json_node* val = kv->items[j];
            if (key == NULL || val == NULL) {
                continue;
            }
            char text[256];
            if (val->type == DSD_JSON_STRING) {
                DSD_SNPRINTF(text, sizeof text, "%s", val->string ? val->string : "");
            } else if (val->type == DSD_JSON_BOOL) {
                DSD_SNPRINTF(text, sizeof text, "%d", val->boolean ? 1 : 0);
            } else if (val->type == DSD_JSON_NUMBER) {
                DSD_SNPRINTF(text, sizeof text, "%.10g", val->number);
            } else {
                continue;
            }
            (void)dsd_user_config_apply_key(&cfg, section, key, text);
        }
    }
    (void)cmd_id;
    return dsd_app_command_apply_config(&cfg);
}

static int
submit_config_meta(int cmd_id, const dsd_json_node* params) {
    dsd_app_config_metadata_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    int enabled = 0;
    (void)get_bool_key(params, "autosave_enabled", &enabled);
    p.autosave_enabled = enabled;
    copy_string(p.path, sizeof p.path, params, "path");
    (void)cmd_id;
    return dsd_app_command_set_config_metadata(&p);
}

static int
dispatch(const api_command_desc* desc, const dsd_json_node* params) {
    switch (desc->kind) {
        case API_K_ACTION: return submit_action(desc->cmd_id);
        case API_K_I32: return submit_i32(desc->cmd_id, params);
        case API_K_U8: return submit_u8(desc->cmd_id, params);
        case API_K_U32: return submit_u32(desc->cmd_id, params);
        case API_K_U64: return submit_u64(desc->cmd_id, params);
        case API_K_DOUBLE: return submit_double(desc->cmd_id, params);
        case API_K_FLOAT: return submit_float(desc->cmd_id, params);
        case API_K_STRING: return submit_string(desc->cmd_id, params);
        case API_K_ENDPOINT: return submit_endpoint(desc->cmd_id, params);
        case API_K_UDP_INPUT: return submit_udp_input(desc->cmd_id, params);
        case API_K_P25P2: return submit_p25p2(desc->cmd_id, params);
        case API_K_TG_LISTEN: return submit_tg_listen(desc->cmd_id, params);
        case API_K_TG_LISTEN_ALL: return submit_tg_listen_all(desc->cmd_id, params);
        case API_K_TG_ROW: return submit_tg_row(desc->cmd_id, params);
        case API_K_TG_RANGE: return submit_tg_range(desc->cmd_id, params);
        case API_K_TG_EXPORT: return submit_tg_export(desc->cmd_id, params);
        case API_K_TG_SELECTION: return submit_tg_selection(desc->cmd_id, params);
        case API_K_TONE_FILTER: return submit_tone_filter(desc->cmd_id, params);
        case API_K_AIRSPY: return submit_airspy(desc->cmd_id, params);
        case API_K_DSP: return submit_dsp(desc->cmd_id, params);
        case API_K_HYTERA: return submit_hytera(desc->cmd_id, params);
        case API_K_AES: return submit_aes(desc->cmd_id, params);
        case API_K_RR_APPLY: return submit_rr_apply(desc->cmd_id, params);
        case API_K_RR_ACCOUNT: return submit_rr_account(desc->cmd_id, params);
        case API_K_DECRYPTION: return submit_decryption(desc->cmd_id, params);
        case API_K_KEY_DIRECT: return submit_key_direct(desc->cmd_id, params);
        case API_K_CONFIG: return submit_config(desc->cmd_id, params);
        case API_K_CONFIG_META: return submit_config_meta(desc->cmd_id, params);
        default: return -1;
    }
}

int
dsd_api_command_execute(const dsd_json_node* request, dsd_json_buf* out) {
    dsd_json_buf_reset(out);
    dsd_json_writer w;
    dsd_json_writer_init(&w, out);
    const dsd_json_node* id = dsd_json_obj_get(request, "id");
    const char* name = dsd_json_as_str(dsd_json_obj_get(request, "cmd"));
    const dsd_json_node* params = dsd_json_obj_get(request, "params");

    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_key(&w, "id");
    if (id == NULL || id->type == DSD_JSON_NULL) {
        (void)dsd_json_value_null(&w);
    } else if (id->type == DSD_JSON_NUMBER) {
        (void)dsd_json_value_double(&w, id->number);
    } else if (id->type == DSD_JSON_STRING) {
        (void)dsd_json_value_str(&w, id->string);
    } else {
        (void)dsd_json_value_null(&w);
    }

    const api_command_desc* desc = find_command(name);
    if (desc == NULL) {
        (void)dsd_json_kv_bool(&w, "ok", 0);
        (void)dsd_json_kv_str(&w, "cmd", name ? name : "");
        (void)dsd_json_key(&w, "error");
        (void)dsd_json_obj_begin(&w);
        (void)dsd_json_kv_str(&w, "code", "unknown_command");
        (void)dsd_json_kv_str(&w, "message", "no such command");
        (void)dsd_json_obj_end(&w);
    } else {
        const int rc = dispatch(desc, params);
        const int ok = (rc == DSD_APP_COMMAND_SUBMIT_QUEUED || rc == DSD_APP_COMMAND_SUBMIT_COALESCED);
        (void)dsd_json_kv_bool(&w, "ok", ok);
        (void)dsd_json_kv_str(&w, "cmd", desc->name);
        if (ok) {
            (void)dsd_json_kv_str(&w, "status", (rc == DSD_APP_COMMAND_SUBMIT_COALESCED) ? "coalesced" : "queued");
        } else if (rc == -2) {
            (void)dsd_json_key(&w, "error");
            (void)dsd_json_obj_begin(&w);
            (void)dsd_json_kv_str(&w, "code", "invalid_params");
            (void)dsd_json_kv_str(&w, "message", "missing or invalid parameters");
            (void)dsd_json_obj_end(&w);
        } else {
            (void)dsd_json_key(&w, "error");
            (void)dsd_json_obj_begin(&w);
            (void)dsd_json_kv_str(&w, "code", "rejected");
            (void)dsd_json_kv_str(&w, "message", "command not admitted (no active session or unsupported)");
            (void)dsd_json_obj_end(&w);
        }
    }
    (void)dsd_json_obj_end(&w);
    (void)dsd_json_buf_putc(out, '\n');
    return dsd_json_writer_failed(&w) ? -1 : 0;
}

int
dsd_api_command_catalog(dsd_json_buf* out) {
    dsd_json_buf_reset(out);
    dsd_json_writer w;
    dsd_json_writer_init(&w, out);
    (void)dsd_json_arr_begin(&w);
    for (int i = 0; i < API_COMMAND_COUNT; i++) {
        (void)dsd_json_obj_begin(&w);
        (void)dsd_json_kv_str(&w, "name", k_commands[i].name);
        (void)dsd_json_kv_i64(&w, "id", k_commands[i].cmd_id);
        (void)dsd_json_kv_str(&w, "params", k_commands[i].params);
        (void)dsd_json_obj_end(&w);
    }
    (void)dsd_json_arr_end(&w);
    return dsd_json_writer_failed(&w) ? -1 : 0;
}
