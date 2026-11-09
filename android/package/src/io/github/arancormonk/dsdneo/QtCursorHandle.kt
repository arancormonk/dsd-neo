// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.view.View
import android.view.ViewGroup
import android.widget.PopupWindow

/**
 * Qt's text cursor handle, taken off the screen right before [SignInAutofill] asks for an autofill request.
 *
 * Qt 6.11.3 draws the insertion handle under a tapped text field as a [PopupWindow] of its own (`CursorHandle`, held
 * by the `QtEditText` under the Qt window in `m_cursorHandle`, its popup in `m_popup`). The structure an autofill
 * service gets has one window per root view of the activity, so with the handle up it has a second window, and a
 * service that reads the app from the last window's title (KeePassDX 4.5.8) refuses the request and never answers.
 * Dismissing the popup takes that window away at once: the handle's popup style has no exit transition, so
 * [PopupWindow.dismiss] removes its view immediately, and the structure is captured later, in a message the request
 * itself causes. Qt shows the handle again on its next cursor move or tap; until then it stays away, as Qt's own
 * four-second hide would leave it.
 *
 * Only the insertion handle: the selection handles and the Cut/Copy/Paste menu come with an edit the person is making,
 * and are left alone. The two fields are read by name, which `qt-rules.pro` keeps and `tools/check_android_r8.py`
 * checks in the release DEX. Fail-soft: when Qt's structure is not what this expects (a Qt upgrade), nothing is
 * changed, the request goes ahead as it would have, and one record says so for the process.
 */
object QtCursorHandle {
    private const val EDIT_TEXT_CLASS = "org.qtproject.qt.android.QtEditText"

    private var reported = false

    /** Dismisses the insertion handle of the Qt text input under [qtWindow], if it is showing. Never throws. */
    fun hide(qtWindow: View?, record: (String) -> Unit) {
        val problem = try {
            dismiss(qtWindow)
        } catch (e: ReflectiveOperationException) {
            e.javaClass.simpleName
        } catch (e: RuntimeException) {
            e.javaClass.simpleName
        }
        if (problem != null && !reported) {
            reported = true
            record("sign-in autofill: Qt's text cursor handle cannot be hidden ($problem)")
        }
    }

    /** What went wrong, or null when the handle is gone (or was never up). */
    private fun dismiss(qtWindow: View?): String? {
        val window = qtWindow as? ViewGroup ?: return "no Qt window"
        val editText = (0 until window.childCount).map(window::getChildAt).firstOrNull {
            it.javaClass.name == EDIT_TEXT_CLASS
        } ?: return "no QtEditText"
        val handle = read(editText, "m_cursorHandle") ?: return null
        val popup = read(handle, "m_popup") as? PopupWindow ?: return null
        if (popup.isShowing) {
            popup.dismiss()
        }
        return null
    }

    private fun read(owner: Any, name: String): Any? {
        val field = owner.javaClass.getDeclaredField(name)
        field.isAccessible = true
        return field.get(owner)
    }
}
