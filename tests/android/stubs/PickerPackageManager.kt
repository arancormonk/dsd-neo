// SPDX-License-Identifier: GPL-3.0-or-later
package android.content.pm
import android.content.ComponentName
import android.content.Intent

class ActivityInfo(@JvmField val packageName: String, @JvmField val name: String)

class ResolveInfo(@JvmField val activityInfo: ActivityInfo)

// Answers activity queries from the handlers a test lists for each MIME type, and keeps each query it was asked.
open class PackageManager {
    class ResolveInfoFlags private constructor(val value: Long) {
        companion object {
            @JvmStatic
            fun of(value: Long) = ResolveInfoFlags(value)
        }
    }

    companion object {
        const val MATCH_DEFAULT_ONLY = 0x00010000
    }

    val handlers = mutableMapOf<String, List<ComponentName>>()
    val queries = mutableListOf<Pair<Intent, Long>>()
    /** The API level of the overload the last query used: 33 for the ResolveInfoFlags one. */
    var lastOverloadSdk = 0

    fun queryIntentActivities(intent: Intent, flags: Int): List<ResolveInfo> = answer(intent, flags.toLong(), 32)

    fun queryIntentActivities(intent: Intent, flags: ResolveInfoFlags): List<ResolveInfo> = answer(intent, flags.value, 33)

    private fun answer(intent: Intent, flags: Long, sdk: Int): List<ResolveInfo> {
        queries += intent to flags
        lastOverloadSdk = sdk
        return handlers[intent.mimeType].orEmpty().map { ResolveInfo(ActivityInfo(it.packageName, it.className)) }
    }
}
