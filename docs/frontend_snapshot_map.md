<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Frontend Snapshot Map

`dsd_frontend_snapshot` is the app-control boundary for read-only frontend state. New frontends should read
`dsd_frontend_snapshot`, `dsd_frontend_status`, and `dsd_frontend_metrics` instead of reaching into live
`dsd_opts`, live `dsd_state`, or terminal UI helpers.

## Status

- `status.frontend_kind`: copied from the active `dsd_opts.frontend_kind`.
- `status.display`: common frontend display preferences copied from `dsd_opts.frontend_display`.
- `status.terminal_display`: terminal-only render preferences copied from `dsd_opts.frontend_display`.
- Decoder/input status fields: copied from stable option and runtime fields needed by existing frontend status views.

## Metrics

- `metrics`: copied from app-control telemetry counters and backend runtime metrics.
- Backend-specific values are exposed through app-control metrics structs, not through backend handles.

## UI Message

- `ui_message.present`: set when `dsd_state.ui_msg` is non-empty.
- `ui_message.text`: copied from `dsd_state.ui_msg`.
- `ui_message.expire_unix_s`: copied from `dsd_state.ui_msg_expire`.
- `ui_message.severity`, `category`, `source`, and `slot`: normalized metadata for frontend rendering.

## Slot Summaries

- `slots[0]`: copied from left/current-slot fields such as `lasttg`, `lastsrc`, payload algorithm/key IDs, audio gate,
  active-call state, and call text.
- `slots[1]`: copied from right-slot equivalents such as `lasttgR`, `lastsrcR`, payload algorithm/key IDs, audio gate,
  active-call state, and call text.

## P25 And Trunking

- `p25`: copied from P25 system, control-channel, voice-channel, TDMA active-slot, audio-ring, and FEC/error counters
  in `dsd_state`.
- `trunk_channels`: copied from the tracked trunk channel map and its sequence counter.
- `trunk_cc_candidates`: copied from the engine trunk CC candidate extension stored in `dsd_state.state_ext`.

## Input Level

- `input_level`: copied from the input-level snapshot held in `dsd_state`.
- Toast fields are copied from the corresponding input-level notification fields in `dsd_state`.

## Event History

- `event_history[*].items[*].present`: true when a core `Event_History` item contains source, target, text, GPS,
  alias, detail, PDU, or rendered summary content.
- `severity` and `category`: copied from neutral core event metadata when present, otherwise inferred from current
  event content for compatibility with older writers.
- `protocol`: derived from the core sync/system type.
- Source/target IDs, labels, modes, system IDs, channel, timestamp, service options, encryption metadata, PDU bytes,
  summary text, detail text, GPS text, text message, and alias are copied from the matching `Event_History` fields.
- Terminal color pairs are not part of the public snapshot. Terminal UI keeps using the internally copied compatibility
  `dsd_state` and maps neutral metadata to curses attributes inside `src/ui/terminal/`.
