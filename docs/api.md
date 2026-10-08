# DSD-neo Control & Telemetry API

DSD-neo can expose a control and telemetry API over TCP using newline-delimited
JSON (**NDJSON**): one JSON object per line, in both directions. It wraps the
app-control command queue the built-in frontends use, and streams the same
decoder state they display, so another process can drive the decoder and
consume live call, event, system and RF data.

The API is part of the desktop CLI. It is not built on Android (`DSD_ENABLE_API`
is forced off there), and a CLI built with `-DDSD_ENABLE_API=OFF` refuses `--api`
with an error.

## Quick start

```bash
# Listen on 127.0.0.1:9911 and decode as usual.
dsd-neo -i rtl:0:851.375M:22:2:24:0:1 --api 9911

# Require a shared secret.
dsd-neo ... --api 9911 --api-token-file ~/.config/dsd-neo/api.token

# Listen on every interface: a token is mandatory off loopback.
dsd-neo ... --api 9911 --api-bind 0.0.0.0 --api-token-file ~/.config/dsd-neo/api.token
```

| Option | Meaning |
| --- | --- |
| `--api <port>` | Enable the API on port 1..65535 of the bind address (`127.0.0.1` by default). `0` disables it. |
| `--api-bind <ipv4>` | Bind to this numeric IPv4 address. A non-loopback bind **requires** a token. |
| `--api-token <secret>` | Shared secret (1..255 bytes) required before any other command. It is visible in the process list; prefer a token file. |
| `--api-token-file <path>` | Read the token from a file holding one line of 1..255 bytes (trailing whitespace dropped). A missing, unreadable, empty or longer file is an error, never a server without the token. |

Each option has an environment fallback, read only when the CLI did not give the
option and only when the API is enabled. An explicit `--api 0` disables the API
even when `DSD_NEO_API_PORT` is set. A token may start with `-`; it is still the
option's value.

| Variable | Meaning |
| --- | --- |
| `DSD_NEO_API_PORT` | Listening port (`--api`). |
| `DSD_NEO_API_BIND` | Bind address (`--api-bind`). |
| `DSD_NEO_API_TOKEN` | Shared secret (`--api-token`). |
| `DSD_NEO_API_TOKEN_FILE` | Token file (`--api-token-file`). |

Token precedence: `--api-token`, then `DSD_NEO_API_TOKEN`, then
`--api-token-file`, then `DSD_NEO_API_TOKEN_FILE`. An invalid value in any of
them is an argument error (exit status 1). If the server cannot start once
decoding has begun (the port is taken, say), DSD-neo logs why and keeps
decoding without it, as it does for the RTL UDP retune control.

## Architecture

```
                         decode thread
  DSP / protocol ──▶ telemetry publish ──▶ API feed (telemetry observer)
                                                 │ encodes call/event/system/metrics/quality/status
                                                 ▼
  TCP client ◀── session thread ◀── per-session outbox
      │
      └─ NDJSON request ─▶ command table ─▶ app-control command queue ─▶ decoder thread
```

* The feed is a multi-consumer telemetry observer
  (`<dsd-neo/app_control/telemetry_observers.h>`). It runs on the decode thread at
  the points the frontend snapshots are published, encodes what clients asked
  for, and appends the lines to the subscribed sessions' outboxes. It never
  blocks on a socket. While no client is authenticated it returns after one
  atomic load.
* Commands enter the same bounded app-command queue the terminal and Qt
  frontends use, and are applied on the decoder thread. A response of
  `"status":"queued"` means the queue admitted the command, not that the decoder
  accepted it: the decoder can still refuse it when it is applied, which it
  reports in the notice line (`status.message`).
* Without the terminal frontend (no `--frontend terminal`), the CLI
  opens the shared frontend runtime for the API: the command session, and the
  20 Hz snapshot publish the terminal frontend also runs.
* The CLI starts the server before decoding begins and publishes once right
  away, so the event feed has its starting point before any row can be
  committed. At the end of a run the decoder commits and publishes the last
  call before it stops the server, and the server sends each client what is
  still queued (for up to a second) before it closes the connections.

Limits:

| Limit | Value |
| --- | --- |
| Concurrent clients | 8 (a connection beyond that receives a `busy` error line and is closed) |
| Request line | 256 KiB (a longer one gets `line_too_long` and the connection is closed) |
| Request document | 32 levels of nesting, 4096 values |
| Outbound backlog | 1 MiB per client; a client that stops reading is disconnected rather than sent a stream with gaps |
| Authentication | 10 s to authenticate after connecting, when a token is required |

