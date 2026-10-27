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
     * The local receiver a start's arguments name: "usb" for an RTL-SDR, "airspy", or empty for any other input
     * (rtl_tcp included). Read as the engine's option parser reads "-i": the last one wins, separate or attached, and
     * nothing after "--" is an option. The engine's own classifier is dsd_opts_audio_in_dev_is_rtl_spec() and
     * _is_airspy_spec() (include/dsd-neo/core/opts.h): the exact name, or the name and a colon.
     */
    fun sourceOfArgs(args: Array<String>): String {
        var input: String? = null
        var index = 0
        while (index < args.size) {
            val arg = args[index]
            when {
                arg == "--" -> break
                arg == "-i" -> {
                    input = args.getOrNull(index + 1)
                    index += 1
                }
                arg.startsWith("-i") -> input = arg.substring(2)
            }
            index += 1
        }
        return when {
            input == null -> ""
            input == "rtl" || input.startsWith("rtl:") -> RTL
            input == "airspy" || input.startsWith("airspy:") -> AIRSPY
            else -> ""
        }
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
