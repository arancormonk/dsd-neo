// SPDX-License-Identifier: GPL-3.0-or-later
package android.content
import android.net.Uri
import java.util.concurrent.Executor
open class Context {
    companion object { const val LOCATION_SERVICE = "location" }
    open val applicationContext: Context get() = this
    open val packageName: String = "io.github.arancormonk.dsdneo"
    open val mainExecutor: Executor = Executor { it.run() }
    open fun getSystemService(name: String): Any = error("unsupported service")
    open fun checkSelfPermission(name: String): Int = -1
}
// The Settings screens the location broker opens keep what they were given for the test to read.
class Intent(val action: String?) {
    constructor(action: String?, uri: Uri?) : this(action) { data = uri }
    var data: Uri? = null
}