An authenticated connection has no idle timeout. Telemetry is never dropped from
a connected client's stream; a client that cannot keep up is disconnected and can
reconnect, then `get` the current state.

## Wire protocol

### Framing

Every message is one JSON value on one line, terminated by `\n`, in UTF-8. A
trailing `\r` is tolerated and blank lines are ignored. The parser is strict
RFC 8259: no comments, no trailing commas, no leading `+` or zeros, and strings
must be valid UTF-8 without NUL (`\u0000`). A line that is not valid JSON, or
that holds a raw NUL byte, gets one `parse_error` response and the connection is
closed: nothing that follows it is read as a request. That also means a web page
cannot drive the API by aiming an HTTP request at the port, since an HTTP request
line is not JSON.

### Handshake

On connect the server sends:

```json
{"type":"welcome","protocol":1,"auth_required":false,
 "topics":["call","event","system","metrics","quality","status","result"],
 "commands":184,"max_line":262144}
```

When `auth_required` is `true`, only `hello`, `ping` and `auth` are accepted
until the client authenticates. A wrong token gets `unauthorized` and the
connection is closed; so does a connection that has not authenticated within
10 s.

```json
{"id":7,"cmd":"auth","params":{"token":"..."}}
{"id":7,"ok":true,"authenticated":true}
```

### Requests and responses

A request is an object with a `cmd` string, an optional `id` (number or
string), and for most commands a `params` object. The session commands `auth`,
`subscribe`, `unsubscribe` and `get` also take their fields at the top level.

```json
{"id":1,"cmd":"manual_tune","params":{"value":851375000}}
```

Every request gets exactly one response, with the request's `id` echoed (a
number exactly as sent, `null` when there was none):

```json
{"id":1,"ok":true,"cmd":"manual_tune","status":"queued"}
{"id":2,"ok":false,"cmd":"manual_tune","error":{"code":"invalid_params","message":"missing, out-of-range or over-long parameters"}}
```

`status` is `queued` or `coalesced` (folded into an identical command already
waiting). Error codes:

| Code | Meaning |
| --- | --- |
| `parse_error` | The line is not valid JSON (the connection is then closed). |
| `line_too_long` | The line exceeds 256 KiB (the connection is then closed). |
| `bad_request` | Not an object with a `cmd` string, or `params` is not an object. |
| `unauthorized` | Not authenticated yet, or a wrong token (the connection is then closed). |
| `unknown_command` | No such command. |
| `invalid_params` | A parameter is missing, of the wrong type, out of range, or too long for its field. Nothing was submitted. |
| `rejected` | The command queue did not admit the command: no decoder session is open. |
| `internal_error` | The server could not build the response. |
| `busy` | Sent in place of `welcome` when all client slots are in use; the connection is then closed. |

Responses and telemetry share the stream: dispatch on `type` (telemetry) and
match responses by `id`.

### Parameter values

* Integer parameters take JSON integers. `5.0` and `5e0` are not integers.
* 64-bit values -- key words, `policy_context`, `wacn`/`sysid`/`cc`,
  `tune_generation`, `key_epoch` -- also take a string holding a decimal value or
  a `0x`-prefixed hexadecimal one, so a client whose JSON numbers are doubles
  can send every bit. The server writes 64-bit identifiers as decimal strings
  for the same reason.
* Booleans take `true`/`false` or `0`/`1`.
* A string longer than the command's field is refused with `invalid_params`,
  never cut short. Each string command's limit is in its catalog entry (most
  paths: 1023 bytes; `m17_user_data_set`: 49).

### Session commands

| Command | Params | Response |
| --- | --- | --- |
| `hello` / `ping` | – | `{protocol, authenticated}` |
| `auth` | `{token}` | `{authenticated:true}`, or `unauthorized` and the connection closes |
| `subscribe` | `{topics:[...]}` | `{topics:[now subscribed]}` |
| `unsubscribe` | `{topics:[...]}` | `{topics:[still subscribed]}` |
| `get` | `{what}`: `status` (default), `snapshot` (same), `call`, `system`, `metrics`, `quality` | `{what, data}`: the latest record of that topic, `null` before the decoder has published one |
| `list_commands` | – | `{commands:[{name,id,params}, ...]}` |

