// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Which supported receivers a USB vendor/product id names, without Android.
 *
 * [UsbSourceManager] asks this for every device it considers, so the attach filter, the access request and the
 * attached-kind reading all agree on what counts as a receiver.
 */
internal object UsbReceiverKind {
    const val RTL = "usb"
    const val AIRSPY = "airspy"
    const val BOTH = "both"

    private const val AIRSPY_ID = 0x1d5060a1

    /**
     * The Airspy R2 / Mini and the RTL2832U ids librtlsdr recognises. Kept in sync with `res/xml/device_filter.xml`,
     * which drives the attach intent filter.
     */
    private val KNOWN_IDS: Set<Int> = intArrayOf(
        AIRSPY_ID,
        0x0bda2832, 0x0bda2838,
        0x04136680, 0x04136f0f,
        0x0458707f,
        0x0ccd00a9, 0x0ccd00b3, 0x0ccd00b4, 0x0ccd00b5, 0x0ccd00b7, 0x0ccd00b8,
        0x0ccd00b9, 0x0ccd00c0, 0x0ccd00c6, 0x0ccd00d3, 0x0ccd00d7, 0x0ccd00e0,
        0x15545020,
        0x15f40131, 0x15f40133,
        0x185b0620, 0x185b0650, 0x185b0680,
        0x1b80d393, 0x1b80d394, 0x1b80d395, 0x1b80d397, 0x1b80d398, 0x1b80d39d,
        0x1b80d3a4, 0x1b80d3a8, 0x1b80d3af, 0x1b80d3b0,
        0x1d191101, 0x1d191102, 0x1d191103, 0x1d191104,
        0x1f4da803, 0x1f4db803, 0x1f4dc803, 0x1f4dd286, 0x1f4dd803,
    ).toHashSet()

    private fun id(vendorId: Int, productId: Int) = (vendorId shl 16) or (productId and 0xffff)

    /**
     * Deliberately id-only. Matching on "has a vendor-specific interface" would drag in hubs, docks and audio devices,
     * and handing librtlsdr one of those descriptors is worse than not finding a dongle at all. An unlisted rebadge
     * belongs in this table and in res/xml/device_filter.xml, not in a loose match.
     */
    fun isKnown(vendorId: Int, productId: Int): Boolean = KNOWN_IDS.contains(id(vendorId, productId))

    /**
     * The engine's option tables (#782 gate r1 G4), so that another option's operand is never read as an input. Kept
     * equal to the native ones, which the usb_receiver suite of tests/android/run_jvm_tests.py checks against the
     * sources: [SHORT_OPTIONS] is the getopt string dsd_parse_short_opts() in src/runtime/cli/args.c reads through
     * dsd_cli_short_options() (`k_short_options` in src/runtime/cli/compact.c), and the long tables are those
     * dsd_cli_compact_args() in compact.c uses to take long options and their values out before getopt runs (named
     * after them below).
     */
    private const val SHORT_OPTIONS =
        ":~yhaepPqs:t:v:z:i:o:d:c:g:n:w:B:C:R:f:m:x:A:S:M:G:D:L:V:U:YK:b:H:X:Q:WrlZTF@:!:01:2:345:6:7:_:89:Ek:I:J:O^Nj"

    /** Long options that take no value. `k_skip_exact_no_arg`. */
    private val LONG_NO_VALUE: Set<String> = setOf(
        "--airspy-list", "--auto-ppm", "--rtltcp-autotune", "--iq-loop", "--tetra-uplink",
        "--rdio-api-delete-after-upload", "--enc-lockout", "--enc-follow", "--tg-lockout-session",
        "--tg-lockout-persist", "--no-config", "--print-config", "--interactive-setup", "--dump-config-template",
        "--strict-config", "--list-profiles", "--no-force-key", "--no-decryption-keys", "--dmr-tg-key-clear",
        "--strict-crc", "--no-scan-voice-only", "--dmr-debug-burst", "--dmr-debug-unsynced", "--show-keys",
        "--scan-voice-only", "--no-tone-filter", "--no-slot-filter", "--no-dcc-filter", "--no-ran-filter",
        "--no-nac-filter", "--no-service-names", "--record-unknown-only",
    )

