// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo
import android.app.Activity
import android.location.Address
import android.location.Geocoder
import android.location.Location
import android.os.Handler
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

private fun request(activity: Activity, id: Long) {
    LocationSupport.requestCurrentLocation(activity, id)
    Handler.drain()
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
        activity.manager.callback!!.accept(Location())
        check(started.await(5, TimeUnit.SECONDS))
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
        timeoutWithFixAndLateReplacement()
        println("PASS: location permission, teardown, fix timeout and late replacement completion")
    } finally {
        val field = LocationSupport::class.java.getDeclaredField("worker").apply { isAccessible = true }
        val queue = field.get(LocationSupport)
        val executorField = LocationGeocodeQueue::class.java.getDeclaredField("executor").apply { isAccessible = true }
        val executor = executorField.get(queue) as ThreadPoolExecutor
        executor.shutdownNow()
        check(executor.awaitTermination(5, TimeUnit.SECONDS))
    }
}
