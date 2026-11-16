// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.Manifest
import android.content.Context
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.location.LocationRequest
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import org.json.JSONObject

/**
 * GPS roaming's continuous location feed (#786), apart from the one-shot [LocationSupport] requests, which it never
 * touches. Qt starts and stops it over JNI and polls its record on its tick; the Activity stops it itself on `onStop`,
 * because Qt's thread is frozen while the app is not visible, and it only registers while the app is. It asks for
 * precise location only: an approximate fix is too coarse and too slow to tell sites apart, so with approximate access
 * alone it registers nothing and says so.
 *
 * The foreground contract: Qt asks for the feed again after every foreground epoch it sees move (each `onStop` moves
 * it), and a start asked for in the current epoch always ends up registered once the app is visible. A start that
 * reaches the main looper while the app is hidden (Qt's tick can still run between `onStop` and its thread's freeze)
 * is kept and run on the next `onStart`; a stop, or a newer start, replaces it. A start asked for in an older epoch
 * never runs: Qt asks again for the new one.
 *
 * All registration state belongs to the main looper. Each start that registers makes a new subscription with its own
 * `generation`, and a callback from an older one is ignored. A callback does a fixed amount of work: it keeps the
 * newest usable fix of its provider and publishes an immutable [Snapshot] under [lock]. Choosing between providers
 * waits for the poll, because freshness depends on when the poll runs.
 */
object LocationStream {
    internal const val INTERVAL_MS = 10_000L
    // A fix this old still outranks a newer, less accurate one from another provider.
    internal const val FRESH_NS = 15_000_000_000L
    internal const val OK = "ok"
    internal const val NO_PERMISSION = "noPermission"
    internal const val APPROXIMATE_ONLY = "approximateOnly"
    internal const val LOCATION_OFF = "locationOff"
    internal const val PROVIDER_UNAVAILABLE = "providerUnavailable"

    private val main = Handler(Looper.getMainLooper())
    private val lock = Any()

    // Under [lock]: every start and stop takes the next serial, so only the newest call's work publishes; [record] is
    // what the poll reads, null while the stream is not wanted.
    private var serial = 0L
    private var record: Snapshot? = null

    // The main looper's own; [deferred] is the newest start that arrived while the app was hidden, for `onStart`.
    private var visible = false
    private var deferred: Deferred? = null
    private var current: Subscription? = null
    private var lastGeneration = 0L

    /** A start asked for in foreground epoch [epoch] that waits for the app to be visible. */
    private class Deferred(val context: Context, val call: Long, val epoch: Long)

    /** One provider's fix, as the poll needs it. */
    private class Fix(val provider: String, val lat: Double, val lon: Double, val accuracyM: Float, val elapsedNs: Long)

    /** What the poll reads: never changed once published. [fixes] holds at most one fix per provider. */
    private class Snapshot(val generation: Long, val state: String, val fixes: List<Fix> = emptyList()) {
        fun with(state: String, fixes: List<Fix> = this.fixes) = Snapshot(generation, state, fixes)
    }

    private class Subscription(val generation: Long, var serial: Long, val context: Context, var snapshot: Snapshot) {
        var manager: LocationManager? = null
        val listeners = mutableListOf<LocationListener>()
        val enabled = mutableSetOf<String>()
    }

    /** One provider's callbacks for one subscription; they reach the main looper (executor or looper). */
    @Suppress("OVERRIDE_DEPRECATION") // onStatusChanged: deprecated, but abstract on API 29, so it is overridden.
    private class Feed(val subscription: Subscription, val provider: String) : LocationListener {
        override fun onLocationChanged(location: Location) = fixArrived(subscription, provider, location)
        override fun onProviderDisabled(provider: String) = providerChanged(subscription, this.provider, false)
        override fun onProviderEnabled(provider: String) = providerChanged(subscription, this.provider, true)
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
    }

    /**
     * Starts the feed from any thread; it registers on the main looper while the app is visible, and on the next
     * `onStart` when the app is hidden. A start asked for before the latest `onStop` is dropped.
     */
    @JvmStatic fun startStream(context: Context) {
        val startedIn = ForegroundEpoch.current()
        val call = synchronized(lock) { ++serial }
        main.post {
            if (ForegroundEpoch.current() != startedIn) return@post
            if (visible) {
                start(context.applicationContext, call)
            } else {
                deferred = Deferred(context.applicationContext, call, startedIn)
            }
        }
    }