    /** Long options that take the next argument as their value, whatever it is. `k_skip_exact_next_any`. */
    private val LONG_NEXT_ANY: Set<String> = setOf(
        "--airspy-serial", "--airspy-sample-rate", "--airspy-gain-mode", "--airspy-sensitivity-gain",
        "--airspy-linearity-gain", "--airspy-lna-gain", "--airspy-mixer-gain", "--airspy-vga-gain", "--airspy-lna-agc",
        "--airspy-mixer-agc", "--airspy-bias-tee", "--input-volume", "--input-level-warn-db", "--frame-log",
        "--p25-sm-log", "--wav-journal", "--rdio-mode", "--rdio-system-id", "--rdio-api-url", "--rdio-api-key",
        "--rdio-upload-timeout-ms", "--rdio-upload-retries", "--dmr-baofeng-pc5", "--dmr-csi-ee72",
        "--dmr-vertex-ks-csv", "--dmr-tg-key-csv", "--p25-bandplan", "--src-csv", "--p25-bandplan-export",
        "--dmr-force-algid", "--m17-signature-public-key", "--m17-scrambler-key", "--m17-aes-key", "--key-profile-ref",
        "--auto-ppm-snr", "--frontend", "--profile", "--p25-vc-grace", "--p25-min-follow-dwell",
        "--p25-grant-voice-timeout", "--p25-mac-hold", "--p25-cc-grace", "--p25-force-release-extra",
        "--p25-force-release-margin", "--p25-p1-err-hold-pct", "--p25-p1-err-hold-sec", "--trunk-scan",
        "--trunk-scan-dwell-ms", "--trunk-scan-activity-hold-ms", "--scan-voice-qualify-ms", "--scan-voice-hold-ms",
        "--scan-max-visit-ms", "--nfm-bandwidth-hz", "--am-bandwidth-hz", "--squelch", "--tone-allow", "--slot-allow",
        "--slot-block", "--dcc-allow", "--dcc-block", "--ran-allow", "--ran-block", "--nac-allow", "--nac-block",
        "--slot-name", "--dcc-name", "--ran-name", "--nac-name", "--tone-name", "--tone-block", "--calc-lcn",
        "--calc-step", "--calc-cc-freq", "--calc-cc-lcn", "--calc-start-lcn",
    )

    /** Long options that take the next argument, whatever it starts with. `k_skip_exact_next_nonnull`. */
    private val LONG_NEXT_PRESENT: Set<String> = setOf(
        "--iq-capture", "--iq-capture-format", "--iq-capture-max-mb", "--symbol-capture-format", "--iq-replay",
        "--iq-replay-rate", "--iq-info",
    )

    /** Long options whose value is free text, taken whatever it starts with: an API token may begin with '-'. `k_free_value_options`. */
    private val LONG_FREE_VALUE: Set<String> = setOf(
        "--api-token", "--api-token-file",
    )

    /** Long options that take the next argument only when it does not start with '-'. `k_skip_exact_next_nonopt`. */
    private val LONG_NEXT_NON_OPTION: Set<String> = setOf(
        "--lrrp-extra-port", "--dmr-udp-comp-port", "--rtl-udp-control", "--rtl-udp-control-bind", "--config",
        "--validate-config", "--api", "--api-bind", "--tetra-net", "--tetra-key",
    )

