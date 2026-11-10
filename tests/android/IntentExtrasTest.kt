// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.Intent
import android.net.Uri
import android.os.BadParcelableException
import android.os.Bundle

private const val DOCUMENT = "content://com.example.files/document/7"

/** What Qt 6.11.3's addReferrer() does inside super.onCreate() and super.onNewIntent(): it reads the extras. */
private fun qtReadsTheExtras(intent: Intent) {
    intent.extras?.get("android.intent.extra.REFERRER_NAME")
}

private fun theRule() {
    val calls = mutableListOf<String>()
    IntentExtras.sanitize(probe = { calls += "probe" }, clear = { calls += "clear" }, log = { calls += "log $it" })
    check(calls == listOf("probe")) { "readable extras are left alone: $calls" }

    calls.clear()
    IntentExtras.sanitize(
        probe = { calls += "probe"; throw BadParcelableException("ClassNotFoundException when unmarshalling: x.Evil") },
        clear = { calls += "clear" },
        log = { calls += "log $it" },
    )
    check(calls == listOf("probe", "clear", "log launch extras dropped: BadParcelableException")) {
        "one clear, then one record with the exception's class only: $calls"
    }

    // Any RuntimeException is a bundle that cannot be trusted.
    calls.clear()
    IntentExtras.sanitize(probe = { throw IllegalStateException("x.Evil") }, clear = { calls += "clear" }, log = { calls += it })
    check(calls == listOf("clear", "launch extras dropped: IllegalStateException")) { "$calls" }
}

private fun aBadBundleIsDroppedBeforeQtReadsIt() {
    val intent = Intent(Intent.ACTION_SEND).replaceExtras(
        Bundle.delivered("evil" to Bundle.Unknown, Intent.EXTRA_STREAM to Uri.parse(DOCUMENT)),
    )
    // As on main: each read of the extras is a fresh copy of the parcel, so Qt's read throws, every time.
    repeat(2) {
        try {
            qtReadsTheExtras(intent)
            error("a bundle with an unknown Parcelable throws on API 29-32")
        } catch (expected: BadParcelableException) {
        }
    }
    val lines = mutableListOf<String>()
    IntentExtras.sanitize(intent, lines::add)
    check(intent.extras == null) { "the extras are gone" }
    check(lines == listOf("launch extras dropped: BadParcelableException")) { "$lines" }
    qtReadsTheExtras(intent)
    // The first failure is final: a second pass finds nothing to probe and records nothing.
    IntentExtras.sanitize(intent, lines::add)
    check(lines.size == 1 && intent.extras == null)
    check(intent.action == Intent.ACTION_SEND) { "only the extras go" }
}

private fun goodExtrasAreKept() {
    val lines = mutableListOf<String>()
    val shared = Intent(Intent.ACTION_SEND).replaceExtras(Bundle.delivered(Intent.EXTRA_STREAM to Uri.parse(DOCUMENT)))
    IntentExtras.sanitize(shared, lines::add)
    qtReadsTheExtras(shared)
    check(shared.extras?.size() == 1) { "readable extras stay" }
    @Suppress("DEPRECATION")
    check(shared.getParcelableExtra<Uri>(Intent.EXTRA_STREAM) == Uri.parse(DOCUMENT)) { "and read as before" }
    val launcher = Intent(Intent.ACTION_MAIN)
    IntentExtras.sanitize(launcher, lines::add)
    check(launcher.extras == null)
    check(lines.isEmpty()) { "nothing to record: $lines" }
}

fun main() {
    theRule()
    aBadBundleIsDroppedBeforeQtReadsIt()
    goodExtrasAreKept()
    println("PASS: launch extras guard (probe, one clear and record, the first failure final)")
}
