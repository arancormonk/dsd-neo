// SPDX-License-Identifier: GPL-3.0-or-later
package android.content

// The values of a MediaStore insert or update, kept in order for a test to read back.
class ContentValues {
    val values = LinkedHashMap<String, Any?>()

    fun put(key: String, value: String?) { values[key] = value }
    fun put(key: String, value: Int?) { values[key] = value }
    fun put(key: String, value: Long?) { values[key] = value }
    fun putNull(key: String) { values[key] = null }
    fun containsKey(key: String) = key in values
    fun get(key: String): Any? = values[key]
}
