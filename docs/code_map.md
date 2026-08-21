# Code Map and Modules

High-level layout with module responsibilities and libraries. All public headers live under `include/dsd-neo/...` and
should be included via `#include <dsd-neo/...>`.

## Top-Level Layout

- `apps/` — executable targets (currently `apps/dsd-cli` → `dsd-neo`)
- `src/<module>/` — implementation code for each module
- `include/dsd-neo/<module>/` — public headers for each module
- `tests/<area>/` — unit tests (CTest)
- `cmake/` — CMake helper modules and install/uninstall scripts
- `tools/` — development scripts (formatting, analysis, coverage)
- `docs/` — documentation
- `examples/` — sample CSV inputs (channel maps, groups, keys) used by the config system and tooling
- `packaging/` — packaging assets/scripts (AppImage, macOS)
- `android/` — Android app shell: Kotlin foreground service, JNI lifecycle glue, and vendored libusb/librtlsdr
  (`android/README.md`); built only for `ANDROID` with `DSD_ENABLE_QT_UI=ON`
- `images/` — screenshots and other project assets used by docs/README
- `vcpkg.json`, `vcpkg-configuration.json`, `vcpkg-ports/`, `vcpkg-triplets/` — vcpkg dependency management

Generated (do not edit/commit):

- `build/`, `vcpkg_installed/`, `compile_commands.json`

## Apps

- Path: `apps/dsd-cli`
- Target: `dsd-neo` (executable)
- Responsibilities: argument parsing + wiring; keep `main.c` thin and push logic into module libraries
  - Runtime CLI/bootstrap helpers: `include/dsd-neo/runtime/cli.h`
- Build files: `apps/dsd-cli/CMakeLists.txt`

## Engine

- Path: `src/engine` (including `src/engine/dispatch`), `include/dsd-neo/engine`
- Targets: `dsd-neo_engine`, `dsd-neo_dispatch`
- Responsibilities:
  - Top-level decode runner and lifecycle (wires core/runtime/IO/protocol state machines)
  - Protocol/frame dispatch glue
  - Installs runtime hook tables used by DSP/frame-sync code
    (`src/engine/frame_sync_hooks_install.c`, `include/dsd-neo/runtime/frame_sync_hooks.h`)
- Build files: `src/engine/CMakeLists.txt`

## Platform

- Path: `src/platform`, `include/dsd-neo/platform`
- Target: `dsd-neo_platform`
- Responsibilities: cross-platform primitives (audio backend, sockets, threading, timing, filesystem/curses
  compatibility)
  - Directory listing: `dsd_dir_list()` in `include/dsd-neo/platform/file_compat.h`, implemented once per
    platform in `file_compat_posix.c` and `file_compat_win32.c`
  - Audio backends: selected by `DSD_AUDIO_BACKEND` (`auto` → PortAudio on Windows, PulseAudio
    elsewhere; `none` → `audio_null.c` discard/silence backend; `aaudio` → Android). Exactly one
    backend translation unit is compiled per build; the shared last-error store lives in
    `src/platform/audio_error_internal.h`
- Build files: `src/platform/CMakeLists.txt`

## Core

- Path: `src/core`, `include/dsd-neo/core`
- Target: `dsd-neo_core`
- Responsibilities: cross-protocol glue (audio output helpers, vocoder glue, frame helpers, GPS, file import),
  misc/util
- API note: the high-pass filter in `<dsd-neo/core/audio_filters.h>` is `dsd_hpf()`. It was renamed from
  `hpf()` because codec2 exports a symbol of that name and Android links codec2 statically, which turns the
  duplicate into a link error. It is the only filter in that header that is prefixed: codec2 exports none of
  the others (`lpf`, `lpf_f`, `hpf_f`, `hpf_dL`, `hpf_dR`, `pbf`), so renaming them would break out-of-tree
  callers for no benefit. Out-of-tree callers of `hpf()` need updating
