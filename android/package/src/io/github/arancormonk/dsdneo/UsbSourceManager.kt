// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.SystemClock
import android.util.Log
import org.json.JSONObject

/**
 * Owns the USB-OTG side of a locally attached RTL-SDR or Airspy.
 *
 * An app cannot open `/dev/bus/usb` nodes, so the descriptor has to come from
 * [UsbDeviceConnection]. This object obtains it — prompting for permission when
 * needed — and hands it to the engine through [DsdNative.nativeSetUsbFd]; librtlsdr
 * then wraps it instead of enumerating, which is why the engine also skips its own
 * device scan while a descriptor is set.
 *
 * The connection outlives a single decode run and is released on detach or a stopped-session source switch: Java
 * keeps ownership of the descriptor for as long as the engine might use it, and the
 * decoder service deliberately leaves it alone when it is destroyed, which happens
 * at the end of every session. Whatever survives that is closed by process death.
 */
object UsbSourceManager {
    private const val TAG = "dsd-neo"
    private const val ACTION_USB_PERMISSION = "io.github.arancormonk.dsdneo.action.USB_PERMISSION"
    /** Which permission request a reply answers; see [UsbRequestState]. */
    private const val EXTRA_GENERATION = "io.github.arancormonk.dsdneo.extra.USB_REQUEST_GENERATION"

    /** How long release() waits for the engine before it gives up and leaks the fd. */
    private const val RELEASE_TIMEOUT_MS = 10000L
    private const val RELEASE_POLL_MS = 50L

    private val lock = Any()
    private var connection: UsbDeviceConnection? = null
    private var attachedName: String? = null
    private var status: String = ""
    // WP-S2: application context only; never retain the Activity.
    private var attachmentContext: Context? = null
    // WP-D5: Android openDevice supplies no claim-owner information.
    private var failureKind: String = ""
    private var deviceLabel: String = "RTL-SDR"
    /** Product and VID:PID of the last device opened, for the diagnostics session line. */
    private var diagnosticsLabel: String? = null
    private var connectedSource: String = ""
    private var connectedSerial: String = ""
    /** The connected receiver's name, for "Ready: <device>". */
    private var connectedLabel: String = ""
    /** The access request and its permission prompt; guarded by [lock]. */
    private val requests = UsbRequestState { SystemClock.elapsedRealtime() }

    /** Set while [open] is between claiming the slot and installing a connection. */
    private var opening = false

