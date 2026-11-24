// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

// The one native call a call recording transfer makes: whether its selection still stands. A test swaps
// RecordingExport.selectionCurrent; this answers for the ones that do not.
object DsdNative {
    fun nativeRecordingSelectionCurrent(transferId: Long): Boolean = transferId > 0
}
