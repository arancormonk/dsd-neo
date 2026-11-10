// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

private const val PACKAGE = "io.github.arancormonk.dsdneo"
private const val VIEW = "android.intent.action.VIEW"
private const val SEND = "android.intent.action.SEND"

// A mixed-case authority and escapes, which the accepted text keeps as the sender wrote them.
private val PICKED = uri("content://com.Example.Files/document/primary%3ADownload%2Fa%20b.csv", "content", "com.Example.Files")
private val OTHER = uri("content://com.example.other/document/7", "content", "com.example.other")

private fun uri(text: String, scheme: String?, authority: String?) = IncomingUri(text, scheme, authority)

private fun view(data: IncomingUri?, stream: IncomingStream = IncomingStream.None) = IncomingRequest(VIEW, data, stream)

private fun send(stream: IncomingStream, data: IncomingUri? = null) = IncomingRequest(SEND, data, stream)

/** The gate with its two lookups recorded: [granted] URIs hold a read grant, [resolved] authorities are this app's. */
private class Gate(val granted: Set<String> = setOf(PICKED.text, OTHER.text), val resolved: Set<String> = emptySet()) {
    val ownerLookups = mutableListOf<String>()
    val grantLookups = mutableListOf<String>()
    val intake = IncomingDocumentIntake(
        PACKAGE,
        ownsAuthority = { ownerLookups += it; it in resolved },
        hasReadGrant = { grantLookups += it; it in granted },
    )

    fun decide(request: IncomingRequest) = intake.decide(request)
}

private fun refused(action: String, reason: String) = IntakeResult.Refuse(action, reason)

private fun viewAndSendAreAccepted() {
    val gate = Gate()
    check(gate.decide(view(PICKED)) == IntakeResult.Accept(PICKED.text)) { "Open with names the document in its data" }
    check(gate.decide(send(IncomingStream.One(PICKED))) == IntakeResult.Accept(PICKED.text)) {
        "Share names the document in EXTRA_STREAM"
    }
    check(gate.grantLookups == listOf(PICKED.text, PICKED.text)) { "the grant is checked on the URI as written" }
    check(gate.ownerLookups == listOf("com.Example.Files", "com.Example.Files")) { "the owner by its authority" }
    // The scheme in any case: the platform's checks and the provider decide whether such a URI then reads.
    val upper = uri("CONTENT://com.example.other/document/7", "CONTENT", "com.example.other")
    check(Gate(granted = setOf(upper.text)).decide(view(upper)) == IntakeResult.Accept(upper.text))
}

private fun onlyViewAndSendAreDocuments() {
    val gate = Gate()
    for (action in listOf("android.intent.action.MAIN", "android.hardware.usb.action.USB_DEVICE_ATTACHED")) {
        check(gate.decide(IncomingRequest(action, PICKED, IncomingStream.One(PICKED))) == IntakeResult.NotADocument) {
            "$action is another launch: nothing to log"
        }
    }
    for (action in listOf("android.intent.action.EDIT", "android.intent.action.SEND_MULTIPLE", null, "")) {
        check(gate.decide(IncomingRequest(action, PICKED, IncomingStream.One(PICKED))) == refused("other", "action")) {
            "$action is refused, whatever URI it carries"
        }
    }
    check(gate.ownerLookups.isEmpty() && gate.grantLookups.isEmpty()) { "no lookup for an intent that is not VIEW or SEND" }
}

private fun eachActionHasItsOneUri() {
    val gate = Gate()
    // SEND's data is a sender's rich preview: the stream alone names the document, and data never stands in for it.
    check(gate.decide(send(IncomingStream.None, data = PICKED)) == refused("SEND", "noUri")) { "SEND with data only" }
    check(gate.decide(send(IncomingStream.One(OTHER), data = PICKED)) == IntakeResult.Accept(OTHER.text)) {
        "SEND takes its stream, not its data"
    }
    check(gate.decide(send(IncomingStream.Many)) == refused("SEND", "manyUris")) { "a list of streams is many" }
    check(gate.decide(send(IncomingStream.Many, data = PICKED)) == refused("SEND", "manyUris"))
    // VIEW's data alone: a stream on a VIEW is not read.
    check(gate.decide(view(null, IncomingStream.One(PICKED))) == refused("VIEW", "noUri")) { "VIEW without data" }
    check(gate.decide(view(OTHER, IncomingStream.One(PICKED))) == IntakeResult.Accept(OTHER.text)) { "VIEW takes its data" }
    check(gate.decide(view(uri("", null, null))) == refused("VIEW", "noUri")) { "an empty URI is none" }
    check(gate.decide(view(null, IncomingStream.Many)) == refused("VIEW", "noUri")) { "VIEW never reads the stream" }
}

private fun onlyContentUrisAreTaken() {
    val gate = Gate(granted = setOf("file:///sdcard/Download/a.csv", "FILE:///sdcard/a.csv", "https://example.com/a.csv"))
    val cases = listOf(
        uri("file:///sdcard/Download/a.csv", "file", ""),
        uri("FILE:///sdcard/a.csv", "FILE", ""),
        uri("https://example.com/a.csv", "https", "example.com"),
        uri("http://example.com/a.csv", "http", "example.com"),
        uri("a.csv", null, null),
        uri("contents://com.example.other/7", "contents", "com.example.other"),
    )
    for (case in cases) {
        check(gate.decide(view(case)) == refused("VIEW", "scheme")) { "${case.text} is refused on VIEW" }
        check(gate.decide(send(IncomingStream.One(case))) == refused("SEND", "scheme")) { "${case.text} is refused on SEND" }
    }
    check(gate.ownerLookups.isEmpty() && gate.grantLookups.isEmpty()) { "a refused scheme makes no lookup" }
}

