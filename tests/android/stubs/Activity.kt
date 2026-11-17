// SPDX-License-Identifier: GPL-3.0-or-later
package android.app
import android.content.Context
import android.content.Intent
import android.location.LocationManager
import java.util.concurrent.Executor
class Activity : Context() {
    val manager = LocationManager()
    var permission = 0
    var finePermission = -1
    var permissionRequests = 0
    var requestedPermissions = emptyList<String>()
    val started = mutableListOf<Intent>()
    var startFailure: RuntimeException? = null
    var requestFailure: RuntimeException? = null
    override val mainExecutor = Executor { it.run() }
    override fun getSystemService(name: String): Any = manager
    override fun checkSelfPermission(name: String) =
        if (name == android.Manifest.permission.ACCESS_FINE_LOCATION) finePermission else permission
    fun requestPermissions(names: Array<String>, code: Int) {
        requestFailure?.let { throw it }
        permissionRequests++
        requestedPermissions = names.toList()
    }
    fun startActivity(intent: Intent) { startFailure?.let { throw it }; started.add(intent) }
}
