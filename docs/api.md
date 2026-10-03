# DSD-neo Control & Telemetry API

DSD-neo exposes a third-party control and telemetry API over TCP using
newline-delimited JSON (**NDJSON**): one JSON object per line, in both
directions. It wraps the same app-control command queue and telemetry publishes
the built-in frontends use, so anything the terminal or Qt UI can do, and
anything they can display, is reachable from another process.

The API is **desktop-only**. It is not built or linked on Android (the module is
forced off by `DSD_ENABLE_API` there), and `--api` is inert in a build configured
with `DSD_ENABLE_API=OFF`.

## Quick start

```bash
# Listen on 127.0.0.1:9911 and decode as usual.
dsd-neo -i rtl:0:851.375M:22:2:24:0:1 --api 9911

# Require a shared secret (needed off-loopback).
dsd-neo ... --api 9911 --api-token-file /etc/dsd-neo-api.token

# Expose on the LAN only with a token.
dsd-neo ... --api 9911 --api-bind 0.0.0.0 --api-token "$(cat token)"
```

| Option | Meaning |
| --- | --- |
| `--api <port>` | Enable the API on `127.0.0.1:<port>`. `0` disables. |
| `--api-bind <ipv4>` | Bind to a numeric IPv4 address. A non-loopback bind **requires** a token. |
| `--api-token <secret>` | Shared secret required before any other command. Visible in the process table; prefer a token file or env. |
| `--api-token-file <path>` | Read the token from a file (trailing CR/LF/space trimmed). |

Environment fallbacks (used only when the CLI did not set the value):

| Variable | Meaning |
| --- | --- |
| `DSD_NEO_API_PORT` | Listening port. |
| `DSD_NEO_API_BIND` | Bind address. |
| `DSD_NEO_API_TOKEN` | Shared secret. |
| `DSD_NEO_API_TOKEN_FILE` | Token file path. |

Token precedence: `--api-token` → `DSD_NEO_API_TOKEN` → `--api-token-file` →
`DSD_NEO_API_TOKEN_FILE`.

## Architecture

```
                       decode thread
   DSP/protocol ──▶ app-control telemetry publish ──▶ API feed observer
                                                          │  (lock-protected latest + event ring)
   TCP client(s) ◀── per-session bounded outbox ◀─────────┘
        │
        └─ NDJSON request ─▶ command table ─▶ dsd_app_command_submit ─▶ control pump
```

* The API registers a multi-consumer telemetry observer with app-control. It
  runs on the decode thread, copies what it needs, and never blocks. This is why
  the server can run **alongside** the terminal or Qt frontend without fighting
  over the single-consumer snapshot buffer.
* When no client is connected, the observer returns after one atomic load, so a
  run with `--api` open but nobody attached costs essentially nothing.
* Commands enter the existing bounded app-command queue and are applied on the
  decoder thread by the control pump.
* When the API runs with no frontend (`--frontend none`), it starts the shared
  frontend runtime itself so the command session is open and telemetry hooks are
  installed.

Limits (fixed): 8 concurrent clients (hard cap 32), 256 KiB maximum request
line, 1 MiB per-client outbound buffer, 10 minute idle timeout. Telemetry is
best-effort: if a client cannot keep up, new telemetry lines are dropped rather
than stalling the decoder; periodic topics self-heal on the next tick.

## Wire protocol

### Framing

Every message is one JSON value on one line, terminated by `\n`. A trailing
`\r` is tolerated (CRLF clients). Blank lines are ignored. Malformed JSON gets a
`parse_error` response and does not close the session.

### Handshake

On connect the server immediately sends:

```json
{"type":"welcome","protocol":1,"auth_required":false,
 "topics":["call","event","system","metrics","quality","status","result"],
 "commands":182}
```

If `auth_required` is `true`, the only accepted command until authentication is
`auth` (and `hello`/`ping`). A wrong token returns `unauthorized` and closes the
connection.

```json
{"cmd":"auth","params":{"token":"..."}}
{"id":7,"ok":true,"authenticated":true}
```

### Requests

