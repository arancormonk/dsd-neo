// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo
import android.app.Activity
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.location.LocationRequest
import android.os.Build
import android.os.Handler
import android.os.SystemClock
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

private const val GPS = LocationManager.GPS_PROVIDER
private const val FUSED = LocationManager.FUSED_PROVIDER

private fun precise() = Activity().apply { finePermission = 0 }

private fun start(activity: Activity) {
    LocationStream.startStream(activity)
    Handler.drain()
}

private fun stop() {
    LocationStream.stopStream()
    Handler.drain()
}

private fun record(): JSONObject = JSONObject(checkNotNull(LocationStream.pollStream()) { "the feed has no record" })

private fun listener(activity: Activity, provider: String): LocationListener = activity.manager.updates.getValue(provider).listener

private fun fix(lat: Double = 10.0, accuracy: Float = 10f, ageMs: Long = 0, setup: Location.() -> Unit = {}) = Location().apply {
    latitude = lat
    longitude = 20.0
    this.accuracy = accuracy
    elapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos() - ageMs * 1_000_000L
    setup()
}

private fun noFix(json: JSONObject) =
    listOf("provider", "elapsedRealtimeNanos", "lat", "lon", "accuracyM", "ageMs").none { json.has(it) }

private fun <T> onSdk(sdk: Int, block: () -> T): T {
    Build.VERSION.SDK_INT = sdk
    try { return block() } finally { stop(); Build.VERSION.SDK_INT = 30 }
}

private fun epochStartsAtZero() {
    check(LocationStream.foregroundEpoch() == 0L) { "the foreground epoch starts at 0" }
    check(LocationStream.pollStream() == null) { "a feed never started has no record" }
}

// Precise access registers GPS, plus the platform fused provider from API 31 when present, at 10 s and 0 m; never the
// network provider. Approximate or no access registers nothing and says which.
private fun providersByPermissionAndApi() {
    onSdk(31) {
        val activity = precise()
        start(activity)
        check(activity.manager.updates.keys == setOf(GPS, FUSED)) { "API 31: ${activity.manager.updates.keys}" }
        for (updates in activity.manager.updates.values) {
            val request = checkNotNull(updates.request) { "API 31 uses the LocationRequest form" }
            check(request.intervalMillis == 10_000L && request.minUpdateIntervalMillis == 10_000L)
            check(request.minUpdateDistanceMeters == 0f && request.quality == LocationRequest.QUALITY_HIGH_ACCURACY)
            check(updates.executor === activity.mainExecutor) { "callbacks run on the main looper" }
        }
        val json = record()
        check(json.optString("state") == "ok" && json.optLong("generation") > 0 && noFix(json))
    }
    onSdk(31) {
        val activity = precise().apply { manager.allProviders.remove(FUSED) }
        start(activity)
        check(activity.manager.updates.keys == setOf(GPS)) { "an absent fused provider is not asked" }
    }
    onSdk(31) {
        val activity = precise().apply { manager.failingProviders.add(GPS) }
        start(activity)
        check(activity.manager.updates.keys == setOf(FUSED) && record().optString("state") == "ok") {
            "a provider that refuses does not stop the other"
        }
    }
    for (sdk in listOf(29, 30)) onSdk(sdk) {
        val activity = precise()
        start(activity)
        val updates = activity.manager.updates
        check(updates.keys == setOf(GPS)) { "API $sdk has GPS only: ${updates.keys}" }
        val legacy = updates.getValue(GPS)
        check(legacy.request == null && legacy.minTimeMs == 10_000L && legacy.minDistanceM == 0f && legacy.looper != null)
    }
    onSdk(30) {
        val activity = precise().apply { manager.allProviders.remove(GPS) }
        start(activity)
        check(activity.manager.updates.isEmpty() && record().optString("state") == "providerUnavailable")
    }
    onSdk(31) {
        val activity = precise().apply { manager.failingProviders.addAll(listOf(GPS, FUSED)) }
        start(activity)
        check(record().optString("state") == "providerUnavailable") { "no provider registered" }
    }
    onSdk(31) {
        val approximate = Activity()
        start(approximate)
        check(approximate.manager.updates.isEmpty() && record().optString("state") == "approximateOnly")
        check(noFix(record()))
    }
    onSdk(31) {
        val denied = Activity().apply { permission = -1 }
        start(denied)
        check(denied.manager.updates.isEmpty() && record().optString("state") == "noPermission")
    }
}

// Registered with location off, the feed says so and starts when it is turned on; a provider turned off takes its fix.
private fun providerStateCallbacks() {
    onSdk(31) {
        val activity = precise().apply { manager.enabled = false }
        start(activity)
        check(activity.manager.updates.keys == setOf(GPS, FUSED) && record().optString("state") == "locationOff")
        listener(activity, GPS).onProviderEnabled(GPS)
        check(record().optString("state") == "ok")
        listener(activity, GPS).onLocationChanged(fix())
        check(record().optString("provider") == GPS)
        listener(activity, GPS).onProviderDisabled(GPS)
        val off = record()
        check(off.optString("state") == "locationOff" && noFix(off)) { "a disabled provider's fix is dropped" }
        listener(activity, FUSED).onProviderEnabled(FUSED)
        listener(activity, FUSED).onLocationChanged(fix(lat = 11.0))
        listener(activity, GPS).onProviderDisabled(GPS)
        check(record().optString("state") == "ok" && record().optDouble("lat") == 11.0) { "one enabled provider is enough" }
    }
}