- Build files: `src/core/CMakeLists.txt`

## Runtime

- Path: `src/runtime`, `include/dsd-neo/runtime`
- Target: `dsd-neo_runtime`
- Responsibilities:
  - Config system (schema, expansion, user config), logging, memory helpers, rings, worker pools, RT scheduling
  - CLI parsing and interactive/bootstrap helpers (`include/dsd-neo/runtime/cli.h`)
  - Hook interfaces that let DSP/protocol code publish state without depending on UI internals
  - RadioReference.com import client (`src/runtime/radioreference/`): SOAP envelope builder, expat response parser,
    worker-thread client with cancellation, and the generators that turn fetched systems into the channel-map and
    talkgroup CSVs `src/core/file/dsd_import.c` already parses. UI-agnostic C API in
    `include/dsd-neo/runtime/radioreference.h` and `radioreference_generate.h`; needs `USE_CURL` and `USE_EXPAT`, and
    reports `dsd_rr_available() == 0` without them. Shared curl setup lives in the module-private
    `src/runtime/curl_common.h`, alongside `rdio_export.c`'s use of it.
    The frontend-agnostic import policy — system classification, the import-plan builder, tune-frequency
    selection, Hz-to-MHz text, the baked-key rule and the output filename stem — lives in
    `radioreference/rr_import.c` behind `include/dsd-neo/runtime/radioreference_import.h` and compiles with
    or without curl and expat, so the no-expat configuration still builds it. `radioreference/rr_provenance.c`
    reads and writes the plain-text `<file>.rr` sidecars that make a generated CSV refreshable. Both
    frontends consume that header, so the Qt model and the terminal wizard cannot drift apart.
- Build files: `src/runtime/CMakeLists.txt`
- Config docs: `docs/config-system.md`, `docs/radioreference-import.md`

### Telemetry Hooks (DSP/Protocol → UI)

The runtime module defines telemetry hook interfaces in `include/dsd-neo/runtime/telemetry.h` that allow DSP and
protocol code to publish state snapshots without depending on UI internals. DSP and protocol code should include this
header rather than UI headers directly.

**Available hooks:**

- `dsd_telemetry_publish_snapshot(state)` — publish demod state for frontend rendering
- `dsd_telemetry_publish_opts_snapshot(opts)` — publish options when they change
- `dsd_telemetry_request_redraw()` — request frontend refresh
- `dsd_telemetry_publish_both_and_redraw(opts, state)` — convenience combo

**Hook registration pattern:** Runtime owns a thread-safe hook table (`src/runtime/telemetry_hooks.c`). App-control
installs frontend callbacks at startup (`src/app_control/telemetry_hooks_install.c`), and headless/test builds simply
run with the default no-callback state.

**Dependency direction:** DSP/Protocol → Runtime (hooks) ← UI (implementations). This keeps DSP UI-agnostic while
allowing state propagation.

### Frame Sync Hooks (DSP → Runtime ← Engine/Protocols)

DSP frame-sync code may need to trigger protocol-specific actions (for example, trunking state machine ticks) without
depending directly on protocol headers. The runtime provides a small hook table in
`include/dsd-neo/runtime/frame_sync_hooks.h`; the engine installs the concrete implementations at startup in
`src/engine/frame_sync_hooks_install.c`.

## App-Control

- Path: `src/app_control`, `include/dsd-neo/app_control`
- Target: `dsd-neo_app_control`
- Responsibilities:
  - Frontend metrics and raw telemetry snapshots used by the terminal renderer
  - Command queue dispatch and menu service helpers
  - Frontend runtime/control-pump glue and telemetry hook installation
  - Public frontend boundary headers under `<dsd-neo/app_control/...>`
  - RadioReference apply: `include/dsd-neo/app_control/rr_import_apply.h` carries the by-value apply payload
    and the pure plan-to-payload mapper; `src/app_control/rr_import_apply.c` implements it, and the
    `DSD_APP_CMD_RR_APPLY_IMPORT` / `DSD_APP_CMD_RR_ACCOUNT_SET` handlers run on the decoder thread so no
    frontend ever writes `dsd_opts` itself
