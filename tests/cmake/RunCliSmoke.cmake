# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs dsd-neo once per named mode and checks the exit code with the output
# together: CTest's PASS_REGULAR_EXPRESSION alone ignores the exit code.
#
# Expected -D inputs: DSD_NEO_CLI, DSD_NEO_CLI_SMOKE_MODE; DSD_NEO_IQ_FIXTURE for the modes that replay one.

if(NOT DEFINED DSD_NEO_CLI)
    message(FATAL_ERROR "DSD_NEO_CLI is required")
endif()

if(NOT DEFINED DSD_NEO_CLI_SMOKE_MODE)
    message(FATAL_ERROR "DSD_NEO_CLI_SMOKE_MODE is required")
endif()

if(DSD_NEO_CLI_SMOKE_MODE STREQUAL "help")
    set(_args "-h")
    set(_want_rc 0)
    # -ft is a decode preset, not a trunking switch: the help line says so and points at -T.
    set(_want_stdout_regex
        "Usage: dsd-neo \\[options\\].*Decoder options:.*\n  -ft           Decode only DMR, P25 Phase 1 and P25 Phase 2; add -T to follow a trunked system\r?\n.*--tetra-uplink.*--tetra-net <mcc,mnc,colour>.*--tetra-key"
    )
    set(_want_stderr_regex "")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "invalid-option")
    # The CLI words these errors itself: each C library's getopt says something
    # different, and the Windows one says nothing.
    set(_args "--definitely-not-an-option")
    set(_want_rc 1)
    set(_want_stdout_regex "Usage: dsd-neo \\[options\\]")
    set(_want_stderr_regex "Unrecognized option '--definitely-not-an-option'")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "invalid-short-option")
    set(_args "-u")
    set(_want_rc 1)
    set(_want_stdout_regex "Usage: dsd-neo \\[options\\]")
    set(_want_stderr_regex "Unrecognized option '-u'")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "missing-argument")
    set(_args "-o")
    set(_want_rc 1)
    set(_want_stdout_regex "Usage: dsd-neo \\[options\\]")
    set(_want_stderr_regex "Option -o requires an argument")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "print-config-rtltcp-bad-port")
    # Issue #743 B15: an rtl_tcp port that is not one fails --print-config, which
    # reads the spec as the engine will, with the field named; nothing is exported.
    set(_args "--no-config" "-i" "rtltcp:radio.local:12x:851M" "--print-config")
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "\"12x\".*65535")
    set(_forbid_output_regex "rtltcp_port")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "print-config-rtl-bad-squelch")
    # Issue #899 B14: a spec's squelch field --squelch would refuse fails --print-config,
    # naming the field, as it fails the start; it was ignored, leaving the -110 dB default.
    set(_args
        "--no-config"
        "-i"
        "rtl:0:154.43M:30:0:24:auto+31:3"
        "--print-config"
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "squelch field \"auto[+]31\".*3 to 30")
    set(_forbid_output_regex "rtl_sql")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "print-config-soapy-bad-squelch")
    # The same on a SoapySDR shorthand tuning tail, which it turned into device args at 850 MHz.
    set(_args
        "--no-config"
        "-i"
        "soapy:driver=foo:154.43M:30:0:24:auto+31:3"
        "--print-config"
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "squelch field \"auto[+]31\".*3 to 30")
    set(_forbid_output_regex "rtl_freq|soapy_args")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "print-config-rtl-rounded-squelch")
    # Issue #899 B15: rtl_sql holds whole dB; an export rounds a level to the nearest one
    # and says so, and an active level never exports as 0 (off).
    set(_args
        "--no-config"
        "-i"
        "rtl:0:155.34M:30:5:24:-65.5:3"
        "--print-config"
    )
    set(_want_rc 0)
    set(_want_stdout_regex "rtl_sql = -66\n")
    set(_want_stderr_regex "squelch level -65[.]50 dB.*rounded")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "no-config-notice")
    # --no-config was accepted and silently did nothing once config loading became opt-in: it says so now, once,
    # and the run goes on.
    set(_args "--no-config" "--print-config")
    set(_want_rc 0)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NOTICE: --no-config has no effect: a config loads only with --config, a single [.]ini file argument, or DSD_NEO_CONFIG[.]"
    )
    set(_forbid_output_regex "Loaded user config")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "print-config-iq-stream")
    # Issue #743: an I/Q stream exports as its own [input] source with the spec's fields in canonical text (RATE and
    # FREQ in Hz) and none of the tuner keys of the inputs DSD-neo tunes; nothing connects.
    set(_args
        "--no-config"
        "-i"
        "iqtcp:127.0.0.1:1234:240k:cs16:851.0125M"
        "--print-config"
    )
    set(_want_rc 0)
    set(_want_stdout_regex
        "source = \"iqtcp\"\niq_host = \"127[.]0[.]0[.]1\"\niq_port = 1234\niq_rate = \"240000\"\niq_format = \"cs16\"\niq_freq = \"851012500\"\n"
    )
    set(_want_stderr_regex "")
    set(_forbid_output_regex "rtl_gain|rtl_ppm|auto_ppm|rtl_freq|connecting to")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-replay-save-validates")
    # Issue #743 (D8 over B1): a replay of a capture under a config whose [input] is an I/Q stream, with -T, saves that
    # [input] with trunking off, so the next start of the config runs: the shutdown autosave and --print-config both
    # pass --validate-config. Before, both wrote the stream with trunking on, which the next start refuses (exit 1).
    if(NOT DEFINED DSD_NEO_IQ_FIXTURE)
        message(FATAL_ERROR "DSD_NEO_IQ_FIXTURE is required")
    endif()
    set(_stream_ini
        "[input]\nsource = \"iqtcp\"\niq_host = \"127.0.0.1\"\niq_port = 1234\niq_rate = \"240k\"\n"
        "iq_format = \"cs16\"\n[trunking]\nenabled = false\n"
    )
    set(_cleanup_file "${CMAKE_CURRENT_BINARY_DIR}/iq_stream_replay_save.ini")
    set(_printed
        "${CMAKE_CURRENT_BINARY_DIR}/iq_stream_replay_save_printed.ini"
    )
    file(WRITE "${_cleanup_file}" ${_stream_ini})
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=DSD_NEO_CONFIG "${DSD_NEO_CLI}"
            --config "${_cleanup_file}" --iq-replay "${DSD_NEO_IQ_FIXTURE}" -T
            --print-config
        RESULT_VARIABLE _pre_rc
        OUTPUT_FILE "${_printed}"
        ERROR_VARIABLE _pre_err
        TIMEOUT 25
    )
    if(NOT _pre_rc EQUAL 0)
        file(REMOVE "${_cleanup_file}" "${_printed}")
        message(
            FATAL_ERROR
            "--print-config of the replay failed (rc=${_pre_rc})\n${_pre_err}"
        )
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=DSD_NEO_CONFIG "${DSD_NEO_CLI}"
            --validate-config "${_printed}"
        RESULT_VARIABLE _pre_rc
        OUTPUT_VARIABLE _pre_out
        ERROR_VARIABLE _pre_err
        TIMEOUT 25
    )
    file(READ "${_printed}" _printed_text)
    file(REMOVE "${_printed}")
    if(NOT _pre_rc EQUAL 0 OR NOT _printed_text MATCHES "source = \"iqtcp\"")
        file(REMOVE "${_cleanup_file}")
        message(
            FATAL_ERROR
            "--print-config of the replay does not validate as the stream config (rc=${_pre_rc})\n${_pre_out}\n${_pre_err}\n${_printed_text}"
        )
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=DSD_NEO_CONFIG "${DSD_NEO_CLI}"
            --config "${_cleanup_file}" --iq-replay "${DSD_NEO_IQ_FIXTURE}" -T
            --frontend none -o null
        RESULT_VARIABLE _pre_rc
        OUTPUT_VARIABLE _pre_out
        ERROR_VARIABLE _pre_err
        TIMEOUT 25
    )
    file(READ "${_cleanup_file}" _saved_text)
    if(
        NOT _pre_rc EQUAL 0
        OR NOT _saved_text MATCHES "source = \"iqtcp\"[^[]*\\[output\\]"
    )
        file(REMOVE "${_cleanup_file}")
        message(
            FATAL_ERROR
            "the replay did not run and autosave its stream config (rc=${_pre_rc})\n${_pre_out}\n${_pre_err}\n${_saved_text}"
        )
    endif()
    # The autosaved file, as the next start reads it.
    set(_args --validate-config "${_cleanup_file}")
    set(_want_rc 0)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex "iq_stream_replay_save[.]ini: OK")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-needs-radio")
    # Issue #743: a build without radio support reads an I/Q stream spec and refuses it, as it refuses --iq-replay.
    set(_args
        "--no-config"
        "-i"
        "iqudp:127.0.0.1:7356:48k:cu8"
        "-o"
        "null"
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "-i iqudp: requires a build with radio pipeline support"
    )
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-rate-refused-iq-stream")
    # Issue #743: an I/Q stream runs at the target its RATE is planned to, known from its spec, so a width that target
    # cannot filter is refused before the stream connects, as for a live radio: the config's 16 kHz DSP bandwidth
    # request plans a 240 kS/s stream to 16 kHz. Nothing listens on port 1.
    set(_cleanup_file "${CMAKE_CURRENT_BINARY_DIR}/iq_stream_width_refused.ini")
    file(
        WRITE "${_cleanup_file}"
        "[input]\nsource = \"iqtcp\"\niq_host = \"127.0.0.1\"\niq_port = 1\niq_rate = \"240k\"\n"
        "iq_format = \"cs16\"\nrtl_bw_khz = 16\n[mode]\ndecode = \"analog\"\n[analog]\n"
        "nfm_bandwidth_hz = 25000\n"
    )
    set(_args
        --config
        "${_cleanup_file}"
        --frontend
        none
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NFM bandwidth 25 kHz does not fit the 16 kHz DSP rate [(]the largest width it fits is 13[.]2 kHz[)]; set the DSP bandwidth to 48 or 72 kHz"
    )
    set(_forbid_output_regex "radio stream")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-tcp-no-listener")
    # Issue #743: a TCP stream says where it connects, warns a rate under TETRA's 25 kHz (36 kHz when the sending
    # program's centring is approximate) and notes that no FREQ was given; with nothing listening the connect fails
    # and the start exits 1. Nothing listens on port 1.
    set(_args
        --no-config
        --frontend
        none
        -fT
        -i
        iqtcp:127.0.0.1:1:20000:cs16
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "I/Q stream: connecting to 127[.]0[.]0[.]1:1 [(]cs16, 20000 samples/s[)].*20000 samples/s is below the 25000 that TETRA needs [(]36000 when the sending program does not centre the channel exactly[)]; decoding may fail[.] Raise the sample rate in the sending program[.].*no receiver frequency [(]FREQ[)] was given.*Add the sending program's centre frequency.*cannot connect to 127[.]0[.]0[.]1:1.*Failed to open radio stream"
    )
    set(_forbid_output_regex "receiving on")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-min-rate-narrow")
    # A session that decodes only 6.25 kHz channels (NXDN48) is warned under 12 kHz; no TETRA notes.
    set(_args
        --no-config
        --frontend
        none
        -fi
        -i
        iqtcp:127.0.0.1:1:8000:cs16
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "8000 samples/s is below the 12000 that NXDN48 or dPMR needs; decoding may fail"
    )
    set(_forbid_output_regex "36000|no receiver frequency")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-min-rate-wide")
    # A DMR session is warned under 16 kHz; at 48 kHz with a FREQ it is warned of nothing.
    set(_args
        --no-config
        --frontend
        none
        -fs
        -i
        iqtcp:127.0.0.1:1:12000:cs16
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "12000 samples/s is below the 16000 that a 12[.]5 kHz digital channel needs; decoding may fail"
    )
    set(_forbid_output_regex "36000|no receiver frequency")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-min-rate-analog")
    # An analog session is warned for its FM or AM channel, not as a digital mode.
    set(_args
        --no-config
        --frontend
        none
        -fA
        -i
        iqtcp:127.0.0.1:1:12000:cs16
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "12000 samples/s is below the 16000 that an analog FM channel needs; decoding may fail"
    )
    set(_forbid_output_regex "digital channel|36000|no receiver frequency")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-rate-above-ceiling")
    # A RATE above the live-device capture ceiling (a typo such as 24M for 240k) is refused before anything connects,
    # with the limit and the fix named, rather than sizing a ring and a chain for it.
    set(_args
        --no-config
        --frontend
        none
        -i
        iqtcp:127.0.0.1:1:24M:cs16
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "I/Q stream: the stream's rate [(]24000000 Hz[)] is above 3200000 Hz; send one channel [(]the sending program's VFO mode[)]"
    )
    set(_forbid_output_regex "connecting to")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-config-rate-above-ceiling")
    # The same RATE from a config's [input] is refused alike at startup.
    set(_cleanup_file
        "${CMAKE_CURRENT_BINARY_DIR}/iq_stream_rate_above_ceiling.ini"
    )
    file(
        WRITE "${_cleanup_file}"
        "[input]\nsource = \"iqtcp\"\niq_host = \"127.0.0.1\"\niq_port = 1\niq_rate = \"24M\"\n"
        "iq_format = \"cs16\"\n"
    )
    set(_args
        --config
        "${_cleanup_file}"
        --frontend
        none
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "I/Q stream: the stream's rate [(]24000000 Hz[)] is above 3200000 Hz; send one channel [(]the sending program's VFO mode[)]"
    )
    set(_forbid_output_regex "connecting to")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "iq-stream-min-rate-met")
    set(_args
        --no-config
        --frontend
        none
        -fT
        -i
        iqtcp:127.0.0.1:1:48000:cs16:420.0125M
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "I/Q stream: connecting to 127[.]0[.]0[.]1:1 [(]cs16, 48000 samples/s[)]"
    )
    set(_forbid_output_regex "is below the|no receiver frequency")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-rate-refused")
    # Issue #525: an explicit NFM width the rtl_tcp input's 24 kHz DSP
    # bandwidth cannot filter is refused before the device opens, with the
    # validator's actionable text, and startup fails. No server is needed:
    # nothing listens on port 1, and a run that went on to open the stream
    # would print the stream failure instead of the refusal.
    set(_args
        --frontend
        none
        -fA
        -i
        rtltcp:127.0.0.1:1:851.375M:0:0:24
        --nfm-bandwidth-hz
        25000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate [(]the largest width it fits is 20[.]4 kHz[)]; set the RTL DSP bandwidth to 48 or 72 kHz"
    )
    set(_forbid_output_regex "radio stream")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-rate-refused-rtl")
    # The same refusal for an RTL-SDR spec, whose DSP bandwidth the spec's
    # sixth field sets. It happens before the input looks for a device, so it
    # needs no dongle and no RTL-SDR support in the build: a run that went on
    # would enumerate devices (or report none, or report the input unsupported)
    # before any refusal, and on a host with a dongle would open the stream.
    set(_args
        --frontend
        none
        -fA
        -i
        rtl:0:851.375M:0:0:24
        --nfm-bandwidth-hz
        25000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate [(]the largest width it fits is 20[.]4 kHz[)]; set the RTL DSP bandwidth to 48 or 72 kHz"
    )
    set(_forbid_output_regex
        "radio stream|RTL Input:|device[(]s[)]|No supported devices|RTL-SDR input"
    )
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-rate-refused-airspy")
    # Issue #806: an Airspy runs at its DSP bandwidth (the live rate step lands
    # its capture rate on it), so the same width is refused before the device
    # opens, with the bandwidths that fit. A run that went on would look for
    # the Airspy (or report the input unsupported) and open the stream.
    set(_args
        --frontend
        none
        -fA
        -i
        airspy:851.375M:24
        --nfm-bandwidth-hz
        25000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate [(]the largest width it fits is 20[.]4 kHz[)]; set the DSP bandwidth to 48 or 72 kHz"
    )
    set(_forbid_output_regex
        "radio stream|Airspy open|AIRSPY_ERROR|Airspy input"
    )
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-rate-refused-soapy")
    # The same for a SoapySDR spec, whose shorthand's bandwidth field sets the
    # DSP bandwidth the device is stepped to: refused before any device opens.
    set(_args
        --frontend
        none
        -fA
        -i
        soapy:driver=nosuchdevice:851.375M:0:0:24
        --nfm-bandwidth-hz
        25000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate [(]the largest width it fits is 20[.]4 kHz[)]; set the DSP bandwidth to 48 or 72 kHz"
    )
    # The spec's own "SoapySDR tuning" notice is printed as it is read; a device
    # open would follow the refusal's place.
    set(_forbid_output_regex "radio stream|Failed|makeDevice|SoapySDR device")
