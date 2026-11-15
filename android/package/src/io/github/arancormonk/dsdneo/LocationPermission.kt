// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Handler
import android.os.Looper
import android.provider.Settings

/**
 * The app's one runtime location prompt, shared by the one-shot requests ([LocationSupport]) and GPS roaming
 * ([LocationStream], #786), and the two Settings screens roaming sends the user to. All state belongs to the main looper.
 *
 * One prompt is open at a time, under one request code. An asker that comes while it is open waits for its answer
 * instead of opening another; each asker keeps one place in the queue, so asking again replaces the earlier wait. The
 * answer goes to every waiter at once, in [onRequestPermissionsResult] itself: nothing waits for `onStart`. Every
 * prompt lists coarse location first, so `grants[0]` is coarse whichever asker opened it. Roaming always asks for both
 * precise and coarse location, also when coarse is already allowed: that is how Android offers the upgrade to precise.
 */
object LocationPermission {
    const val REQUEST_CODE = 4303
    private val main = Handler(Looper.getMainLooper())
    private var promptActivity: Activity? = null
    private var promptPermissions = emptyList<String>()
    private var roamingId: Long? = null
    private val waiters = linkedMapOf<Asker, (Activity, IntArray) -> Unit>()

    /** Who waits for an answer. */
    internal enum class Asker { ONE_SHOT, ROAMING }

    internal fun granted(context: Context, permission: String) =
        context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    /**
     * On the main looper: opens the prompt for [permissions] unless one is open, and waits for its answer. Returns what
     * the prompt that will answer asks for, which is less than [permissions] when an open prompt asked for less.
     */
    internal fun ask(
        activity: Activity, permissions: Array<String>, asker: Asker, answer: (Activity, IntArray) -> Unit,
    ): List<String> {
        waiters.remove(asker)
        waiters[asker] = answer
        if (promptActivity != null) return promptPermissions
        promptActivity = activity
        promptPermissions = permissions.toList()
        activity.requestPermissions(permissions, REQUEST_CODE)
        return promptPermissions
    }

    /**
     * Roaming's prompt for precise location, once per request [id]: the caller makes a new id for each thing the user
     * does, so a repeated call can never put the prompt up again by itself, nor loop on Android's instant "don't ask
     * again" denial. Roaming needs no answer of its own: the stream reads the outcome from the permissions themselves.
     */
    @JvmStatic fun requestRoamingPermission(activity: Activity, id: Long) {
        main.post {
            if (roamingId == id) return@post
            roamingId = id
            if (granted(activity, Manifest.permission.ACCESS_FINE_LOCATION)) {
                // Allowed in Settings while away: nothing to ask, and a stream waiting for it starts now.
                LocationStream.permissionAnswered()
                return@post
            }
            val permissions = arrayOf(Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION)
            var offeredPrecise = true
            val asked = ask(activity, permissions, Asker.ROAMING) { answered, grants ->
                // A coarse-only prompt that was already open (Use my location) never offered precise: the same id
                // may ask again.
                if (!offeredPrecise && !granted(answered, Manifest.permission.ACCESS_FINE_LOCATION) && roamingId == id) {
                    roamingId = null
                }
                // An interrupted prompt changes nothing; the user can ask again.
                if (grants.isNotEmpty()) LocationStream.permissionAnswered()
            }
            // Set before any answer can come: answers arrive later, on this looper.
            offeredPrecise = Manifest.permission.ACCESS_FINE_LOCATION in asked
        }
    }

    /** Claims [REQUEST_CODE]. An answer for another activity's prompt, or for one forgotten in [onDestroy], is dropped. */
    @JvmStatic fun onRequestPermissionsResult(activity: Activity, code: Int, grants: IntArray): Boolean {
        if (code != REQUEST_CODE) return false
        if (promptActivity !== activity) return true
        promptActivity = null
        val answered = waiters.values.toList()
        waiters.clear()
        answered.forEach { it(activity, grants) }
        return true
    }

    /** Forgets [activity]'s open prompt and who waits for it. */
    @JvmStatic fun onDestroy(activity: Activity) {
        if (promptActivity !== activity) return
        promptActivity = null
        waiters.clear()
    }

    /** App info, where location access and precise location are allowed. */
    @JvmStatic fun openAppLocationSettings(activity: Activity) {
        main.post { SettingsScreens.open(activity, SettingsScreens.appDetails(activity.packageName)) }
    }

    /** The system's Location page, where location is turned on. */
    @JvmStatic fun openLocationSourceSettings(activity: Activity) {
        main.post { SettingsScreens.open(activity, Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)) }
    }
}
