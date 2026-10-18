// SPDX-License-Identifier: GPL-3.0-or-later
package android.util

// Logcat, for the overloads the document copies use. Nothing reads it in the tests.
object Log {
    @JvmStatic
    fun i(tag: String, msg: String): Int = 0

    @JvmStatic
    fun e(tag: String, msg: String): Int = 0

    @JvmStatic
    fun e(tag: String, msg: String, tr: Throwable): Int = 0

    @JvmStatic
    fun i(tag: String, msg: String, tr: Throwable): Int = 0

    @JvmStatic
    fun w(tag: String, msg: String): Int = 0

    @JvmStatic
    fun w(tag: String, msg: String, tr: Throwable): Int = 0
}