elseif(
    DSD_NEO_CLI_SMOKE_MODE STREQUAL "airspy-config-width-validate"
    OR DSD_NEO_CLI_SMOKE_MODE STREQUAL "airspy-config-width-startup"
)
    # Issue #806 (BUG-4): --validate-config and the startup it vouches for agree
    # on an Airspy [input] at a 24 kHz DSP bandwidth with an NFM width that
    # bandwidth cannot filter: both refuse it, with the same text, and the
    # startup before it looks for the Airspy. Each mode writes its own copy
    # (the startup's autosave rewrites the file it loaded).
    set(_config "${CMAKE_CURRENT_BINARY_DIR}/${DSD_NEO_CLI_SMOKE_MODE}.ini")
    file(
        WRITE "${_config}"
        "[input]\nsource = \"airspy\"\nrtl_freq = \"851.375M\"\nrtl_bw_khz = 24\n"
        "[output]\nbackend = \"null\"\nfrontend = \"none\"\n"
        "[mode]\ndecode = \"analog\"\n[analog]\nnfm_bandwidth_hz = 25000\n"
    )
    if(DSD_NEO_CLI_SMOKE_MODE STREQUAL "airspy-config-width-validate")
        set(_args --validate-config "${_config}")
    else()
        set(_args --config "${_config}")
    endif()
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate [(]the largest width it fits is 20[.]4 kHz[)]; set the DSP bandwidth to 48 or 72 kHz"
    )
    set(_forbid_output_regex "radio stream|Airspy open|AIRSPY_ERROR")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-default-low-rate")
    # Issue #525: an existing low-rate -fA command line keeps starting. With
    # no width given, the unset default is DSP-limited below a 20 kHz DSP rate
    # and never refused for it, so startup goes on to open the rtl_tcp stream
    # (and fails there only because nothing listens on port 1). The negative
    # control for nfm-width-rate-refused: the stream failure, not a refusal.
    set(_args
        --frontend
        none
        -fA
        -i
        rtltcp:127.0.0.1:1:851.375M:0:0:12
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex "Failed to open radio stream")
    set(_forbid_output_regex "does not fit|cannot be filtered")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "nfm-width-fits-low-rate")
    # An explicit width the 16 kHz DSP rate filters (12.5 kHz of its 13.2 kHz
    # maximum) passes the pre-open check the same way.
    set(_args
        --frontend
        none
        -fA
        -i
        rtltcp:127.0.0.1:1:851.375M:0:0:16
        --nfm-bandwidth-hz
        12500
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex "Failed to open radio stream")
    set(_forbid_output_regex "does not fit|cannot be filtered")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-width-rate-refused")
    # Issue #524: an AM width the rtl_tcp input's 16 kHz DSP bandwidth cannot
    # filter is refused before the device opens, with the validator's
    # actionable text, and startup fails. No server is needed: nothing listens
    # on port 1, and a run that went on to open the stream would print the
    # stream failure instead of the refusal.
    set(_args
        --frontend
        none
        -fM
        -i
        rtltcp:127.0.0.1:1:118.1M:0:0:16
        --am-bandwidth-hz
        20000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "AM bandwidth 20 kHz does not fit the 16 kHz DSP rate [(]the largest width it fits is 13[.]2 kHz[)]; set the RTL DSP bandwidth to 24, 48 or 72 kHz"
    )
    set(_forbid_output_regex "radio stream")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-width-rate-refused-rtl")
    # The same refusal for an RTL-SDR spec, whose DSP bandwidth the spec's
    # sixth field sets. It happens before the input looks for a device, so it
    # needs no dongle and no RTL-SDR support in the build: a run that went on
    # would enumerate devices (or report none, or report the input unsupported)
    # before any refusal, and on a host with a dongle would open the stream.
    set(_args
        --frontend
        none
        -fM
        -i
        rtl:0:118.1M:22:0:16
        --am-bandwidth-hz
        20000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "AM bandwidth 20 kHz does not fit the 16 kHz DSP rate [(]the largest width it fits is 13[.]2 kHz[)]; set the RTL DSP bandwidth to 24, 48 or 72 kHz"
    )
    set(_forbid_output_regex
        "radio stream|RTL Input:|device[(]s[)]|No supported devices|RTL-SDR input"
    )
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-default-rate-refused")
    # The unset AM default (6 kHz) is held to the rate like an explicit width:
    # a 6 kHz DSP bandwidth fits at most 4.2 kHz, so -fM alone is refused.
    set(_args
        --frontend
        none
        -fM
        -i
        rtltcp:127.0.0.1:1:118.1M:0:0:6
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex
        "AM bandwidth 6 kHz does not fit the 6 kHz DSP rate [(]the largest width it fits is 4[.]2 kHz[)]; set the RTL DSP bandwidth to 8, 12, 16, 24, 48 or 72 kHz"
    )
    set(_forbid_output_regex "radio stream")
