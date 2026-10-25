// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * What the USB broker has been asked for, and the permission prompt it is waiting on, without Android.
 *
 * [UsbSourceManager] drives this from its access requests and broadcasts under its own lock. Each prompt belongs to a
 * generation: its PendingIntent uses the generation as the request code and carries it back, so a reply to an older
 * prompt, to another device, or one replayed after the answer is recognised and ignored (#647 B9). The target
 * ([requestedSource], [requestedSerial]) stays empty until the first request, so "nothing asked yet" reads apart from
 * "asked for an RTL-SDR" (B10).
 */
internal class UsbRequestState(private val now: () -> Long) {
    enum class Reply { GRANTED, DENIED, IGNORED }

    /** What an access request does about a connection to another receiver. */
    enum class Switch { KEEP, RELEASE, REFUSE }

    companion object {
        /** How long a prompt may go unanswered before it is retired; the broadcast can go missing. */
        const val TIMEOUT_MS = 60000L

        /** Failure kind of the mid-session switch refusal: feedback on the request, not a device failure (B11). */
        const val REFUSED = "refused"

        /**
         * A request for [source] / [serial] while [connectedSource] / [connectedSerial] is connected: keep a connection
         * that already satisfies it, release one that does not, or refuse while a session ([running]) may be using it.
         */
        fun switchFor(connectedSource: String, connectedSerial: String, source: String, serial: String, running: Boolean): Switch {
            if (connectedSource.isEmpty()) return Switch.KEEP
            val same = connectedSource == source && (serial.isEmpty() || connectedSerial.equals(serial, ignoreCase = true))
            return when {
                same -> Switch.KEEP
                running -> Switch.REFUSE
                else -> Switch.RELEASE
            }
        }
    }

    /** Whether a permission prompt is outstanding. */
    var pending = false
        private set
    var requestedSource = ""
        private set
    var requestedSerial = ""
        private set

    /** The device the request is aimed at, prompted for or not; null before one is found and once it is unplugged. */
    var device: String? = null
        private set

    /** The newest prompt's generation; 0 before the first. */
    var generation = 0
        private set
    private var promptedAtMs = 0L

    /** Aims the request at [source] / [serial]; a different target retires the prompt for the old one. */
    fun select(source: String, serial: String) {
        if (source != requestedSource || serial != requestedSerial) {
            pending = false
            device = null
        }
        requestedSource = source
        requestedSerial = serial
    }

    /** Aims the request at a device that is opened without a prompt, which leaves none outstanding. */
    fun aim(device: String) {
        this.device = device
        pending = false
    }

    /** Prompts for [device] as a new request; returns its generation. */
    fun begin(source: String, serial: String, device: String): Int {
        select(source, serial)
        generation += 1
        prompt(device)
        return generation
    }

    /**
     * Prompts for the next device of the same request, when the one just granted carries another serial. The
     * generation is kept: it is still the one request, and the device tells its replies apart.
     */
    fun hop(device: String): Int {
        if (generation == 0) return begin(requestedSource, requestedSerial, device)
        prompt(device)
        return generation
    }

    private fun prompt(device: String) {
        this.device = device
        pending = true
        promptedAtMs = now()
    }

    /** The answer to prompt [generation] for [device]; anything but the outstanding prompt's own answer is ignored. */
    fun onReply(generation: Int, device: String, granted: Boolean): Reply {
        if (!pending || generation != this.generation || device != this.device) return Reply.IGNORED
        pending = false
        return if (granted) Reply.GRANTED else Reply.DENIED
    }

    /** [device] was unplugged; true when that retired the outstanding prompt, so Connect works again at once. */
    fun onDetach(device: String): Boolean {
        if (device != this.device) return false
        this.device = null
        val retired = pending
        pending = false
        return retired
    }

    /** Retires a prompt unanswered for [TIMEOUT_MS]; true once, when it does. Never prompts again by itself. */
    fun expireIfStale(): Boolean {
        if (!pending || now() - promptedAtMs < TIMEOUT_MS) return false
        pending = false
        return true
    }

    /** Whether a receiver of [source] with [serial] ("" when unreadable) is what the request asks for. */
    fun satisfiedBy(source: String, serial: String): Boolean =
        requestedSource.isNotEmpty() && source == requestedSource &&
            (requestedSerial.isEmpty() || serial.equals(requestedSerial, ignoreCase = true))

    /**
     * Whether an attach of [source] / [serial] may replace the shown diagnostic: never over a live connection's text
     * ([connected]), and otherwise only for the receiver the request asks for, or any receiver when nothing is asked.
     * An RTL-SDR plugged in therefore never hides a missing-Airspy diagnostic (B2).
     */
    fun attachReplacesDiagnostic(source: String, serial: String, connected: Boolean): Boolean =
        !connected && (requestedSource.isEmpty() || satisfiedBy(source, serial))

    /**
     * Source and serial a plain Connect asks for: the request's own target first, then the connected receiver's kind,
     * then the first receiver on the bus ([bus]: each known device's source, in bus order), else an RTL-SDR.
     */
    fun accessTarget(connectedSource: String, bus: List<String>): Pair<String, String> = when {
        requestedSource.isNotEmpty() -> requestedSource to requestedSerial
        connectedSource.isNotEmpty() -> connectedSource to ""
        else -> (bus.firstOrNull() ?: UsbReceiverKind.RTL) to ""
    }
}
