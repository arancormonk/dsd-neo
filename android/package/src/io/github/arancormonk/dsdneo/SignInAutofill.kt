// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.app.Activity
import android.content.Context
import android.graphics.Rect
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.SparseArray
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.ViewStructure
import android.view.ViewTreeObserver
import android.view.autofill.AutofillManager
import android.view.autofill.AutofillValue
import io.github.arancormonk.dsdneo.SignInAutofillState.Companion.PASSWORD
import io.github.arancormonk.dsdneo.SignInAutofillState.Companion.USERNAME
import io.github.arancormonk.dsdneo.SignInAutofillState.Effect

/**
 * The RadioReference sign-in form for Android's Autofill Framework, so a password manager can fill it and offer to save
 * what RadioReference accepted. Qt 6.11.3 draws its own text fields and gives Android nothing to autofill, so a host
 * [View] of the app's own describes the two fields as virtual children: field 1 the username, field 2 the password.
 *
 * Qt's relay (`src/ui/qt/sign_in_autofill.cpp`, through `DecoderHostAndroid`) calls [begin], [field], [request],
 * [commit] and [end] from its own thread. Each posts to the main looper, in order, and hands [SignInAutofillState]
 * the event; the effects it returns run there against the attached activity's [AutofillManager], read at that moment.
 * With no host attached, no manager, or the service off, the state is told [SignInAutofillState.disabled] and nothing
 * reaches the framework; the snapshot stays, so a service turned on later works from the next focus entry, and so does
 * [onResume]'s fresh read. A commit's notifications and the commit itself run in one posted operation.
 *
 * The host is the activity content's first child, behind Qt's layout and as large: childless, never focused or clicked,
 * no touches, hidden from accessibility, important for autofill, drawing nothing. It is never focused, so
 * [AutofillManager.requestAutofill] marks each request as coming from an unfocused view, and the framework shows its
 * dropdown instead of waiting for the keyboard's inline suggestions, which match only the view the keyboard types into
 * (Qt's). Qt sends rects in its window's pixels; the origin of the Qt window view (the largest visible
 * `org.qtproject.qt.android.QtWindow` under Qt's root layout, found again when it leaves the window) relative to the
 * host turns them into host pixels, and is read again on that view's layout changes, before each request and for each
 * structure. A request made before it is known is held (one diagnostic record, no values) and goes out once a layout
 * pass or Qt's next field report finds it, while the field is still focused and shown.
 *
 * Right before each request, and before each re-anchor's enter (which starts a request when the framework holds no
 * usable response), Qt's text cursor handle is taken off the screen ([QtCursorHandle]), so the structure the service
 * gets holds the activity's window only. An [AutofillManager.AutofillCallback] registered on the activity's
 * manager tells the state when the framework shows or hides its fill UI for a field; while it is shown, a move of the
 * focused field that has been still for [SETTLE_MS] re-anchors it once (as does a UI shown for a field that moved
 * after its request): the field is exited and entered again at its new bounds, which shows the UI there from the
 * response it already has, with no new request. While the UI is shown, a pre-draw listener on the host also reads its
 * place on screen, and a window move (which can leave the UI behind when the keyboard overlaps the window) arms the
 * same settled re-anchor; the listener is registered only while the state follows a shown UI.
 *
 * A fill goes back to Qt in one [DsdNative.nativeSignInFilled] call, under the token live when the framework filled.
 * Nothing on this path logs a value: a failure is recorded in [HostDiagnostics] by its exception's class name only.
 * Main thread, apart from the entry points Qt calls.
 */
object SignInAutofill {
    private const val QT_WINDOW_CLASS = "org.qtproject.qt.android.QtWindow"

    /** How long a moved field must stay still before a shown fill UI follows it; the same wait as Qt's first request. */
    const val SETTLE_MS = 300L