    /** Long options given with their value attached ("--name=value"), as prefixes. `k_skip_prefix`. */
    private val LONG_ATTACHED_PREFIXES: Set<String> = setOf(
        "--airspy-serial=", "--airspy-sample-rate=", "--airspy-gain-mode=", "--airspy-sensitivity-gain=",
        "--airspy-linearity-gain=", "--airspy-lna-gain=", "--airspy-mixer-gain=", "--airspy-vga-gain=",
        "--airspy-lna-agc=", "--airspy-mixer-agc=", "--airspy-bias-tee=", "--lrrp-extra-port=", "--dmr-udp-comp-port=",
        "--rtl-udp-control=", "--rtl-udp-control-bind=", "--api=", "--api-bind=", "--api-token=", "--api-token-file=",
        "--iq-capture=", "--iq-capture-format=", "--iq-capture-max-mb=", "--symbol-capture-format=", "--iq-replay=",
        "--iq-replay-rate=", "--iq-info=", "--tetra-net=", "--tetra-key=", "--dmr-baofeng-pc5=", "--dmr-csi-ee72=",
        "--dmr-vertex-ks-csv=", "--dmr-tg-key-csv=", "--p25-bandplan=", "--src-csv=", "--p25-bandplan-export=",
        "--dmr-force-algid=", "--m17-signature-public-key=", "--m17-scrambler-key=", "--m17-aes-key=",
        "--key-profile-ref=", "--config=", "--trunk-scan=", "--trunk-scan-dwell-ms=", "--trunk-scan-activity-hold-ms=",
        "--scan-voice-qualify-ms=", "--scan-voice-hold-ms=", "--scan-max-visit-ms=", "--nfm-bandwidth-hz=",
        "--am-bandwidth-hz=", "--squelch=", "--tone-allow=", "--slot-allow=", "--slot-block=", "--dcc-allow=",
        "--dcc-block=", "--ran-allow=", "--ran-block=", "--nac-allow=", "--nac-block=", "--slot-name=", "--dcc-name=",
        "--ran-name=", "--nac-name=", "--tone-name=", "--tone-block=", "--frontend=",
    )

    /**
     * Long options with a value that dsd_parse_args()'s pre-scan (DSD_PARSE_ARGS_PRESCAN_BLOCK in
     * src/runtime/cli/args.c) leaves alone, because the bootstrap (src/runtime/bootstrap/bootstrap.c) reads them: the
     * pre-scan steps over the option only, so their value is read as an argument of its own. Every other long option
     * with a value takes it there.
     */
    private val PRESCAN_BOOTSTRAP_ONLY: Set<String> = setOf(
        "--config", "--validate-config", "--profile",
    )

    /** The I/Q replay option, separate or attached; args.c makes the replay the input once getopt has read "-i". */
    private const val IQ_REPLAY = "--iq-replay"

    /**
     * The local receiver a start's arguments name: "usb" for an RTL-SDR, "airspy", or empty for any other input
     * (rtl_tcp included), or for arguments the engine's parser refuses. Read as the engine reads them: the long-option
     * pass takes long options and their values out ([longOptionsRemoved]), then getopt reads "-i" among the short
     * options ([lastInput]), separate, attached or bundled, the last one winning, past bare arguments, and never as
     * another option's value; nothing after "--" is an option. The engine's own classifier is
     * dsd_opts_audio_in_dev_is_rtl_spec() and _is_airspy_spec() (include/dsd-neo/core/opts.h): the exact name, or the
     * name and a colon. A start that asks for an I/Q replay ([replayRequested]) takes no receiver, whatever its "-i":
     * the replay replaces the input after getopt, or the parser refuses a replay without a path. An INI file's input
     * (`--config`) is not read here.
     */
    fun sourceOfArgs(args: Array<String>): String {
        if (replayRequested(args)) return ""
        val input = lastInput(longOptionsRemoved(args))
        return when {
            input == null -> ""
            input == "rtl" || input.startsWith("rtl:") -> RTL
            input == "airspy" || input.startsWith("airspy:") -> AIRSPY
            else -> ""
        }
    }

