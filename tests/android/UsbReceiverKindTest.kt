// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

private val AIRSPY = 0x1d50 to 0x60a1
private val RTL_2832 = 0x0bda to 0x2832
private val RTL_2838 = 0x0bda to 0x2838
private val HUB = 0x1d6b to 0x0002
private val KEYBOARD = 0x046d to 0xc31c
// The Airspy's vendor with another product, and another vendor with the Airspy's product: neither is a receiver.
private val AIRSPY_VENDOR_ONLY = 0x1d50 to 0x5678
private val AIRSPY_PRODUCT_ONLY = 0x1234 to 0x60a1

private fun expectKind(want: String, devices: List<Pair<Int, Int>>, what: String) {
    val got = UsbReceiverKind.attachedKind(devices)
    check(got == want) { "$what: expected \"$want\", got \"$got\"" }
}

private fun airspyBesideUnrelatedDevicesIsAirspy() {
    // An Airspy behind a hub, with a keyboard on the same hub: the hub and keyboard are not RTL-SDRs.
    expectKind(UsbReceiverKind.AIRSPY, listOf(HUB, AIRSPY, KEYBOARD), "Airspy and unrelated devices")
    expectKind(UsbReceiverKind.AIRSPY, listOf(AIRSPY, AIRSPY_VENDOR_ONLY, AIRSPY_PRODUCT_ONLY), "Airspy and look-alikes")
}

private fun twoRtlDonglesAreUsb() {
    expectKind(UsbReceiverKind.RTL, listOf(RTL_2832, RTL_2838), "two RTL dongles")
    expectKind(UsbReceiverKind.RTL, listOf(RTL_2832, RTL_2832, HUB), "two of one RTL model and a hub")
}

private fun rtlAndAirspyAreBoth() {
    expectKind(UsbReceiverKind.BOTH, listOf(RTL_2832, AIRSPY), "RTL and Airspy")
    expectKind(UsbReceiverKind.BOTH, listOf(HUB, AIRSPY, RTL_2838, AIRSPY), "RTL, two Airspys and a hub")
}

private fun nothingKnownIsEmpty() {
    expectKind("", emptyList(), "an empty bus")
    expectKind("", listOf(HUB, KEYBOARD, AIRSPY_VENDOR_ONLY, AIRSPY_PRODUCT_ONLY), "only unrelated devices")
}

private fun classificationMatchesTheAccessRequest() {
    check(UsbReceiverKind.isKnown(AIRSPY.first, AIRSPY.second)) { "the Airspy is a receiver" }
    check(UsbReceiverKind.isKnown(RTL_2832.first, RTL_2832.second)) { "the RTL2832U is a receiver" }
    check(!UsbReceiverKind.isKnown(HUB.first, HUB.second)) { "a hub is not a receiver" }
    check(UsbReceiverKind.sourceOf(AIRSPY.first, AIRSPY.second) == "airspy") { "an Airspy starts as airspy" }
    check(UsbReceiverKind.sourceOf(RTL_2838.first, RTL_2838.second) == "usb") { "an RTL dongle starts as usb" }
}

private fun aLabelBelongsToItsKindOfReceiver() {
    val label = "RTL2838UHIDIR 0bda:2838"
    check(UsbReceiverKind.labelFor("usb", "usb", label) == label) { "the connected RTL labels an RTL start" }
    check(UsbReceiverKind.labelFor("airspy", "usb", label) == null) { "an RTL label never labels an Airspy start" }
    check(UsbReceiverKind.labelFor("usb", "", label) == null) { "no connection, no label" }
    check(UsbReceiverKind.labelFor("usb", "usb", null) == null) { "nothing opened yet, no label" }
}

// #782: the receiver a start takes is read from its arguments as the engine reads "-i" (opts.h rtl/airspy specs).
private fun aStartsReceiverComesFromItsInput() {
    fun of(vararg args: String) = UsbReceiverKind.sourceOfArgs(arrayOf(*args))
    check(of("-i", "rtl:0:851.0125M:0:0:24") == "usb" && of("-i", "rtl") == "usb") { "an RTL-SDR spec" }
    check(of("-i", "airspy:26a464dc:0:0:0", "--airspy-serial", "26A464DC") == "airspy" && of("-iairspy") == "airspy") {
        "an Airspy spec, separate or attached"
    }
    check(of("-i", "rtltcp:127.0.0.1:1234") == "" && of("-i", "rtlx") == "" && of("-i", "udp:0.0.0.0:7355") == "") {
        "rtl_tcp and other inputs take no USB receiver"
    }
    check(of("-i", "rtl:0", "-i", "rtltcp:10.0.0.2:1234") == "" && of("-i", "rtltcp:h:1", "-i", "rtl:0") == "usb") {
        "the last -i wins"
    }
    check(of("-i") == "" && of() == "" && of("--", "-i", "rtl:0") == "") { "no input, or none before --" }
}

// #782 gate r1 G4: another option's operand is never an input, as the engine's parser reads it: the long-option pass
// (src/runtime/cli/compact.c) takes long options and their values out first, then getopt reads the short options.
private fun anotherOptionsOperandIsNoInput() {
    fun of(vararg args: String) = UsbReceiverKind.sourceOfArgs(arrayOf(*args))
    check(of("-i", "rtltcp:127.0.0.1:1234", "--api-token", "-irtl") == "") { "an API token that reads like -i" }
    check(of("-i", "rtltcp:h:1", "--api-token-file", "-iairspy") == "") { "a token file that reads like -i" }
    check(of("--api-token=-irtl", "-i", "udp:0.0.0.0:7355") == "") { "a token attached with =" }
    check(of("-i", "udp:0.0.0.0:7355", "--frame-log", "-irtl") == "") { "a long option's value, whatever it reads like" }
    check(of("-i", "udp:0.0.0.0:7355", "--iq-capture", "-irtl") == "") { "a value taken whatever it starts with" }
    check(of("--rtl-udp-control", "-irtl") == "usb") { "a value that must not start with '-' leaves an -i alone" }
    check(of("--rtl-udp-control", "6020", "-i", "rtl") == "usb") { "and is taken when it does not" }
    check(of("-i", "udp:0.0.0.0:7355", "-c", "-irtl") == "") { "a short option's separate value" }
    check(of("-i", "udp:0.0.0.0:7355", "-cirtl") == "") { "a short option's attached value" }
    check(of("-qirtl") == "usb" && of("-qi", "airspy") == "airspy") { "-i bundled behind options that take none" }
    check(of("-i", "rtl", "-o", "-i") == "usb") { "an -i that is another option's value names no input" }
    check(of("udp", "-i", "rtl") == "usb") { "getopt reads options past a bare argument" }
    check(of("-i", "rtl", "--no-such-option") == "" && of("-i", "rtl", "-c") == "") {
        "a start the engine's parser refuses opens no receiver"
    }
}

fun main() {
    aStartsReceiverComesFromItsInput()
    anotherOptionsOperandIsNoInput()
    airspyBesideUnrelatedDevicesIsAirspy()
    twoRtlDonglesAreUsb()
    rtlAndAirspyAreBoth()
    nothingKnownIsEmpty()
    classificationMatchesTheAccessRequest()
    aLabelBelongsToItsKindOfReceiver()
    println("UsbReceiverKindTest: OK")
}
