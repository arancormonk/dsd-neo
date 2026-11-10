// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle

private const val PICKED = "content://com.Example.Files/document/primary%3ADownload%2Fa%20b.csv"
private const val OTHER = "content://com.example.other/document/7"

/** A context whose read grants are [granted]; records go to [lines]. */
private class Rig(vararg granted: String) {
    val context = Context().also { it.grants += granted }
    val lines = mutableListOf<String>()

    fun report(intent: Intent?): String {
        IncomingDocuments.report(context, intent, lines::add)
        return IncomingDocuments.take()
    }
}

private fun view(data: String?) = Intent(Intent.ACTION_VIEW).also { it.data = data?.let(Uri::parse) }

private fun send(vararg extras: Pair<String, Any?>) = Intent(Intent.ACTION_SEND).replaceExtras(Bundle.delivered(*extras))

private fun refusal(action: String, reason: String) = "incoming document refused: action=$action reason=$reason"

private fun theIntentIsReadAsDelivered() {
    for (sdk in listOf(30, 33, 34)) {
        Build.VERSION.SDK_INT = sdk
        val rig = Rig(PICKED, OTHER)
        check(rig.report(view(PICKED)) == PICKED) { "Open with, API $sdk: the data's text as written" }
        check(rig.report(send(Intent.EXTRA_STREAM to Uri.parse(PICKED))) == PICKED) { "Share, API $sdk" }
        // A SEND's data is its preview; the stream is the document.
        val both = send(Intent.EXTRA_STREAM to Uri.parse(OTHER)).also { it.data = Uri.parse(PICKED) }
        check(rig.report(both) == OTHER) { "SEND reads EXTRA_STREAM only, API $sdk" }
        check(rig.context.grantChecks == listOf(PICKED, PICKED, OTHER)) { "the grant is asked on each accepted URI" }
        check(rig.lines.isEmpty()) { "an accepted document logs nothing: ${rig.lines}" }
    }
    Build.VERSION.SDK_INT = 30
}

private fun aStreamThatIsNotOneUriIsRefused() {
    for (sdk in listOf(30, 34)) {
        Build.VERSION.SDK_INT = sdk
        val rig = Rig(PICKED)
        check(rig.report(send(Intent.EXTRA_STREAM to arrayListOf(Uri.parse(PICKED), Uri.parse(OTHER)))).isEmpty())
        check(rig.report(send(Intent.EXTRA_STREAM to arrayListOf(Uri.parse(PICKED)))).isEmpty()) { "a list of one too" }
        check(rig.report(send(Intent.EXTRA_STREAM to PICKED)).isEmpty()) { "a string is not a Uri" }
        check(rig.report(send(Intent.EXTRA_TEXT to PICKED)).isEmpty()) { "EXTRA_TEXT is not read" }
        check(rig.report(Intent(Intent.ACTION_SEND).also { it.data = Uri.parse(PICKED) }).isEmpty()) { "nor SEND's data" }
        // Extras the guard did not get to first: the read fails, and that is no URI, not a crash.
        check(rig.report(send("evil" to Bundle.Unknown, Intent.EXTRA_STREAM to Uri.parse(PICKED))).isEmpty())
        val expected = listOf("manyUris", "manyUris", "noUri", "noUri", "noUri", "noUri").map { refusal("SEND", it) }
        check(rig.lines == expected) { "API $sdk: ${rig.lines}" }
        check(rig.context.grantChecks.isEmpty())
    }
    Build.VERSION.SDK_INT = 30
}

