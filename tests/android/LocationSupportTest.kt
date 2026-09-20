// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo
import android.app.Activity
import android.location.Address
import android.location.Geocoder
import android.location.Location
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.SystemClock
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.FutureTask
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

private fun request(activity: Activity, id: Long) {
    LocationSupport.requestCurrentLocation(activity, id)
    Handler.drain()
}

private fun geocodeResult(): JSONObject {
    val queue = LocationSupport::class.java.getDeclaredField("worker").apply { isAccessible = true }.get(LocationSupport)
    val task = queue.javaClass.getDeclaredField("current").apply { isAccessible = true }.get(queue) as FutureTask<*>
    task.get(5, TimeUnit.SECONDS)
    Handler.drain()
    return JSONObject(LocationSupport.pollResult()).also {
        check(it.optBoolean("fixOk") && it.optBoolean("geocodeOk"))
        check(it.optString("postalCode") == "52240")
        check(Handler.timeouts.isEmpty())
    }
}

private fun slowPermissionGrant() {
    val activity = Activity().apply { permission = -1 }
    request(activity, 10)
    Handler.advanceBy(60_000)
    check(LocationSupport.pollResult().isEmpty() && activity.manager.callback == null)
    check(Handler.timeouts.isEmpty()) { "permission prompt must not consume acquisition time" }
    activity.permission = 0
    check(LocationSupport.onRequestPermissionsResult(activity, 4303, intArrayOf(0)))
    Handler.advanceBy(19_000)
    check(LocationSupport.pollResult().isEmpty())
    activity.manager.callback!!.accept(Location())
    check(geocodeResult().optLong("id") == 10L)
}

private fun recentCacheAvoidsThrottledRequest() {
    for (sdk in listOf(29, 30, 31)) {
        Build.VERSION.SDK_INT = sdk
        val activity = Activity()
        // Older than getCurrentLocation's cache window, but still useful for nearby lookup.
        val cached = Location().apply {
            elapsedRealtimeNanos -= 2 * 60 * 1_000_000_000L
            accuracy = 2_000f
        }
        activity.manager.lastKnown[LocationManager.NETWORK_PROVIDER] = cached
        if (sdk >= 31) {
            activity.manager.lastKnown[LocationManager.FUSED_PROVIDER] = Location().apply {
                elapsedRealtimeNanos -= 5 * 60 * 1_000_000_000L
                latitude = 40.0
            }
        }
        request(activity, 20L + sdk)
        check(activity.manager.callbacks.isEmpty()) { "usable cache must not wait for a throttled live fix" }
        val result = geocodeResult()
        check(result.optDouble("lat") == cached.latitude && result.optDouble("accuracyM") == 2_000.0)
        check(result.optLong("fixAtMs") == cached.time) { "cached fix must retain its original timestamp" }
    }
    Build.VERSION.SDK_INT = 30
}

private fun invalidCacheFallsBackToFreshFix() {
    val invalid = listOf<(Location) -> Unit>(
        { it.elapsedRealtimeNanos -= 10 * 60 * 1_000_000_000L + 1 },
        { it.elapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos() + 1 },
        { it.elapsedRealtimeNanos = 0 },
        { it.accuracy = 10_001f },
        { it.accuracy = Float.NaN },
        { it.accuracyAvailable = false },
        { it.latitude = Double.NaN },
        { it.longitude = 181.0 },
        { it.time = 0 }
    )
    for ((index, invalidate) in invalid.withIndex()) {
        val activity = Activity()
        activity.manager.lastKnown[LocationManager.NETWORK_PROVIDER] = Location().also(invalidate)
        request(activity, 60L + index)
        check(activity.manager.callback != null && LocationSupport.pollResult().isEmpty())
        activity.manager.callback!!.accept(Location())
        check(geocodeResult().optLong("id") == 60L + index)
    }
}

private fun alternateProviderAndRetiredCallbacks() {
    Build.VERSION.SDK_INT = 31
    try {
        // A silent network provider must not prevent the fused provider from returning a fix.
        val activity = Activity()
        request(activity, 80)
        val retired = activity.manager.callbacks.getValue(LocationManager.NETWORK_PROVIDER)
        activity.manager.callbacks.getValue(LocationManager.FUSED_PROVIDER).accept(Location())
        check(geocodeResult().optLong("id") == 80L)
        check(activity.manager.signals.size == 2 && activity.manager.signals.values.all { it.cancelled })
        request(activity, 81)
        retired.accept(Location().apply { latitude = 12.0 })
        Handler.drain()
        check(LocationSupport.pollResult().isEmpty())
        // Null from one provider must not fail the other still-pending provider.
        activity.manager.callbacks.getValue(LocationManager.FUSED_PROVIDER).accept(null)
        check(LocationSupport.pollResult().isEmpty())
        activity.manager.callbacks.getValue(LocationManager.NETWORK_PROVIDER).accept(Location())
        val result = geocodeResult()
        check(result.optLong("id") == 81L && result.optDouble("lat") == 41.0)

        val noNetwork = Activity()
        noNetwork.manager.disabledProviders.add(LocationManager.NETWORK_PROVIDER)
        request(noNetwork, 82)
        noNetwork.manager.callbacks.getValue(LocationManager.FUSED_PROVIDER).accept(Location())
        check(geocodeResult().optLong("id") == 82L)

        val brokenFused = Activity()
        brokenFused.manager.failingProviders.add(LocationManager.FUSED_PROVIDER)
        request(brokenFused, 83)
        brokenFused.manager.callbacks.getValue(LocationManager.NETWORK_PROVIDER).accept(Location())
        check(geocodeResult().optLong("id") == 83L)

        request(activity, 84)
        Handler.advanceBy(20_000)
        val timeout = JSONObject(LocationSupport.pollResult())
        check(!timeout.optBoolean("fixOk") && timeout.optString("error").contains("Location request timed out"))
        check(activity.manager.signals.values.all { it.cancelled } && Handler.timeouts.isEmpty())
        activity.manager.callbacks.values.forEach { it.accept(Location()) }
        Handler.drain()
        check(LocationSupport.pollResult().isEmpty())
    } finally { Build.VERSION.SDK_INT = 30 }
}

