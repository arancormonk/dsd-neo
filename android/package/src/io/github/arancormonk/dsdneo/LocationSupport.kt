// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.location.Geocoder
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.location.LocationRequest
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import org.json.JSONObject
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.FutureTask
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

/** One executing geocoder and at most one queued replacement. Contains no Activity references. */
internal class LocationGeocodeQueue(
    private val executor: ThreadPoolExecutor = ThreadPoolExecutor(
        1, 1, 0, TimeUnit.MILLISECONDS, ArrayBlockingQueue<Runnable>(1)
    )
) {
    @Volatile private var current: FutureTask<Unit>? = null

    @Synchronized fun submit(block: () -> Unit): FutureTask<Unit> {
        current?.let { cancel(it) }
        lateinit var task: FutureTask<Unit>
        task = FutureTask(Runnable {
            // Cancellation may race with dequeue. Retired requests must not enter the geocoder.
            if (current === task) block()
        }, Unit)
        current = task
        try {
            executor.execute(task)
        } catch (error: RejectedExecutionException) {
            cancel(task)
            throw error
        }
        return task
    }

    @Synchronized fun cancel(task: FutureTask<Unit>) {
        if (current === task) current = null
        task.cancel(true)
        // Future cancellation alone leaves a tombstone in ThreadPoolExecutor's queue.
        executor.remove(task)
    }
}

/**
 * One cancellable foreground request. All mutable request state belongs to the main looper. Its permission prompt goes
 * through [LocationPermission], which GPS roaming shares.
 */
object LocationSupport {
    private const val TIMEOUT_MS = 20_000L
    // Coarse providers can retain their last fix for ten minutes. getCurrentLocation
    // accepts a much shorter cache age and may throttle the replacement past our deadline.
    private const val MAX_FIX_AGE_NS = 10 * 60 * 1_000_000_000L
    private const val MAX_ACCURACY_M = 10_000f
    private const val SITE_FIX_AGE_NS = 30 * 1_000_000_000L
    private const val SITE_TARGET_ACCURACY_M = 100f
    // How long, from its start, a site request with GPS asked waits for GPS before another provider's answer may
    // settle it. Google's fused provider answers a new request at once with the position it already holds, stamped as
    // new, so the first answer is often not a new measurement: taken at once, every Locate me after the first came back
    // where the first one was.
    internal const val GPS_GRACE_MS = 10_000L
    private val main = Handler(Looper.getMainLooper())
    private val worker = LocationGeocodeQueue()
    private var active: Request? = null
    private var result: String? = null

    /** How a request may ask for permission. */
    private enum class Prompt {
        /** Only when location is not allowed at all (every request but the map's). */
        IF_NONE,
        /** Also when only approximate location is allowed: the map's Locate me offers precise. */
        PRECISE,
        /** Never: the map's opening fix ends at once without permission. */
        NEVER,
    }

    /** [forSites]: coordinates only (no postal code), as fresh and accurate as the providers give. */
    private class Request(val activity: Activity, val id: Long, val forSites: Boolean, val prompt: Prompt) {
        val manager = activity.getSystemService(Context.LOCATION_SERVICE) as LocationManager
        val signals = mutableListOf<CancellationSignal>()
        val listeners = mutableListOf<LocationListener>()
        val pendingProviders = mutableSetOf<String>()
        var fix: Location? = null
        // Each provider's newest answer: the one used is the best still usable when the request settles.
        val answers = mutableMapOf<String, Location>()
        var timeout: Runnable? = null
        var gpsGrace: Runnable? = null
        var graceOver = false
        var geocodeTask: FutureTask<Unit>? = null
    }

    @JvmStatic fun requestCurrentLocation(activity: Activity, id: Long) = request(activity, id, false, Prompt.IF_NONE)

    @JvmStatic fun requestSiteLocation(activity: Activity, id: Long) = request(activity, id, true, Prompt.IF_NONE)

    /**
     * The map's Locate me: a site request that, with approximate location only, first offers precise location. Asking
     * for both again is how Android offers the upgrade; once the user has turned it down, Android answers at once
     * without a prompt (Android 11 and later), and the request goes on with approximate location.
     */
    @JvmStatic fun requestPreciseLocation(activity: Activity, id: Long) = request(activity, id, true, Prompt.PRECISE)

