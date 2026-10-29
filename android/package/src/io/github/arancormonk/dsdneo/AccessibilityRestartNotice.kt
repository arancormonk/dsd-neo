// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.Activity
import android.app.AlertDialog
import android.util.Log
import android.view.WindowManager

/**
 * [AccessibilityBridge]'s platform side (#782): the notice that TalkBack, turned on after launch, can read DSD-neo only
 * once the app is closed and opened again, and the host diagnostics records.
 *
 * The dialog is native on purpose: with Qt's accessibility bridge off, nothing in the Qt UI can be heard. It has one OK
 * button; the app never closes itself. Back also acknowledges it, so it never traps the user, while a touch outside it
 * does not, since explore-by-touch touches the screen to read it.
 *
 * Main thread only.
 */
object AccessibilityRestartNotice : AccessibilityBridge.Effects {
    private const val TAG = "dsd-neo"

    private var dialog: AlertDialog? = null

    override fun showNotice(activity: Activity): Boolean {
        dismissNotice()
        val built = AlertDialog.Builder(activity)
            .setTitle(R.string.a11y_restart_title)
            .setMessage(R.string.a11y_restart_message)
            .setPositiveButton(android.R.string.ok) { _, _ -> acknowledged() }
            .setOnCancelListener { acknowledged() }
            .create()
        built.setCanceledOnTouchOutside(false)
        return try {
            built.show()
            dialog = built
            true
        } catch (e: WindowManager.BadTokenException) {
            Log.w(TAG, "accessibility notice not shown: ${e.javaClass.simpleName}")
            false
        }
    }

    override fun dismissNotice() {
        val showing = dialog ?: return
        dialog = null
        showing.dismiss()
    }

    override fun record(text: String) {
        HostDiagnostics.submit(text)
    }

    private fun acknowledged() {
        dialog = null
        AccessibilityBridge.noticeAcknowledged()
    }
}