Topics: `call`, `event`, `system`, `metrics`, `quality`, `status`, `result`, or
`all`. An unknown topic name is `invalid_params`. `get` works without a
subscription.

## Telemetry

Subscribed topics stream objects with a `type` and a `seq` that increases by one
per telemetry line across all topics. `call`, `status`, `system`, `metrics` and
`quality` are sent at most every 250 ms. `event` lines are sent as rows are
committed to the event history.

### `call`: per-slot calls

```json
{"type":"call","seq":12,"slots":[
  {"slot":0,"state":2,"name":"Dispatch","tg_text":"101","src_text":"Unit 7",
   "channel":"Site 1","tg_id":101,"elapsed_ms":1815,"kid":0,"algid":0,
   "emergency":false,"priority":0,"enc":false,
   "frequency_hz":851375000,"protocol":2,"kind":2,"ota_source_id":7}]}
```

`state` is a `DSD_APP_CALL_LINE_*` value (`1` idle, `2` active, `3` ended); a
slot with nothing to show is omitted. The fields are documented in
[`include/dsd-neo/app_control/call_view.h`](../include/dsd-neo/app_control/call_view.h).

### `event`: event history rows (alpha tags, aliases, system info)

```json
{"type":"event","seq":31,"update":false,"row":{
  "slot":0,"push":42,"ring":"9314705728061123445","systype":0,"subtype":27,
  "severity":2,"category":2,"crc_invalid":0,"sys_id1":0,"sys_id2":0,
  "sys_id3":659,"sys_id4":0,"sys_id5":0,"sysid_string":"P25_293","gi":0,
  "emergency":0,"priority":0,"enc":0,"enc_alg":128,"enc_key":0,"mi":"0","svc":0,
  "source_id":1,"target_id":1,"src_str":"","tgt_str":"","t_name":"1",
  "s_name":"","t_mode":"","s_mode":"","channel_label":"","channel":0,
  "freq_hz":851012500,"access_code_kind":2,"access_code":659,
  "event_time":1760000000,"event_start_time":1760000000,"alias":"","gps":"",
  "text":"","event":"...","internal":""}}
```

A row is identified by `ring`, `slot` and `push`, which stay the same for its
life. Each new committed row is sent once (`"update":false`). When a committed
row changes in place -- a talker alias, GPS or text that arrives after the call
ended, or a reacquired call merged into its row -- it is sent again with
`"update":true`. A subscriber is sent every row committed or changed after its
`subscribe` response (and possibly one committed in the instant before it), but
not the history from before.

