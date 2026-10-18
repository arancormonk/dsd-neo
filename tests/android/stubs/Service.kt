// SPDX-License-Identifier: GPL-3.0-or-later
package android.app

import android.content.Context
import android.content.Intent
import android.os.IBinder
import java.util.concurrent.ConcurrentLinkedQueue

open class Service : Context() {
    companion object {
        const val START_NOT_STICKY = 2
        const val STOP_FOREGROUND_REMOVE = 1
    }
    val stoppedIds = ConcurrentLinkedQueue<Int>()
    var beforeForeground: (() -> Unit)? = null
    var beforeStopSelf: ((Int) -> Unit)? = null
    var foreground = false
    open fun onBind(intent: Intent?): IBinder? = null
    open fun onCreate() {}
    open fun onDestroy() {}
    open fun onTaskRemoved(rootIntent: Intent?) {}
    open fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_NOT_STICKY
    fun startForeground(id: Int, notification: Notification, type: Int = 0) {
        beforeForeground?.invoke()
        foreground = true
    }
    fun stopForeground(flags: Int) { foreground = false }
    fun stopSelf(startId: Int) { stoppedIds.add(startId); beforeStopSelf?.invoke(startId) }
}

class Notification {
    class Builder(context: Context, channel: String) {
        fun setSmallIcon(id: Int) = this
        fun setOngoing(value: Boolean) = this
        fun setOnlyAlertOnce(value: Boolean) = this
        fun addAction(value: Action) = this
        fun setContentIntent(value: PendingIntent) = this
        fun setContentTitle(value: String) = this
        fun setContentText(value: String) = this
        fun setShowWhen(value: Boolean) = this
        fun setSubText(value: String) = this
        fun setWhen(value: Long) = this
        fun setUsesChronometer(value: Boolean) = this
        fun setStyle(value: BigTextStyle) = this
        fun build() = Notification()
    }
    class Action {
        class Builder(icon: Any?, title: String, intent: PendingIntent) { fun build() = Action() }
    }
    class BigTextStyle { fun bigText(text: String) = this }
}

class NotificationChannel(id: String, title: String, importance: Int) {
    var vibrationPattern: LongArray? = null
    fun enableVibration(on: Boolean) {}
}

class NotificationManager {
    companion object { const val IMPORTANCE_LOW = 2 }
    fun createNotificationChannel(channel: NotificationChannel) {}
    fun notify(id: Int, notification: Notification) {}
}

class PendingIntent {
    companion object {
        const val FLAG_IMMUTABLE = 1
        const val FLAG_UPDATE_CURRENT = 2
        fun getService(context: Context, code: Int, intent: Intent, flags: Int) = PendingIntent()
        fun getActivity(context: Context, code: Int, intent: Intent, flags: Int) = PendingIntent()
    }
}