    /** Stops the feed from any thread. The poll answers null at once; the listeners go on the main looper. */
    @JvmStatic fun stopStream() {
        withdraw()
        main.post {
            deferred = null
            unsubscribe()
        }
    }

    /**
     * The feed's record as JSON, or null while it is stopped: `generation`, `state`, and with a usable fix `provider`,
     * `elapsedRealtimeNanos` (together the fix's identity), `lat`, `lon`, `accuracyM` and `ageMs`, its age now.
     * Not @Synchronized: on a @JvmStatic member of an object that locks the class, while the writers lock [lock].
     */
    @JvmStatic fun pollStream(): String? {
        val snapshot = synchronized(lock) { record } ?: return null
        val now = SystemClock.elapsedRealtimeNanos()
        val json = JSONObject().put("generation", snapshot.generation).put("state", snapshot.state)
        best(snapshot.fixes, now)?.let { fix ->
            json.put("provider", fix.provider).put("elapsedRealtimeNanos", fix.elapsedNs)
                .put("lat", fix.lat).put("lon", fix.lon).put("accuracyM", fix.accuracyM.toDouble())
                .put("ageMs", (now - fix.elapsedNs) / 1_000_000L)
        }
        return json.toString()
    }

    /** How many times the app has left the foreground: 0 at launch, one more on every Activity `onStop`. */
    @JvmStatic fun foregroundEpoch(): Long = ForegroundEpoch.current()

    /**
     * Activity `onStart`, on the main looper: the feed may register again. A start Qt asked for while the app was hidden
     * in this epoch registers now; Qt asks for any other once it sees the epoch moved.
     */
    @JvmStatic fun onActivityStart() {
        visible = true
        val waiting = deferred ?: return
        deferred = null
        if (waiting.epoch == ForegroundEpoch.current()) start(waiting.context, waiting.call)
    }

    /** Activity `onStop`, on the main looper: the foreground is gone, so the feed stops before Qt's thread freezes. */
    @JvmStatic fun onActivityStop() {
        visible = false
        deferred = null
        ForegroundEpoch.advance()
        withdraw()
        unsubscribe()
    }

    /** Activity `onDestroy`, on the main looper. */
    @JvmStatic fun onDestroy() {
        visible = false
        deferred = null
        withdraw()
        unsubscribe()
    }

    /**
     * A permission answer, on the main looper ([LocationPermission]): a feed that is still wanted and waits for access
     * registers at once, without waiting for `onStart`. A stop since its start means roaming was turned off during the
     * prompt, and the answer starts nothing.
     */
    internal fun permissionAnswered() {
        val subscription = current ?: return
        if (subscription.listeners.isEmpty()) start(subscription.context, subscription.serial, renew = true)
    }

    private fun withdraw() {
        synchronized(lock) {
            serial++
            record = null
        }
    }

    private fun isCurrent(call: Long) = synchronized(lock) { serial == call }

    private fun start(context: Context, call: Long, renew: Boolean = false) {
        if (!isCurrent(call)) return
        val running = current
        if (running != null && running.listeners.isNotEmpty() && !renew) {
            // Already registered: the newer start adopts it rather than restarting the providers.
            running.serial = call
            publish(running, running.snapshot)
            return
        }
        unsubscribe()
        val generation = ++lastGeneration
        val subscription = Subscription(generation, call, context, Snapshot(generation, NO_PERMISSION))
        current = subscription
        val state = when {
            LocationPermission.granted(context, Manifest.permission.ACCESS_FINE_LOCATION) -> register(subscription)
            LocationPermission.granted(context, Manifest.permission.ACCESS_COARSE_LOCATION) -> APPROXIMATE_ONLY
            else -> NO_PERMISSION
        }
        publish(subscription, subscription.snapshot.with(state))
    }

