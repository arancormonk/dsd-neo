// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.system.Os

// The QML collector's limit is written once, before Qt starts, and never again in the process (#782): a later
// activity's onCreate runs while Qt's main() reads the environment.
fun main() {
    val records = mutableListOf<String>()
    QmlGcPolicy.beforeQt { records += it }
    check(Os.getenv("QV4_GC_TIMELIMIT") == "0") { "the limit is 0 before Qt starts" }
    check(Os.calls == listOf("setenv QV4_GC_TIMELIMIT=0"))
    Os.env.remove("QV4_GC_TIMELIMIT")
    QmlGcPolicy.beforeQt { records += it }
    check(Os.calls.size == 1) { "a later activity writes nothing, even with the entry gone: ${Os.calls}" }
    check(records.isEmpty())
    println("PASS: the QML collector's limit is set once, before Qt starts")
}
