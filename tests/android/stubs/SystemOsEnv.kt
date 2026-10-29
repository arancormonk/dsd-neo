// SPDX-License-Identifier: GPL-3.0-or-later
package android.system

class ErrnoException(val functionName: String, val errno: Int) : Exception("$functionName failed: errno $errno")

// The process environment as a map. A test makes the next calls fail by setting [failure].
object Os {
    val env = linkedMapOf<String, String>()
    var failure: ErrnoException? = null
    val calls = mutableListOf<String>()

    @JvmStatic
    fun setenv(name: String, value: String, overwrite: Boolean) {
        calls += "setenv $name=$value"
        failure?.let { throw it }
        if (overwrite || name !in env) env[name] = value
    }

    @JvmStatic
    fun unsetenv(name: String) {
        calls += "unsetenv $name"
        failure?.let { throw it }
        env.remove(name)
    }

    @JvmStatic
    fun getenv(name: String): String? = env[name]
}
