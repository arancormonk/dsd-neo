// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.Context
import android.graphics.Rect
import android.view.WindowInsets
import android.content.pm.PackageManager
import android.os.Build
import android.util.DisplayMetrics
import android.util.TypedValue
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.view.WindowInsetsController
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/** Platform odds and ends the Qt host reaches through QJniObject. */
object AppSupport {
    private const val REQUEST_NOTIFICATIONS = 4711
    fun ownsPermissionRequest(requestCode: Int): Boolean = requestCode == REQUEST_NOTIFICATIONS
    private val pendingBack = AtomicInteger(0)
    private val visibleKeyboardTop = AtomicInteger(-1)
    private val pendingNotificationPermissionRefresh = AtomicBoolean(true)

    // Called on the Activity thread by its layout observer. Frame coordinates
    // are physical pixels; the Qt host converts them to window logical units.
    fun updateKeyboardBounds(activity: Activity) {
        val view = activity.window.decorView
        val frame = Rect()
        view.getWindowVisibleDisplayFrame(frame)
        val position = IntArray(2)
        view.getLocationOnScreen(position)
        val occluded = view.height + position[1] - frame.bottom
        val visible = if (Build.VERSION.SDK_INT >= 30)
            view.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) == true
        else occluded > view.height / 5
        visibleKeyboardTop.set(if (visible) frame.bottom - position[1] else -1)
    }

    @JvmStatic
    fun keyboardTopPixels(): Int = visibleKeyboardTop.get()


    fun requestBack() { pendingBack.incrementAndGet() }

    @JvmStatic
    fun takeBackRequests(): Int = pendingBack.getAndSet(0)

    // What the font table depends on: the font scale and the density. Read on every UI tick to notice a change.
    @JvmStatic
    fun fontKey(context: Context): FloatArray {
        val config = context.resources.configuration
        return floatArrayOf(config.fontScale, config.densityDpi.toFloat())
    }

    // The font scale, the density, then the physical pixels for 0..128 sp, from one consistent read. Empty while
    // Android is part-way through updating the resources: the configuration changed during the read, or the
    // metrics do not match its density yet. The caller tries again on its next tick.
    @JvmStatic
    fun fontTable(context: Context): FloatArray {
        val resources = context.resources
        val config = resources.configuration
        val fontScale = config.fontScale
        val densityDpi = config.densityDpi
        val metrics = resources.displayMetrics
        if (kotlin.math.abs(metrics.density * DisplayMetrics.DENSITY_DEFAULT - densityDpi) > 0.5f) return FloatArray(0)
        val table = FloatArray(2 + FONT_TABLE_SIZE)
        table[0] = fontScale
        table[1] = densityDpi.toFloat()
        for (sp in 0 until FONT_TABLE_SIZE) {
            table[2 + sp] = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, sp.toFloat(), metrics)
        }
        val after = resources.configuration
        if (after.fontScale != fontScale || after.densityDpi != densityDpi) return FloatArray(0)
        return table
    }

    // Must match PresentationScale::kFontTableMaxSp + 1 (android/presentation_scale.h).
    private const val FONT_TABLE_SIZE = 129

    @JvmStatic
    fun fontPixels(context: Context, sp: Float): Float =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, sp, context.resources.displayMetrics)

    // The density Qt's display listener reads, from the Activity's resources (Qt 6.11.3 QtDisplayManager); 0 for a
    // Service or Application context, which has no display of its own to follow.
    @JvmStatic
    fun activityDensity(context: Context): Float =
        (context as? Activity)?.resources?.displayMetrics?.density ?: 0f

    @JvmStatic
    fun setDarkAppearance(activity: Activity, dark: Boolean) {
        if (Build.VERSION.SDK_INT >= 30) {
            val mask = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS or
                WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
            activity.window.insetsController?.setSystemBarsAppearance(if (dark) 0 else mask, mask)
        } else {
            @Suppress("DEPRECATION")
            val mask = android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or
                android.view.View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
            @Suppress("DEPRECATION")
            activity.window.decorView.systemUiVisibility = if (dark)
                activity.window.decorView.systemUiVisibility and mask.inv()
            else activity.window.decorView.systemUiVisibility or mask
        }
    }

    @JvmStatic
    fun audioRouteName(context: Context, id: Int): String {
        if (id < 0) return "No device audio output"
        if (id == 0) return "System default"
        val manager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val device = manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.id == id }
        return when (device?.type) {
            AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Speaker"
            AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "Earpiece"
            AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Wired headphones"
            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP, AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
            AudioDeviceInfo.TYPE_BLE_HEADSET, AudioDeviceInfo.TYPE_BLE_SPEAKER -> "Bluetooth"
            AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "USB audio"
            else -> "System output"
        }
    }

    // Activity callbacks invalidate the reading; the host publishes on the Qt poll thread.
    fun refreshNotificationPermission() { pendingNotificationPermissionRefresh.set(true) }

    @JvmStatic
    fun takeNotificationPermissionRefresh(): Boolean = pendingNotificationPermissionRefresh.getAndSet(false)

    @JvmStatic
    fun notificationPermissionNeeded(context: Context): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED

    /**
     * Ask for POST_NOTIFICATIONS on API 33+. The service runs without it, but its
     * notification stays invisible, which reads as "nothing happened".
     */
    @JvmStatic
    fun ensureNotificationPermission(activity: Activity) {
        if (!notificationPermissionNeeded(activity)) return
        activity.requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
    }

    /**
     * Finish the app for a committed backup restore (#580); called on the main thread.
     *
     * DsdNeoActivity.onDestroy then stops the engine and the service and ends the process, as for any other finish,
     * so the next launch applies the restore before anything reads the stores it replaces.
     */
    @JvmStatic
    fun finishApp(context: Context) {
        (context as? Activity)?.finishAndRemoveTask()
    }
}