- Behavior note: `dsd_app_frontend_get_metrics*` reports tuner and demodulator readings only for RTL-family
  input (`AUDIO_IN_RTL`, which covers both a local dongle and rtl_tcp). Every one of those readings comes from
  the RTL stream, whose state is process-global and outlives the session that produced it, so on a WAV, stdin,
  UDP, TCP or symbol-file session they would be a previous run's measurements rather than the current one's.
  Such sessions therefore report the defaults — no carrier lock, no CFO, no output/symbol rate, and the
  invalid-SNR sentinel — and a frontend should omit those rows rather than render them as zeros. Applies to
  every frontend, not just the Android app
- Build files: `src/app_control/CMakeLists.txt`

## DSP

- Path: `src/dsp`, `include/dsd-neo/dsp`
- Target: `dsd-neo_dsp`
- Responsibilities: demodulation pipeline, cascaded decimation/resampler, filters, OP25-style CQPSK timing/carrier
  recovery, CQPSK helpers
  (matched/RRC), and SIMD helpers; exposes runtime-tunable parameters consumed by the UI
- Build files: `src/dsp/CMakeLists.txt`

Runtime controls (via `include/dsd-neo/io/rtl_stream_c.h`):

- CQPSK control/status: `rtl_stream_toggle_cqpsk`, `rtl_stream_get_cqpsk_status`,
  `rtl_stream_request_cqpsk_reacquire`,
  `rtl_stream_set_ted_sps`/`rtl_stream_get_ted_sps`, `rtl_stream_set_ted_gain`/`rtl_stream_get_ted_gain`,
  and CQPSK timing residual via `rtl_stream_cqpsk_timing_bias`.
- FM/FSK conditioning: I/Q DC blocker get/set.
- Spectral/diagnostics: constellation/eye/spectrum getters, spectrum FFT size set/get, SNR getters/estimates for
  C4FM/CQPSK/GFSK.
- Front-end assists: tuner autogain get/set, IQ balance toggle/get, and auto-PPM query/lock/toggle.

## IO

- Path: `src/io`, `include/dsd-neo/io`
- Targets:
  - `dsd-neo_io_iq` — I/Q capture/replay metadata and file helpers; no SDR dependency
  - `dsd-neo_io_radio` — radio front-end and orchestrator for RTL-SDR (USB), RTL-TCP, and SoapySDR backends; provides
    constellation/eye/spectrum snapshots, optional bias-tee (RTL path), and auto-PPM hooks
    - Built when `DSD_HAS_RADIO` is true (RTL and/or Soapy available); otherwise provided as an INTERFACE stub target
  - `dsd-neo_io_audio` — network audio/input backends: UDP PCM16LE input, TCP PCM16LE input, UDP audio output helpers,
    and M17 UDP helpers
  - `dsd-neo_io_udp_control` — UDP retune control server (used by the RTL-SDR/FM helpers)
  - `dsd-neo_io_control` — rigctl/serial control interfaces

Key public headers:

- RTL stream C API: `include/dsd-neo/io/rtl_stream_c.h`
- RTL C++ orchestrator: `include/dsd-neo/io/rtl_stream.h` (class `RtlSdrOrchestrator`)
- RTL device/config/metrics: `include/dsd-neo/io/rtl_device.h`, `include/dsd-neo/io/rtl_demod_config.h`,
  `include/dsd-neo/io/rtl_metrics.h`
- Rig/control: `include/dsd-neo/io/control.h`, `include/dsd-neo/io/rigctl_client.h`,
  `include/dsd-neo/io/m17_udp.h`