| Field | Meaning |
| --- | --- |
| `systype`, `subtype` | Protocol and message kind. |
| `sys_id1..5`, `sysid_string` | System identity (P25 WACN:SYS:CC:SITE:RFSS, NAC, DMR color code, NXDN RAN, ...). `sysid_string` prints `--` for a DMR or dPMR color code, an NXDN RAN or an M17 CAN that never decoded (`DMR_CC_--`, `NXDN_RAN_--`, `M17_CAN_--`), and for an NXDN value that is not a RAN (an IDAS area, DCR's fixed 7); a P25 NAC the call was not heard with prints `---`, three wide like the code (`P25_---`, `P25_45564006---_10_10`). The DMR, P25, M17 and dPMR codes there are the row's `access_code` (so `--playfiles`, which takes none, prints `DMR_CC_--`); the numeric ids keep the decoder's raw value. |
| `source_id`, `target_id` | Source and target/group identifiers. |
| `src_str`, `tgt_str` | Text identities (M17, YSF, D-STAR, dPMR callsigns). |
| `t_name`, `s_name`, `t_mode`, `s_mode` | Group/source names and modes from the imported CSVs. |
| `alias`, `gps`, `text` | Talker alias, GPS report and text message. |
| `event`, `internal` | The event text, and DSD-neo's own notices. |
| `enc`, `enc_alg`, `enc_key`, `mi` | Encryption indicators: the algorithm, the key **id** and the over-the-air MI, never key material. |
| `channel`, `channel_label` | Trunk channel number and named scan channel. |
| `freq_hz` | Frequency in Hz the call was heard on, `0` when unknown: the call's own frequency (a grant's channel, the voice channel a trunking receiver followed) when it has one, else the receiver's tuned frequency as it stood when the call was first seen live. Off a radio input (rigctl on an audio input included) and under `--playfiles`, only the call's own frequency is recorded. |
| `access_code_kind`, `access_code` | The access code the call was heard with. `access_code_kind`: `0` none known, `1` colour code (DMR 0-15, dPMR 0-63), `2` P25 NAC (Phase 1 or 2), `3` NXDN RAN, `4` M17 CAN; these values never change. `access_code` is the code (decimal; NAC 0x293 is `659`) and means nothing when the kind is `0`. Taken while the call is live, from decodes the protocol checked (a CRC or FEC; for a dPMR colour code the call's confirmation; for a P25 Phase 2 NAC a burst descrambled with it that passed its check, or a network status broadcast naming it, on the carrier the call is on; a call carried only by unscrambled control bursts records none), and frozen once it has ended; never taken under `--playfiles`. An NXDN IDAS (Type-D) or DCR call has none: neither carries a RAN. |
| `event_time`, `event_start_time` | Unix time of the last activity and of the start. |

### `system`: site identity

```json
{"type":"system","seq":13,"protocol":"P25p1","vc_freq_hz":851375000,
 "cc_freq_hz":851037500,"center_freq_hz":851375000,"trunking":true,
 "synctype":0,"p25_neighbors":[
   {"freq_hz":851037500,"wacn":781824,"sysid":293,"rfss":1,"site":1,"lra":0,
    "current_cc":true,"candidate":false,"cfva":"...","last_seen":1767225600}]}
```

`p25_neighbors` lists the decoder's P25 neighbor table, most recently heard
first. `wacn` and `lra` are `null` when no announcement of that site carried
them (abbreviated and frequency-only announcements do not); `cfva` is `?`
when its flags are unknown; `last_seen` is the Unix time it was last announced.

### `metrics`: RF and decode health

`stream_active`, `rf_mod`, `snr_db`, `output_rate_hz`, `symbol_rate_hz`,
`symbol_levels`, `channel_profile`, `channel_bandwidth_hz`, `cfo_hz`,
`carrier_lock`, `tuner_gain_tenth_db`, `tuner_gain_is_auto`, `requested_ppm`,
`auto_ppm_enabled`, `auto_ppm_locked`, `auto_ppm_locked_ppm`, and
`decode_health` (`p25p1_fec_ok/err`, `p25p2_facch_ok/err`,
`p25p2_sacch_ok/err`, `p25p2_voice_err`). A reading that does not exist is
`null`: the RF fields need an RTL-family stream (`stream_active`), `snr_db` is
the estimator for the current modulation and is `null` until it has a
measurement, and the tuner gain needs a tuner that reports it. These are the
rules every DSD-neo frontend applies.

### `quality`: P25 decode quality

`valid`, `cc_fec`, `voice_fec`, `rs` (each `{valid,ok,err,ok_pct}`), `p1_voice`
and `p2_voice[2]` (each `{valid,errs_per_frame,samples}`).

### `status`: decoder status and command context

`available`, `protocol`, `vc_freq_hz`, `cc_freq_hz`, `center_freq_hz`,
`radio_input`, `trunking`, `trunk_tuned`, `lead_slot`, per-slot
`slots[{slot,state,name,src_text,tg_id,emergency,enc}]`, `synctype`, and:

* `message`: `{text, expires}` while the decoder shows a notice (for example the
  reason it refused a queued command), else `null`.
* `tg_policy`: `{context, generation}` of the talkgroup list. The list-editing
  commands must quote both; an edit naming an older list is refused.
* `decryption`: `{target_id, tune_generation, key_epoch}`, the context a
  `decryption_apply` with `"scope":1` (the scan target on air) must quote.
* `scan_row`: the scan row on air (`{scanner, session, row, mode, editable,
  edited, listed, opts_match, target_id, label}`), which `scan_row_edit` must
  quote, or `null` when no scan row is on air.

### `result`: asynchronous completions

```json
{"type":"result","command":"tg_list_export","sequence":3,"ok":true,"policy_context":"1","policy_generation":2,"path":"/tmp/tg.csv"}
{"type":"result","command":"decryption_apply","sequence":4,"request_id":"9223372036854775809","status":1,"ok":true,"scope":"defaults"}
```

Every completion of these two commands is reported. A `decryption_apply`
response carries the `request_id` its result will have; results of decryption
requests made by a frontend rather than the API are not sent. The decoder keeps
the last 64 results of each kind; should more complete before the server reads
them, it says so instead of skipping them silently:

```json
{"type":"result_gap","command":"decryption_apply","missed":3}
```

A `decryption_apply` result's `status` is a `DSD_APP_KEY_*` value (`1`
applied, `-1` invalid, `-2` stale, `-3` unavailable, `-4` file error, `-5`
busy, `-6` cancelled). A request still queued when the decoder stops is
reported as cancelled (a `tg_list_export` as `"ok":false`) before the server
closes the connections.