private fun theLookupsAreThePlatformsOwn() {
    for ((sdk, overload) in listOf(30 to 32, 32 to 32, 33 to 33, 36 to 33)) {
        Build.VERSION.SDK_INT = sdk
        val alias = "content://com.example.alias/a.csv"
        val rig = Rig(alias, OTHER)
        rig.context.packageManager.providers["com.example.alias"] = rig.context.packageName
        rig.context.packageManager.providers["com.example.other"] = "com.example.files"
        check(rig.report(view(alias)).isEmpty()) { "a provider the package manager gives this app, API $sdk" }
        check(rig.context.packageManager.lastOverloadSdk == overload) { "API $sdk uses the API $overload lookup" }
        check(rig.report(view(OTHER)) == OTHER) { "another app's provider" }
        check(rig.context.packageManager.lookups == listOf("com.example.alias", "com.example.other"))
        // The decoded authority is the one the resolver reaches, so an escaped dot is still the app's own name.
        val escaped = "content://${rig.context.packageName}%2Eqtprovider/files/a.csv"
        check(rig.report(view(escaped)).isEmpty())
        check(rig.lines == listOf(refusal("VIEW", "ownProvider"), refusal("VIEW", "ownProvider"))) { "${rig.lines}" }
    }
    Build.VERSION.SDK_INT = 30
    // A URI no grant was ever recorded for.
    val rig = Rig()
    check(rig.report(view(OTHER)).isEmpty())
    check(rig.report(send(Intent.EXTRA_STREAM to Uri.parse(OTHER))).isEmpty())
    check(rig.context.grantChecks == listOf(OTHER, OTHER))
    check(rig.lines == listOf(refusal("VIEW", "noGrant"), refusal("SEND", "noGrant")))
    for (line in rig.lines) check("example" !in line && "content" !in line) { "no URI in the record: $line" }
}

private fun otherLaunchesAreNotDocuments() {
    val rig = Rig(PICKED)
    check(rig.report(null).isEmpty())
    check(rig.report(Intent(Intent.ACTION_MAIN)).isEmpty())
    check(rig.report(Intent("android.hardware.usb.action.USB_DEVICE_ATTACHED")).isEmpty())
    check(rig.lines.isEmpty()) { "the launcher, the notification and a USB attachment log nothing: ${rig.lines}" }
    check(rig.report(view("file:///sdcard/a.csv")).isEmpty())
    check(rig.report(Intent(Intent.ACTION_EDIT).also { it.data = Uri.parse(PICKED) }).isEmpty())
    check(rig.report(Intent().also { it.data = Uri.parse(PICKED) }).isEmpty()) { "no action" }
    check(rig.lines == listOf(refusal("VIEW", "scheme"), refusal("other", "action"), refusal("other", "action")))
}

private fun theMailboxKeepsTheNewest() {
    check(IncomingDocuments.take() == "") { "nothing waits" }
    val rig = Rig(PICKED, OTHER)
    IncomingDocuments.report(rig.context, view(PICKED), rig.lines::add)
    IncomingDocuments.report(rig.context, view(OTHER), rig.lines::add)
    check(IncomingDocuments.take() == OTHER) { "a newer document replaces one not yet taken" }
    check(IncomingDocuments.take() == "") { "taken once" }
    // A refusal leaves a waiting document as it was.
    IncomingDocuments.report(rig.context, view(PICKED), rig.lines::add)
    IncomingDocuments.report(rig.context, view("https://example.com/a.csv"), rig.lines::add)
    check(IncomingDocuments.take() == PICKED)

    val mailbox = IncomingMailbox()
    check(mailbox.take() == "")
    mailbox.offer("a")
    mailbox.offer("b")
    check(mailbox.take() == "b" && mailbox.take() == "")
    // Offers from many threads leave exactly one of them waiting.
    val threads = (1..8).map { n -> Thread { repeat(500) { mailbox.offer("t$n-$it") } } }
    threads.forEach(Thread::start)
    threads.forEach(Thread::join)
    check(Regex("t[1-8]-499").matches(mailbox.take())) { "the last offer of some thread waits" }
    check(mailbox.take() == "")
}

fun main() {
    theIntentIsReadAsDelivered()
    aStreamThatIsNotOneUriIsRefused()
    theLookupsAreThePlatformsOwn()
    otherLaunchesAreNotDocuments()
    theMailboxKeepsTheNewest()
    check(IncomingDocuments.take() == "") { "every case takes what it offered" }
    println("PASS: incoming documents adapter (VIEW data, SEND stream, lists, bad parcels, lookups) and mailbox")
}