// The poll's choice: the most accurate fix at most 15 s old, else the newest. A newer unusable or less accurate fused
// fix never displaces a fresh GPS fix, and an older or replayed fix never replaces a newer one of its provider.
private fun bestFreshSelection() {
    onSdk(31) {
        val activity = precise()
        start(activity)
        val gps = listener(activity, GPS)
        val fused = listener(activity, FUSED)
        gps.onLocationChanged(fix(lat = 10.0, accuracy = 5f))
        val gpsNanos = SystemClock.elapsedRealtimeNanos()
        Handler.advanceBy(2_000)
        fused.onLocationChanged(fix(lat = 10.5, accuracy = 20f))
        var json = record()
        check(json.optString("provider") == GPS && json.optDouble("accuracyM") == 5.0 && json.optLong("ageMs") == 2_000L)
        check(json.optLong("elapsedRealtimeNanos") == gpsNanos && json.optDouble("lon") == 20.0)
        for (unusable in listOf<Location.() -> Unit>(
            { latitude = Double.NaN }, { latitude = 91.0 }, { longitude = -181.0 }, { longitude = Double.POSITIVE_INFINITY },
            { accuracyAvailable = false }, { accuracy = 0f }, { accuracy = Float.NaN },
            { elapsedRealtimeNanos = 0 }, { elapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos() + 1 },
        )) {
            fused.onLocationChanged(fix(lat = 12.0, accuracy = 1f, setup = unusable))
            check(record().optString("provider") == GPS) { "an unusable fused fix displaced a fresh GPS fix" }
        }
        // GPS is now 16 s old: the fused fix (14 s) is the only fresh one, and the unusable fixes left it as it was.
        Handler.advanceBy(14_000)
        json = record()
        check(json.optString("provider") == FUSED && json.optDouble("lat") == 10.5 && json.optDouble("accuracyM") == 20.0)
        // Both stale: the newest.
        Handler.advanceBy(10_000)
        check(record().optString("provider") == FUSED && record().optLong("ageMs") == 24_000L)
        // A fresh but rough GPS fix beats a stale precise one.
        gps.onLocationChanged(fix(lat = 13.0, accuracy = 50f))
        check(record().optString("provider") == GPS && record().optDouble("lat") == 13.0)
        // Equal accuracy, both fresh: the newer.
        Handler.advanceBy(1_000)
        fused.onLocationChanged(fix(lat = 14.0, accuracy = 50f))
        check(record().optString("provider") == FUSED && record().optDouble("lat") == 14.0)
        // An older GPS fix than the one held, and the held one replayed, change nothing.
        gps.onLocationChanged(fix(lat = 15.0, accuracy = 1f, ageMs = 2_000))
        fused.onLocationChanged(fix(lat = 16.0, accuracy = 1f))
        check(record().optDouble("lat") == 14.0) { "a replayed fix (same time) is not a new one" }
        Handler.advanceBy(3_000)
        check(record().optLong("ageMs") == 3_000L) { "the age is taken at the poll" }
    }
}

// Each start that registers is a new generation; a stop answers null at once and removes the listeners; callbacks of
// an older subscription are ignored; a second start while registered keeps the subscription.
private fun generations() {
    onSdk(30) {
        val activity = precise()
        start(activity)
        val first = record().optLong("generation")
        val old = listener(activity, GPS)
        start(activity)
        check(record().optLong("generation") == first && activity.manager.removedListeners.isEmpty()) {
            "a start while registered keeps the subscription"
        }
        LocationStream.stopStream()
        check(LocationStream.pollStream() == null) { "a stop answers at once, before the main looper runs" }
        // A fix that reaches the listener before the queued removal publishes nothing.
        old.onLocationChanged(fix())
        check(LocationStream.pollStream() == null) { "a fix between a stop and its removal published a record" }
        Handler.drain()
        check(old in activity.manager.removedListeners)
        old.onLocationChanged(fix())
        check(LocationStream.pollStream() == null) { "a stopped feed's callbacks publish nothing" }

        start(activity)
        val second = record().optLong("generation")
        check(second > first)
        old.onLocationChanged(fix())
        old.onProviderDisabled(GPS)
        check(noFix(record()) && record().optString("state") == "ok") { "an older generation's callbacks are ignored" }
        listener(activity, GPS).onLocationChanged(fix())
        check(record().optLong("generation") == second && record().optString("provider") == GPS)
        stop()

        // A stop before the main looper runs the start: nothing registers.
        val quick = precise()
        LocationStream.startStream(quick)
        LocationStream.stopStream()
        Handler.drain()
        check(quick.manager.updates.isEmpty() && LocationStream.pollStream() == null)
        // A start after the stop, both still queued: the newer start registers.
        LocationStream.stopStream()
        LocationStream.startStream(quick)
        Handler.drain()
        check(quick.manager.updates.keys == setOf(GPS) && record().optLong("generation") > second)
    }
}