elseif(DSD_NEO_CLI_SMOKE_MODE STREQUAL "am-width-fits-low-rate")
    # An AM width the 16 kHz DSP rate filters (10 kHz of its 13.2 kHz maximum)
    # passes the pre-open check: startup goes on to open the rtl_tcp stream
    # and fails there only because nothing listens on port 1.
    set(_args
        --frontend
        none
        -fM
        -i
        rtltcp:127.0.0.1:1:118.1M:0:0:16
        --am-bandwidth-hz
        10000
        -o
        null
    )
    set(_want_rc 1)
    set(_want_stdout_regex "")
    set(_want_stderr_regex "")
    set(_want_output_regex "Failed to open radio stream")
    set(_forbid_output_regex "does not fit|cannot be filtered")
else()
    message(
        FATAL_ERROR
        "unknown DSD_NEO_CLI_SMOKE_MODE: ${DSD_NEO_CLI_SMOKE_MODE}"
    )
endif()

# A config in the caller's environment never reaches a row: --no-config does nothing (it says so).
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env --unset=DSD_NEO_CONFIG "${DSD_NEO_CLI}"
        ${_args}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 25
)

if(DEFINED _cleanup_file)
    file(REMOVE "${_cleanup_file}")
endif()

set(_combined "${_stdout}\n${_stderr}")

if(NOT _rc EQUAL _want_rc)
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} rc=${_want_rc}, got ${_rc}\n${_combined}"
    )
endif()

if(_want_stdout_regex AND NOT _stdout MATCHES "${_want_stdout_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} stdout to match ${_want_stdout_regex}\n${_combined}"
    )
endif()

if(_want_stderr_regex AND NOT _stderr MATCHES "${_want_stderr_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} stderr to match ${_want_stderr_regex}\n${_combined}"
    )
endif()

if(_want_output_regex AND NOT _combined MATCHES "${_want_output_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} output to match ${_want_output_regex}\n${_combined}"
    )
endif()

if(_forbid_output_regex AND _combined MATCHES "${_forbid_output_regex}")
    message(
        FATAL_ERROR
        "expected ${DSD_NEO_CLI_SMOKE_MODE} output not to match ${_forbid_output_regex}\n${_combined}"
    )
endif()
