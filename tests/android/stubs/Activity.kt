// SPDX-License-Identifier: GPL-3.0-or-later
package android.app
import android.content.Context
import android.location.LocationManager
import java.util.concurrent.Executor
class Activity : Context() {
    val manager = LocationManager()
    var permission = 0
    var permissionRequests = 0
    val mainExecutor = Executor { it.run() }
    override fun getSystemService(name: String): Any = manager
    fun checkSelfPermission(name: String) = permission
    fun requestPermissions(names: Array<String>, code: Int) { permissionRequests++ }
}
