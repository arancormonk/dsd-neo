<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Frontend Snapshot Map

`dsd_frontend_snapshot` is the app-control boundary for cheap read-only frontend state. New frontends should read
`dsd_frontend_snapshot`, `dsd_frontend_status`, `dsd_frontend_metrics`, and the paged event-history APIs instead of
reaching into live `dsd_opts`, live `dsd_state`, or terminal UI helpers.

## Native UI Integration Gate

- Runtime control pumping must remain thread-safe because native frontends can own the main thread while the engine runs
  on a worker thread.
- Native UI code must use app-control snapshots, status, metrics, event-history pages, command descriptors, and tracked
  command results. Missing state should be promoted into app-control instead of read from backend structs.
- Native UI code must not include terminal-private headers or depend on curses, IO, DSP, protocol, live `dsd_opts`, or
  live `dsd_state` details.

## Boundary Audit Findings

Current audit result: there is no known refactoring gate that must block native UI toolkit selection or initial
integration. The branch has the required separation points in place:

- Read path: app-control publishes copied frontend state through `dsd_frontend_snapshot`, `dsd_frontend_status`,
  `dsd_frontend_metrics`, and paged event-history APIs.
- Write path: frontends submit typed app commands and poll tracked command results instead of mutating decoder state.
- Runtime path: native providers may own the main thread while the engine runs on a worker thread through
  `DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI`.
- Guardrails: `cmake/arch_rules.cmake` rejects native UI includes of backend, terminal, curses, private app-control, and
  forbidden native UI link dependencies.

Remaining risks to keep visible during native UI work:

- The provider contract still passes forwarded `dsd_opts` and `dsd_state` pointers to `prepare()` and lifecycle hooks.
  Native UI code may use those pointers only to seed `dsd_app_frontend_runtime_start()`; it must not retain them or read
  fields from them.
- The public snapshot APIs are intended for a normal single UI event thread. If a selected toolkit polls frontend state
  from multiple UI/render threads, add caller-owned copy APIs or locking around the consume buffers before relying on
  concurrent polling.
- Terminal UI remains a legacy compatibility frontend and still reads some backend/protocol details directly. It is not
  the native UI implementation template. New native views should copy missing state into app-control instead of porting
  terminal helper logic.
- Backend diagnostics may still write through runtime logging. A native frontend should install a log sink if it needs
  in-app logs; it should not scrape `stderr` or depend on terminal output.

## Status

- `status.frontend_kind`: copied from the active `dsd_opts.frontend_kind`.
- `status.display`: common frontend display preferences copied from `dsd_opts.frontend_display`.
- `status.terminal_display`: terminal-only render preferences copied from `dsd_opts.frontend_display`.
- Decoder/input status fields: copied from stable option and runtime fields needed by existing frontend status views.
- Native-visible controls and labels can read promoted recording, logging, trunk policy, connection, call-alert, and
  capture/playback state from `dsd_frontend_status` instead of raw `dsd_opts` or terminal menu helpers.

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
- `p25.neighbors`: copied P25 adjacent-site summaries with frequency, SysID, RFSS, Site, CFVA, and last-seen time.
- `p25.iden_plan`: copied FDMA/TDMA IDEN bandplan entries with ID, class, trust, base frequency, channel spacing,
  channel type, bandwidth, transmit offset, and provenance fields.
- `active_channels`: copied per-slot active P25 channel summaries with source/target IDs, payload encryption metadata,
  voice-channel frequencies, and audio gate state. Terminal display strings are not part of this public summary.
- `trunk_channels`: copied from the tracked trunk channel map and its sequence counter.
- `trunk_cc_candidates`: copied from the engine trunk CC candidate extension stored in `dsd_state.state_ext`.

## Input Level

- `input_level`: copied from the input-level snapshot held in `dsd_state`.
- Toast fields are copied from the corresponding input-level notification fields in `dsd_state`.

## Event History

- The live snapshot carries only metadata: `event_history_present`, `event_history_sequence`,
  `event_history_slot_count`, and `event_history_items_per_slot`.
- Frontends should call `dsd_app_frontend_event_history_page_get()` when `event_history_sequence` changes. Passing the
  last known sequence lets app-control report `unchanged` without copying rows.
- Page rows use `dsd_frontend_event_history_summary`: compact source/target labels, IDs, protocol, severity/category,
  encryption state, timestamp, and short summary/detail text.
- Frontends should call `dsd_app_frontend_event_history_item_get()` only for selected rows that need full detail.
  The full detail item includes source/target IDs, labels, modes, system IDs, channel, timestamp, service options,
  encryption metadata, PDU bytes, summary text, detail text, GPS text, text message, and alias.
- Terminal color pairs are not part of the public snapshot. Terminal UI keeps using the internally copied compatibility
  `dsd_state` and maps neutral metadata to curses attributes inside `src/ui/terminal/`.

## Commands

- `dsd_app_command_descriptors_get()` is the native-control metadata source. It exposes command labels, payload kind,
  payload size, value ranges, enum options, units, radio/runtime availability flags, restart hints, and validation
  hints.
- `dsd_app_command_capabilities_get()` remains as the compatibility view and is derived from the descriptor table.

## Native Host Model

- Native UI v1 is currently hosted through the CLI `--frontend native` provider path.
- Providers may set `DSD_FRONTEND_PROVIDER_MAIN_THREAD_UI` when a frontend needs main-thread UI ownership.
- No separate native app host library or executable is part of this boundary cleanup; that should be revisited only
  when a selected native UI toolkit requires a separate executable or app bundle.

## Boundary Verification Coverage

Use these focused checks when changing the native frontend boundary or adding toolkit integration:

```sh
cmake -P cmake/arch_rules.cmake
cmake --preset dev-debug-native-ui
cmake --build --preset dev-debug-native-ui -j
ctest --preset dev-debug-native-ui --output-on-failure \
  -R '^(ARCH_RULES|HEADERS_PUBLIC_UI_NATIVE_PROVIDER|HEADERS_PUBLIC_RUNTIME_CONTROL_PUMP|RUNTIME_CLI_FRONTEND_PROVIDER|RUNTIME_CONTROL_PUMP|APP_CONTROL_TELEMETRY_HOOKS_INSTALL|APP_CONTROL_SNAPSHOT_EVENT_HISTORY|APP_CONTROL_FRONTEND_PUBLIC_BOUNDARY|APP_CONTROL_FRONTEND_STATUS_METRICS|UI_NATIVE_PROVIDER|RUNTIME_TELEMETRY_HOOKS|APP_COMMAND_QUEUE)$'
```
