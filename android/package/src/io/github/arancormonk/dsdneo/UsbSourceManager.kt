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
import android.os.Handler
import android.os.Looper
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
 * The connection is released on detach or a stopped-session source switch: Java
 * keeps ownership of the descriptor for as long as the engine might use it, and the
 * decoder service deliberately leaves it alone when it is destroyed, which happens
 * at the end of every session. Whatever survives that is closed by process death.
 * Each session gets a connection of its own (#782): the first takes the one opened
 * for it, and every later one reopens the device ([takeForSession]), so a descriptor
 * a session left unusable is never handed to the next.
 *
 * One serialized owner (#782): every change to this state runs on the main thread,
 * where the USB broadcasts and the service's start arrive; the Qt thread's access
 * requests are posted there ([onMain]). [lock] only makes the state readable from
 * other threads, and is never held across a binder call: the bus listing and serial
 * reads come before the change they feed, and a listing older than an attach or
 * detach applied since is discarded ([events]).
 */
object UsbSourceManager {
    private const val TAG = "dsd-neo"
    private const val ACTION_USB_PERMISSION = "io.github.arancormonk.dsdneo.action.USB_PERMISSION"
    /** Which permission request a reply answers; see [UsbRequestState]. */
    private const val EXTRA_GENERATION = "io.github.arancormonk.dsdneo.extra.USB_REQUEST_GENERATION"

    /** How long release() waits for the engine before it gives up and leaks the fd. */
    private const val RELEASE_TIMEOUT_MS = 10000L
    private const val RELEASE_POLL_MS = 50L

    private const val NO_ANSWER = "No answer to the permission request"

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

    /** Attach and detach events applied so far; a bus listing taken before the newest one is stale (#782). */
    private var events = 0L
    /** Validated attach edges seen so far, and each attached device's at its edge: the attach epoch (#782). */
    private var attaches = 0L
    private val attachEpochs = HashMap<String, Long>()
    /** Connections installed so far; the current one's number, its attach epoch and how many sessions took it. */
    private var connections = 0L
    private var connectionId = 0L
    private var connectedEpoch = 0L
    private var sessionsTaken = 0

    private val mainHandler: Handler by lazy { Handler(Looper.getMainLooper()) }

    /** Runs [work] on the main thread, the broker's one owner: at once when already there, else posted. */
    private fun onMain(work: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            work()
        } else {
            mainHandler.post { work() }
        }
    }

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
                    // Listed before the state change, never under the lock: the listing is a binder call (#782).
                    val current = currentDevices(context)
                    val (attached, requestedGone) = synchronized(lock) {
                        // The requested device going before it was opened retires its prompt, so Connect prompts
                        // again at once after a re-plug instead of waiting out the old one (B9).
                        val unopened = device != null && requests.device == device.deviceName && attachedName != device.deviceName
                        if (device != null) {
                            UsbAttachmentTracker.detach(identity(device))
                            requests.onDetach(device.deviceName)
                            attachEpochs.remove(device.deviceName)
                        }
                        UsbAttachmentTracker.reconcile(current)
                        events += 1
                        attachedName to unopened
                    }
                    // The filter sees every device on the bus, so an unresolvable
                    // EXTRA_DEVICE counts as ours only when there is an "ours" to lose.
                    // Without that, unplugging an unrelated accessory would stop an
                    // rtl_tcp, UDP or file session that never touched USB at all.
                    if (attached != null && (device == null || device.deviceName == attached)) {
                        // A connection outlives the sessions that used it, so holding one is not enough: the
                        // session must actually use the local receiver, or the engine have the descriptor wrapped
                        // (#647 B8). A session being configured is not asked about: configure runs on this main
                        // thread too, so the detach lands after it, once the native record names the receiver.
                        val ownsSession = DsdNative.nativeSessionUsesLocalReceiver() ||
                            DsdNative.nativeIsUsbFdInUse()
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

    /** An attach edge, from the broadcast or the Activity's launch intent; main thread. */
    fun reportAttachment(context: Context, intent: Intent?) {
        if (intent?.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) return
        val device = usbDeviceExtra(intent) ?: return
        if (!isKnown(device)) return
        // Both binder calls come before the state change, never under the lock (#782): a slow serial read must not
        // hold up the host's poll or another broadcast.
        val current = currentDevices(context)
        val serial = serialOf(device)
        synchronized(lock) {
            events += 1
            // Every validated edge is recorded for auto-start; the shown diagnostic is replaced only by the receiver
            // the request is waiting for, or by any receiver when nothing is asked and nothing connected (#647 B2).
            if (UsbAttachmentTracker.report(identity(device), current)) {
                attaches += 1
                attachEpochs[device.deviceName] = attaches
                if (requests.attachReplacesDiagnostic(sourceOf(device), serial, connection != null)) {
                    // Cold Activity startup may precede native library initialization.
                    status = "${describe(device)} attached"
                    failureKind = ""
                }
            }
        }
    }

    /** WP-D5: publish diagnostic fields together for the UI's existing poll. */
    @JvmStatic
    fun deviceStatus(): String {
        // The host polls this every 250 ms, so a prompt whose answer never came is retired here, with no new request
        // and without prompting again: Connect comes back instead (#647 B9). Retired and written in one locked step,
        // so a detach's diagnostic cannot land between the two and be overwritten; logged after.
        var expired = false
        val record = synchronized(lock) {
            if (requests.expireIfStale()) {
                expired = true
                status = NO_ANSWER
                failureKind = ""
            }
            JSONObject().put("ready", connection != null).put("text", status)
                .put("kind", failureKind).put("name", deviceLabel).put("source", connectedSource).put("serial", connectedSerial)
                .put("pending", requests.pending).put("requestedSource", requests.requestedSource)
                .put("requestedSerial", requests.requestedSerial)
                // #782: which connection is ready, so the host shows a session's receiver error only for its own.
                .put("device", attachedName.orEmpty()).put("epoch", if (connection != null) connectedEpoch else 0L)
                .put("connection", if (connection != null) connectionId else 0L).toString()
        }
        if (expired) logStatus(NO_ANSWER)
        return record
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
    fun takeAttachment(): String {
        // The Qt thread's poll: the bus is listed outside the lock, and a listing older than an attach or detach
        // applied meanwhile is dropped rather than applied, so it can never clear a newer pending edge (#782). The
        // next poll, 250 ms later, lists again.
        val (context, seen) = synchronized(lock) { attachmentContext to events }
        if (context == null) return ""
        val current = currentDevices(context)
        return synchronized(lock) { if (events != seen) "" else UsbAttachmentTracker.take(current) }
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
        // From the Qt thread: run by the broker's owner, so it cannot change the connection in the middle of a
        // session's swap or a broadcast (#782).
        onMain {
            // The request already made comes first (its serial included), then the connected receiver, then the bus
            // (#647 B10): a plain Connect after an Airspy request never opens whichever RTL-SDR happens to be listed first.
            val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
            val bus = manager.deviceList.values.filter { isKnown(it) }.map { sourceOf(it) }
            val (source, serial) = synchronized(lock) { requests.accessTarget(if (connection != null) connectedSource else "", bus) }
            requestDevice(context, source, serial, hop = false)
        }
    }

    @JvmStatic
    fun requestAccessForSource(context: Context, source: String, serial: String) {
        onMain { requestDevice(context, source, serial, hop = false) }
    }

    /**
     * Body of [requestAccessForSource]. [hop] is the serial search moving on from a granted device that carries
     * another serial: the same request, so it keeps its generation.
     */
    private fun requestDevice(context: Context, source: String, serial: String, hop: Boolean) {
        if (source != "usb" && source != "airspy") return
        // A configured session owns its receiver before it runs (#782), as the detach and the swap ask too.
        val owned = DsdNative.nativeIsRunning() || DsdNative.nativeSessionUsesLocalReceiver() ||
            DsdNative.nativeIsUsbFdInUse()
        val switch = synchronized(lock) {
            UsbRequestState.switchFor(if (connection != null) connectedSource else "", connectedSerial, source, serial, owned)
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
                // A Connect may already have prompted for this device between the granted reply and here.
                hop && requests.pending && requests.device == device.deviceName -> return
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
        val open = detachConnection() ?: return
        val fd = open.fileDescriptor
        Thread({ closeWhenReleased(open, fd) }, "dsd-neo-usb-release").start()
    }

    /**
     * Forgets the connection and clears both native slots; returns it, or null when there was none. The slots are
     * cleared before anything closes it: once they are, the engine either holds the descriptor (and the held query
     * says so) or can no longer take it.
     */
    private fun detachConnection(): UsbDeviceConnection? {
        val open = synchronized(lock) {
            val current = connection
            connection = null
            attachedName = null
            connectedSource = ""
            connectedSerial = ""
            connectedLabel = ""
            connectionId = 0L
            connectedEpoch = 0L
            sessionsTaken = 0
            current
        } ?: return null
        DsdNative.nativeSetUsbFd(-1)
        DsdNative.nativeSetAirspyUsbFd(-1)
        return open
    }

    /**
     * Waits for the engine to let go of [fd], the released connection's own descriptor, then closes the connection.
     *
     * Bound to that descriptor (#782): [DsdNative.nativeHeldUsbFds] names the descriptor libusb has wrapped, from
     * just before the wrap until the device is closed, and the slot was cleared first, so a session that has not
     * taken it yet never will. A later session on a replacement connection holds another descriptor and keeps
     * nothing here waiting; global "a session uses the receiver" flags would, and would leak the connection after
     * the timeout. Nor is it asked whether the engine runs: an rtl_tcp, UDP or file session never takes the
     * descriptor (#647).
     *
     * If it never lets go the connection is deliberately leaked: one file descriptor
     * held for the rest of the process's life is a far better outcome than pulling it
     * out from under an in-flight USB transfer.
     */
    private fun closeWhenReleased(open: UsbDeviceConnection, fd: Int) {
        var waited = 0L
        while (isHeld(fd)) {
            if (waited >= RELEASE_TIMEOUT_MS) {
                Log.e(TAG, "engine still using the receiver after ${RELEASE_TIMEOUT_MS}ms; leaking the USB connection")
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

    /** Whether the engine holds [fd]; an unreadable answer counts as held, so nothing closes under a transfer. */
    private fun isHeld(fd: Int): Boolean {
        val held = DsdNative.nativeHeldUsbFds() ?: return true
        return held.any { it == fd }
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
                val result = publish(sourceOf(device), fd)
                if (result == DsdNative.STATUS_OK) {
                    connection = opened
                    attachedName = device.deviceName
                    connectedSource = sourceOf(device)
                    connectedSerial = serial
                    connectedLabel = describe(device)
                    connections += 1
                    connectionId = connections
                    connectedEpoch = attachEpochs[device.deviceName] ?: 0L
                    sessionsTaken = 0
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

    /**
     * Hands the engine [fd] for a receiver of kind [source]. [resetFirst] asks the next RTL-SDR open to reset the
     * device before it opens it, for an attachment that owes a reset; false unless the caller knows it does.
     */
    private fun publish(source: String, fd: Int, resetFirst: Boolean = false): Int =
        if (source == UsbReceiverKind.AIRSPY) {
            DsdNative.nativeSetAirspyUsbFd(fd)
        } else {
            DsdNative.nativeSetUsbResetFirst(resetFirst)
            DsdNative.nativeSetUsbFd(fd)
        }

    /** The connection a start found, read in one locked step. */
    private class Held(
        val name: String,
        val serial: String,
        val label: String,
        val epoch: Long,
        val id: Long,
        val sessions: Int,
    )

    /**
     * A start's take of its local receiver [source] ("usb" or "airspy"), on the main thread between the service's
     * IDLE->STARTING and nativeConfigure() (#782). Nothing else can change the broker meanwhile: the broadcasts and
     * the Qt thread's requests run on this thread too.
     *
     * - No connection of that kind: nothing to take, and the start goes on as before.
     * - The previous session still owns a receiver (configured, or the descriptor still wrapped): refused, as
     *   "previous session still stopping".
     * - A connection no session took yet: this session takes it.
     * - One a session already took: one transaction closes it and opens the same UsbDevice again (no prompt while
     *   permission holds), then publishes the new descriptor. A descriptor the last session left unusable is never
     *   reused (the reporter's "Failed to open rtlsdr device from descriptor"). If the device is gone, permission was
     *   revoked or the open fails, the start fails before configure with that kind, and the broker reads not ready:
     *   Connect or Retry takes the normal request path.
     */
    internal fun takeForSession(context: Context, source: String): UsbSessionTake {
        if (source != UsbReceiverKind.RTL && source != UsbReceiverKind.AIRSPY) return UsbSessionTake()
        val held = synchronized(lock) {
            if (connection == null || connectedSource != source) {
                null
            } else {
                Held(attachedName.orEmpty(), connectedSerial, connectedLabel, connectedEpoch, connectionId, sessionsTaken)
            }
        } ?: return UsbSessionTake()
        if (DsdNative.nativeSessionUsesLocalReceiver() || DsdNative.nativeIsUsbFdInUse()) {
            return UsbSessionTake.failed(
                UsbSessionTake.PREVIOUS_STOPPING,
                "usb: ${held.label} is still held by the previous session; not reopened"
            )
        }
        if (held.sessions == 0) {
            synchronized(lock) { sessionsTaken = 1 }
            DsdNative.nativeHostDiagnostic("usb: the session takes ${held.label} (connection ${held.id})")
            return UsbSessionTake(device = held.name, epoch = held.epoch, connection = held.id)
        }
        return reopen((synchronized(lock) { attachmentContext } ?: context.applicationContext), source, held)
    }

    /** Body of [takeForSession] for a connection a session already took: close it, open the device again, publish. */
    private fun reopen(context: Context, source: String, held: Held): UsbSessionTake {
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        // Binder calls, all before the state changes below and none under the lock.
        val listed = manager.deviceList[held.name]?.takeIf { isKnown(it) && sourceOf(it) == source }
        val permitted = listed != null && manager.hasPermission(listed)
        // The serial, readable only with permission, tells a device that took over the name since from this one.
        val device = listed?.takeIf { !permitted || held.serial.isEmpty() || matchesSerial(it, held.serial) }
        // The old connection goes first whatever comes next: the engine is idle (checked above), so nothing holds it.
        detachConnection()?.close()
        if (device == null) {
            setStatus("Device detached: ${held.label}", "detached")
            return UsbSessionTake.failed(UsbSessionTake.USB_GONE, "usb: ${held.label} is no longer attached; not reopened")
        }
        if (!permitted) {
            setStatus("No permission for ${describe(device)}", "permission")
            return UsbSessionTake.failed(UsbSessionTake.USB_PERMISSION, "usb: no permission to reopen ${held.label}")
        }
        val label = String.format("%s %04x:%04x", describe(device), device.vendorId, device.productId)
        DsdNative.nativeArmStartupDiagnostics()
        DsdNative.nativeHostDiagnostic("usb: reopening $label for the new session")
        val opened = try {
            manager.openDevice(device)
        } catch (_: SecurityException) {
            null
        }
        if (opened == null) {
            // As openClaimed() rechecks: a detach or a revocation can race the open.
            return when {
                !manager.deviceList.containsKey(device.deviceName) -> {
                    setStatus("Device detached: ${describe(device)}", "detached")
                    UsbSessionTake.failed(UsbSessionTake.USB_GONE, "usb: $label went away while it was reopened")
                }
                !manager.hasPermission(device) -> {
                    setStatus("No permission for ${describe(device)}", "permission")
                    UsbSessionTake.failed(UsbSessionTake.USB_PERMISSION, "usb: no permission to reopen $label")
                }
                else -> {
                    setStatus("Android could not open ${describe(device)} (open_failed).", "open_failed")
                    UsbSessionTake.failed(UsbSessionTake.USB_OPEN_FAILED, "usb: Android could not reopen $label")
                }
            }
        }
        val fd = opened.fileDescriptor
        val installed = fd >= 0 && synchronized(lock) {
            val rc = publish(source, fd)
            if (rc == DsdNative.STATUS_OK) {
                connection = opened
                attachedName = device.deviceName
                connectedSource = source
                connectedSerial = held.serial
                connectedLabel = held.label
                connections += 1
                connectionId = connections
                connectedEpoch = held.epoch
                sessionsTaken = 1
            }
            rc == DsdNative.STATUS_OK
        }
        if (!installed) {
            opened.close()
            setStatus("No descriptor for ${describe(device)} (open_failed)", "open_failed")
            return UsbSessionTake.failed(UsbSessionTake.USB_OPEN_FAILED, "usb: no usable descriptor for $label")
        }
        val id = synchronized(lock) { connectionId }
        DsdNative.nativeHostDiagnostic("usb: the session takes $label (connection $id, reopened)")
        return UsbSessionTake(device = device.deviceName, epoch = held.epoch, connection = id)
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
        logStatus(text)
    }

    private fun logStatus(text: String) {
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
