// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.provider.Settings
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Facts about this process and the device for the persistent diagnostics (issue #745).
 *
 * After a phone restarts, a diagnostics share has to tell whether Android restarted while a
 * session ran or only the app process ended. Android records app exits but not restarts of the
 * phone itself, so the process line carries the boot count, and each session's start line carries
 * it again. Native code calls the three static methods below (see android/decoder_host_android.cpp);
 * R8 keeps them by name.
 */
object ProcessFacts {
    @Volatile
    private var appContext: Context? = null

    /** Called from the Activity's and the service's onCreate, before native code can ask. */
    @JvmStatic
    fun attach(context: Context) {
        if (appContext == null) {
            appContext = context.applicationContext
        }
    }

    /** Extra facts for the diagnostics process line: boot count, device and SDK (native code adds the app version). */
    @JvmStatic
    fun processFacts(): String =
        listOf(bootPart(appContext), "${Build.MANUFACTURER} ${Build.MODEL}", "SDK ${Build.VERSION.SDK_INT}").joinToString(", ")

    /** The newest recorded exits of this app's processes, newest first (Android 11+; empty before). */
    @JvmStatic
    fun exitLines(): Array<String> {
        val context = appContext ?: return emptyArray()
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return emptyArray()
        }
        return try {
            val manager = context.getSystemService(ActivityManager::class.java) ?: return emptyArray()
            val records = manager.getHistoricalProcessExitReasons(context.packageName, 0, ExitFacts.MAX_LINES).map {
                ExitFacts.Record(it.timestamp, it.pid, it.reason, it.status, it.importance, it.pss, it.description)
            }
            val format = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
            ExitFacts.lines(records) { format.format(Date(it)) }.toTypedArray()
        } catch (_: RuntimeException) {
            emptyArray()
        }
    }

    /** What a session start line says about the attempt: source, boot count and, for a dongle, which one. */
    @JvmStatic
    fun attemptContext(sourceKind: String): String {
        val parts = mutableListOf(sourceKind.ifEmpty { "unknown source" }, bootPart(appContext))
        if (sourceKind == "usb" || sourceKind == "airspy") {
            UsbSourceManager.diagnosticsLabel(sourceKind)?.let { parts += it }
        }
        return parts.joinToString(", ")
    }

    private fun bootPart(context: Context?): String = try {
        if (context == null) "boot ?" else "boot ${Settings.Global.getInt(context.contentResolver, Settings.Global.BOOT_COUNT)}"
    } catch (_: Settings.SettingNotFoundException) {
        "boot ?"
    }
}
