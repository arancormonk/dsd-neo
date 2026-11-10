// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Process
import androidx.core.content.IntentCompat

/**
 * One document waiting for the Qt host to take it; a newer one replaces one not yet taken. Nothing is persisted.
 * Any thread.
 */
class IncomingMailbox {
    private val lock = Any()
    private var pending = ""

    fun offer(uri: String) {
        synchronized(lock) { pending = uri }
    }

    /** The waiting document's URI, once; `""` when none waits. */
    fun take(): String = synchronized(lock) { pending.also { pending = "" } }
}

/**
 * Documents other apps open with DSD-neo or share to it (#831): the activity hands each fresh delivery to [report],
 * which reads the intent, runs [IncomingDocumentIntake] and leaves an accepted URI in the mailbox for the Qt host's poll
 * to [take]. A refusal is one diagnostics record with no URI or name in it.
 *
 * The URI travels as the sender's text: nothing here opens, queries or copies the document. The two lookups are a
 * package manager call and the grant check, no provider call.
 */
object IncomingDocuments {
    private val mailbox = IncomingMailbox()

    /** Main thread, from `onCreate` (a fresh delivery only, [LaunchDelivery]) and `onNewIntent`. */
    fun report(context: Context, intent: Intent?, log: (String) -> Unit) {
        if (intent == null) return
        when (val result = intake(context).decide(request(intent))) {
            is IntakeResult.Accept -> mailbox.offer(result.uri)
            is IntakeResult.Refuse -> log(result.line)
            IntakeResult.NotADocument -> Unit
        }
    }

    /** The URI of the document waiting, once; `""` when none waits. The Qt host's poll. */
    @JvmStatic
    fun take(): String = mailbox.take()

    internal fun request(intent: Intent): IncomingRequest =
        IncomingRequest(intent.action, intent.data?.let(::incomingUri), stream(intent))

    private fun intake(context: Context): IncomingDocumentIntake {
        val packageName = context.packageName
        return IncomingDocumentIntake(
            packageName,
            ownsAuthority = { providerPackage(context.packageManager, it) == packageName },
            hasReadGrant = {
                context.checkUriPermission(
                    Uri.parse(it), Process.myPid(), Process.myUid(), Intent.FLAG_GRANT_READ_URI_PERMISSION,
                ) == PackageManager.PERMISSION_GRANTED
            },
        )
    }

    // The typed getter only through IntentCompat, which uses it on API 34+ (it can throw for some classes on 33). A
    // stream that is a list is told apart from one that is absent or of another type; a failed read is none.
    private fun stream(intent: Intent): IncomingStream = try {
        val one = IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java)
        when {
            one != null -> IncomingStream.One(incomingUri(one))
            IntentCompat.getParcelableArrayListExtra(intent, Intent.EXTRA_STREAM, Uri::class.java) != null ->
                IncomingStream.Many
            else -> IncomingStream.None
        }
    } catch (e: RuntimeException) {
        IncomingStream.None
    }

    private fun incomingUri(uri: Uri) = IncomingUri(uri.toString(), uri.scheme, uri.authority)
}

/** The package that declares the provider for [authority], or null when there is none this app can see. */
@Suppress("DEPRECATION")
private fun providerPackage(packages: PackageManager, authority: String): String? {
    val provider = if (Build.VERSION.SDK_INT >= 33) {
        packages.resolveContentProvider(authority, PackageManager.ComponentInfoFlags.of(0))
    } else {
        packages.resolveContentProvider(authority, 0)
    }
    return provider?.packageName
}