- UDP control API: `include/dsd-neo/io/udp_control.h`
- UDP audio output: `include/dsd-neo/io/udp_audio.h` (implemented in `src/io/audio_backends/udp_audio.c`)
- UDP/TCP PCM input: `include/dsd-neo/io/udp_input.h`, `include/dsd-neo/io/tcp_input.h`
- I/Q capture/replay: `include/dsd-neo/io/iq_capture.h`, `include/dsd-neo/io/iq_replay.h`,
  `include/dsd-neo/io/iq_types.h`

Notes:

- Local audio output backends and audio device listing live in `dsd-neo_platform` (see `src/platform/audio_*.c`).
- Network audio/input backends live in `src/io/audio_backends/` (`udp_input.c`, `tcp_input.c`, `udp_audio.c`,
  `m17_udp.c`, `udp_bind.c`).
- M17 protocol frame packing/parsing lives in `src/protocol/m17/m17.c`; M17 UDP socket helpers are exposed via
  `include/dsd-neo/io/m17_udp.h`.

Build files: `src/io/CMakeLists.txt` (defines radio/audio/control subtargets)

## FEC

- Path: `src/fec`, `include/dsd-neo/fec`
- Target: `dsd-neo_fec`
- Responsibilities: BCH, Golay, Hamming, RS, and BPTC helpers. Protocol-specific CRC/FCS helpers live with the
  corresponding protocol modules under `src/protocol/...`.
- Build files: `src/fec/CMakeLists.txt`

## Crypto

- Path: `src/crypto`, `include/dsd-neo/crypto`
- Target: `dsd-neo_crypto`
- Responsibilities: stream/block ciphers and helpers (RC2/RC4/DES/AES/etc)
- Build files: `src/crypto/CMakeLists.txt`

## Protocols

- Path: `src/protocol`, `include/dsd-neo/protocol`
- Targets (one per protocol):
  - `dsd-neo_proto_dmr`, `dsd-neo_proto_dpmr`, `dsd-neo_proto_dstar`, `dsd-neo_proto_nxdn`, `dsd-neo_proto_p25`
    (phase1/phase2), `dsd-neo_proto_m17`, `dsd-neo_proto_x2tdma`, `dsd-neo_proto_edacs`, `dsd-neo_proto_provoice`,
    `dsd-neo_proto_ysf`

Notes:

- Optional codec integrations are expressed via feature interface targets:
  - `dsd-neo_feature_codec2` → `USE_CODEC2` (used by M17 when available)

Key public headers (selection):

- DMR: `<dsd-neo/protocol/dmr/dmr_utils_api.h>`, `<dsd-neo/protocol/dmr/dmr_trunk_sm.h>`
- P25: `<dsd-neo/protocol/p25/p25p1_const.h>`, `<dsd-neo/protocol/p25/p25_trunk_sm.h>`,
  `<dsd-neo/protocol/p25/p25_sm_watchdog.h>`
- NXDN: `<dsd-neo/protocol/nxdn/nxdn_const.h>`
- D‑STAR: `<dsd-neo/protocol/dstar/dstar_const.h>`, `<dsd-neo/protocol/dstar/dstar_header.h>`
- ProVoice/EDACS: `<dsd-neo/protocol/provoice/provoice_const.h>`

Build files: `src/protocol/CMakeLists.txt` and per‑protocol `src/protocol/<name>/CMakeLists.txt`

## Third‑Party

- Paths:
  - `src/third_party/ezpwd` — Target: `dsd-neo_ezpwd` (INTERFACE; headers included via `src/third_party` path)
  - `src/third_party/pffft` — Target: `dsd-neo_pffft` (STATIC; FFT helper for spectrum/diagnostics)
- Build files: `src/third_party/CMakeLists.txt` and subdirectory `CMakeLists.txt` files

## UI

- Path: `src/ui`
- Targets: `dsd-neo_ui_terminal` (option `DSD_ENABLE_TERMINAL_UI`, default ON), `dsd-neo_ui_qt`
  (option `DSD_ENABLE_QT_UI`, default OFF)