    private val main = Handler(Looper.getMainLooper())
    private val slot = ActivitySlot<Activity>()
    private val state = SignInAutofillState()
    private var host: Host? = null
    private var qtWindow: View? = null
    private val qtWindowLayout = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> guarded { originChanged() } }

    /** Any layout pass in the window may be the one that lays the Qt window out: a held request retries then. */
    private val hostLayout = ViewTreeObserver.OnGlobalLayoutListener {
        guarded {
            if (state.hasHeldRequest) {
                originChanged()
            }
        }
    }

    /**
     * While the fill UI is shown: every traversal of the host's window reads its place on screen, and only a change of
     * it goes on to the state ([hostMoved], then [refreshOrigin]). A window move reaches
     * the app as a resize or an insets change, each of which requests a layout, so a traversal follows; see
     * docs/code_map.md "Sign-in autofill". Never cancels the draw.
     */
    private val hostPreDraw = ViewTreeObserver.OnPreDrawListener {
        guarded {
            if (hostMoved()) {
                refreshOrigin()
            }
        }
        true
    }

    /** The host's place on screen as last reported to the state; null when none was. */
    private var reportedScreen: SignInAutofillState.Offset? = null
    private var preDrawWatched: View? = null

    private val settle = Runnable {
        guarded {
            refreshOrigin()
            execute { state.settled() }
        }
    }

    /** The framework's fill UI for the host's fields, shown or hidden. Delivered on the main thread. */
    private val fillUiEvents = object : AutofillManager.AutofillCallback() {
        override fun onAutofillEvent(view: View, virtualId: Int, event: Int) {
            guarded {
                if (view === host) {
                    execute { state.fillUi(virtualId, event == EVENT_INPUT_SHOWN) }
                }
            }
        }
    }
    private var callbackManager: AutofillManager? = null

    /** A sign-in form was shown: session [token] starts, and any older one is over. */
    @JvmStatic
    fun begin(token: Int) {
        post { execute { state.begin(token) } }
    }

    /** Field [vid] as it is now, in Qt window pixels; [value] is the username's text, empty for the password. */
    @JvmStatic
    fun field(token: Int, vid: Int, left: Int, top: Int, width: Int, height: Int, visible: Boolean, focused: Boolean,
              value: String?) {
        val rect = SignInAutofillState.Rect(left, top, width, height)
        post {
            execute { state.field(token, vid, rect, visible, focused, value.orEmpty()) }
            if (state.hasHeldRequest) {
                originChanged()
            }
        }
    }

    /** Field [vid] has focus and has been still: ask the password manager to fill it. */
    @JvmStatic
    fun request(token: Int, vid: Int, left: Int, top: Int, width: Int, height: Int) {
        val rect = SignInAutofillState.Rect(left, top, width, height)
        post {
            refreshOrigin()
            execute { enabled -> state.request(token, vid, rect, enabled) }
        }
    }

    /** RadioReference accepted these credentials: the password manager may offer to save them. */
    @JvmStatic
    fun commit(token: Int, username: String?, password: String?) {
        post { execute { state.commit(token, username.orEmpty(), password.orEmpty()) } }
    }

    /** The form of session [token] left the screen. */
    @JvmStatic
    fun end(token: Int) {
        post { execute { state.end(token) } }
    }

    /** Right after the activity's `super.onCreate()`, which set Qt's layout as the content view. */
    fun attach(activity: Activity) {
        guarded {
            val content = activity.findViewById<ViewGroup>(android.R.id.content)
            if (content != null) {
                removeHost()
                val view = Host(activity)
                content.addView(view, 0, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT))
                view.viewTreeObserver.addOnGlobalLayoutListener(hostLayout)
                host = view
                slot.attach(activity)
                val manager = activity.getSystemService(AutofillManager::class.java)
                manager?.registerCallback(fillUiEvents)
                callbackManager = manager
            }
        }
    }

    /** The service may have been turned on or off while the app was away. */
    fun onResume(activity: Activity) {
        guarded {
            if (slot.current === activity) {
                state.resumed(host?.let(::enabledManager) != null)
            }
        }
    }

    /** The activity is going: a framework session still open is cancelled, and the form is forgotten. */
    fun detach(activity: Activity) {
        guarded {
            if (slot.current === activity) {
                execute { state.end(state.token) }
                slot.detach(activity)
                removeHost()
            }
        }
    }

    private fun post(block: () -> Unit) {
        main.post { guarded(block) }
    }

    private fun guarded(block: () -> Unit) {
        try {
            block()
        } catch (e: RuntimeException) {
            HostDiagnostics.submit("sign-in autofill: ${e.javaClass.simpleName}")
        }
    }

    /** Runs [event] with whether the service takes calls now, and carries out its effects if it does. */
    private fun execute(event: (enabled: Boolean) -> List<Effect>) {
        val view = host
        val manager = view?.let(::enabledManager)
        if (view == null || manager == null) {
            state.disabled()
            event(false)
            watchHostWhileShown()
            return
        }
        event(true).forEach { apply(manager, view, it) }
        watchHostWhileShown()
    }

    /** The pre-draw watch on the host runs exactly while the state follows a shown fill UI. */
    private fun watchHostWhileShown() {
        val view = host?.takeIf { state.fillUiShown }
        val watched = preDrawWatched
        if (watched === view) {
            return
        }
        if (watched != null && watched.viewTreeObserver.isAlive) {
            watched.viewTreeObserver.removeOnPreDrawListener(hostPreDraw)
        }
        view?.viewTreeObserver?.addOnPreDrawListener(hostPreDraw)
        preDrawWatched = view
    }

    private fun enabledManager(view: View): AutofillManager? =
        view.context.getSystemService(AutofillManager::class.java)?.takeIf { it.isEnabled }

    private fun apply(manager: AutofillManager, view: View, effect: Effect) {
        when (effect) {
            is Effect.Request -> manager.requestAutofill(view, effect.vid, onScreen(view, effect.rect))
            Effect.HideTextHandle -> QtCursorHandle.hide(qtWindow, HostDiagnostics::submit)
            is Effect.Enter -> manager.notifyViewEntered(view, effect.vid, onScreen(view, effect.rect))
            is Effect.Exit -> manager.notifyViewExited(view, effect.vid)
            is Effect.Held -> HostDiagnostics.submit("sign-in autofill: request held until the Qt window is laid out")
            is Effect.Settle -> {
                main.removeCallbacks(settle)
                main.postDelayed(settle, SETTLE_MS)
            }
            is Effect.Value -> manager.notifyValueChanged(view, effect.vid, AutofillValue.forText(effect.text))
            is Effect.Visibility -> manager.notifyViewVisibilityChanged(view, effect.vid, effect.visible)
            Effect.Commit -> manager.commit()
            Effect.Cancel -> manager.cancel()
        }
    }

    /** A host-pixel rect in absolute screen coordinates, as requestAutofill() takes it. */
    private fun onScreen(view: View, rect: SignInAutofillState.Rect): Rect {
        val location = IntArray(2)
        view.getLocationOnScreen(location)
        val left = rect.left + location[0]
        val top = rect.top + location[1]
        return Rect(left, top, left + rect.width, top + rect.height)
    }

    /** The origin may have changed: read it again, and send a request held for want of it. */
    private fun originChanged() {
        refreshOrigin()
        if (state.hasHeldRequest) {
            execute { enabled -> state.retryHeld(enabled) }
        }
    }

    /**
     * Whether the host is laid out at another place on screen than the one last reported. A pre-draw pass with the host
     * where it was does no more, so the frequent redraws while a dropdown is shown cost one location read each.
     */
    private fun hostMoved(): Boolean {
        val view = host ?: return false
        if (!view.isLaidOut) {
            return false
        }
        val location = IntArray(2)
        view.getLocationOnScreen(location)
        return reportedScreen != SignInAutofillState.Offset(location[0], location[1])
    }

    /** The Qt window's origin relative to the host, or none while either is not laid out. */
    private fun refreshOrigin() {
        val view = host
        val window = view?.let(::qtWindowView)
        val hostLocation = IntArray(2)
        if (view != null && view.isLaidOut) {
            view.getLocationOnScreen(hostLocation)
            // The host's own place on screen, which a window move changes with nothing inside Qt's window moving.
            val place = SignInAutofillState.Offset(hostLocation[0], hostLocation[1])
            reportedScreen = place
            execute { state.screen(place) }
        }
        if (view == null || window == null || !view.isLaidOut || !window.isLaidOut) {
            state.origin(null)
            return
        }
        val windowLocation = IntArray(2)
        window.getLocationOnScreen(windowLocation)
        state.origin(SignInAutofillState.Offset(windowLocation[0] - hostLocation[0], windowLocation[1] - hostLocation[1]))
    }

    /** The cached Qt window view, or the largest visible one under Qt's root layout once that left the window. */
    private fun qtWindowView(view: Host): View? {
        qtWindow?.let { cached ->
            if (cached.isAttachedToWindow && cached.isShown) {
                return cached
            }
        }
        forgetQtWindow()
        val content = view.parent as? ViewGroup ?: return null
        var best: View? = null
        var bestArea = -1L
        for (i in 0 until content.childCount) {
            val layout = content.getChildAt(i) as? ViewGroup ?: continue
            for (j in 0 until layout.childCount) {
                val child = layout.getChildAt(j)
                val area = child.width.toLong() * child.height
                if (child.javaClass.name == QT_WINDOW_CLASS && child.isShown && area > bestArea) {
                    best = child
                    bestArea = area
                }
            }
        }
        best?.addOnLayoutChangeListener(qtWindowLayout)
        qtWindow = best
        return best
    }

    private fun forgetQtWindow() {
        qtWindow?.removeOnLayoutChangeListener(qtWindowLayout)
        qtWindow = null
    }

    private fun removeHost() {
        forgetQtWindow()
        main.removeCallbacks(settle)
        preDrawWatched?.let { if (it.viewTreeObserver.isAlive) it.viewTreeObserver.removeOnPreDrawListener(hostPreDraw) }
        preDrawWatched = null
        reportedScreen = null
        callbackManager?.unregisterCallback(fillUiEvents)
        callbackManager = null
        host?.let { view ->
            if (view.viewTreeObserver.isAlive) {
                view.viewTreeObserver.removeOnGlobalLayoutListener(hostLayout)
            }
            (view.parent as? ViewGroup)?.removeView(view)
        }
        host = null
        state.origin(null)
    }

    private fun provideStructure(view: Host, structure: ViewStructure) {
        if (host !== view) {
            return
        }
        refreshOrigin()
        val children = state.structure()
        val parent = structure.autofillId
        if (children.isEmpty() || parent == null) {
            return
        }
        val first = structure.addChildCount(children.size)
        children.forEachIndexed { index, field ->
            val child = structure.newChild(first + index)
            val username = field.vid == USERNAME
            child.setAutofillId(parent, field.vid)
            child.setAutofillType(View.AUTOFILL_TYPE_TEXT)
            child.setAutofillHints(arrayOf(if (username) View.AUTOFILL_HINT_USERNAME else View.AUTOFILL_HINT_PASSWORD))
            // The password's is always empty: the state never holds one.
            child.setAutofillValue(AutofillValue.forText(field.value))
            child.setDimens(field.rect.left, field.rect.top, 0, 0, field.rect.width, field.rect.height)
            child.setVisibility(if (field.visible) View.VISIBLE else View.INVISIBLE)
            child.setClassName("android.widget.EditText")
            child.setInputType(
                if (username) InputType.TYPE_CLASS_TEXT
                else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            )
            child.setDataIsSensitive(true)
            child.setHint(if (username) "Username" else "Password")
            child.setId(field.vid, view.context.packageName, null, if (username) "username" else "password")
            child.setFocused(field.focused)
        }
    }

    /** One fill, for the token live now, back to Qt in a single call. */
    private fun fill(view: Host, values: SparseArray<AutofillValue>) {
        val token = state.token
        if (host !== view || token == 0) {
            return
        }
        val username = values.get(USERNAME)?.takeIf { it.isText }?.textValue?.toString()
        val password = values.get(PASSWORD)?.takeIf { it.isText }?.textValue?.toString()
        if (username == null && password == null) {
            return
        }
        try {
            DsdNative.nativeSignInFilled(token, username != null, username, password != null, password)
        } catch (e: LinkageError) {
            HostDiagnostics.submit("sign-in autofill: ${e.javaClass.simpleName}")
        }
    }

    /** The virtual form's host view; see [SignInAutofill]. */
    private class Host(context: Context) : View(context) {
        init {
            isFocusable = false
            isFocusableInTouchMode = false
            isClickable = false
            isLongClickable = false
            importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
            importantForAutofill = IMPORTANT_FOR_AUTOFILL_YES
            setWillNotDraw(true)
        }

        override fun onTouchEvent(event: MotionEvent): Boolean = false

        override fun onProvideAutofillVirtualStructure(structure: ViewStructure, flags: Int) {
            guarded { provideStructure(this, structure) }
        }

        override fun isVisibleToUserForAutofill(virtualId: Int): Boolean = host === this && state.isVisible(virtualId)

        override fun autofill(values: SparseArray<AutofillValue>) {
            guarded { fill(this, values) }
        }
    }
}
