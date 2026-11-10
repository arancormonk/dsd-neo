// SPDX-License-Identifier: GPL-3.0-or-later
package org.json

// The broker's fixed scalar record needs no Android runtime. Encoding itself is
// owned by Android's JSONObject and is outside these scheduling tests; strings are
// escaped and objects and arrays nest, so a record's text parses as JSON.
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
    fun has(name: String) = name in values
    override fun toString() = values.entries.joinToString(",", "{", "}") { quoted(it.key) + ":" + encoded(it.value) }
}

class JSONArray {
    private val values = mutableListOf<Any>()
    fun put(value: Any): JSONArray { values += value; return this }
    fun length() = values.size
    override fun toString() = values.joinToString(",", "[", "]") { encoded(it) }
}

private fun encoded(value: Any) = if (value is String) quoted(value) else value.toString()

private fun quoted(text: String) = buildString {
    append('"')
    for (c in text) {
        when {
            c == '"' || c == '\\' -> append('\\').append(c)
            c == '\n' -> append("\\n")
            c < ' ' -> append("\\u%04x".format(c.code))
            else -> append(c)
        }
    }
    append('"')
}
