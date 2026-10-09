// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/**
 * The record test_encode_matches_expected_record_field_order() in tests/ui/test_app_control_notification_status.c makes
 * the C encoder write, as NOTIFICATION_GOLDEN_V3_RECORD. Keep the two byte-identical: they are how the encoder and this
 * reader pin one format between them.
 */
private const val GOLDEN_V3 =
    "v3\tP25p2\t1\t0\t1\t851006250\t851012500\t851500000\t0\t5000000000\t250" +
        "\t2\tRiverside Fire\t51023\t7654321\t51023\t1\t170\t4660\t1500\t1\t3" +
        "\t0\t\t\t\t0\t0\t0\t0\t0\t0\t0"

private const val HEADER_FIELDS = 11
private const val SLOT_FIELDS = 11

/** A v3 record with every field distinct where it can be, so a reader taking any field from the wrong index shows. */
private fun distinctFields(): MutableList<String> = mutableListOf(
    "v3", "DMR", "1", "1", "0", "851006250", "851012500", "851500000", "1", "123456789", "4321",
    // Slot 0: state, name, tgText, srcText, tgId, enc, algid, kid, elapsedMs, emergency, priority.
    "3", "Alpha", "100", "Unit 7", "100", "0", "0", "0", "64000", "0", "0",
    // Slot 1.
    "2", "Bravo", "200", "Unit 9", "18446744073709551615", "1", "132", "65535", "9000", "1", "7",
)

private fun record(fields: List<String>): String = fields.joinToString("\t")

private fun parseOrFail(record: String): DecoderStatus =
    checkNotNull(DecoderStatus.parse(record)) { "expected a status from: $record" }

private fun goldenRecordParses() {
    val expected = DecoderStatus(
        protocol = "P25p2",
        radioInput = true,
        trunking = false,
        trunkTuned = true,
        ccFreqHz = 851006250L,
        vcFreqHz = 851012500L,
        centerFreqHz = 851500000L,
        slots = listOf(
            SlotCall(
                state = DecoderStatus.LINE_ACTIVE,
                name = "Riverside Fire",
                tgText = "51023",
                srcText = "7654321",
                tgId = 51023uL,
                enc = true,
                algid = 0xAA,
                kid = 0x1234,
                elapsedMs = 1500L,
                emergency = true,
                priority = 3,
            ),
            SlotCall(
                state = DecoderStatus.LINE_NONE,
                name = "",
                tgText = "",
                srcText = "",
                tgId = 0uL,
                enc = false,
                algid = 0,
                kid = 0,
                elapsedMs = 0L,
                emergency = false,
                priority = 0,
            ),
        ),
        leadSlotIndex = 0,
        audibleStamp = 5_000_000_000uL,
        audibleAgeMs = 250L,
    )
    check(GOLDEN_V3.split('\t').size == HEADER_FIELDS + 2 * SLOT_FIELDS)
    check(parseOrFail(GOLDEN_V3) == expected)
    check(parseOrFail(GOLDEN_V3).leadSlot == expected.slots[0])
}

private fun audiblePairParses() {
    val status = parseOrFail(record(distinctFields()))
    check(status.audibleStamp == 123456789uL)
    check(status.audibleAgeMs == 4321L)

    // No audible audio yet: stamp 0 and the age's -1 sentinel.
    val none = distinctFields().apply {
        this[9] = "0"
        this[10] = "-1"
    }
    val quiet = parseOrFail(record(none))
    check(quiet.audibleStamp == 0uL)
    check(quiet.audibleAgeMs == -1L)

    // The stamp is unsigned on the wire; the C test writes this same largest value.
    val max = distinctFields().apply { this[9] = "18446744073709551615" }
    check(parseOrFail(record(max)).audibleStamp == ULong.MAX_VALUE)
}