    /** Registers every present provider, enabled or not (a disabled one starts when location is turned on). */
    private fun register(subscription: Subscription): String {
        val manager = subscription.context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager
            ?: return PROVIDER_UNAVAILABLE
        subscription.manager = manager
        val present = try { manager.allProviders } catch (_: Exception) { emptyList<String>() }
        val providers = buildList {
            if (LocationManager.GPS_PROVIDER in present) add(LocationManager.GPS_PROVIDER)
            if (Build.VERSION.SDK_INT >= 31 && LocationManager.FUSED_PROVIDER in present) add(LocationManager.FUSED_PROVIDER)
        }
        for (provider in providers) {
            val feed = Feed(subscription, provider)
            try {
                if (Build.VERSION.SDK_INT >= 31) {
                    val request = LocationRequest.Builder(INTERVAL_MS).setMinUpdateIntervalMillis(INTERVAL_MS)
                        .setMinUpdateDistanceMeters(0f).setQuality(LocationRequest.QUALITY_HIGH_ACCURACY).build()
                    manager.requestLocationUpdates(provider, request, subscription.context.mainExecutor, feed)
                } else {
                    manager.requestLocationUpdates(provider, INTERVAL_MS, 0f, feed, Looper.getMainLooper())
                }
            } catch (_: Exception) {
                continue
            }
            subscription.listeners.add(feed)
            if (try { manager.isProviderEnabled(provider) } catch (_: Exception) { false }) subscription.enabled.add(provider)
        }
        return when {
            subscription.listeners.isEmpty() -> PROVIDER_UNAVAILABLE
            subscription.enabled.isEmpty() -> LOCATION_OFF
            else -> OK
        }
    }

    private fun unsubscribe() {
        val subscription = current ?: return
        current = null
        subscription.listeners.forEach { listener ->
            try { subscription.manager?.removeUpdates(listener) } catch (_: Exception) { }
        }
        subscription.listeners.clear()
    }

    /** The subscription's newest snapshot; the poll sees it only while the start that made it is the newest call. */
    private fun publish(subscription: Subscription, snapshot: Snapshot) {
        if (current !== subscription) return
        subscription.snapshot = snapshot
        synchronized(lock) { if (serial == subscription.serial) record = snapshot }
    }

    private fun fixArrived(subscription: Subscription, provider: String, location: Location) {
        if (current !== subscription || !usable(location)) return
        val old = subscription.snapshot
        val previous = old.fixes.firstOrNull { it.provider == provider }
        if (previous != null && location.elapsedRealtimeNanos <= previous.elapsedNs) return
        val fix = Fix(provider, location.latitude, location.longitude, location.accuracy, location.elapsedRealtimeNanos)
        subscription.enabled.add(provider)
        publish(subscription, old.with(OK, old.fixes.filter { it.provider != provider } + fix))
    }

    private fun providerChanged(subscription: Subscription, provider: String, enabled: Boolean) {
        if (current !== subscription) return
        val old = subscription.snapshot
        if (enabled) subscription.enabled.add(provider) else subscription.enabled.remove(provider)
        // A turned-off provider's last fix goes with it.
        val fixes = if (enabled) old.fixes else old.fixes.filter { it.provider != provider }
        publish(subscription, old.with(if (subscription.enabled.isEmpty()) LOCATION_OFF else OK, fixes))
    }

    /** Finite, on the globe, with an accuracy above zero, and measured no later than now. */
    private fun usable(location: Location): Boolean =
        location.latitude.isFinite() && location.latitude in -90.0..90.0
            && location.longitude.isFinite() && location.longitude in -180.0..180.0
            && location.hasAccuracy() && location.accuracy.isFinite() && location.accuracy > 0f
            && location.elapsedRealtimeNanos > 0L && location.elapsedRealtimeNanos <= SystemClock.elapsedRealtimeNanos()

    /** The most accurate fix no older than [FRESH_NS] (the newer on a tie), else the newest. */
    private fun best(fixes: List<Fix>, now: Long): Fix? {
        val fresh = fixes.filter { now - it.elapsedNs <= FRESH_NS }
        return fresh.minWithOrNull(compareBy<Fix> { it.accuracyM }.thenByDescending { it.elapsedNs })
            ?: fixes.maxByOrNull { it.elapsedNs }
    }
}
