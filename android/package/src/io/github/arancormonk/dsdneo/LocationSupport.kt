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

/** One cancellable foreground request. All mutable request state belongs to the main looper. */
object LocationSupport {
    private const val PERMISSION = 4303
    private const val TIMEOUT_MS = 20_000L
    // Coarse providers can retain their last fix for ten minutes. getCurrentLocation
    // accepts a much shorter cache age and may throttle the replacement past our deadline.
    private const val MAX_FIX_AGE_NS = 10 * 60 * 1_000_000_000L
    private const val MAX_ACCURACY_M = 10_000f
    private val main = Handler(Looper.getMainLooper())
    private val worker = LocationGeocodeQueue()
    private var active: Request? = null
    private var permissionActivity: Activity? = null
    private var result: String? = null

    private class Request(val activity: Activity, val id: Long) {
        val manager = activity.getSystemService(Context.LOCATION_SERVICE) as LocationManager
        val signals = mutableListOf<CancellationSignal>()
        val listeners = mutableListOf<LocationListener>()
        val pendingProviders = mutableSetOf<String>()
        var fix: Location? = null
        var timeout: Runnable? = null
        var geocodeTask: FutureTask<Unit>? = null
    }

    @JvmStatic fun requestCurrentLocation(activity: Activity, id: Long) {
        main.post {
            active?.let { release(it) }
            synchronized(this) { result = null }
            val request = Request(activity, id)
            active = request
            if (activity.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
                locate(request)
            } else if (permissionActivity == null) {
                permissionActivity = activity
                activity.requestPermissions(arrayOf(Manifest.permission.ACCESS_COARSE_LOCATION), PERMISSION)
            }
        }
    }

    @JvmStatic fun onRequestPermissionsResult(activity: Activity, code: Int, grants: IntArray): Boolean {
        if (code != PERMISSION) return false
        if (permissionActivity !== activity) return true
        permissionActivity = null
        val request = active ?: return true
        if (request.activity !== activity) return true
        if (grants.isNotEmpty() && grants[0] == PackageManager.PERMISSION_GRANTED) locate(request)
        else finish(request, false, "", "", "Location permission denied; use Browse.")
        return true
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    private fun locate(request: Request) {
        if (active !== request) return
        armTimeout(request, "Location request timed out; use Browse.")
        // Both providers honor coarse permission. The platform fused provider is public
        // from API 31; it does not require a Google Play Services dependency.
        val candidates = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) candidates.add(LocationManager.FUSED_PROVIDER)
        candidates.add(LocationManager.NETWORK_PROVIDER)
        val providers = candidates.filter { provider ->
            try { request.manager.isProviderEnabled(provider) } catch (_: Exception) { false }
        }
        if (providers.isEmpty()) {
            finish(request, false, "", "", "Location provider unavailable; use Browse.")
            return
        }
        val cached = providers.mapNotNull { provider ->
            try { request.manager.getLastKnownLocation(provider) } catch (_: Exception) { null }
        }.filter { usableFix(it) }.maxByOrNull { it.elapsedRealtimeNanos }
        if (cached != null) {
            gotFix(request, cached)
            return
        }
        // Register all candidates before any callback can arrive. A silent or failed
        // provider must not prevent another provider from satisfying the request.
        request.pendingProviders.addAll(providers)
        for (provider in providers) {
            if (active !== request || request.fix != null) break
            try {
                if (Build.VERSION.SDK_INT >= 30) {
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

    private fun usableFix(location: Location?): Boolean {
        if (location == null || !location.latitude.isFinite() || location.latitude !in -90.0..90.0
            || !location.longitude.isFinite() || location.longitude !in -180.0..180.0
            || !location.hasAccuracy() || !location.accuracy.isFinite()
            || location.accuracy <= 0f || location.accuracy > MAX_ACCURACY_M
            || location.time <= 0L || location.elapsedRealtimeNanos <= 0L) return false
        val age = SystemClock.elapsedRealtimeNanos() - location.elapsedRealtimeNanos
        return age in 0L..MAX_FIX_AGE_NS
    }

    private fun providerResult(request: Request, provider: String, location: Location?) {
        if (active !== request || request.fix != null || !request.pendingProviders.remove(provider)) return
        if (usableFix(location)) gotFix(request, location!!)
        else if (request.pendingProviders.isEmpty()) finish(request, false, "", "", "No location fix available; use Browse.")
    }

    private fun armTimeout(request: Request, error: String) {
        request.timeout?.let { main.removeCallbacks(it) }
        request.timeout = Runnable { finish(request, false, "", "", error) }
            .also { main.postDelayed(it, TIMEOUT_MS) }
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    private fun gotFix(request: Request, location: Location) {
        if (active !== request || request.fix != null) return
        request.fix = location
        removeUpdates(request)
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

    private fun finish(request: Request, geocodeOk: Boolean, postal: String, country: String, error: String) {
        if (active !== request) return
        val fix = request.fix
        val record = JSONObject().put("id", request.id).put("fixOk", fix != null)
            .put("lat", fix?.latitude ?: 0.0).put("lon", fix?.longitude ?: 0.0)
            .put("accuracyM", fix?.accuracy?.toDouble() ?: 0.0).put("fixAtMs", fix?.time ?: 0L)
            .put("geocodeOk", geocodeOk).put("postalCode", postal).put("countryCode", country).put("error", error)
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
        if (permissionActivity === activity) permissionActivity = null
    }

    @JvmStatic @Synchronized fun pollResult(): String {
        val record = result ?: ""
        result = null
        return record
    }
}
