// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo
import android.app.Activity
import android.location.Address
import android.location.Geocoder
import android.location.Location
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
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
    // A cached fix starts the geocoder inside request()'s drain, which may already have
    // run its completion and retired the task; the result checks below still apply.
    val task = queue.javaClass.getDeclaredField("current").apply { isAccessible = true }.get(queue) as FutureTask<*>?
    task?.get(5, TimeUnit.SECONDS)
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

private fun cachedFixCompletesInsideRequestDrain() {
    val activity = Activity()
    activity.manager.lastKnown[LocationManager.NETWORK_PROVIDER] = Location()
    LocationSupport.requestCurrentLocation(activity, 30)
    // Queued behind the request, this holds the drain until the cache-started geocoder posts its completion.
    Handler(Looper.getMainLooper()).post {
        val deadline = System.nanoTime() + 5_000_000_000L
        while (Handler.pending.isEmpty()) {
            check(System.nanoTime() < deadline) { "cached geocode did not complete" }
            Thread.sleep(1)
        }
    }
    Handler.drain()
    check(geocodeResult().optLong("id") == 30L)
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
    check(denied.optString("error").contains("permission denied") && !denied.optBoolean("cancelled"))
    activity.permission = 0
    request(activity, 2)
    val pending = activity.manager.signal!!
    LocationSupport.onDestroy(activity)
    check(pending.cancelled)
    check(JSONObject(LocationSupport.pollResult()).optString("error").contains("cancelled"))
    check(Handler.timeouts.isEmpty())
}

// #782: the launcher icon or the notification brings the singleTask activity forward and closes the permission prompt.
// Android then answers with empty arrays, a cancellation: the request ends with nothing to report, not a denial.
private fun interruptedPermissionPromptIsCancelled() {
    for (forSites in listOf(false, true)) {
        val activity = Activity().apply { permission = -1 }
        if (forSites) LocationSupport.requestSiteLocation(activity, 30) else LocationSupport.requestCurrentLocation(activity, 30)
        Handler.drain()
        check(activity.permissionRequests == 1)
        check(LocationSupport.onRequestPermissionsResult(activity, 4303, intArrayOf()))
        val cancelled = JSONObject(LocationSupport.pollResult())
        check(cancelled.optLong("id") == 30L && cancelled.optBoolean("cancelled")) { "an interrupted prompt is a cancel" }
        check(!cancelled.optBoolean("fixOk") && cancelled.optString("error").isEmpty()) {
            "an interrupted prompt reports no denial: ${cancelled.optString("error")}"
        }
        check(activity.manager.callback == null && activity.manager.signals.isEmpty() && Handler.timeouts.isEmpty()) {
            "a cancelled request asks no provider"
        }
        // The next request asks again.
        request(activity, 31)
        check(activity.permissionRequests == 2) { "a cancelled prompt does not block the next request" }
        check(LocationSupport.onRequestPermissionsResult(activity, 4303, intArrayOf(-1)))
        check(!JSONObject(LocationSupport.pollResult()).optBoolean("cancelled"))
    }
}

private fun siteFixIsFreshAndDoesNotGeocode() {
    val original = Geocoder.lookup
    Geocoder.lookup = { _, _ -> error("site selection must not reverse geocode") }
    try {
        for (sdk in listOf(29, 30, 31)) {
            Build.VERSION.SDK_INT = sdk
            val activity = Activity()
            activity.manager.lastKnown[LocationManager.NETWORK_PROVIDER] = Location().apply {
                elapsedRealtimeNanos -= 2 * 60 * 1_000_000_000L
                latitude = 40.0
            }
            LocationSupport.requestSiteLocation(activity, 100L + sdk)
            Handler.drain()
            check(activity.manager.callbacks.isNotEmpty()) { "site lookup must refresh a minutes-old cache" }
            activity.manager.callbacks.getValue(LocationManager.NETWORK_PROVIDER).accept(Location())
            Handler.drain()
            val result = JSONObject(LocationSupport.pollResult())
            check(result.optBoolean("fixOk") && !result.optBoolean("geocodeOk"))
            check(result.optDouble("lat") == 41.0 && Handler.timeouts.isEmpty())
        }
    } finally { Build.VERSION.SDK_INT = 30; Geocoder.lookup = original }
}

