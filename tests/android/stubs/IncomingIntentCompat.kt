// SPDX-License-Identifier: GPL-3.0-or-later
package androidx.core.content

import android.content.Intent
import android.os.Build

// As androidx.core 1.17.0's: the typed getters on API 34+ only (on 33 they can throw for some classes), and below that
// the old getter with a type check.
object IntentCompat {
    @JvmStatic
    fun <T> getParcelableExtra(intent: Intent, name: String?, clazz: Class<T>): T? {
        if (Build.VERSION.SDK_INT >= 34) return intent.getParcelableExtra(name!!, clazz)
        @Suppress("DEPRECATION")
        val extra: Any? = intent.getParcelableExtra(name!!)
        return if (clazz.isInstance(extra)) clazz.cast(extra) else null
    }

    @JvmStatic
    fun <T> getParcelableArrayListExtra(intent: Intent, name: String?, clazz: Class<out T>): ArrayList<T>? {
        if (Build.VERSION.SDK_INT >= 34) return intent.getParcelableArrayListExtra(name!!, clazz)
        @Suppress("DEPRECATION")
        return intent.getParcelableArrayListExtra(name!!)
    }
}
