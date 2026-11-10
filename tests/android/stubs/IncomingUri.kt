// SPDX-License-Identifier: GPL-3.0-or-later
package android.net

// The text as written, with its scheme and its decoded authority read as android.net.Uri reads a hierarchical URI. A Uri
// another app parcelled can carry parts that are not what its text parses to (unparcelled, so that one reports
// [partAuthority]); parsing its text again gives the consistent one.
class Uri private constructor(private val text: String, private val partAuthority: String? = null) {
    val scheme: String?
        get() {
            val colon = text.indexOf(':')
            return if (colon > 0 && text.indexOfAny(charArrayOf('/', '?', '#')).let { it < 0 || it > colon }) {
                text.substring(0, colon)
            } else {
                null
            }
        }

    val authority: String?
        get() {
            if (partAuthority != null) return partAuthority
            val rest = scheme?.let { text.substring(it.length + 1) } ?: text
            if (!rest.startsWith("//")) return null
            val end = rest.indexOfAny(charArrayOf('/', '?', '#'), 2).let { if (it < 0) rest.length else it }
            return decode(rest.substring(2, end))
        }

    override fun toString() = text
    override fun equals(other: Any?) = other is Uri && other.text == text
    override fun hashCode() = text.hashCode()

    companion object {
        @JvmStatic
        fun parse(uriString: String): Uri = Uri(uriString)

        /** As a Uri read from another app's parcel: [text] as its string, [authority] as its authority part. */
        fun parcelled(text: String, authority: String): Uri = Uri(text, authority)

        private fun decode(encoded: String): String {
            val bytes = java.io.ByteArrayOutputStream()
            var index = 0
            while (index < encoded.length) {
                if (encoded[index] == '%' && index + 2 < encoded.length) {
                    bytes.write(encoded.substring(index + 1, index + 3).toInt(16))
                    index += 3
                } else {
                    bytes.write(encoded[index].toString().toByteArray(Charsets.UTF_8))
                    index += 1
                }
            }
            return bytes.toString(Charsets.UTF_8.name())
        }
    }
}