private fun siteFixPrefersAccuracyAndHonorsCoarsePermission() {
    Build.VERSION.SDK_INT = 31
    try {
        val activity = Activity().apply { finePermission = 0 }
        LocationSupport.requestSiteLocation(activity, 140)
        Handler.drain()
        check(LocationManager.GPS_PROVIDER in activity.manager.callbacks)
        activity.manager.callbacks.getValue(LocationManager.NETWORK_PROVIDER).accept(Location().apply {
            latitude = 40.0; accuracy = 3_000f
        })
        check(LocationSupport.pollResult().isEmpty()) { "a coarse callback must not beat a pending precise fix" }
        activity.manager.callbacks.getValue(LocationManager.GPS_PROVIDER).accept(Location().apply { accuracy = 10f })
        val result = JSONObject(LocationSupport.pollResult())
        check(result.optDouble("lat") == 41.0 && result.optDouble("accuracyM") == 10.0)
        check(activity.manager.signals.values.all { it.cancelled })

        val cached = Activity().apply { finePermission = 0 }
        cached.manager.lastKnown[LocationManager.NETWORK_PROVIDER] = Location().apply { accuracy = 3_000f }
        cached.manager.lastKnown[LocationManager.GPS_PROVIDER] = Location().apply {
            accuracy = 15f; elapsedRealtimeNanos -= 5_000_000_000L
        }
        LocationSupport.requestSiteLocation(cached, 142)
        Handler.drain()
        check(JSONObject(LocationSupport.pollResult()).optDouble("accuracyM") == 15.0)
        check(cached.manager.callbacks.isEmpty()) { "a recent precise cache outranks a newer coarse cache" }

        val rough = Activity()
        LocationSupport.requestSiteLocation(rough, 143)
        Handler.drain()
        rough.manager.callbacks.getValue(LocationManager.FUSED_PROVIDER).accept(Location().apply { accuracy = 500f })
        rough.manager.callbacks.getValue(LocationManager.NETWORK_PROVIDER).accept(Location().apply { accuracy = 4_000f })
        check(JSONObject(LocationSupport.pollResult()).optDouble("accuracyM") == 500.0)
        check(Handler.timeouts.isEmpty()) { "all providers answered, so no deadline wait is necessary" }

        val coarse = Activity()
        LocationSupport.requestSiteLocation(coarse, 141)
        Handler.drain()
        check(coarse.permissionRequests == 0 && LocationManager.GPS_PROVIDER !in coarse.manager.callbacks)
        coarse.manager.callbacks.getValue(LocationManager.NETWORK_PROVIDER).accept(Location().apply { accuracy = 2_000f })
        Handler.advanceBy(20_000)
        val fallback = JSONObject(LocationSupport.pollResult())
        check(fallback.optBoolean("fixOk") && fallback.optDouble("accuracyM") == 2_000.0)
        check(Handler.timeouts.isEmpty() && coarse.manager.signals.values.all { it.cancelled })
    } finally { Build.VERSION.SDK_INT = 30 }
}

private fun siteCacheAndPermissionBoundaries() {
    val activity = Activity().apply { permission = -1 }
    LocationSupport.requestSiteLocation(activity, 150)
    Handler.drain()
    check(activity.requestedPermissions == listOf(android.Manifest.permission.ACCESS_COARSE_LOCATION,
        android.Manifest.permission.ACCESS_FINE_LOCATION))
    activity.permission = 0
    check(LocationSupport.onRequestPermissionsResult(activity, 4303, intArrayOf(0, -1)))
    activity.manager.callback!!.accept(Location().apply { accuracy = 2_000f })
    check(JSONObject(LocationSupport.pollResult()).optBoolean("fixOk")) { "approximate-only grants remain usable" }

    val cached = Activity()
    cached.manager.lastKnown[LocationManager.NETWORK_PROVIDER] = Location().apply { elapsedRealtimeNanos -= 30_000_000_000L }
    LocationSupport.requestSiteLocation(cached, 151)
    Handler.drain()
    check(JSONObject(LocationSupport.pollResult()).optBoolean("fixOk") && cached.manager.callbacks.isEmpty())
    cached.manager.lastKnown[LocationManager.NETWORK_PROVIDER]!!.elapsedRealtimeNanos--
    LocationSupport.requestSiteLocation(cached, 152)
    Handler.drain()
    check(cached.manager.callback != null && LocationSupport.pollResult().isEmpty())
    cached.manager.callback!!.accept(cached.manager.lastKnown[LocationManager.NETWORK_PROVIDER])
    check(!JSONObject(LocationSupport.pollResult()).optBoolean("fixOk")) { "stale live results cannot rank sites either" }

    val cancelled = Activity()
    LocationSupport.requestSiteLocation(cancelled, 153)
    Handler.drain()
    LocationSupport.cancelLocationRequest(153)
    Handler.drain()
    cancelled.manager.callback!!.accept(Location())
    check(LocationSupport.pollResult().isEmpty() && cancelled.manager.signal!!.cancelled)
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
        interruptedPermissionPromptIsCancelled()
        slowPermissionGrant()
        recentCacheAvoidsThrottledRequest()
        cachedFixCompletesInsideRequestDrain()
        invalidCacheFallsBackToFreshFix()
        alternateProviderAndRetiredCallbacks()
        legacyLiveFixAndCancellation()
        siteFixIsFreshAndDoesNotGeocode()
        siteFixPrefersAccuracyAndHonorsCoarsePermission()
        siteCacheAndPermissionBoundaries()
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
