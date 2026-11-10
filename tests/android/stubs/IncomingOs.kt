// SPDX-License-Identifier: GPL-3.0-or-later
package android.os

// A delivered bundle, as API 29-32 read one: still parcelled until its first use, which unparcels every value in turn.
// An unknown Parcelable throws there and leaves the values before it, and that bundle never throws again. A copy of a
// parcelled bundle copies the parcel, so the copy throws on its own first use.
class Bundle private constructor(private val entries: List<Pair<String, Any?>>, private var parcelled: Boolean) {
    private val map = linkedMapOf<String, Any?>()

    constructor() : this(emptyList(), false)

    constructor(other: Bundle) : this(if (other.parcelled) other.entries else other.map.toList(), other.parcelled)

    /** A value no class loader here knows. */
    object Unknown

    fun size(): Int {
        unparcel()
        return map.size
    }

    fun get(key: String): Any? {
        unparcel()
        return map[key]
    }

    // As BaseBundle: a value of another type is a warning and null, not an error.
    fun <T> getParcelable(key: String): T? {
        val value = get(key)
        @Suppress("UNCHECKED_CAST")
        return if (value is ArrayList<*> || value is String) null else value as T?
    }

    fun <T> getParcelableArrayList(key: String): ArrayList<T>? {
        @Suppress("UNCHECKED_CAST")
        return get(key) as? ArrayList<T>
    }

    private fun unparcel() {
        if (!parcelled) return
        parcelled = false
        for ((key, value) in entries) {
            if (value === Unknown) throw BadParcelableException("ClassNotFoundException when unmarshalling: com.example.Evil")
            map[key] = value
        }
    }

    companion object {
        /** The extras of an intent as it arrives from another app. */
        fun delivered(vararg entries: Pair<String, Any?>) = Bundle(entries.toList(), true)
    }
}

class BadParcelableException(message: String) : RuntimeException(message)

object Process {
    @JvmStatic
    fun myPid() = 4242

    @JvmStatic
    fun myUid() = 10_123
}

object Build {
    object VERSION { var SDK_INT = 30 }
}
