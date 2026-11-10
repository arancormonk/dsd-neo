// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/** One URI an intent names, as `android.net.Uri` reads it: its text, its scheme and its decoded authority. */
data class IncomingUri(val text: String, val scheme: String?, val authority: String?)

/** What a SEND's `EXTRA_STREAM` held, as [IncomingDocuments] read it. */
sealed class IncomingStream {
    /** Absent, not a Uri, or unreadable. */
    object None : IncomingStream()

    data class One(val uri: IncomingUri) : IncomingStream()

    /** A list of Uris, as SEND_MULTIPLE carries; this app takes one document at a time. */
    object Many : IncomingStream()
}

/** The parts of a delivered intent the gate reads; [data] is the intent's own data, [stream] its `EXTRA_STREAM`. */
data class IncomingRequest(val action: String?, val data: IncomingUri?, val stream: IncomingStream)

/** The gate's answer for one delivered intent. */
sealed class IntakeResult {
    /** A document to offer, by its URI's text as the sender wrote it. */
    data class Accept(val uri: String) : IntakeResult()

    /** Not taken; [line] is the diagnostics record, which names only the action's kind and the reason. */
    data class Refuse(val action: String, val reason: String) : IntakeResult() {
        val line: String get() = "incoming document refused: action=$action reason=$reason"
    }

    /** A launch that is not about a document (the launcher, the notification, a USB attachment): nothing to log. */
    object NotADocument : IntakeResult()
}

/**
 * The intake gate for a document another app opens with or shares to DSD-neo (#831). The activity is exported, and
 * neither its intent filters nor Android's matching ever look at `EXTRA_STREAM`, and an intent with no action passes the
 * action test, so every check that matters runs here, at run time, on every API level:
 *
 * 1. the action is VIEW or SEND; MAIN and USB_DEVICE_ATTACHED are other launches, and anything else is refused;
 * 2. exactly one URI: VIEW's data, or SEND's `EXTRA_STREAM` alone. A SEND's data (a sender's rich preview), its
 *    `EXTRA_TEXT` and its ClipData are never read: a sender that sets its own ClipData skips Android's migration of the
 *    stream, so ClipData is not where the document is;
 * 3. the `content` scheme, in any case: `file:` is refused, as are links;
 * 4. not one of this app's own providers ([ownsAuthority], or any authority under this package's name), which would
 *    turn the import into a read of the app's own files on another app's behalf;
 * 5. a read grant recorded for this app on the URI ([hasReadGrant]). That is not proof of who sent it: the grant may
 *    come from an earlier delivery. It refuses a URI for which no grant is recorded at all.
 *
 * Refusals are log-only ([IntakeResult.Refuse.line]), and the line never names the URI, its authority or a file name.
 * Plain Kotlin with no Android imports, so the JVM tests run it as is; [IncomingDocuments] reads the intent and makes
 * the two lookups.
 */
class IncomingDocumentIntake(
    private val packageName: String,
    private val ownsAuthority: (String) -> Boolean,
    private val hasReadGrant: (String) -> Boolean,
) {
    fun decide(request: IncomingRequest): IntakeResult {
        val action = when (request.action) {
            VIEW -> "VIEW"
            SEND -> "SEND"
            MAIN, USB_DEVICE_ATTACHED -> return IntakeResult.NotADocument
            else -> return IntakeResult.Refuse("other", ACTION)
        }
        val uri = when (request.action) {
            VIEW -> request.data
            else -> when (val stream = request.stream) {
                is IncomingStream.One -> stream.uri
                IncomingStream.Many -> return IntakeResult.Refuse(action, MANY_URIS)
                IncomingStream.None -> null
            }
        }
        if (uri == null || uri.text.isEmpty()) return IntakeResult.Refuse(action, NO_URI)
        if (!uri.scheme.equals(CONTENT, ignoreCase = true)) return IntakeResult.Refuse(action, SCHEME)
        if (isOwn(uri.authority.orEmpty())) return IntakeResult.Refuse(action, OWN_PROVIDER)
        if (!hasReadGrant(uri.text)) return IntakeResult.Refuse(action, NO_GRANT)
        return IntakeResult.Accept(uri.text)
    }

    // The resolver reaches a provider by the authority after a "<user>@" prefix (ContentProvider's
    // getAuthorityWithoutUserId), so that is the name checked.
    private fun isOwn(authority: String): Boolean {
        val provider = authority.substringAfterLast('@')
        if (provider.isEmpty()) return false
        return provider.startsWith("$packageName.") || ownsAuthority(provider)
    }

    companion object {
        const val VIEW = "android.intent.action.VIEW"
        const val SEND = "android.intent.action.SEND"
        const val MAIN = "android.intent.action.MAIN"
        const val USB_DEVICE_ATTACHED = "android.hardware.usb.action.USB_DEVICE_ATTACHED"
        const val CONTENT = "content"

        // The refusal reasons, as the diagnostics line names them.
        const val ACTION = "action"
        const val NO_URI = "noUri"
        const val MANY_URIS = "manyUris"
        const val SCHEME = "scheme"
        const val OWN_PROVIDER = "ownProvider"
        const val NO_GRANT = "noGrant"
    }
}