private fun legacyLiveFixAndCancellation() {
    Build.VERSION.SDK_INT = 29
    try {
        val activity = Activity()
        request(activity, 90)
        val listener = activity.manager.listeners.getValue(LocationManager.NETWORK_PROVIDER)
        listener.onLocationChanged(Location())
        check(geocodeResult().optLong("id") == 90L)
        check(listener in activity.manager.removedListeners)
        request(activity, 91)
        val cancelled = activity.manager.listeners.getValue(LocationManager.NETWORK_PROVIDER)
        LocationSupport.cancelLocationRequest(91)
        Handler.drain()
        check(cancelled in activity.manager.removedListeners && Handler.timeouts.isEmpty())
        cancelled.onLocationChanged(Location())
        Handler.drain()
        check(LocationSupport.pollResult().isEmpty())
    } finally { Build.VERSION.SDK_INT = 30 }
}

private fun permissionAndTeardown() {
    val activity = Activity()
    activity.permission = -1
    request(activity, 1)
    check(activity.permissionRequests == 1)
    check(LocationSupport.onRequestPermissionsResult(activity, 4303, intArrayOf(-1)))
    val denied = JSONObject(LocationSupport.pollResult())
    check(denied.optLong("id") == 1L && !denied.optBoolean("fixOk"))
    check(denied.optString("error").contains("permission denied"))
    activity.permission = 0
    request(activity, 2)
    val pending = activity.manager.signal!!
    LocationSupport.onDestroy(activity)
    check(pending.cancelled)
    check(JSONObject(LocationSupport.pollResult()).optString("error").contains("cancelled"))
    check(Handler.timeouts.isEmpty())
}

private fun timeoutWithFixAndLateReplacement() {
    val activity = Activity()
    val started = CountDownLatch(1)
    val release = CountDownLatch(1)
    val finished = CountDownLatch(1)
    Geocoder.lookup = { _, _ ->
        started.countDown()
        var released = false
        while (!released) {
            try { release.await(); released = true } catch (_: InterruptedException) { }
        }
        finished.countDown()
        listOf(Address("OLD", "US"))
    }
    try {
        request(activity, 3)
        Handler.advanceBy(19_000)
        activity.manager.callback!!.accept(Location())
        check(started.await(5, TimeUnit.SECONDS))
        Handler.advanceBy(1_000)
        check(LocationSupport.pollResult().isEmpty()) { "geocoding must have its own deadline after a slow location fix" }
        Handler.expire()
        val timedOut = JSONObject(LocationSupport.pollResult())
        check(timedOut.optLong("id") == 3L && timedOut.optBoolean("fixOk"))
        check(timedOut.optDouble("lat") == 41.0 && timedOut.optDouble("accuracyM") == 25.0)
        check(!timedOut.optBoolean("geocodeOk") && timedOut.optString("error").contains("Geocoding timed out"))
        request(activity, 4)
        release.countDown()
        check(finished.await(5, TimeUnit.SECONDS))
        // Synchronize with the worker after it has enqueued the stale completion.
        val queueField = LocationSupport::class.java.getDeclaredField("worker").apply { isAccessible = true }
        val queue = queueField.get(LocationSupport) as LocationGeocodeQueue
        val drained = CountDownLatch(1)
        queue.submit { drained.countDown() }
        check(drained.await(5, TimeUnit.SECONDS))
        Handler.drain()
        check(LocationSupport.pollResult().isEmpty()) { "late geocoder completed a replacement request" }
        LocationSupport.cancelLocationRequest(4)
        Handler.drain()
        check(activity.manager.signal!!.cancelled && Handler.timeouts.isEmpty())
    } finally { release.countDown() }
}

fun main() {
    try {
        permissionAndTeardown()
        slowPermissionGrant()
        recentCacheAvoidsThrottledRequest()
        invalidCacheFallsBackToFreshFix()
        alternateProviderAndRetiredCallbacks()
        legacyLiveFixAndCancellation()
        timeoutWithFixAndLateReplacement()
        println("PASS: location permission, cache freshness, provider fallback, legacy callbacks, deadlines and cancellation")
    } finally {
        val field = LocationSupport::class.java.getDeclaredField("worker").apply { isAccessible = true }
        val queue = field.get(LocationSupport)
        val executorField = LocationGeocodeQueue::class.java.getDeclaredField("executor").apply { isAccessible = true }
        val executor = executorField.get(queue) as ThreadPoolExecutor
        executor.shutdownNow()
        check(executor.awaitTermination(5, TimeUnit.SECONDS))
    }
}