private fun theAppsOwnProvidersAreRefused() {
    // The three providers the manifest declares: refused by name, whether or not the package manager answers.
    for (suffix in listOf("qtprovider", "diagnostics", "recordings")) {
        val own = uri("content://$PACKAGE.$suffix/files/a.csv", "content", "$PACKAGE.$suffix")
        val gate = Gate(granted = setOf(own.text))
        check(gate.decide(view(own)) == refused("VIEW", "ownProvider")) { "$suffix is the app's own" }
        check(gate.decide(send(IncomingStream.One(own))) == refused("SEND", "ownProvider"))
        check(gate.grantLookups.isEmpty()) { "an own provider is refused before the grant is asked" }
        // A "<user>@" prefix still reaches the provider, so it is not a way around the check.
        val user = uri("content://0@$PACKAGE.$suffix/files/a.csv", "content", "0@$PACKAGE.$suffix")
        check(gate.decide(view(user)) == refused("VIEW", "ownProvider")) { "a user-prefixed $suffix is the app's own" }
    }
    // One the package manager says this app declares, under any name.
    val alias = uri("content://com.example.alias/a.csv", "content", "com.example.alias")
    val gate = Gate(granted = setOf(alias.text), resolved = setOf("com.example.alias"))
    check(gate.decide(view(alias)) == refused("VIEW", "ownProvider")) { "a provider the resolver gives this app" }
    val prefixed = uri("content://10@com.example.alias/a.csv", "content", "10@com.example.alias")
    check(gate.decide(send(IncomingStream.One(prefixed))) == refused("SEND", "ownProvider"))
    check(gate.ownerLookups == listOf("com.example.alias", "com.example.alias")) { "looked up without the user prefix" }
    // Only names under the package's own, dot and all: another app whose package starts the same is not this one.
    val lookalike = uri("content://${PACKAGE}x.files/a.csv", "content", "${PACKAGE}x.files")
    check(Gate(granted = setOf(lookalike.text)).decide(view(lookalike)) == IntakeResult.Accept(lookalike.text))
}

private fun aUriWithNoGrantIsRefused() {
    // A URI this app was never handed: another app names a provider it can read itself, but no grant is recorded.
    val never = uri("content://com.example.open/document/1", "content", "com.example.open")
    val gate = Gate()
    check(gate.decide(view(never)) == refused("VIEW", "noGrant"))
    check(gate.decide(send(IncomingStream.One(never))) == refused("SEND", "noGrant"))
    check(gate.grantLookups == listOf(never.text, never.text)) { "asked on the URI's text" }
    // A content URI with no authority names no provider of this app's; the grant decides.
    val bare = uri("content:///a.csv", "content", "")
    check(gate.decide(view(bare)) == refused("VIEW", "noGrant"))
    check(gate.ownerLookups == listOf("com.example.open", "com.example.open")) { "an empty authority is not looked up" }
}

private fun theRefusalLineNamesNoDocument() {
    check(refused("SEND", "manyUris").line == "incoming document refused: action=SEND reason=manyUris")
    check(refused("other", "action").line == "incoming document refused: action=other reason=action")
    val secret = uri("content://com.example.secret/Secret%20Name.csv", "content", "com.example.secret")
    val requests = listOf(
        view(secret), send(IncomingStream.One(secret)), send(IncomingStream.Many, data = secret),
        IncomingRequest("android.intent.action.EDIT", secret, IncomingStream.One(secret)),
        view(uri("file:///Secret Name.csv", "file", "")),
        view(uri("content://$PACKAGE.recordings/Secret.wav", "content", "$PACKAGE.recordings")),
    )
    val reasons = mutableSetOf<String>()
    for (request in requests) {
        val result = Gate(granted = emptySet()).decide(request)
        check(result is IntakeResult.Refuse) { "$request is refused" }
        reasons += result.reason
        check(result.action in setOf("VIEW", "SEND", "other"))
        check(Regex("incoming document refused: action=(VIEW|SEND|other) reason=[a-zA-Z]+").matches(result.line)) {
            "the line has its exact shape: ${result.line}"
        }
        for (part in listOf("Secret", "secret", "content", "file", PACKAGE)) {
            check(part !in result.line) { "the line names no URI, authority or name: ${result.line}" }
        }
    }
    check(reasons == setOf("noGrant", "manyUris", "action", "scheme", "ownProvider")) { "the cases cover the reasons: $reasons" }
}

private fun aDeliveryIsFreshOnlyWhenNew() {
    // A cold launch, by the launcher or by a dongle plugged in: the first attachment still reports.
    check(LaunchDelivery.fresh(saved = false, fromHistory = false))
    // Recreated after process death, from Recents: the saved state says the intent was delivered before.
    check(!LaunchDelivery.fresh(saved = true, fromHistory = false))
    // An emptied task relaunched from Recents carries the history flag.
    check(!LaunchDelivery.fresh(saved = false, fromHistory = true))
    check(!LaunchDelivery.fresh(saved = true, fromHistory = true))
}

fun main() {
    viewAndSendAreAccepted()
    onlyViewAndSendAreDocuments()
    eachActionHasItsOneUri()
    onlyContentUrisAreTaken()
    theAppsOwnProvidersAreRefused()
    aUriWithNoGrantIsRefused()
    theRefusalLineNamesNoDocument()
    aDeliveryIsFreshOnlyWhenNew()
    println("PASS: incoming document gate (actions, one URI, schemes, own providers, grants, log line) and launch delivery")
}
