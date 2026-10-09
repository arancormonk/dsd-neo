// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/** Stands in for an Activity instance; equal by value, so identity is what the slot must go by. */
private data class Window(val name: String)

private fun attachAndDetach() {
    val slot = ActivitySlot<Window>()
    check(slot.current == null)
    check(!slot.detach(Window("a"))) { "nothing to detach" }
    val first = Window("main")
    slot.attach(first)
    check(slot.current === first)
    check(slot.detach(first) && slot.current == null)
    check(!slot.detach(first)) { "a second detach is a no-op" }
}

private fun aRecreatedActivityReattachesAndTheStaleDetachIsIgnored() {
    val slot = ActivitySlot<Window>()
    val old = Window("main")
    val recreated = Window("main") // Equal to the old instance, but a different object.
    slot.attach(old)
    slot.attach(recreated) // The new instance's onCreate can run before the old one's onDestroy.
    check(slot.current === recreated)
    check(!slot.detach(old)) { "the old instance's late onDestroy must not detach the new one" }
    check(slot.current === recreated)
    check(slot.detach(recreated) && slot.current == null)
    // And in the usual order: the old one goes first, then the new one attaches.
    slot.attach(old)
    check(slot.detach(old))
    slot.attach(recreated)
    check(slot.current === recreated)
}

fun main() {
    attachAndDetach()
    aRecreatedActivityReattachesAndTheStaleDetachIsIgnored()
    println("PASS: activity slot attaches and detaches by identity")
}
