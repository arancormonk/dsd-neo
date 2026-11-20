// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

// The one native call a diagnostics share makes: its failure line, kept for the test to read.
object DsdNative {
    val diagnostics = mutableListOf<String>()
    fun nativeHostDiagnostic(line: String) {
        diagnostics += line
    }
}
