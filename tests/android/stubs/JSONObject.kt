// SPDX-License-Identifier: GPL-3.0-or-later
package org.json

// The broker's fixed scalar record needs no Android runtime. Encoding itself is
// owned by Android's JSONObject and is outside these scheduling tests.
class JSONObject(text: String = "") {
    private val values = mutableMapOf<String, Any>()
    init {
        Regex("\"([^\"]+)\":(\"[^\"]*\"|[^,}]+)").findAll(text).forEach {
            val raw = it.groupValues[2]
            values[it.groupValues[1]] = if (raw.startsWith('"')) raw.trim('"')
                else if (raw == "true" || raw == "false") raw == "true" else raw.toDouble()
        }
    }
    fun put(name: String, value: Any): JSONObject { values[name] = value; return this }
    fun optLong(name: String) = (values[name] as? Number)?.toLong() ?: 0L
    fun optDouble(name: String) = (values[name] as? Number)?.toDouble() ?: 0.0
    fun optBoolean(name: String) = values[name] == true
    fun optString(name: String) = values[name]?.toString() ?: ""
    override fun toString() = values.entries.joinToString(",", "{", "}") {
        "\"${it.key}\":" + if (it.value is String) "\"${it.value}\"" else it.value.toString()
    }
}