Every request is an object with an optional `id`, a `cmd`, and (for most
commands) a `params` object. For `subscribe`, `unsubscribe` and `get`, `params`
may be omitted and its fields placed at the top level.

* Session commands: `hello`, `ping`, `auth`, `subscribe`, `unsubscribe`, `get`,
  `list_commands`.
* Every other `cmd` name is a control command from the catalog below.

```json
{"id":1,"cmd":"manual_tune","params":{"hz":851375000}}
```

### Responses

Every request produces exactly one response object correlated by `id` (echoed
verbatim; `null` when the request had none):

```json
{"id":1,"ok":true,"cmd":"manual_tune","status":"queued"}
{"id":2,"ok":false,"cmd":"manual_tune","error":{"code":"invalid_params","message":"missing or invalid parameters"}}
```

`status` is `queued` or `coalesced` for accepted commands. `ok:false` carries an
`error.code` of `parse_error`, `bad_request`, `unauthorized`, `unknown_command`,
`invalid_params`, `rejected`, or `internal_error`.

Telemetry arrives unsolicited on the same stream, so a client must dispatch by
`type` and match responses by `id`; they can interleave.

### Session commands

| Command | Params | Response |
| --- | --- | --- |
| `hello` / `ping` | – | `{protocol, authenticated}` |
| `auth` | `{token}` | `{authenticated:true}` or `unauthorized` |
| `subscribe` | `{topics:[...]}` | `{topics:[currently subscribed]}` |
| `unsubscribe` | `{topics:[...]}` | `{topics:[remaining]}` |
| `get` | `{what:"status"\|"snapshot"\|"call"\|"system"\|"metrics"\|"quality"}` | `{what, data:<cached object or null>}` |
| `list_commands` | – | `{commands:[{name,id,params}, ...]}` |

Topics: `call`, `event`, `system`, `metrics`, `quality`, `status`, `result`, or
`all`.

### Async results

Commands that complete asynchronously publish a result object:

```json
{"type":"result","command":"talkgroup_export","sequence":3,"ok":true,"path":"/tmp/tg.csv"}
{"type":"result","command":"decryption_apply","sequence":4,"request_id":9,"status":1}
```

## Telemetry topics

Subscribe once and the server streams objects with a `type` and a monotonic
`seq`. `call`, `system`, `metrics`, `quality` and `status` are produced at most
every **250 ms** while subscribed; `event` is emitted immediately when a new
event-history row is committed (and re-emitted when a committed row is
enriched).

### `call` — per-slot active/ended calls

```json
{"type":"call","seq":12,"slots":[
  {"slot":0,"state":2,"name":"Dispatch","tg_text":"101","src_text":"Unit 7",
   "channel":"Site 1","tg_id":101,"elapsed_ms":1815,"kid":0,"algid":0,
   "emergency":false,"priority":0,"enc":false,
   "frequency_hz":851375000,"protocol":2,"kind":2,"ota_source_id":7}
]}
```

`state` is one of the `DSD_APP_CALL_LINE_*` values (`0` none, `1` idle,
`2` active, `3` ended). Fields are documented in
[`include/dsd-neo/app_control/call_view.h`](../include/dsd-neo/app_control/call_view.h).

### `event` — alpha tags and system info (real time)

Emitted for each new history row. This is the primary source of decoded alpha
tags, source/target aliases, system identifiers, GPS and text messages.

```json
{"type":"event","seq":31,"row":{
  "seq":31,"slot":0,"systype":0,"subtype":27,"severity":2,"category":2,
  "crc_invalid":0,"sys_id1":0,"sys_id2":0,"sys_id3":659,"sys_id4":0,"sys_id5":0,
  "sysid_string":"P25_293","gi":0,"emergency":0,"priority":0,"enc":0,
  "enc_alg":128,"enc_key":0,"mi":0,"svc":0,"source_id":1,"target_id":1,
  "src_str":"","tgt_str":"","t_name":"1","s_name":"","t_mode":"","s_mode":"",
  "channel_label":"","channel":0,"event_time":1760000000,
  "event_start_time":1760000000,"alias":"","gps":"","text":"","event":"...",
  "internal":""}}
```

