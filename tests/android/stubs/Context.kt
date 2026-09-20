// SPDX-License-Identifier: GPL-3.0-or-later
package android.content
open class Context {
    companion object { const val LOCATION_SERVICE = "location" }
    open val applicationContext: Context get() = this
    open fun getSystemService(name: String): Any = error("unsupported service")
}
