// SPDX-License-Identifier: GPL-3.0-or-later
package android.app

import android.content.Context
import android.content.Intent
import android.os.IBinder
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CopyOnWriteArrayList

// The platform's side of a started service, as far as the notification is concerned. startForeground() posts under
// the id, stopForeground(REMOVE) takes it down, and stopSelf(id) for the newest delivered start brings the service
// down as ActiveServices does: the foreground notification is cancelled at once, and onDestroy() follows later, when
// the test delivers it. Each instance is its own service token, so one instance's stopSelf() never stops another.
open class Service : Context() {
    companion object {
        const val START_NOT_STICKY = 2
        const val STOP_FOREGROUND_REMOVE = 1
    }
    val stoppedIds = ConcurrentLinkedQueue<Int>()
    var beforeForeground: (() -> Unit)? = null
    var beforeStopSelf: ((Int) -> Unit)? = null
    @Volatile var foreground = false
    /** The newest start id handed to this instance through [deliver]; ActiveServices' lastStartId. */
    @Volatile var lastDeliveredId = 0
    private var foregroundId = 0
    open fun onBind(intent: Intent?): IBinder? = null
    open fun onCreate() {}
    open fun onDestroy() {}
    open fun onTaskRemoved(rootIntent: Intent?) {}
    open fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_NOT_STICKY
    /** Delivers a start command as ActivityThread does, after the platform has recorded its id. */
    fun deliver(intent: Intent?, startId: Int): Int {
        lastDeliveredId = startId
        return onStartCommand(intent, 0, startId)
    }
    fun startForeground(id: Int, notification: Notification, type: Int = 0) {
        beforeForeground?.invoke()
        foregroundId = id
        notifications.platformPost(id, notification)
        foreground = true
    }
    fun stopForeground(flags: Int) {
        if (foreground && flags == STOP_FOREGROUND_REMOVE) notifications.platformRemove(foregroundId)
        foreground = false
    }
    fun stopSelf(startId: Int) {
        stoppedIds.add(startId)
        beforeStopSelf?.invoke(startId)
        if (startId == lastDeliveredId) {
            bringDown()
        }
    }
    /** The platform bringing this service down, as stopSelf() or another component's stopService() does. */
    fun bringDown() {
        if (foreground) {
            foreground = false
            notifications.platformRemove(foregroundId)
        }
    }
}

class Notification(val actions: List<Action> = emptyList()) {
    class Builder(context: Context, channel: String) {
        private val actions = mutableListOf<Action>()
        fun setSmallIcon(id: Int) = this
        fun setOngoing(value: Boolean) = this
        fun setOnlyAlertOnce(value: Boolean) = this
        fun addAction(value: Action) = apply { actions += value }
        fun setContentIntent(value: PendingIntent) = this
        fun setContentTitle(value: String) = this
        fun setContentText(value: String) = this
        fun setShowWhen(value: Boolean) = this
        fun setSubText(value: String) = this
        fun setWhen(value: Long) = this
        fun setUsesChronometer(value: Boolean) = this
        fun setStyle(value: BigTextStyle) = this
        fun build() = Notification(actions.toList())
    }
    class Action(val intent: PendingIntent) {
        class Builder(icon: Any?, title: String, private val intent: PendingIntent) { fun build() = Action(intent) }
    }
    class BigTextStyle { fun bigText(text: String) = this }
}

class NotificationChannel(id: String, title: String, importance: Int) {
    var vibrationPattern: LongArray? = null
    fun enableVibration(on: Boolean) {}
}

// The shade, by notification id: one per device, so it outlives every service instance. The app's own notify() and
// cancel() calls are recorded apart from what the platform posts and removes for a foreground service, so a test can
// tell which side put a notification up or took it down.
class NotificationManager {
    companion object {
        const val IMPORTANCE_LOW = 2
        val device = NotificationManager()
    }
    val shown = ConcurrentHashMap<Int, Notification>()
    val posts = CopyOnWriteArrayList<Int>()
    val cancels = CopyOnWriteArrayList<Int>()
    fun createNotificationChannel(channel: NotificationChannel) {}
    fun notify(id: Int, notification: Notification) { posts += id; shown[id] = notification }
    fun cancel(id: Int) { cancels += id; shown.remove(id) }
    fun clear() { shown.clear(); posts.clear(); cancels.clear() }
    internal fun platformPost(id: Int, notification: Notification) { shown[id] = notification }
    internal fun platformRemove(id: Int) { shown.remove(id) }
}

// Keeps the intent it was built with, which is what the platform sends, unchanged, however much later it fires.
class PendingIntent(val intent: Intent) {
    companion object {
        const val FLAG_IMMUTABLE = 1
        const val FLAG_UPDATE_CURRENT = 2
        fun getService(context: Context, code: Int, intent: Intent, flags: Int) = PendingIntent(intent)
        fun getActivity(context: Context, code: Int, intent: Intent, flags: Int) = PendingIntent(intent)
    }
}
