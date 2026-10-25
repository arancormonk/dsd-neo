// SPDX-License-Identifier: GPL-3.0-or-later
package android.os

object Build {
    object VERSION { var SDK_INT = 30 }
    object VERSION_CODES { const val TIRAMISU = 33 }
}

// The broker's clock, moved by the test only.
object SystemClock {
    @Volatile var nowMs = 1_000_000L
    fun elapsedRealtime() = nowMs
}