- Responsibilities:
  - Terminal frontend implementation (panels, logging, protocol displays, visualizers)
  - Data-driven, nonblocking menu overlay implemented under `src/ui/terminal/` (`menu_*.c`, `menus/menu_defs.c`)
  - RadioReference import wizard: `rr_wizard_core.{h,c}` is the curses-free state machine (headless-testable,
    driven by the `RrWizardHooks` table) and `rr_panel.{h,c}` is the modal presenter that renders it and
    implements those hooks. Both live directly in `src/ui/terminal/` next to `menu_prompts.c`, not in
    `panels/`, which holds only the two non-modal display strips (`header.c`, `footer.c`)
  - Imported RadioReference systems: `rr_library.{h,c}` is the curses-free model of the imports directory
    (folds the group-list and channel-map halves of an import back into one system by RR system id, sorts,
    marks the in-use one, formats a row; capped at `RR_LIBRARY_MAX`) and `rr_panel.c` presents it as the
    **Imported Systems** browser. `csv_picker.{h,c}` offers the same directory's files of one `kind` to the
    "Import channel map/group list CSV..." menu items, with an "Enter a path..." row falling back to the plain
    prompt. Both headers are terminal-private, both modules are filesystem-only (no curses, no app-control),
    and both are tested headless against a scratch directory (`UI_RR_LIBRARY`, `UI_CSV_PICKER`)
  - Frontend-facing controls and DSP/RTL metrics normally flow through app-control commands and
    `include/dsd-neo/app_control/frontend.h`. The terminal frontend retains a small set of terminal-private backend
    integrations.
  - Radio-driven UI controls are gated by `USE_RADIO`; visualizers consume app-control frontend metric APIs.

Qt Quick frontend (`src/ui/qt`):

- QML plus C++ view-models (metrics, call history + per-view filters, saved systems, imported CSV files, app
  preferences, command bridge) that poll app-control on a timer; used by the Android app today and intended as the
  shared basis for a desktop GUI. `imported_files_model.{h,cpp}` is the library behind the CSV pickers: it copies
  picked documents into durable app storage through `DecoderHost::importDocument()` and dry-run validates them via
  `<dsd-neo/core/csv_validate.h>` (`src/core/file/dsd_import.c`) for row-count feedback.
  `radio_reference_model.{h,cpp}` plus `qml/RadioReferenceScreen.qml` are the RadioReference import: the model drives
  the runtime client, previews what an import would produce, and writes the generated CSVs into that same library with
  provenance, while the add-system wizard stays the single writer of a saved system. See
  `docs/radioreference-import.md`.
- Platform-free by rule: it may include Qt and `include/dsd-neo/app_control/` headers, never engine/io/protocol
  internals, and never platform APIs (`QJniObject`, `<android/*.h>`). Platform specifics live behind the `DecoderHost`
  interface, implemented per host (`android/decoder_host_android.cpp` today).
- Backend code must never include Qt headers; `cmake/arch_rules.cmake` enforces both directions.

Build files: `src/ui/CMakeLists.txt`, `src/ui/terminal/CMakeLists.txt`, `src/ui/qt/CMakeLists.txt`

Key public headers:

- Frontend commands, history, metrics, and lifecycle: `include/dsd-neo/app_control/commands.h`,
  `include/dsd-neo/app_control/history.h`, `include/dsd-neo/app_control/frontend.h`, and
  `include/dsd-neo/app_control/frontend_runtime.h`
- Terminal-only headers live under `src/ui/terminal/dsd-neo/ui/` and are private to the terminal target/tests.

### Adding Menu Items

- Define a handler:
  - Prefer an app-control service in `src/app_control/services.h` with implementation in `src/app_control/` for side
    effects (I/O, mode switches, file ops).
  - Menu action handlers live in `src/ui/terminal/menu_actions.c` and should be thin wrappers that call service helpers
    and use `ui_prompt_open_*_async` to gather input.