    /** The map's opening fix: a site request that never prompts, and ends at once when location is not allowed. */
    @JvmStatic fun requestQuietLocation(activity: Activity, id: Long) = request(activity, id, true, Prompt.NEVER)

    private fun request(activity: Activity, id: Long, forSites: Boolean, prompt: Prompt) {
        main.post {
            active?.let { release(it) }
            synchronized(this) { result = null }
            val request = Request(activity, id, forSites, prompt)
            active = request
            val coarse = activity.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED
            val fine = activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
            when {
                coarse && (fine || prompt != Prompt.PRECISE) -> locate(request)
                prompt == Prompt.NEVER -> finish(request, false, "", "", "Location permission not granted.")
                else -> {
                    // A prompt already open answers this request too; the broker opens no second one.
                    val permissions = if (forSites) {
                        arrayOf(Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION)
                    } else {
                        arrayOf(Manifest.permission.ACCESS_COARSE_LOCATION)
                    }
                    // The answer resumes this request only: one given up or replaced meanwhile is left alone.
                    LocationPermission.ask(activity, permissions, LocationPermission.Asker.ONE_SHOT) { answered, grants ->
                        permissionAnswered(request, answered, grants)
                    }
                }
            }
        }
    }

    /** Hands a permission answer to [LocationPermission], which owns the prompt; true when the code is its own. */
    @JvmStatic fun onRequestPermissionsResult(activity: Activity, code: Int, grants: IntArray): Boolean =
        LocationPermission.onRequestPermissionsResult(activity, code, grants)