// Every onStop advances the foreground epoch and stops the feed; a start registers only while the app is visible, and a
// start queued before a stop never runs after it, even when the app is back before the main looper gets to it.
private fun onStopEndsTheFeed() {
    onSdk(30) {
        val activity = precise()
        start(activity)
        val registered = listener(activity, GPS)
        LocationStream.onActivityStop()
        check(LocationStream.foregroundEpoch() == 1L && LocationStream.pollStream() == null)
        check(registered in activity.manager.removedListeners) { "onStop removes the listeners itself" }

        val hidden = precise()
        start(hidden)
        check(hidden.manager.updates.isEmpty()) { "no feed while the app is not visible" }

        LocationStream.onActivityStart()
        check(hidden.manager.updates.keys == setOf(GPS)) { "the start asked for while hidden registers on onStart" }
        val raced = precise()
        LocationStream.startStream(raced)
        LocationStream.onActivityStop()
        LocationStream.onActivityStart()
        Handler.drain()
        check(raced.manager.updates.isEmpty() && LocationStream.foregroundEpoch() == 2L) {
            "a start from before the stop must not run after it"
        }

        start(activity)
        check(record().optString("state") == "ok") { "Qt restarts it on resume" }
        LocationStream.onDestroy()
        check(LocationStream.pollStream() == null && LocationStream.foregroundEpoch() == 2L)
        LocationStream.onActivityStart()
    }
}

// The return to the foreground after Qt asked for the feed between onStop and its thread's freeze (D1 of the emulator
// run): the start waits for onStart and registers then; a stop or a newer start in between replaces it, and an onStop
// or onDestroy in between drops it.
private fun startWhileHiddenRunsOnStart() {
    onSdk(30) {
        val first = precise()
        start(first)
        val before = record().optLong("generation")
        LocationStream.onActivityStop()
        val epoch = LocationStream.foregroundEpoch()

        // Qt sees the epoch move and asks again while the app is hidden.
        val asked = precise()
        start(asked)
        check(asked.manager.updates.isEmpty() && LocationStream.pollStream() == null) { "nothing registers while hidden" }
        LocationStream.onActivityStart()
        check(asked.manager.updates.keys == setOf(GPS)) { "the start registers on onStart" }
        check(record().optString("state") == "ok" && record().optLong("generation") > before)
        check(LocationStream.foregroundEpoch() == epoch) { "onStart leaves the epoch" }
        listener(asked, GPS).onLocationChanged(fix())
        check(record().optString("provider") == GPS) { "the registered feed publishes its fixes" }

        // A stop between onStop and onStart cancels the start.
        LocationStream.onActivityStop()
        val cancelled = precise()
        start(cancelled)
        stop()
        LocationStream.onActivityStart()
        check(cancelled.manager.updates.isEmpty() && LocationStream.pollStream() == null) { "a stop cancels it" }

        // Of two starts asked for while hidden, the newer registers.
        LocationStream.onActivityStop()
        val older = precise()
        val newer = precise()
        start(older)
        start(newer)
        LocationStream.onActivityStart()
        check(older.manager.updates.isEmpty() && newer.manager.updates.keys == setOf(GPS)) { "the newer start wins" }

        // A start kept for one onStart never runs after a later onStop, nor after onDestroy.
        LocationStream.onActivityStop()
        val stale = precise()
        start(stale)
        LocationStream.onActivityStop()
        LocationStream.onActivityStart()
        check(stale.manager.updates.isEmpty() && LocationStream.pollStream() == null) { "a later onStop drops it" }
        LocationStream.onActivityStop()
        val destroyed = precise()
        start(destroyed)
        LocationStream.onDestroy()
        LocationStream.onActivityStart()
        check(destroyed.manager.updates.isEmpty() && LocationStream.pollStream() == null) { "onDestroy drops it" }
    }
}

/** The record is written under the lock on the main looper, so the Qt thread's poll takes the same monitor. */
private fun pollSharesTheWritersLock() {
    val lock = LocationStream::class.java.getDeclaredField("lock").apply { isAccessible = true }.get(LocationStream)
    val polled = CountDownLatch(1)
    val poller = Thread {
        LocationStream.pollStream()
        polled.countDown()
    }
    synchronized(lock) {
        poller.start()
        check(!polled.await(200, TimeUnit.MILLISECONDS)) { "pollStream() ran while a writer held the lock" }
    }
    check(polled.await(5, TimeUnit.SECONDS)) { "pollStream() finished once the writer let go" }
    poller.join()
}

fun main() {
    epochStartsAtZero()
    LocationStream.onActivityStart()
    providersByPermissionAndApi()
    providerStateCallbacks()
    bestFreshSelection()
    generations()
    onStopEndsTheFeed()
    startWhileHiddenRunsOnStart()
    pollSharesTheWritersLock()
    println(
        "PASS: location feed providers, provider state, best fresh fix, generations, onStop, a start while hidden " +
            "and the poll's lock"
    )
}
