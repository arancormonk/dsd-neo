// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo
import android.Manifest
import android.app.Activity
import android.location.LocationManager
import android.os.Handler
import android.provider.Settings
import org.json.JSONObject

private const val CODE = 4303
private val PRECISE_AND_COARSE = listOf(Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION)

private var nextId = 1L

private fun askRoaming(activity: Activity, id: Long = nextId++) {
    LocationPermission.requestRoamingPermission(activity, id)
    Handler.drain()
}

private fun startFeed(activity: Activity) {
    LocationStream.startStream(activity)
    Handler.drain()
}

private fun stopFeed() {
    LocationStream.stopStream()
    Handler.drain()
}

private fun feed(): JSONObject = JSONObject(checkNotNull(LocationStream.pollStream()) { "the feed has no record" })

private fun answer(activity: Activity, vararg grants: Int) =
    check(LocationPermission.onRequestPermissionsResult(activity, CODE, grants)) { "the broker claims its code" }

private fun noAccess() = Activity().apply { permission = -1 }

/** What the user picked in the dialog, as the permission checks then read it. */
private fun Activity.grant(coarse: Boolean, fine: Boolean) {
    permission = if (coarse) 0 else -1
    finePermission = if (fine) 0 else -1
}

private fun requestCodes() {
    val activity = Activity()
    check(!LocationPermission.onRequestPermissionsResult(activity, CODE + 1, intArrayOf(0))) { "other codes go on" }
    check(LocationPermission.onRequestPermissionsResult(activity, CODE, intArrayOf(0))) {
        "a late answer to no open prompt is still the broker's"
    }
    check(LocationSupport.onRequestPermissionsResult(activity, CODE, intArrayOf(0))) { "the old entry point hands over" }
}

// Precise access is asked with coarse first, so grants[0] is coarse whoever opened the prompt; the answer starts a
// waiting feed inside the result callback itself, without onStart.
private fun preciseAnswerStartsTheFeedAtOnce() {
    val activity = noAccess()
    startFeed(activity)
    val before = feed()
    check(before.optString("state") == "noPermission" && activity.manager.updates.isEmpty())
    askRoaming(activity)
    check(activity.permissionRequests == 1 && activity.requestedPermissions == PRECISE_AND_COARSE)
    activity.grant(coarse = true, fine = true)
    answer(activity, 0, 0)
    check(LocationManager.GPS_PROVIDER in activity.manager.updates) { "a precise answer registers at once" }
    val after = feed()
    check(after.optString("state") == "ok" && after.optLong("generation") > before.optLong("generation"))
    stopFeed()
}

// Coarse already allowed is not enough for roaming: it asks for precise and coarse again (Android's upgrade dialog),
// while a one-shot request with coarse allowed still asks nothing.
private fun coarseAlreadyGrantedAsksAgain() {
    val activity = Activity()
    startFeed(activity)
    check(feed().optString("state") == "approximateOnly")
    askRoaming(activity)
    check(activity.permissionRequests == 1 && activity.requestedPermissions == PRECISE_AND_COARSE)
    // The user keeps approximate, and the request that offered precise does not ask again by itself.
    val id = nextId - 1
    answer(activity, 0, -1)
    check(feed().optString("state") == "approximateOnly" && activity.manager.updates.isEmpty())
    askRoaming(activity, id)
    check(activity.permissionRequests == 1) { "a prompt that offered precise latches its request id" }

    LocationSupport.requestCurrentLocation(activity, 500)
    Handler.drain()
    check(activity.permissionRequests == 1 && activity.manager.callback != null) { "coarse allowed: one-shot asks nothing" }
    LocationSupport.cancelLocationRequest(500)
    Handler.drain()
    stopFeed()
}

// A denial leaves the feed without access; asking again (a new id) prompts again even when Android answers at once
// ("don't ask again"); repeating an id never prompts again by itself.
private fun denialAndDontAskAgain() {
    val activity = noAccess()
    startFeed(activity)
    val id = nextId++
    askRoaming(activity, id)
    answer(activity, -1, -1)
    check(feed().optString("state") == "noPermission" && activity.manager.updates.isEmpty())
    askRoaming(activity, id)
    check(activity.permissionRequests == 1) { "the same request id asks once" }
    askRoaming(activity)
    check(activity.permissionRequests == 2) { "the broker never latches a denial" }
    // Don't ask again: Android answers denied without showing anything.
    answer(activity, -1, -1)
    check(feed().optString("state") == "noPermission")
    stopFeed()
}

// An interrupted prompt (empty answers) is a cancellation: the feed is left as it was, and the next ask prompts again.
// A one-shot request waiting on roaming's prompt ends cancelled, as it does on its own.
private fun interruptedPromptIsCancelled() {
    val activity = noAccess()
    startFeed(activity)
    val generation = feed().optLong("generation")
    askRoaming(activity)
    LocationSupport.requestSiteLocation(activity, 600)
    Handler.drain()
    check(activity.permissionRequests == 1) { "the one-shot request waits on the open prompt" }
    answer(activity)
    check(feed().optLong("generation") == generation && feed().optString("state") == "noPermission") {
        "a cancelled prompt changes nothing"
    }
    val cancelled = JSONObject(LocationSupport.pollResult())
    check(cancelled.optLong("id") == 600L && cancelled.optBoolean("cancelled"))
    askRoaming(activity)
    check(activity.permissionRequests == 2) { "a cancelled prompt does not block the next" }
    answer(activity, -1, -1)
    stopFeed()
}