| Field | Meaning |
| --- | --- |
| `systype`, `subtype` | Protocol / message kind. |
| `sys_id1..5`, `sysid_string` | Hierarchical system identity (e.g. P25 WACN:SYS:CC:SITE:RFSS, NAC, DMR color code, NXDN RAN). |
| `source_id`, `target_id` | OTA source and target/group identifiers. |
| `src_str`, `tgt_str` | String identities (M17/YSF/D-STAR/dPMR callsigns). |
| `t_name`, `s_name` | CSV-imported group/source alpha tags. |
| `t_mode`, `s_mode` | CSV-imported mode columns (A/B/D/DE). |
| `alias` | Decoded radio talker alias. |
| `gps` | GPS string, when present. |
| `text` | Decoded text message. |
| `event`, `internal` | Human-readable event text and DSD-neo internal notices. |
| `enc`, `enc_alg`, `enc_key`, `mi` | Encryption indicators (never key material). |
| `emergency`, `priority`, `crc_invalid` | Call metadata. |
| `channel`, `channel_label` | Trunk channel number and named scan channel. |
| `event_time`, `event_start_time` | Wall-clock last-activity and start. |

### `system` — site identity

```json
{"type":"system","seq":13,"protocol":"P25p1","vc_freq_hz":851375000,
 "cc_freq_hz":851037500,"center_freq_hz":851375000,"trunking":true,
 "synctype":0,"p25_neighbors":[
   {"freq_hz":851037500,"wacn":781824,"sysid":293,"rfss":1,"site":1,"lra":0,
    "current_cc":true,"candidate":false,"cfva":"..."}]}
```

`p25_neighbors` is present only when the decoder has learned any.

### `metrics` — RF/DSP health

`snr_*_db` values are `null` when no estimator has produced a reading. Also
includes `cfo_hz`, `carrier_lock`, `symbol_levels`, `channel_profile`,
`channel_bandwidth_hz`, `output_rate_hz`, tuner gain, auto-PPM state, and the
`decode_health` FEC counters.

### `quality` — P25 decode quality

`cc_fec`, `voice_fec`, `rs` (`{valid,ok,err,ok_pct}`), `p1_voice` and
`p2_voice[2]` (`{valid,errs_per_frame,samples}`), and `last_frame[2]`.

### `status` — compact decoder status

The same record the Android notification uses: `protocol`, `vc_freq_hz`,
`cc_freq_hz`, `center_freq_hz`, `radio_input`, `trunking`, `trunk_tuned`,
`lead_slot`, per-slot `state`/`name`/`src_text`/`tg_id`/`emergency`/`enc`, and
`synctype`. Also returned by `get {"what":"snapshot"}`.

## Command reference

All 182 commands are listed below. `params` may be omitted for commands whose
Parameters column is `-`. Scalar commands use `params.value`; structured
commands use the named fields shown.

