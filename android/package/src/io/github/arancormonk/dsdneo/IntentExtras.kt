// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.Intent
import android.os.Bundle

/**
 * Drops a delivered intent's extras when they cannot be read, before Qt reads them (#831 B6).
 *
 * Qt 6.11.3's `QtActivityBase.addReferrer()` runs inside `super.onCreate()` and `super.onNewIntent()` and reads
 * `getExtras().getString(...)` with no guard. On API 29-32 the first read of a bundle unparcels every value, so any app
 * that opens a document with DSD-neo, or shares one to it, can put an unknown Parcelable in the extras and crash the
 * app there, before any of its own code runs. The activity therefore calls [sanitize] before each of those two `super`
 * calls: `size()` unparcels the whole bundle, and a bundle that throws is replaced with none at all. The first failure
 * is final: `getExtras()` hands out a fresh copy of the intent's parcel, so every later read unparcels it again and
 * throws again, and the bundle is dropped at once rather than probed again.
 * On API 33+ values unparcel one at a time, past this probe, which stays for a malformed bundle; so each reader of an
 * extra guards its own read too ([IncomingDocuments], and `UsbSourceManager`'s device extra, #831 B9).
 *
 * A USB attachment's extras are framework Parcelables and read fine.
 */
object IntentExtras {
    /** Main thread, before `super.onCreate()` (on `getIntent()`) and before `super.onNewIntent(intent)`. */
    fun sanitize(intent: Intent, log: (String) -> Unit) =
        sanitize(probe = { intent.extras?.size() }, clear = { intent.replaceExtras(null as Bundle?) }, log = log)

    /** The rule, apart from the platform: a [probe] that throws is answered by one [clear] and one [log] record. */
    fun sanitize(probe: () -> Unit, clear: () -> Unit, log: (String) -> Unit) {
        try {
            probe()
        } catch (e: RuntimeException) {
            clear()
            log("launch extras dropped: " + e.javaClass.simpleName)
        }
    }
}