## Command reference

Commands whose parameters are shown as `–` take none. Commands shown with
`{value:...}` take `params.value`; the others take the named fields. The ranges
in the table are the decoder's; the API refuses only values that do not fit the
field, and the decoder refuses the rest when it applies the command.

| Command | Parameters |
| --- | --- |
| `toggle_mute` | – |
| `toggle_compact` | – |
| `history_cycle` | – |
| `slot1_toggle` | – |
| `slot2_toggle` | – |
| `slot_pref_cycle` | – |
| `gain_delta` | `{value:int}` |
| `again_delta` | `{value:int}` |
| `trunk_toggle` | – |
| `scanner_toggle` | – |
| `payload_toggle` | – |
| `p25_ga_toggle` | – |
| `tg_hold_toggle` | `{value:slot 0\|1}` |
| `lpf_toggle` | – |
| `hpf_toggle` | – |
| `pbf_toggle` | – |
| `hpf_d_toggle` | – |
| `aggr_sync_toggle` | – |
| `call_alert_toggle` | – |
| `call_alert_events_set` | `{value:event mask 0..255}` |
| `const_toggle` | – |
| `const_norm_toggle` | – |
| `const_gate_delta` | `{value:number}` |
| `eye_toggle` | – |
| `eye_unicode_toggle` | – |
| `eye_color_toggle` | – |
| `fsk_hist_toggle` | – |
| `spectrum_toggle` | – |
| `spec_size_delta` | `{value:int}` |
| `input_vol_cycle` | – |
| `eh_next` | – |
| `eh_prev` | – |
| `eh_toggle_slot` | – |
| `ppm_delta` | `{value:int}` |
| `invert_toggle` | – |
| `mod_toggle` | – |
| `dmr_reset` | – |
| `gain_set` | `{value:int 0..50}` |
| `again_set` | `{value:int 0=auto,1..100}` |
| `input_warn_db_set` | `{value:dB}` |
| `input_monitor_toggle` | – |
| `cosine_filter_toggle` | – |
| `tcp_connect_audio` | – |
| `rigctl_connect` | – |
| `return_cc` | – |
| `channel_cycle` | – |
| `symcap_save` | – |
| `symcap_stop` | – |
| `replay_last` | – |
| `wav_start` | – |
| `wav_stop` | – |
| `stop_playback` | – |
| `trunk_wlist_toggle` | – |
| `trunk_priv_toggle` | – |
| `trunk_data_toggle` | – |
| `trunk_enc_toggle` | – |
| `wav_toggle` | – |
| `enc_lockout_clear` | – |
| `scan_hold_toggle` | – |
| `scan_avoid` | – |
| `scan_avoid_clear` | – |
| `quit` | – |
| `force_priv_toggle` | – |
| `force_rc4_toggle` | – |
| `trunk_group_toggle` | – |
| `sim_nocar` | – |
| `mod_p2_toggle` | – |
| `lockout_slot` | `{value:slot 0\|1}` |
| `m17_tx_toggle` | – |
| `provoice_esk_toggle` | – |
| `provoice_mode_toggle` | – |
| `skip_slot` | `{value:slot 0\|1}` |
| `ui_msg_clear` | – |
| `eh_reset` | – |
| `event_log_disable` | – |
| `event_log_set` | `{value:path, <=1023 bytes}` |
| `lcw_retune_toggle` | – |
| `p25_cc_cand_toggle` | – |
| `reverse_mute_toggle` | – |
| `dmr_le_toggle` | – |
| `all_mutes_toggle` | – |
| `inv_x2_toggle` | – |
| `inv_dmr_toggle` | – |
| `inv_dpmr_toggle` | – |
| `inv_m17_toggle` | – |
| `wav_static_open` | `{value:path, <=1023 bytes}` |
| `wav_raw_open` | `{value:path, <=1023 bytes}` |
| `dsp_out_set` | `{value:filename, <=255 bytes}` |
| `symcap_open` | `{value:path, <=1023 bytes}` |
| `symbol_in_open` | `{value:path, <=1023 bytes}` |
| `input_wav_set` | `{value:path, <=2047 bytes}` |
| `input_sym_stream_set` | `{value:path, <=2047 bytes}` |
| `input_set_pulse` | – |
| `udp_out_cfg` | `{host,port}` |
| `tcp_connect_audio_cfg` | `{host,port}` |
| `rigctl_connect_cfg` | `{host,port}` |
| `udp_input_cfg` | `{bind,port}` |
| `rtl_enable_input` | – |
| `rtl_restart` | – |
| `rtl_set_dev` | `{value:device index}` |
| `rtl_set_freq` | `{value:Hz}` |
| `rtl_set_gain` | `{value:int}` |
| `rtl_set_ppm` | `{value:int}` |
| `rtl_set_bw` | `{value:kHz}` |
| `rtl_set_sql_db` | `{value:dB}` |
| `rtl_set_vol_mult` | `{value:int}` |
| `rtl_set_bias_tee` | `{value:0\|1}` |
| `rtltcp_set_autotune` | `{value:0\|1}` |
| `rtl_set_auto_ppm` | `{value:0\|1}` |
| `manual_tune` | `{value:Hz}` |
| `tuner_release` | – |
| `mod_set` | `{value:0=C4FM\|1=QPSK\|2=GFSK}` |
| `decode_mode_set` | `{value:decode mode 1..16}` |
| `trunk_set` | `{value:0\|1}` |
| `airspy_set` | `{key,value}` |
| `airspy_enable_input` | – |
| `rtl_set_sql_setting` | `{mode:0=level\|1=auto\|2=noise,level,margin_db}` |
| `rigctl_set_mod_bw` | `{value:Hz}` |
| `tg_hold_set` | `{value:talkgroup}` |
| `hangtime_set` | `{value:seconds}` |
| `slot_pref_set` | `{value:0=slot 1\|1=slot 2\|2=auto}` |
| `slots_onoff_set` | `{value:mask}` |
| `scan_voice_only_set` | `{value:0\|1}` |
| `scan_voice_qualify_ms_set` | `{value:ms 100..600000}` |
| `scan_voice_hold_ms_set` | `{value:ms 100..600000}` |
| `nfm_bandwidth_set` | `{value:Hz 8000..25000, 0=default}` |
| `am_bandwidth_set` | `{value:Hz 5000..20000, 0=default}` |
| `tone_filter_set` | `{mode:0\|1\|2,list,keep_list}` |
| `scan_row_edit` | `{scanner,session,row,mode,target_id,field,action,squelch_db,squelch_mode,squelch_margin_db,width_hz,tone_mode,tone_list,gain_db}` |
| `pulse_out_set` | `{value:name, <=99 bytes}` |
| `pulse_in_set` | `{value:name, <=99 bytes}` |
| `input_vol_set` | `{value:mult 1..16}` |
| `lrrp_set_home` | – |
| `lrrp_set_dsdp` | – |
| `lrrp_set_custom` | `{value:path, <=1023 bytes}` |
| `lrrp_disable` | – |
| `import_channel_map` | `{value:path, <=1023 bytes}` |
| `import_group_list` | `{value:path, <=1023 bytes}` |
| `import_keys_dec` | `{value:path, <=1023 bytes}` |
| `import_keys_hex` | `{value:path, <=1023 bytes}` |
| `import_channel_map_clear` | – |
| `import_group_list_clear` | – |
| `import_keys_clear` | – |
| `import_p25_bandplan` | `{value:path, <=1023 bytes}` |
| `export_p25_bandplan` | `{value:path, <=1023 bytes}` |
| `rr_apply_import` | `{decode_mode,edacs_ea,edacs_esk,simulcast_qpsk,p25_prefer_candidates,trunking,scanner,chan_path,group_path,tune_hz}` |
| `rr_account_set` | `{username,app_key}` |
| `import_src_list` | `{value:path, <=1023 bytes}` |
| `import_src_list_clear` | – |
| `p25_p2_params_set` | `{wacn,sysid,cc}` |
| `tg_listen_set` | `{id_start,id_end,listen}` |
| `tg_listen_set_all` | `{listen,tags}` |
| `tg_row_set` | `{id_start,id_end,fields,listen,priority,preempt,name,tags,policy_context,policy_generation}` |
| `tg_row_remove` | `{id_start,id_end,policy_context,policy_generation}` |
| `tg_list_export` | `{path,policy_context,policy_generation}` |
| `tg_selection_set` | `{path,count,listening,policy_context,policy_generation}` |
| `tg_lockout_persist_set` | `{value:0\|1}` |
| `tg_session_avoid_clear` | `{value:policy context}` |
| `ui_show_dsp_panel_toggle` | – |
| `ui_show_p25_metrics_toggle` | – |
| `ui_show_p25_affil_toggle` | – |
| `ui_show_p25_neighbors_toggle` | – |
| `ui_show_p25_iden_toggle` | – |
| `ui_show_p25_ccc_toggle` | – |
| `ui_show_channels_toggle` | – |
| `ui_show_p25_callsign_toggle` | – |
| `key_basic_set` | `{value:uint32}` |
| `key_scrambler_set` | `{value:uint32}` |
| `key_rc4des_set` | `{value:uint64}` |
| `key_hytera_set` | `{H,K1,K2,K3,K4}` |
| `key_aes_set` | `{K1,K2,K3,K4}` |
| `key_tyt_ap_set` | `{value:hex, <=255 bytes}` |
| `key_retevis_rc2_set` | `{value:hex, <=255 bytes}` |
| `key_tyt_ep_set` | `{value:hex, <=255 bytes}` |
| `key_ken_scr_set` | `{value:decimal, <=127 bytes}` |
| `key_anytone_bp_set` | `{value:hex16, <=127 bytes}` |
| `key_xor_set` | `{value:len:hex, <=255 bytes}` |
| `m17_user_data_set` | `{value:string, <=49 bytes}` |
| `key_direct_set` | `{key_type,value}` |
| `force_key_set` | `{value:0\|1\|2}` |
| `decryption_apply` | `{type,value,hex,dec,map,profile,force,source,scope,fields,target_id,tune_generation,key_epoch}` |
| `dsp_op` | `{op,a,b,c,d}` |
| `config_apply` | `{sections:{section:{key:value}}}` |
| `config_metadata_set` | `{autosave_enabled,path}` |