- Find the row's home. The root (`src/ui/terminal/menus/menu_defs.c`) is the receiver's signal chain — Input, Decoder,
  Trunking, Encryption, Audio, Recording & logs — then Display, Config, Advanced. A setting goes where the signal it
  acts on lives, not where the module that implements it lives; every concept has exactly one home.
  - Within a submenu: a status row first (if any), the primary on/off switch, the settings people change often, then
    one-shot actions; destructive actions last, after a separator.
  - No single-item submenus. Nothing deeper than three submenus below the root. No submenu longer than fifteen
    rows (what a 24-row terminal shows).
  - A row that opens a submenu gets a trailing ` >` from the renderer, so its label never ends in `...`; `...` is
    reserved for rows that open a prompt or picker.
- Extend a menu table (`src/ui/terminal/menu_items.c`): add an `NcMenuItem` entry to the submenu array. The fields
  (`src/ui/terminal/dsd-neo/ui/menu_core.h`):
  - `id` — stable identifier; `help` — required on every action row (`h` shows it).
  - `label` or `label_fn` — a static label, or a generator in `src/ui/terminal/menu_labels.c` that renders the live
    state.
  - `on_select` — the action; or `submenu` + `submenu_len` for a nested array.
  - `is_enabled` — optional predicate; a hidden row is not drawn at all.
  - `hotkey` — the main-screen key(s) for the same action, drawn right-aligned on the row (`"t"`, `"+ -"`, `"P/p"`).
    The character must be the one defined in `src/ui/terminal/dsd-neo/ui/keymap.h` and dispatched in
    `src/ui/terminal/dsd_ncurses_handler.c`; a row never invents a key.
  - `kind` — `NC_ITEM_ACTION` (default, selectable), `NC_ITEM_STATUS` (dimmed, read-only, skipped by navigation),
    or `NC_ITEM_SEPARATOR` (a rule; `label` ignored).
- Label grammar: `Noun [State]` for toggles with `On`/`Off` (never `Active`/`Inactive`, never a `Toggle` prefix);
  `Noun... [current]` for rows that open a prompt or picker; an imperative verb (`Import ...`, `Clear ...`,
  `Restart ...`, `Save ...`, `Stop ...`) only for a one-shot action; sentence case; ASCII `...`.
- `tests/ui/test_ui_menu_tree_audit.c` (`UI_MENU_TREE_AUDIT`) walks the whole tree and enforces the above: help text
  on every action row, at least two action rows per submenu, the depth and length limits, no two rows sharing an
  action, no banned words in labels, and a hotkey table cross-checked against `keymap.h`. A new row therefore needs
  help text, a home that fits the rules, and — if it carries a hotkey — an entry in that test's hotkey table.
- Keep UI/business logic separate:
  - Do not perform device or file operations directly in menu callbacks. Use services instead to make behavior
    testable and reusable across command entry points.
- Prompts and exit:
  - Use the nonblocking prompt overlays provided by the menu core (string/int/double/confirm equivalents handled
    asynchronously). Handlers can set `exitflag` to request immediate exit; the loop will return.

## Include Prefix Summary

- Core: `<dsd-neo/core/...>`
- Engine: `<dsd-neo/engine/...>`
- Platform: `<dsd-neo/platform/...>`
- Runtime: `<dsd-neo/runtime/...>`
- DSP: `<dsd-neo/dsp/...>`
- IO: `<dsd-neo/io/...>`
- FEC: `<dsd-neo/fec/...>`
- Crypto: `<dsd-neo/crypto/...>`
- Protocols: `<dsd-neo/protocol/<name>/...>`

Additional includes of interest:

- Runtime: `<dsd-neo/runtime/cli.h>`, `<dsd-neo/runtime/frame_sync_hooks.h>`, `<dsd-neo/runtime/telemetry.h>`,
  `<dsd-neo/runtime/radioreference.h>`, `<dsd-neo/runtime/radioreference_generate.h>`,
  `<dsd-neo/runtime/radioreference_import.h>`
