// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.provider.Settings

/** The Android Settings screens the app sends the user to: the file browser (#830) and GPS roaming (#786). */
internal object SettingsScreens {
    /** The app's own App info page, where its permissions are allowed. */
    fun appDetails(packageName: String) =
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName"))

    /** Opens [intent]; false when no screen opened. Settings sends no result, so there is nothing to wait for. */
    fun open(activity: Activity, intent: Intent): Boolean = try {
        activity.startActivity(intent)
        true
    } catch (_: Exception) {
        false
    }
}