    /**
     * Guards receiver registration only.
     *
     * Deliberately not [lock]: registerReceiver is a binder call, and holding the lock
     * that every setStatus takes across it would put the main thread behind it.
     */
    private val receiverLock = Any()
    private var receiverRegistered = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    val device = usbDeviceExtra(intent)
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    // Only the outstanding prompt's own answer counts: a reply to an older prompt, for another
                    // device, or replayed (below API 33 this receiver is exported) changes nothing (#647 B9).
                    val reply = synchronized(lock) {
                        requests.onReply(intent.getIntExtra(EXTRA_GENERATION, 0), device?.deviceName.orEmpty(), granted)
                    }
                    when (reply) {
                        UsbRequestState.Reply.IGNORED -> Log.i(TAG, "usb: ignoring a permission reply for another request")
                        // A matched reply names the requested device, so it is never null here.
                        UsbRequestState.Reply.GRANTED -> if (device != null) open(context, device)
                        // Declined USB permission does not prevent using rtl_tcp.
                        UsbRequestState.Reply.DENIED -> setStatus("USB permission denied", "permission")
                    }
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val device = usbDeviceExtra(intent)
                    val (attached, requestedGone) = synchronized(lock) {
                        var requested = false
                        if (device != null) {
                            UsbAttachmentTracker.detach(identity(device))
                            // The requested device going before it was opened retires its prompt, so Connect
                            // prompts again at once after a re-plug instead of waiting out the old one (B9).
                            val aimed = requests.device == device.deviceName && attachedName != device.deviceName
                            requests.onDetach(device.deviceName)
                            requested = aimed
                        }
                        UsbAttachmentTracker.reconcile(currentDevices(context))
                        attachedName to requested
                    }
                    // The filter sees every device on the bus, so an unresolvable
                    // EXTRA_DEVICE counts as ours only when there is an "ours" to lose.
                    // Without that, unplugging an unrelated accessory would stop an
                    // rtl_tcp, UDP or file session that never touched USB at all.
                    if (attached != null && (device == null || device.deviceName == attached)) {
                        // A connection outlives the sessions that used it, so holding one is not enough: the
                        // session must actually use the local receiver, or the engine have the descriptor wrapped,
                        // or a local-receiver session be mid-configure (#647 B8).
                        val ownsSession = DsdNative.nativeSessionUsesLocalReceiver() ||
                            DsdNative.nativeIsUsbFdInUse() ||
                            DecoderService.configuringLocalReceiver()
                        if (ownsSession) {
                            Log.i(TAG, "SDR detached; stopping the engine")
                            // The engine is mid-transfer on a descriptor that just went
                            // away: ask it to stop, and let release() close the connection
                            // once it has actually unwound. A normal end, so pending rdio uploads
                            // still drain (#718 D4).
                            EngineStop.request(StopCause.DEVICE_DETACHED, DsdNative::nativeStop)
                        } else {
                            Log.i(TAG, "SDR detached; no session is using it")
                        }
                        release()
                        setStatus("Device detached", "detached")
                    } else if (device != null && requestedGone) {
                        setStatus("Device detached: ${describe(device)}", "detached")
                    }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    reportAttachment(context, intent)
                }
            }
        }
    }

    /** Short human-readable attachment/permission state for the UI. */
    @JvmStatic
    fun statusText(): String = synchronized(lock) { status }

    // WP-S2: Activity intents and broadcasts share validation and dedupe.
    fun initializeAttachments(context: Context) {
        val app = context.applicationContext
        synchronized(lock) { attachmentContext = app }
        ensureReceiver(app)
    }

    fun reportAttachment(context: Context, intent: Intent?) {
        if (intent?.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) return
        val device = usbDeviceExtra(intent) ?: return
        if (!isKnown(device)) return
        synchronized(lock) {
            // Every validated edge is recorded for auto-start; the shown diagnostic is replaced only by the receiver
            // the request is waiting for, or by any receiver when nothing is asked and nothing connected (#647 B2).
            if (UsbAttachmentTracker.report(identity(device), currentDevices(context)) &&
                requests.attachReplacesDiagnostic(sourceOf(device), serialOf(device), connection != null)) {
                // Cold Activity startup may precede native library initialization.
                status = "${describe(device)} attached"
                failureKind = ""
            }
        }
    }

    /** WP-D5: publish diagnostic fields together for the UI's existing poll. */
    @JvmStatic
    fun deviceStatus(): String {
        // The host polls this every 250 ms, so a prompt whose answer never came is retired here, with no new request
        // and without prompting again: Connect comes back instead (#647 B9).
        if (synchronized(lock) { requests.expireIfStale() }) {
            setStatus("No answer to the permission request")
        }
        return synchronized(lock) {
            JSONObject().put("ready", connection != null).put("text", status)
                .put("kind", failureKind).put("name", deviceLabel).put("source", connectedSource).put("serial", connectedSerial)
                .put("pending", requests.pending).put("requestedSource", requests.requestedSource)
                .put("requestedSerial", requests.requestedSerial).toString()
        }
    }

    /**
     * The kind of receiver attached right now, for the UI's existing poll: "usb", "airspy", "both" or empty.
     *
     * Reads the bus, not the permission state: listing devices needs no permission, so this answers before any
     * prompt, and the UI can start on the receiver that is actually plugged in.
     */
    @JvmStatic
    fun attachedSource(): String {
        val context = synchronized(lock) { attachmentContext } ?: return ""
        val manager = context.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return ""
        return UsbReceiverKind.attachedKind(manager.deviceList.values.map { it.vendorId to it.productId })
    }

    /**
     * Retained attach edge for the UI's existing poll, independent of permission: "<kind>:<device name>" with kind
     * "usb" or "airspy", so auto-start can match the receiver that was plugged in (#647 D3), or empty.
     */
    @JvmStatic
    fun takeAttachment(): String = synchronized(lock) {
        val context = attachmentContext ?: return@synchronized ""
        UsbAttachmentTracker.take(currentDevices(context))
    }

    private fun identity(device: UsbDevice) =
        UsbAttachmentTracker.Identity(device.deviceName, device.vendorId, device.productId)

    private fun currentDevices(context: Context): Set<UsbAttachmentTracker.Identity> {
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        return manager.deviceList.values.map { identity(it) }.toSet()
    }

    /** Whether a descriptor has been handed to the engine. */
    @JvmStatic
    fun isReady(): Boolean = synchronized(lock) { connection != null }

    /**
     * Find an attached SDR and make it usable, prompting for permission if needed.
     *
     * Safe to call repeatedly; the outcome shows up in [statusText] and [isReady]
     * because the permission dialog answer arrives asynchronously.
     */
    @JvmStatic
    fun requestAccess(context: Context) {
        // The request already made comes first (its serial included), then the connected receiver, then the bus
        // (#647 B10): a plain Connect after an Airspy request never opens whichever RTL-SDR happens to be listed first.
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val bus = manager.deviceList.values.filter { isKnown(it) }.map { sourceOf(it) }
        val (source, serial) = synchronized(lock) { requests.accessTarget(if (connection != null) connectedSource else "", bus) }
        requestAccessForSource(context, source, serial)
    }

    @JvmStatic
    fun requestAccessForSource(context: Context, source: String, serial: String) {
        requestDevice(context, source, serial, hop = false)
    }

    /**
     * Body of [requestAccessForSource]. [hop] is the serial search moving on from a granted device that carries
     * another serial: the same request, so it keeps its generation.
     */
    private fun requestDevice(context: Context, source: String, serial: String, hop: Boolean) {
        if (source != "usb" && source != "airspy") return
        val running = DsdNative.nativeIsRunning()
        val switch = synchronized(lock) {
            UsbRequestState.switchFor(if (connection != null) connectedSource else "", connectedSerial, source, serial, running)
        }
        when (switch) {
            UsbRequestState.Switch.REFUSE -> {
                // The connection works; this is an answer to the request, not a device failure (#647 B11).
                setStatus("Stop the current receiver before switching USB devices", UsbRequestState.REFUSED)
                return
            }
            UsbRequestState.Switch.RELEASE -> release()
            UsbRequestState.Switch.KEEP -> {}
        }
        synchronized(lock) { requests.select(source, serial) }

        val appContext = context.applicationContext
        ensureReceiver(appContext)

        if (isReady()) {
            // Rewritten even though nothing changes: a failure written while ready (a refused switch, a stale
            // attach) must not outlive the request that finds the receiver ready (#647 B4).
            setStatus("Ready: ${synchronized(lock) { connectedLabel }}")
            return
        }

        val manager = appContext.getSystemService(Context.USB_SERVICE) as UsbManager
        val candidates = manager.deviceList.values.filter { isKnown(it) && sourceOf(it) == source }
        val device = if (serial.isEmpty()) candidates.firstOrNull() else
            candidates.firstOrNull { manager.hasPermission(it) && matchesSerial(it, serial) } ?:
            candidates.firstOrNull { !manager.hasPermission(it) }
        if (device == null) {
            setStatus(if (serial.isNotEmpty()) "Requested Airspy serial not found" else if (source == "airspy") "No Airspy attached" else "No RTL-SDR attached", "detached")
            return
        }

        if (manager.hasPermission(device)) {
            synchronized(lock) { requests.aim(device.deviceName) }
            open(appContext, device)
            return
        }

        // The permission broadcast can go missing — the dialog is dismissed by a
        // configuration change, or the process is backgrounded while it is up. A prompt
        // still inside its wait is not repeated; one past it has already been retired by
        // the deviceStatus() poll, so this Connect asks afresh.
        val generation = synchronized(lock) {
            requests.expireIfStale()
            when {
                hop -> requests.hop(device.deviceName)
                requests.pending -> return
                else -> requests.begin(source, serial, device.deviceName)
            }
        }
        setStatus("Requesting permission for ${describe(device)}")
        // One PendingIntent identity per request: the generation is both the request code and the extra the reply
        // carries back. FLAG_CANCEL_CURRENT retires a leftover with the same code (from an earlier process) rather
        // than reusing it; FLAG_MUTABLE lets the framework add the device and the answer.
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(appContext.packageName).putExtra(EXTRA_GENERATION, generation)
        val flags = PendingIntent.FLAG_CANCEL_CURRENT or PendingIntent.FLAG_MUTABLE
        manager.requestPermission(device, PendingIntent.getBroadcast(appContext, generation, intent, flags))
    }

    /**
     * Drop the descriptor and close the connection.
     *
     * Clearing the native slot only affects the *next* open: a device the engine
     * already has wrapped in libusb keeps working, and closing the connection
     * underneath it would be a use-after-close. The close therefore happens on a
     * background thread once the engine has actually stopped — callers (a detach
     * broadcast, service teardown) must not block waiting for that, and the engine
     * may take a moment to unwind.
     *
     * Note that discovery does not come back: `rtlsdr_open_fd()` sets libusb's
     * process-global LIBUSB_OPTION_NO_DEVICE_DISCOVERY, which cannot be undone. That
     * costs nothing here, because an app cannot enumerate `/dev/bus/usb` anyway.
     */
    @JvmStatic
    fun release() {
        val open = synchronized(lock) {
            val current = connection
            connection = null
            attachedName = null
            connectedSource = ""
            connectedSerial = ""
            connectedLabel = ""
            current
        }
        if (open == null) {
            return
        }
        DsdNative.nativeSetUsbFd(-1)
        DsdNative.nativeSetAirspyUsbFd(-1)
        Thread({ closeWhenEngineStops(open) }, "dsd-neo-usb-release").start()
    }

    /**
     * Waits for the engine to let go of the descriptor, then closes the connection.
     *
     * Both conditions matter. [DsdNative.nativeIsUsbFdInUse] is the precise one — it
     * brackets exactly the period during which libusb has the descriptor wrapped —
     * while [DsdNative.nativeIsRunning] additionally covers a run that is about to
     * take it but has not reached input setup yet. Waiting on "running" alone would
     * be both too coarse and, in the moment between a configured start and the engine
     * thread being scheduled, too narrow.
     *
     * If it never lets go the connection is deliberately leaked: one file descriptor
     * held for the rest of the process's life is a far better outcome than pulling it
     * out from under an in-flight USB transfer.
     */
    private fun closeWhenEngineStops(open: UsbDeviceConnection) {
        var waited = 0L
        while (DsdNative.nativeIsRunning() || DsdNative.nativeIsUsbFdInUse()) {
            if (waited >= RELEASE_TIMEOUT_MS) {
                Log.e(TAG, "engine still running after ${RELEASE_TIMEOUT_MS}ms; leaking the USB connection")
                return
            }
            try {
                Thread.sleep(RELEASE_POLL_MS)
            } catch (e: InterruptedException) {
                Log.w(TAG, "interrupted waiting for the engine to release the descriptor", e)
                Thread.currentThread().interrupt()
                return
            }
            waited += RELEASE_POLL_MS
        }
        open.close()
        Log.i(TAG, "usb: connection closed")
    }

    private fun open(context: Context, device: UsbDevice) {
        if (!isKnown(device)) return
        val wanted = synchronized(lock) { sourceOf(device) == requests.requestedSource && device.deviceName == requests.device }
        if (!wanted) return

        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val physical = manager.deviceList[device.deviceName] ?: return
        if (physical.vendorId != device.vendorId || physical.productId != device.productId) return
        // Below API 33 a dynamically registered receiver is implicitly exported, so a
        // broadcast claiming permission was granted is not by itself trustworthy. Ask
        // the framework rather than believing the extra.
        if (!manager.hasPermission(device)) {
            setStatus("No permission for ${describe(device)}", "permission")
            return
        }

        // Claim the slot before touching the framework. This path runs again for a
        // device we already hold — the re-prompt in requestAccess racing a late first
        // dialog result, or, below API 33, a replayed ACTION_USB_PERMISSION, which is
        // not a protected broadcast. Opening a second connection there leaks the first
        // descriptor for the life of the process and hands the engine a new fd while
        // libusb is still transferring on the old one; repeat it and the process runs
        // out of descriptors. hasPermission above cannot catch this: permission
        // genuinely is held.
        val serial = synchronized(lock) { requests.requestedSerial }
        if (serial.isNotEmpty() && !matchesSerial(device, serial)) {
            requestDevice(context, sourceOf(device), serial, hop = true)
            return
        }
        synchronized(lock) {
            if (connection != null || opening) {
                Log.i(TAG, "usb: already holding a descriptor; ignoring a duplicate open")
                return
            }
            opening = true
        }
        try {
            openClaimed(manager, device)
        } finally {
            synchronized(lock) { opening = false }
        }
    }

    /** Product and VID:PID of the connected receiver when it is of kind [source] ("usb" or "airspy"), else null. */
    @JvmStatic
    fun diagnosticsLabel(source: String): String? =
        synchronized(lock) { UsbReceiverKind.labelFor(source, if (connection != null) connectedSource else "", diagnosticsLabel) }

    /** Body of [open], with the open slot claimed so nothing else can install one. */
    private fun openClaimed(manager: UsbManager, device: UsbDevice) {
        val label = String.format("%s %04x:%04x", describe(device), device.vendorId, device.productId)
        synchronized(lock) {
            deviceLabel = describe(device)
            diagnosticsLabel = label
        }
        // The open is a step into the kernel's USB stack: re-arm the durable start window here too, since a
        // permission dialog can outlast the one the start opened (issue #745).
        DsdNative.nativeArmStartupDiagnostics()
        DsdNative.nativeHostDiagnostic("usb: opening $label")
        val opened = try {
            manager.openDevice(device)
        } catch (e: SecurityException) {
            setStatus("No permission for ${describe(device)}", "permission")
            return
        }
        if (opened == null) {
            // Recheck after open: detach or permission revocation can race it.
            when {
                !manager.deviceList.containsKey(device.deviceName) ->
                    setStatus("Device detached: ${describe(device)}", "detached")
                !manager.hasPermission(device) ->
                    setStatus("No permission for ${describe(device)}", "permission")
                else -> setStatus("Android could not open ${describe(device)} (open_failed).", "open_failed")
            }
            return
        }

        val fd = opened.fileDescriptor
        if (fd < 0) {
            opened.close()
            setStatus("No descriptor for ${describe(device)} (open_failed)", "open_failed")
            return
        }

        if (!manager.deviceList.containsKey(device.deviceName)) { opened.close(); return }
        val serial = serialOf(device)
        val rc = synchronized(lock) {
            if (sourceOf(device) != requests.requestedSource || device.deviceName != requests.device ||
                !requests.satisfiedBy(sourceOf(device), serial)) {
                DsdNative.STATUS_ERROR
            } else {
                val result = if (sourceOf(device) == "airspy") DsdNative.nativeSetAirspyUsbFd(fd) else DsdNative.nativeSetUsbFd(fd)
                if (result == DsdNative.STATUS_OK) {
                    connection = opened
                    attachedName = device.deviceName
                    connectedSource = sourceOf(device)
                    connectedSerial = serial
                    connectedLabel = describe(device)
                }
                result
            }
        }
        if (rc != DsdNative.STATUS_OK) {
            opened.close()
            setStatus("Engine rejected the descriptor ($rc)", "open_failed")
            return
        }
        setStatus("Ready: ${describe(device)}")
    }

    private fun ensureReceiver(context: Context) {
        synchronized(receiverLock) {
            if (receiverRegistered) {
                return
            }
            val filter = IntentFilter().apply {
                addAction(ACTION_USB_PERMISSION)
                addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            }
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
                } else {
                    context.registerReceiver(receiver, filter)
                }
            } catch (e: RuntimeException) {
                // Marking it registered anyway would be permanent: every later
                // requestAccess would skip registration, so no permission result and no
                // detach would ever be delivered again and the UI would sit on
                // "Requesting permission…" for the rest of the process's life.
                Log.e(TAG, "could not register the USB receiver; will retry", e)
                return
            }
            receiverRegistered = true
        }
    }

    private fun serialOf(device: UsbDevice): String = try {
        device.serialNumber?.takeLast(16) ?: ""
    } catch (_: SecurityException) { "" }

    private fun matchesSerial(device: UsbDevice, serial: String): Boolean =
        serialOf(device).equals(serial, ignoreCase = true)

    private fun sourceOf(device: UsbDevice): String = UsbReceiverKind.sourceOf(device.vendorId, device.productId)

    // See UsbReceiverKind.isKnown: id-only, so hubs and other accessories never count.
    private fun isKnown(device: UsbDevice): Boolean = UsbReceiverKind.isKnown(device.vendorId, device.productId)

    private fun describe(device: UsbDevice): String =
        device.productName ?: String.format("%04x:%04x", device.vendorId, device.productId)

    private fun setStatus(text: String, kind: String = "") {
        synchronized(lock) {
            status = text
            failureKind = kind
        }
        Log.i(TAG, "usb: $text")
        DsdNative.nativeHostDiagnostic("usb: $text")
    }

    @Suppress("DEPRECATION")
    private fun usbDeviceExtra(intent: Intent): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
}