    /** The answer to the prompt: it resumes [request] only while it is the active one and asked [activity]. */
    private fun permissionAnswered(request: Request, activity: Activity, grants: IntArray) {
        if (active !== request || request.activity !== activity) return
        when {
            // An interrupted prompt comes back with no answers, and Android says to treat it as a cancellation: the
            // launcher icon or the notification brings the singleTask activity forward and closes the prompt (#782).
            // The user denied nothing, so nothing is reported; the request just ends.
            grants.isEmpty() -> finish(request, false, "", "", "", cancelled = true)
            grants[0] == PackageManager.PERMISSION_GRANTED -> locate(request)
            else -> finish(request, false, "", "", "Location permission denied; use Browse.")
        }
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    private fun locate(request: Request) {
        if (active !== request) return
        armTimeout(request, "Location request timed out; use Browse.")
        // Both providers honor coarse permission. The platform fused provider is public
        // from API 31; it does not require a Google Play Services dependency.
        val candidates = mutableListOf<String>()
        val fine = request.activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        if (Build.VERSION.SDK_INT >= 31) candidates.add(LocationManager.FUSED_PROVIDER)
        candidates.add(LocationManager.NETWORK_PROVIDER)
        if (request.forSites && fine) candidates.add(LocationManager.GPS_PROVIDER)
        val providers = candidates.filter { provider ->
            try { request.manager.isProviderEnabled(provider) } catch (_: Exception) { false }
        }
        if (providers.isEmpty()) {
            finish(request, false, "", "", "Location provider unavailable; use Browse.")
            return
        }
        // With GPS asked, only its own cache is a measurement: the fused provider's last location can be the position
        // it handed an earlier request, stamped as new.
        val cacheProviders = if (LocationManager.GPS_PROVIDER in providers) listOf(LocationManager.GPS_PROVIDER) else providers
        val cached = cacheProviders.mapNotNull { provider ->
            try { request.manager.getLastKnownLocation(provider) } catch (_: Exception) { null }
        }.filter { usableFix(it, request.forSites) }
            .let { fixes ->
                if (request.forSites) fixes.minByOrNull { it.accuracy }
                else fixes.maxByOrNull { it.elapsedRealtimeNanos }
            }
        if (cached != null && (!request.forSites || cached.accuracy <= SITE_TARGET_ACCURACY_M)) {
            gotFix(request, cached)
            return
        }
        // Register all candidates before any callback can arrive. A silent or failed
        // provider must not prevent another provider from satisfying the request.
        request.pendingProviders.addAll(providers)
        if (request.forSites && LocationManager.GPS_PROVIDER in providers) {
            request.gpsGrace = Runnable {
                request.gpsGrace = null
                request.graceOver = true
                settleSite(request)
            }.also { main.postDelayed(it, GPS_GRACE_MS) }
        }
        for (provider in providers) {
            if (active !== request || request.fix != null) break
            try {
                if (Build.VERSION.SDK_INT >= 31) {
                    val signal = CancellationSignal()
                    request.signals.add(signal)
                    // High accuracy when precise location is allowed, so the fused provider measures (GPS included)
                    // rather than answering from what it holds; approximate location gets the balanced default.
                    val quality = if (request.forSites && fine) LocationRequest.QUALITY_HIGH_ACCURACY
                        else LocationRequest.QUALITY_BALANCED_POWER_ACCURACY
                    val current = LocationRequest.Builder(0L).setQuality(quality).setDurationMillis(TIMEOUT_MS).build()
                    request.manager.getCurrentLocation(provider, current, signal, request.activity.mainExecutor) { location ->
                        providerResult(request, provider, location)
                    }
                } else if (Build.VERSION.SDK_INT >= 30) {
                    val signal = CancellationSignal()
                    request.signals.add(signal)
                    request.manager.getCurrentLocation(provider, signal, request.activity.mainExecutor) { location ->
                        providerResult(request, provider, location)
                    }
                } else {
                    val listener = object : LocationListener {
                        override fun onLocationChanged(location: Location) { providerResult(request, provider, location) }
                        override fun onProviderDisabled(provider: String) { providerResult(request, provider, null) }
                        override fun onProviderEnabled(provider: String) {}
                        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
                    }
                    request.listeners.add(listener)
                    request.manager.requestSingleUpdate(provider, listener, Looper.getMainLooper())
                }
            } catch (_: Exception) {
                providerResult(request, provider, null)
            }
        }
    }

    private fun usableFix(location: Location?, forSites: Boolean = false): Boolean {
        if (location == null || !location.latitude.isFinite() || location.latitude !in -90.0..90.0
            || !location.longitude.isFinite() || location.longitude !in -180.0..180.0
            || !location.hasAccuracy() || !location.accuracy.isFinite()
            || location.accuracy <= 0f || location.accuracy > MAX_ACCURACY_M
            || location.time <= 0L || location.elapsedRealtimeNanos <= 0L) return false
        val age = SystemClock.elapsedRealtimeNanos() - location.elapsedRealtimeNanos
        return age in 0L..(if (forSites) SITE_FIX_AGE_NS else MAX_FIX_AGE_NS)
    }

    private fun providerResult(request: Request, provider: String, location: Location?) {
        if (active !== request || request.fix != null || !request.pendingProviders.remove(provider)) return
        if (request.forSites) {
            if (provider == LocationManager.GPS_PROVIDER && usableFix(location, true)
                && location!!.accuracy <= SITE_TARGET_ACCURACY_M) {
                // A new measurement, so it beats an earlier answer that only claimed to be more accurate.
                gotFix(request, location)
                return
            }
            if (location != null) request.answers[provider] = location
            settleSite(request)
            return
        }
        if (usableFix(location)) gotFix(request, location!!)
        else if (request.pendingProviders.isEmpty()) finish(request, false, "", "", "No location fix available; use Browse.")
    }

    /** The most accurate answer still fresh enough now (then the newest), or null. */
    private fun bestSiteFix(request: Request): Location? =
        request.answers.values.filter { usableFix(it, true) }
            .minWithOrNull(compareBy<Location> { it.accuracy }.thenByDescending { it.elapsedRealtimeNanos })

    /**
     * A site request settles on its best answer once every provider has answered, or once that answer is close enough
     * and GPS is not still owed its grace (see [GPS_GRACE_MS]); with nothing usable left to wait for it fails.
     */
    private fun settleSite(request: Request) {
        if (active !== request || request.fix != null) return
        val best = bestSiteFix(request)
        val gpsOwed = LocationManager.GPS_PROVIDER in request.pendingProviders && !request.graceOver
        when {
            best != null && (request.pendingProviders.isEmpty() || (best.accuracy <= SITE_TARGET_ACCURACY_M && !gpsOwed)) ->
                gotFix(request, best)
            request.pendingProviders.isEmpty() ->
                finish(request, false, "", "", "No current location fix available. Try again or choose a site manually.")
        }
    }

    private fun armTimeout(request: Request, error: String) {
        request.timeout?.let { main.removeCallbacks(it) }
        request.timeout = Runnable {
            val best = if (request.forSites && request.fix == null) bestSiteFix(request) else null
            if (best != null) gotFix(request, best) else finish(request, false, "", "", error)
        }
            .also { main.postDelayed(it, TIMEOUT_MS) }
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    private fun gotFix(request: Request, location: Location) {
        if (active !== request || request.fix != null) return
        request.fix = location
        removeUpdates(request)
        if (request.forSites) {
            // Site distances need coordinates, so a missing or blocked geocoder cannot delay them.
            finish(request, false, "", "", "")
            return
        }
        armTimeout(request, "Geocoding timed out; use Browse.")
        // Capture only application context and coordinates; an abandoned geocoder must not retain an Activity.
        val context = request.activity.applicationContext
        val latitude = location.latitude
        val longitude = location.longitude
        val id = request.id
        try {
            request.geocodeTask = worker.submit {
                var postal = ""
                var country = ""
                var ok = false
                try {
                    if (Geocoder.isPresent()) {
                        val address = Geocoder(context).getFromLocation(latitude, longitude, 1)?.firstOrNull()
                        if (address != null) {
                            postal = address.postalCode ?: ""
                            country = address.countryCode ?: ""
                            ok = true
                        }
                    }
                } catch (_: Exception) { /* Never expose platform exception text. */ }
                main.post {
                    active?.takeIf { it.id == id }?.let {
                        finish(it, ok, postal, country, if (ok) "" else "Geocoding unavailable; use Browse.")
                    }
                }
            }
        } catch (_: RejectedExecutionException) {
            finish(request, false, "", "", "Geocoding unavailable; use Browse.")
        }
    }

    private fun removeUpdates(request: Request) {
        request.gpsGrace?.let { main.removeCallbacks(it) }
        request.gpsGrace = null
        request.pendingProviders.clear()
        request.signals.forEach { it.cancel() }
        request.signals.clear()
        request.listeners.forEach { listener ->
            try { request.manager.removeUpdates(listener) } catch (_: Exception) { }
        }
        request.listeners.clear()
    }

    private fun release(request: Request) {
        active = null
        request.geocodeTask?.let { worker.cancel(it) }
        request.geocodeTask = null
        request.timeout?.let { main.removeCallbacks(it) }
        removeUpdates(request)
    }

    /** The request's one result; [cancelled] marks a request that ended with nothing to report. */
    private fun finish(
        request: Request, geocodeOk: Boolean, postal: String, country: String, error: String, cancelled: Boolean = false,
    ) {
        if (active !== request) return
        val fix = request.fix
        val record = JSONObject().put("id", request.id).put("fixOk", fix != null)
            .put("lat", fix?.latitude ?: 0.0).put("lon", fix?.longitude ?: 0.0)
            .put("accuracyM", fix?.accuracy?.toDouble() ?: 0.0).put("fixAtMs", fix?.time ?: 0L)
            .put("geocodeOk", geocodeOk).put("postalCode", postal).put("countryCode", country).put("error", error)
            .put("cancelled", cancelled)
        release(request)
        synchronized(this) { result = record.toString() }
    }

    @JvmStatic fun cancelLocationRequest(id: Long) {
        main.post {
            active?.takeIf { it.id == id }?.let { release(it) }
            synchronized(this) {
                if (result?.let { JSONObject(it).optLong("id") == id } == true) result = null
            }
        }
    }

    @JvmStatic fun onDestroy(activity: Activity) {
        active?.takeIf { it.activity === activity }?.let {
            finish(it, false, "", "", "Location request cancelled; use Browse.")
        }
        LocationPermission.onDestroy(activity)
    }

    /**
     * The one result waiting, or "", read and cleared under the writers' monitor. Not @Synchronized: on a @JvmStatic
     * member of an object that locks the class, while finish() and the posted writers lock the object.
     */
    @JvmStatic fun pollResult(): String = synchronized(this) { result.also { result = null } } ?: ""
}
