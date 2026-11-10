// SPDX-License-Identifier: GPL-3.0-or-later
package android.app
import android.content.Context
import android.content.Intent

// An activity that keeps each launch (for a result or not), each code it was asked to finish and each permission request,
// refuses the actions a test names as the platform would, and answers the permission rationale a test sets.
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

    val started = mutableListOf<Intent>()

    fun startActivity(intent: Intent) {
        started += intent
        refusals[intent.action]?.let { throw it }
    }

    val permissionRequests = mutableListOf<Pair<List<String>, Int>>()
    var rationale = false

    fun requestPermissions(permissions: Array<String>, requestCode: Int) {
        permissionRequests += permissions.toList() to requestCode
    }

    fun shouldShowRequestPermissionRationale(permission: String) = rationale

    val finished = mutableListOf<Int>()

    fun finishActivity(requestCode: Int) {
        finished += requestCode
    }
}