| Command | Parameters |
| --- | --- |
| `toggle_mute` | `-` |
| `toggle_compact` | `-` |
| `history_cycle` | `-` |
| `slot1_toggle` | `-` |
| `slot2_toggle` | `-` |
| `slot_pref_cycle` | `-` |
| `gain_delta` | `{value:int}` |
| `again_delta` | `{value:int}` |
| `trunk_toggle` | `-` |
| `scanner_toggle` | `-` |
| `payload_toggle` | `-` |
| `p25_ga_toggle` | `-` |
| `tg_hold_toggle` | `{value:0\|1 slot}` |
| `lpf_toggle` | `-` |
| `hpf_toggle` | `-` |
| `pbf_toggle` | `-` |
| `hpf_d_toggle` | `-` |
| `aggr_sync_toggle` | `-` |
| `call_alert_toggle` | `-` |
| `call_alert_events_set` | `{value:uint8 mask}` |
| `const_toggle` | `-` |
| `const_norm_toggle` | `-` |
| `const_gate_delta` | `{value:float}` |
| `eye_toggle` | `-` |
| `eye_unicode_toggle` | `-` |
| `eye_color_toggle` | `-` |
| `fsk_hist_toggle` | `-` |
| `spectrum_toggle` | `-` |
| `spec_size_delta` | `{value:int}` |
| `input_vol_cycle` | `-` |
| `eh_next` | `-` |
| `eh_prev` | `-` |
| `eh_toggle_slot` | `-` |
| `ppm_delta` | `{value:int}` |
| `invert_toggle` | `-` |
| `mod_toggle` | `-` |
| `dmr_reset` | `-` |
| `gain_set` | `{value:int}` |
| `again_set` | `{value:int}` |
| `input_warn_db_set` | `{value:double}` |
| `input_monitor_toggle` | `-` |
| `cosine_filter_toggle` | `-` |
| `tcp_connect_audio` | `-` |
| `rigctl_connect` | `-` |
| `return_cc` | `-` |
| `channel_cycle` | `-` |
| `symcap_save` | `-` |
| `symcap_stop` | `-` |
| `replay_last` | `-` |
| `wav_start` | `-` |
| `wav_stop` | `-` |
| `stop_playback` | `-` |
| `trunk_wlist_toggle` | `-` |
| `trunk_priv_toggle` | `-` |
| `trunk_data_toggle` | `-` |
| `trunk_enc_toggle` | `-` |
| `wav_toggle` | `-` |
| `enc_lockout_clear` | `-` |
| `scan_hold_toggle` | `-` |
| `scan_avoid` | `-` |
| `scan_avoid_clear` | `-` |
| `quit` | `-` |
| `force_priv_toggle` | `-` |
| `force_rc4_toggle` | `-` |
| `trunk_group_toggle` | `-` |
| `sim_nocar` | `-` |
| `mod_p2_toggle` | `-` |
| `lockout_slot` | `{value:0\|1 slot}` |
| `m17_tx_toggle` | `-` |
| `provoice_esk_toggle` | `-` |
| `provoice_mode_toggle` | `-` |
| `skip_slot` | `{value:0\|1 slot}` |
| `ui_msg_clear` | `-` |
| `eh_reset` | `-` |
| `event_log_disable` | `-` |
| `event_log_set` | `{value:path}` |
| `lcw_retune_toggle` | `-` |
| `p25_cc_cand_toggle` | `-` |
| `reverse_mute_toggle` | `-` |
| `dmr_le_toggle` | `-` |
| `all_mutes_toggle` | `-` |
| `inv_x2_toggle` | `-` |
| `inv_dmr_toggle` | `-` |
| `inv_dpmr_toggle` | `-` |
| `inv_m17_toggle` | `-` |
| `wav_static_open` | `{value:path}` |
| `wav_raw_open` | `{value:path}` |
| `dsp_out_set` | `{value:filename}` |
| `symcap_open` | `{value:path}` |
| `symbol_in_open` | `{value:path}` |
| `input_wav_set` | `{value:path}` |
| `input_sym_stream_set` | `{value:path}` |
| `input_set_pulse` | `-` |
| `udp_out_cfg` | `{host,port}` |
| `tcp_connect_audio_cfg` | `{host,port}` |
| `rigctl_connect_cfg` | `{host,port}` |
| `udp_input_cfg` | `{bind,port}` |
| `rtl_enable_input` | `-` |
| `rtl_restart` | `-` |
| `rtl_set_dev` | `{value:int}` |
| `rtl_set_freq` | `{value:hz}` |
| `rtl_set_gain` | `{value:int}` |
| `rtl_set_ppm` | `{value:int}` |
| `rtl_set_bw` | `{value:khz}` |
| `rtl_set_sql_db` | `{value:db}` |
| `rtl_set_vol_mult` | `{value:int}` |
| `rtl_set_bias_tee` | `{value:0\|1}` |
| `rtltcp_set_autotune` | `{value:0\|1}` |
| `rtl_set_auto_ppm` | `{value:0\|1}` |
| `manual_tune` | `{value:hz}` |
| `tuner_release` | `-` |
| `mod_set` | `{value:0\|1\|2}` |
| `decode_mode_set` | `{value:dsdneoUserDecodeMode}` |
| `trunk_set` | `{value:0\|1}` |
| `airspy_set` | `{key,value}` |
| `airspy_enable_input` | `-` |
| `rigctl_set_mod_bw` | `{value:hz}` |
| `tg_hold_set` | `{value:talkgroup}` |
| `hangtime_set` | `{value:seconds}` |
| `slot_pref_set` | `{value:0\|1\|2}` |
| `slots_onoff_set` | `{value:mask}` |
| `scan_voice_only_set` | `{value:0\|1}` |
| `scan_voice_qualify_ms_set` | `{value:ms}` |
| `scan_voice_hold_ms_set` | `{value:ms}` |
| `nfm_bandwidth_set` | `{value:hz,0=default}` |
| `am_bandwidth_set` | `{value:hz,0=default}` |
| `tone_filter_set` | `{mode,list,keep_list}` |
| `pulse_out_set` | `{value:name}` |
| `pulse_in_set` | `{value:name}` |
| `input_vol_set` | `{value:mult}` |
| `lrrp_set_home` | `-` |
| `lrrp_set_dsdp` | `-` |
| `lrrp_set_custom` | `{value:path}` |
| `lrrp_disable` | `-` |
| `import_channel_map` | `{value:path}` |
| `import_group_list` | `{value:path}` |
| `import_keys_dec` | `{value:path}` |
| `import_keys_hex` | `{value:path}` |
| `import_channel_map_clear` | `-` |
| `import_group_list_clear` | `-` |
| `import_keys_clear` | `-` |
| `import_p25_bandplan` | `{value:path}` |
| `export_p25_bandplan` | `{value:path}` |
| `rr_apply_import` | `{decode_mode,edacs_ea,edacs_esk,simulcast_qpsk,p25_prefer_candidates,trunking,scanner,chan_path,group_path,tune_hz}` |
| `rr_account_set` | `{username,app_key}` |
| `import_src_list` | `{value:path}` |
| `import_src_list_clear` | `-` |
| `p25_p2_params_set` | `{wacn,sysid,cc}` |
| `tg_listen_set` | `{id_start,id_end,listen}` |
| `tg_listen_set_all` | `{listen,tags}` |
| `tg_row_set` | `{id_start,id_end,fields,listen,priority,preempt,name,tags,policy_context,policy_generation}` |
| `tg_row_remove` | `{id_start,id_end,policy_context,policy_generation}` |
| `tg_list_export` | `{policy_context,policy_generation,path}` |
| `tg_selection_set` | `{policy_context,policy_generation,count,listening,path}` |
| `tg_lockout_persist_set` | `{value:0\|1}` |
| `tg_session_avoid_clear` | `{value:context}` |
| `ui_show_dsp_panel_toggle` | `-` |
| `ui_show_p25_metrics_toggle` | `-` |
| `ui_show_p25_affil_toggle` | `-` |
| `ui_show_p25_neighbors_toggle` | `-` |
| `ui_show_p25_iden_toggle` | `-` |
| `ui_show_p25_ccc_toggle` | `-` |
| `ui_show_channels_toggle` | `-` |
| `ui_show_p25_callsign_toggle` | `-` |
| `key_basic_set` | `{value:uint32}` |
| `key_scrambler_set` | `{value:uint32}` |
| `key_rc4des_set` | `{value:uint64}` |
| `key_hytera_set` | `{H,K1,K2,K3,K4}` |
| `key_aes_set` | `{K1,K2,K3,K4}` |
| `key_tyt_ap_set` | `{value:hex}` |
| `key_retevis_rc2_set` | `{value:hex}` |
| `key_tyt_ep_set` | `{value:hex}` |
| `key_ken_scr_set` | `{value:decimal}` |
| `key_anytone_bp_set` | `{value:hex16}` |
| `key_xor_set` | `{value:len:hex}` |
| `m17_user_data_set` | `{value:string}` |
| `key_direct_set` | `{key_type,value}` |
| `force_key_set` | `{value:0\|1\|2}` |
| `decryption_apply` | see [Decryption](#decryption_apply) |
| `dsp_op` | `{op,a,b,c,d}` |
| `config_apply` | `{sections:{section:{key:value}}}` |
| `config_metadata_set` | `{autosave_enabled,path}` |

### Structured command details

* **`airspy_set`** – `key` is the canonical `airspy_*` configuration key
  (without the `airspy_` prefix), `value` its string.
* **`tone_filter_set`** – `mode` is `0` off, `1` allow, `2` block; `list` is the
  slash-separated tone list. Set `keep_list:true` to change only the mode and
  keep the configured list.
* **`dsp_op`** – `op` is a `dsd_app_dsp_op` value; the meaning of `a`–`d` depends
  on the op (see `commands.h`).
* **`tg_row_set`** – `fields` is a bitmask (`1` listen, `2` priority, `4`
  preempt, `8` name, `16` tags); absent fields keep their existing value.
  `policy_context`/`policy_generation` identify the list the edit applies to, as
  reported by telemetry.
* **`tg_selection_set`** – `path` points to a CSV of `index,start,end` rows the
  decoder reads for the selection.
* **`config_apply`** – builds a user config in memory from INI-style
  `section → key → value` pairs and applies it, exactly like loading a config
  file. Unknown keys are ignored. Example:

  ```json
  {"cmd":"config_apply","params":{"sections":{
     "input":{"rtl_freq":"851.375M"},
     "mode":{"decode_mode":"p25p1"},
     "trunking":{"trunk_enabled":"1"}}}}
  ```

* <a id="decryption_apply"></a>**`decryption_apply`** – sensitive. Accepts
  `type` (`basic`/`hex`/`rc4`/`scrambler`/`m17_scrambler`/`m17_aes`), `value`,
  `hex`/`dec` CSV paths, `map` (DMR key-map CSV), `target`, `profile`, `force`,
  `source`, `scope`, `key_epoch`, `tune_generation`, and optional explicit
  `fields` (bitmask: `1` material, `2` map, `4` force). Material is erased after
  the queue copies it, and the result object never echoes it.

Secret material (`key_direct_set`, `decryption_apply`, `key_hytera_set`,
`key_aes_set`, config keys, `rr_account_set`) is never logged or echoed.

## Examples

### Python

```python
import json, socket

s = socket.create_connection(("127.0.0.1", 9911))
f = s.makefile("rwb")
print(json.loads(f.readline()))            # welcome

f.write(b'{"id":1,"cmd":"subscribe","topics":["call","event","system"]}\n')
print(json.loads(f.readline()))            # subscribe ack

f.write(b'{"id":2,"cmd":"manual_tune","params":{"hz":851375000}}\n')

for line in f:
    msg = json.loads(line)
    if msg.get("id") == 2:
        print("tune:", msg)                # command response
    elif msg.get("type") == "event":
        row = msg["row"]
        print("call", row["t_name"] or row["tg_text"], "src", row["source_id"],
              "sys", row["sysid_string"])
    elif msg.get("type") == "call":
        print("slots", msg["slots"])
```

### Inspecting the command catalog

```bash
printf '{"id":1,"cmd":"list_commands"}\n' | nc 127.0.0.1 9911
```

## Build

The API is enabled by default on desktop (`DSD_ENABLE_API=ON`) and is always
disabled on Android. Build a CLI without it with `-DDSD_ENABLE_API=OFF`. The
implementation lives in `src/api/` (`dsd-neo_api`), with public headers under
`include/dsd-neo/api/`.

## Security

* Binds to `127.0.0.1` by default. A non-loopback `--api-bind` without a token is
  refused at startup.
* The token is compared in constant time. There is no TLS in this version: for
  off-host use, tunnel over SSH or a trusted network.
* The API has the same authority as the local user running `dsd-neo`, including
  applying encryption keys and importing files. Treat the port as privileged.
* Text originating off the air or from CSV imports is JSON-escaped, so a crafted
  talkgroup name or alias cannot inject fields or break framing.
