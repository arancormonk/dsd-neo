// SPDX-License-Identifier: GPL-3.0-or-later
package android.database
import java.io.Closeable

// The cursor calls a document's name and size lookup makes.
interface Cursor : Closeable {
    fun getColumnIndex(columnName: String): Int
    fun moveToFirst(): Boolean
    fun isNull(columnIndex: Int): Boolean
    fun getLong(columnIndex: Int): Long
    fun getString(columnIndex: Int): String?
}
