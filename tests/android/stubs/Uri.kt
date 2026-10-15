// SPDX-License-Identifier: GPL-3.0-or-later
package android.net

// Only the text matters to the copies; equal texts are equal URIs, as the platform's are.
class Uri private constructor(private val text: String) {
    companion object {
        @JvmStatic
        fun parse(uriString: String): Uri = Uri(uriString)
    }

    override fun toString() = text
    override fun equals(other: Any?) = other is Uri && other.text == text
    override fun hashCode() = text.hashCode()
}
