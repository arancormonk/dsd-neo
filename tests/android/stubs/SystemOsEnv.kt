// SPDX-License-Identifier: GPL-3.0-or-later
package android.system

class ErrnoException(val functionName: String, val errno: Int) : Exception("$functionName failed: errno $errno")

// The process environment as a map. A test makes calls fail by setting [failure]: every call, or only the next
// [failuresLeft] of them.
object Os {
    val env = linkedMapOf<String, String>()
    var failure: ErrnoException? = null
    var failuresLeft = Int.MAX_VALUE
    val calls = mutableListOf<String>()

    private fun fail() {
        val e = failure ?: return
        if (failuresLeft <= 0) return
        failuresLeft--
        throw e
    }

    @JvmStatic
    fun setenv(name: String, value: String, overwrite: Boolean) {
        calls += "setenv $name=$value"
        fail()
        if (overwrite || name !in env) env[name] = value
    }

    @JvmStatic
    fun unsetenv(name: String) {
        calls += "unsetenv $name"
        fail()
        env.remove(name)
    }

    @JvmStatic
    fun getenv(name: String): String? = env[name]
}
