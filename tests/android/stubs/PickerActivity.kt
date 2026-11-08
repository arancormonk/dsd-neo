// SPDX-License-Identifier: GPL-3.0-or-later
package android.app
import android.content.Context
import android.content.Intent

// An activity that keeps each launch for a result, and refuses the actions a test names as the platform would.
open class Activity : Context() {
    companion object {
        const val RESULT_CANCELED = 0
        const val RESULT_OK = -1
        const val RESULT_FIRST_USER = 1
    }

    val launched = mutableListOf<Pair<Intent, Int>>()
    val refusals = mutableMapOf<String?, Exception>()

    fun startActivityForResult(intent: Intent, requestCode: Int) {
        launched += intent to requestCode
        refusals[intent.action]?.let { throw it }
    }
}