### Structured command details

* **`airspy_set`**: `key` is the full canonical config key, such as
  `airspy_lna_gain` (see [docs/config-system.md](config-system.md)), and `value`
  its text.
* **`rtl_set_sql_setting`**: the whole squelch setting. `mode` is `0` level, `1`
  auto (follows the noise floor) or `2` noise (FM quieting); `level` is the
  level-mode threshold in mean-power units (`0` = off) and `margin_db` the
  auto/noise margin (3..30 dB).
* **`tone_filter_set`**: `mode` is `0` off, `1` allow, `2` block; `list` is the
  `/`-separated CTCSS/DCS list. `keep_list:true` changes only the mode and keeps
  the configured list (`list` must then be absent or empty).
* **`scan_row_edit`**: a session edit of the scan row on air. Quote `scanner`,
  `session`, `row`, `mode` and `target_id` from `status.scan_row`; `field` is one
  `DSD_SCAN_ROW_FIELD_*` (`1` squelch, `2` width, `4` tone, `8` gain) and `action`
  one `dsd_scan_row_edit_action` (`1` set, `2` inherit, `3` reset). For a set,
  give the field's value: `squelch_db` (or `squelch_mode` with
  `squelch_margin_db`), `width_hz`, `tone_mode` with `tone_list`, or `gain_db`.
  See [`include/dsd-neo/app_control/commands.h`](../include/dsd-neo/app_control/commands.h).