- IO: `<dsd-neo/io/rtl_stream_c.h>`, `<dsd-neo/io/rtl_stream.h>`, `<dsd-neo/io/rtl_device.h>`,
  `<dsd-neo/io/rtl_demod_config.h>`, `<dsd-neo/io/rtl_metrics.h>`, `<dsd-neo/io/control.h>`,
  `<dsd-neo/io/rigctl_client.h>`, `<dsd-neo/io/m17_udp.h>`, `<dsd-neo/io/udp_audio.h>`,
  `<dsd-neo/io/udp_control.h>`, `<dsd-neo/io/udp_input.h>`,
  `<dsd-neo/io/tcp_input.h>`
- App-control/UI: command, history, metrics, and lifecycle APIs live in `include/dsd-neo/app_control`; terminal internals
  are private under `src/ui/terminal/dsd-neo/ui`

## Build Targets

- Libraries build under `src/...`; the CLI builds under `apps/dsd-cli` as `dsd-neo`.
- Use CMake presets (see `CMakePresets.json`).
- Tests live under `tests/<area>` and are wired with CTest; run with `ctest --preset dev-debug --output-on-failure`.

Top‑level build files: `CMakeLists.txt`, `CMakePresets.json`, `apps/CMakeLists.txt`, `tests/CMakeLists.txt`

Common interface targets:

- `dsd-neo_warnings` — common warning flags (optional; controlled by `DSD_ENABLE_WARNINGS`, `DSD_WARNINGS_AS_ERRORS`)
- `dsd-neo_test_support` — test-only compile/link defaults used by `tests/` executables

Optional feature interface targets (compile definitions + include paths; stubbed out when deps are missing):

- `dsd-neo_feature_colors` — `PRETTY_COLORS` when terminal UI colors are enabled (`COLORS`)
- `dsd-neo_feature_colors_logs` — `PRETTY_COLORS_LOGS` when colored terminal/log output is enabled (`COLORSLOGS`)
- `dsd-neo_feature_pvc` — `PVCONVENTIONAL` when ProVoice conventional frame sync is enabled (`PVC`)
- `dsd-neo_feature_lz` — `LIMAZULUTWEAKS` when LimaZulu NXDN tweaks are enabled (`LZ`)
- `dsd-neo_feature_sid` — `SOFTID` when P25p1 soft ID decoding is enabled (`SID`)
- `dsd-neo_feature_radio` — `USE_RADIO` when any radio backend is available (`DSD_HAS_RADIO`)
- `dsd-neo_feature_rtlsdr` — `USE_RTLSDR` (+ `USE_RTLSDR_BIAS_TEE` when supported by librtlsdr)
- `dsd-neo_feature_soapy` — `USE_SOAPYSDR` + SoapySDR >= 0.8.1 imported-target link/includes when available
- `dsd-neo_feature_codec2` — `USE_CODEC2` (require with `DSD_REQUIRE_CODEC2=ON`)
- `dsd-neo_feature_curl` — `USE_CURL` + libcurl link when available (require with `DSD_REQUIRE_CURL=ON`)
- `dsd-neo_feature_expat` — `USE_EXPAT` + expat link when available (require with `DSD_REQUIRE_EXPAT=ON`)

External dependencies (resolved via CMake):

- Required: OpenSSL 3.x libcrypto; LibSndFile; an audio backend (PulseAudio by default, PortAudio on Windows); MBE
  vocoder (`mbe-neo` 2.x).
- Terminal frontend: curses (ncursesw/PDCurses), enabled by default with `DSD_ENABLE_TERMINAL_UI=ON`.
- Optional: RTL‑SDR, SoapySDR >= 0.8.1, CODEC2, libcurl >= 7.56.0.
