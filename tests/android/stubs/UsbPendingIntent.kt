// SPDX-License-Identifier: GPL-3.0-or-later
package android.app

import android.content.Context
import android.content.Intent

// Records what the broker asked for; the test sends the framework's reply from it.
class PendingIntent private constructor(val requestCode: Int, val intent: Intent, val flags: Int) {
    companion object {
        const val FLAG_CANCEL_CURRENT = 0x10000000
        const val FLAG_UPDATE_CURRENT = 0x08000000
        const val FLAG_MUTABLE = 0x02000000
        fun getBroadcast(context: Context, requestCode: Int, intent: Intent, flags: Int) = PendingIntent(requestCode, intent, flags)
    }
}