    /**
     * Whether the long-option pre-scan of dsd_parse_args() reads [IQ_REPLAY] in [args]: before any "--", and not as the
     * value of another long option it reads. Unlike dsd_cli_compact_args(), the pre-scan takes the next argument of a
     * `k_skip_exact_next_nonopt` option whatever it starts with, and takes no value for [PRESCAN_BOOTSTRAP_ONLY].
     */
    private fun replayRequested(args: Array<String>): Boolean {
        var index = 0
        while (index < args.size) {
            val arg = args[index]
            if (arg == "--") return false
            if (arg == IQ_REPLAY || arg.startsWith("$IQ_REPLAY=")) return true
            val takesValue = arg !in PRESCAN_BOOTSTRAP_ONLY && (
                arg in LONG_NEXT_ANY || arg in LONG_NEXT_PRESENT || arg in LONG_FREE_VALUE ||
                    arg in LONG_NEXT_NON_OPTION
                )
            index += if (takesValue && index + 1 < args.size) 2 else 1
        }
        return false
    }

    /** What dsd_cli_compact_args() leaves for getopt: [args] without the long options it knows and their values. */
    private fun longOptionsRemoved(args: Array<String>): List<String> {
        val left = ArrayList<String>(args.size)
        var index = 0
        while (index < args.size) {
            val arg = args[index]
            if (arg == "--") {
                left.addAll(args.copyOfRange(index, args.size))
                break
            }
            val hasNext = index + 1 < args.size
            index += when {
                LONG_ATTACHED_PREFIXES.any { arg.startsWith(it) } -> 1
                arg in LONG_NO_VALUE -> 1
                arg in LONG_NEXT_NON_OPTION -> if (hasNext && !args[index + 1].startsWith("-")) 2 else 1
                arg in LONG_FREE_VALUE || arg in LONG_NEXT_PRESENT || arg in LONG_NEXT_ANY -> if (hasNext) 2 else 1
                else -> {
                    left += arg
                    1
                }
            }
        }
        return left
    }

    /**
     * The value of the last "-i" getopt reads from [args] with [SHORT_OPTIONS], or null when there is none, or when
     * getopt meets an option it does not know or one missing its value: the engine refuses that start before it opens
     * anything.
     */
    private fun lastInput(args: List<String>): String? {
        var input: String? = null
        var index = 0
        while (index < args.size) {
            val arg = args[index]
            index += 1
            if (arg == "--") break
            // A bare argument: getopt reads on past it.
            if (arg.length < 2 || arg[0] != '-') continue
            var at = 1
            while (at < arg.length) {
                val option = arg[at]
                at += 1
                val spec = if (option == ':') -1 else SHORT_OPTIONS.indexOf(option, startIndex = 1)
                if (spec < 0) return null
                if (SHORT_OPTIONS.getOrNull(spec + 1) != ':') continue
                // It takes a value: the rest of this argument, or else the next one, whatever it is.
                val value = if (at < arg.length) arg.substring(at) else args.getOrNull(index)?.also { index += 1 }
                if (value == null) return null
                if (option == 'i') input = value
                break
            }
        }
        return input
    }

    /** "airspy" for an Airspy, else "usb"; meaningful only for a device [isKnown] accepts. */
    fun sourceOf(vendorId: Int, productId: Int): String = if (id(vendorId, productId) == AIRSPY_ID) AIRSPY else RTL

    /**
     * The dongle label a session start line may carry for [requested]: the open connection's, and only when it is a
     * receiver of that kind. Before a first open, or while another kind is connected, there is none, rather than
     * the previous receiver's (issue #745).
     */
    fun labelFor(requested: String, connectedSource: String, label: String?): String? =
        label?.takeIf { connectedSource.isNotEmpty() && connectedSource == requested }

    /**
     * The kind of receiver attached, from the (vendor id, product id) of every device on the bus: "usb" or "airspy"
     * when only that kind is attached (however many of it), "both" when both are, empty when neither is. Devices that
     * are not receivers -- hubs, keyboards, storage -- are left out first, so they never count as an RTL-SDR.
     */
    fun attachedKind(devices: Iterable<Pair<Int, Int>>): String {
        var rtl = false
        var airspy = false
        for ((vendorId, productId) in devices) {
            if (!isKnown(vendorId, productId)) continue
            if (sourceOf(vendorId, productId) == AIRSPY) airspy = true else rtl = true
        }
        return when {
            rtl && airspy -> BOTH
            airspy -> AIRSPY
            rtl -> RTL
            else -> ""
        }
    }
}