// Roaming turned off during the prompt: the answer starts nothing, whether the stop has reached the main looper or not.
private fun disableDuringPrompt() {
    for (stopQueued in listOf(false, true)) {
        val activity = noAccess()
        startFeed(activity)
        askRoaming(activity)
        LocationStream.stopStream()
        if (!stopQueued) Handler.drain()
        activity.grant(coarse = true, fine = true)
        answer(activity, 0, 0)
        Handler.drain()
        check(activity.manager.updates.isEmpty() && LocationStream.pollStream() == null) {
            "an answer after a stop started the feed (stop queued: $stopQueued)"
        }
    }
}

// One prompt serves both users: whichever opens it, the other waits on it and both get the answer.
private fun sharedPrompt() {
    for (roamingFirst in listOf(true, false)) {
        val activity = noAccess()
        startFeed(activity)
        if (roamingFirst) askRoaming(activity)
        LocationSupport.requestSiteLocation(activity, 700)
        Handler.drain()
        if (!roamingFirst) askRoaming(activity)
        check(activity.permissionRequests == 1) { "one prompt at a time (roaming first: $roamingFirst)" }
        check(activity.requestedPermissions == PRECISE_AND_COARSE) { "both ask for precise and coarse" }
        activity.grant(coarse = true, fine = true)
        answer(activity, 0, 0)
        check(LocationManager.GPS_PROVIDER in activity.manager.updates) { "the feed got the answer" }
        check(activity.manager.callbacks.isNotEmpty()) { "the one-shot request got the answer" }
        LocationSupport.cancelLocationRequest(700)
        Handler.drain()
        stopFeed()
    }
    // A Use my location prompt asks coarse only; a roaming ask while it is open waits on it rather than prompting. That
    // prompt never offered precise, so the same roaming request may ask for it afterwards.
    val activity = noAccess()
    startFeed(activity)
    LocationSupport.requestCurrentLocation(activity, 710)
    Handler.drain()
    val id = nextId++
    askRoaming(activity, id)
    check(activity.permissionRequests == 1 && activity.requestedPermissions == listOf(Manifest.permission.ACCESS_COARSE_LOCATION))
    activity.grant(coarse = true, fine = false)
    answer(activity, 0)
    check(feed().optString("state") == "approximateOnly")
    LocationSupport.cancelLocationRequest(710)
    Handler.drain()
    askRoaming(activity, id)
    check(activity.permissionRequests == 2 && activity.requestedPermissions == PRECISE_AND_COARSE) {
        "a roaming request that joined a coarse-only prompt asks for precise itself"
    }
    activity.grant(coarse = true, fine = true)
    answer(activity, 0, 0)
    check(feed().optString("state") == "ok")
    stopFeed()
}

// Another activity's answer, and one for a prompt forgotten on destroy, are dropped.
private fun foreignAndDestroyedAnswers() {
    val activity = noAccess()
    startFeed(activity)
    askRoaming(activity)
    val other = Activity().apply { grant(coarse = true, fine = true) }
    activity.grant(coarse = true, fine = true)
    answer(other, 0, 0)
    check(activity.manager.updates.isEmpty()) { "another activity's answer is not this prompt's" }
    LocationPermission.onDestroy(activity)
    answer(activity, 0, 0)
    check(activity.manager.updates.isEmpty()) { "a destroyed activity's prompt is forgotten" }
    stopFeed()
    val next = noAccess()
    askRoaming(next)
    check(next.permissionRequests == 1) { "a forgotten prompt does not block the next" }
    answer(next, -1, -1)
}

// Precise allowed in Settings while the feed waited: the ask prompts nothing and the feed registers now.
private fun alreadyPrecise() {
    val activity = noAccess()
    startFeed(activity)
    activity.grant(coarse = true, fine = true)
    askRoaming(activity)
    check(activity.permissionRequests == 0 && LocationManager.GPS_PROVIDER in activity.manager.updates)
    check(feed().optString("state") == "ok")
    stopFeed()
}

private fun settingsScreens() {
    val activity = Activity()
    LocationPermission.openAppLocationSettings(activity)
    LocationPermission.openLocationSourceSettings(activity)
    check(activity.started.isEmpty()) { "Settings opens from the main looper" }
    Handler.drain()
    check(activity.started.map { it.action } == listOf(
        Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Settings.ACTION_LOCATION_SOURCE_SETTINGS))
    check(activity.started[0].data.toString() == "package:io.github.arancormonk.dsdneo" && activity.started[1].data == null)
    activity.startFailure = IllegalStateException("no Settings app")
    LocationPermission.openAppLocationSettings(activity)
    LocationPermission.openLocationSourceSettings(activity)
    Handler.drain()
    check(activity.started.size == 2) { "a missing Settings screen is not an error" }
}

fun main() {
    LocationStream.onActivityStart()
    requestCodes()
    preciseAnswerStartsTheFeedAtOnce()
    coarseAlreadyGrantedAsksAgain()
    denialAndDontAskAgain()
    interruptedPromptIsCancelled()
    disableDuringPrompt()
    sharedPrompt()
    foreignAndDestroyedAnswers()
    alreadyPrecise()
    settingsScreens()
    println("PASS: location prompt codes, precise and approximate answers, denial, cancellation, disable, sharing, Settings")
}