private fun headerAndSlotOffsets() {
    val status = parseOrFail(record(distinctFields()))
    check(status.protocol == "DMR")
    check(status.radioInput && status.trunking && !status.trunkTuned)
    check(status.ccFreqHz == 851006250L && status.vcFreqHz == 851012500L && status.centerFreqHz == 851500000L)
    // The lead slot stays at index 8, ahead of the audible-audio pair.
    check(status.leadSlotIndex == 1)
    check(status.leadSlot == status.slots[1])

    val first = status.slots[0]
    check(first.state == DecoderStatus.LINE_ENDED)
    check(first.name == "Alpha" && first.tgText == "100" && first.srcText == "Unit 7")
    check(first.tgId == 100uL && !first.enc && first.algid == 0 && first.kid == 0)
    check(first.elapsedMs == 64000L && !first.emergency && first.priority == 0)

    val second = status.slots[1]
    check(second.state == DecoderStatus.LINE_ACTIVE)
    check(second.name == "Bravo" && second.tgText == "200" && second.srcText == "Unit 9")
    check(second.tgId == ULong.MAX_VALUE && second.enc && second.algid == 132 && second.kid == 65535)
    check(second.elapsedMs == 9000L && second.emergency && second.priority == 7)

    // Each slot starts where the header (11 fields) and the slots before it end.
    for ((slot, base) in listOf(0 to HEADER_FIELDS, 1 to HEADER_FIELDS + SLOT_FIELDS)) {
        val fields = distinctFields().apply { this[base + 1] = "Moved" }
        check(parseOrFail(record(fields)).slots[slot].name == "Moved") { "slot $slot name is not at index ${base + 1}" }
    }
}

private fun wrongShapesAreRejected() {
    // A v2 record: the 31 fields before the audible-audio pair existed.
    val v2 = distinctFields().apply {
        this[0] = "v2"
        removeAt(10)
        removeAt(9)
    }
    check(v2.size == 31)
    check(DecoderStatus.parse(record(v2)) == null)
    // The v3 shape under the old version string is not read either.
    check(DecoderStatus.parse(record(distinctFields().apply { this[0] = "v2" })) == null)

    check(DecoderStatus.parse(record(distinctFields().dropLast(1))) == null)
    check(DecoderStatus.parse(record(distinctFields() + "0")) == null)
    check(DecoderStatus.parse(null) == null)
    check(DecoderStatus.parse("") == null)
}

private fun malformedNumbersAreRejected() {
    for ((index, value) in listOf(9 to "", 9 to "abc", 9 to "-1", 9 to "18446744073709551616", 10 to "", 10 to "x")) {
        val fields = distinctFields().apply { this[index] = value }
        check(DecoderStatus.parse(record(fields)) == null) { "field $index = \"$value\" must not parse" }
    }
}

private fun sameDisplayIgnoresOnlyTheAudiblePair() {
    val status = parseOrFail(record(distinctFields()))
    check(status.sameDisplay(status))
    check(!status.sameDisplay(null))

    // The stamp and its age move every second while audio is recent; the notification shows neither.
    val aged = distinctFields().apply {
        this[9] = "123459999"
        this[10] = "0"
    }
    check(status.sameDisplay(parseOrFail(record(aged))))
    val quiet = distinctFields().apply {
        this[9] = "0"
        this[10] = "-1"
    }
    check(status.sameDisplay(parseOrFail(record(quiet))))

    // Everything else is something the notification renders. Each index below, changed alone, must count.
    val changes = listOf(
        1 to "P25p1", 2 to "0", 3 to "0", 4 to "1", 5 to "1", 6 to "2", 7 to "3", 8 to "0",
        11 to "2", 12 to "Other", 13 to "101", 14 to "Unit 8", 15 to "101", 16 to "1", 17 to "1", 18 to "1",
        19 to "65000", 20 to "1", 21 to "1",
        22 to "3", 23 to "Other", 24 to "201", 25 to "Unit 10", 26 to "201", 27 to "0", 28 to "1", 29 to "1",
        30 to "9001", 31 to "0", 32 to "6",
    )
    check(changes.size == HEADER_FIELDS + 2 * SLOT_FIELDS - 3) // All but version, stamp and age.
    for ((index, value) in changes) {
        val other = parseOrFail(record(distinctFields().apply { this[index] = value }))
        check(!status.sameDisplay(other)) { "a change at index $index must count as a display change" }
        check(!other.sameDisplay(status)) { "sameDisplay must be symmetric at index $index" }
    }
}

fun main() {
    goldenRecordParses()
    audiblePairParses()
    headerAndSlotOffsets()
    wrongShapesAreRejected()
    malformedNumbersAreRejected()
    sameDisplayIgnoresOnlyTheAudiblePair()
    println("PASS: status record v3 golden, audible stamp and age, field offsets, rejected shapes and display equality")
}