* **`tg_row_set`**: `fields` is a bitmask (`1` listen, `2` priority, `4` preempt,
  `8` name, `16` tags); fields not in the mask keep their value.
  `policy_context` and `policy_generation` (from `status.tg_policy`) are
  required, as they are for `tg_row_remove`, `tg_list_export` and
  `tg_selection_set`.
* **`tg_selection_set`**: `path` names a CSV of `table_index,start,end` rows the
  decoder reads, `count` the number of rows, `listening` the value to apply.
* **`decryption_apply`**: `type` (`basic`, `hex`, `rc4`, `scrambler`,
  `m17_scrambler`, `m17_aes`), `value`, `hex`/`dec` key CSV paths, `map` (DMR
  key-map CSV), `profile`, `force`, `source` (`0`..`3`, default `2` direct) and
  `scope` (`0` defaults, `1` the scan target on air, which also needs
  `target_id`, `tune_generation` and `key_epoch` from `status.decryption`).
  `fields` (bitmask: `1` material, `2` map, `4` force) defaults to the fields
  the request names, even with an empty value: `value`, `hex`, `dec` or
  `source` edit the material (`{"source":0}` clears it), `map` the key map
  (`"map":""` clears it), `force` the force setting. The response carries the
  `request_id` of the later `result`.
* **`dsp_op`**: `op` is a `dsd_app_dsp_op` value; `a`..`d` default to 0 and mean
  what that op says in `commands.h`.
* **`config_apply`**: builds a user config from INI-style
  `section → key → value` pairs, starting from the defaults the INI loader
  starts every file from, and applies it as loading that file would. A section
  with no keys counts, as an empty `[section]` does (`"analog":{}` puts the
  analog settings back to their defaults). String, number and boolean values
  are accepted; unknown keys are ignored, as in a file. See
  [docs/config-system.md](config-system.md) for the keys.

  ```json
  {"cmd":"config_apply","params":{"sections":{
     "mode":{"decode":"p25p1"},
     "trunking":{"enabled":true}}}}
  ```

* **`nfm_bandwidth_set`**, **`am_bandwidth_set`** and **`rigctl_set_mod_bw`**: on
  a radio input a width is the channel filter. On audio input with a rigctl peer
  (`-U`), which demodulates it, a width is the passband the peer is asked for,
  and `-B` (`rigctl_set_mod_bw`) stands in for an unset NFM width on the FM
  monitor (see `-B` in [docs/cli.md](cli.md#trunking--scanning)). While the
  monitor of that kind runs, a value that changes the passband in force is asked
  of the peer when the command applies; one the peer refuses, or whose reply is
  lost, is put back, and `status.message` says why (`Refused: NFM bandwidth ->
  20 kHz: the rigctl peer refused the passband`). Off that monitor (a digital
  mode, or a typed digital row on air) the value is stored for the next tune
  that asks for it.
* **`decode_mode_set`** and **`rr_apply_import`**: `decode_mode` is a
  `dsdneoUserDecodeMode` (`1` auto, `2` P25p1, `3` P25p2, `4` DMR, `5` NXDN48,
  `6` NXDN96, `7` X2-TDMA, `8` YSF, `9` D-STAR, `10` EDACS/ProVoice, `11` dPMR,
  `12` M17, `13` TDMA, `14` analog, `15` DMR mono, `16` AM).
* Key words (`key_rc4des_set`, `key_hytera_set`, `key_aes_set`) are 64-bit:
  send them as `"0x..."` strings.

Secret material (keys, tokens, account keys, configs) is never logged or echoed,
and the server erases its copies of request lines and parsed values once a
request is handled.

## Examples

### Python

```python
import json, socket

s = socket.create_connection(("127.0.0.1", 9911))
f = s.makefile("rwb", buffering=0)
print(json.loads(f.readline()))            # welcome

def send(obj):
    f.write(json.dumps(obj).encode() + b"\n")

send({"id": 1, "cmd": "subscribe", "topics": ["call", "event", "system"]})
send({"id": 2, "cmd": "manual_tune", "params": {"value": 851375000}})

for line in f:
    msg = json.loads(line)
    if "id" in msg:
        print("response:", msg)            # command responses carry the request id
    elif msg.get("type") == "event":
        row = msg["row"]
        print("event", row["t_name"] or row["target_id"], "src", row["source_id"],
              "sys", row["sysid_string"], "(update)" if msg["update"] else "")
    elif msg.get("type") == "call":
        print("slots", msg["slots"])
```

### Inspecting the command catalog

```bash
printf '{"id":1,"cmd":"list_commands"}\n' | nc 127.0.0.1 9911   # Ctrl-C when done
```

## Build

The API is built by default on desktop (`DSD_ENABLE_API=ON`) and never on
Android. It lives in `src/api/` (the `dsd-neo_api` library) with its public
header `<dsd-neo/api/api.h>`; [docs/code_map.md](code_map.md) describes the
module.

## Security

* The server binds to `127.0.0.1` unless `--api-bind` says otherwise, and refuses
  a non-loopback bind without a token.
* Without a token, any local process -- any local user on a shared machine --
  can connect. Use a token where that matters.
* The token is compared in constant time; one wrong guess closes the connection.
* There is no TLS: off the host, tunnel the port over SSH or use it only on a
  trusted network.
* The API has the authority of the user running DSD-neo: it can apply keys,
  import files, write logs and configs to paths it names, and quit the decoder.
  Treat the port as privileged.
* Text from the air or from CSV imports is JSON-escaped, so a crafted name or
  alias cannot add fields or break the line framing.
