// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import android.content.Intent
import org.qtproject.qt.android.bindings.QtActivity

/**
 * The app's Activity, which is stock [QtActivity] plus one thing it has to do on the way
 * out.
 *
 * `QtActivityBase.onDestroy()` calls `QtNative.terminateQtNativeApplication()`, which
 * blocks until Qt's `main()` returns, and only then `System.exit(0)`. Left alone, `main()`
 * never returns here and that block is permanent. Two things have to line up wrong for
 * that, and both do:
 *
 * 1. Qt's own teardown raises the quit that would end `main()` only when its Android event
 *    dispatcher is *already stopped*; when the dispatcher is still running — the state a
 *    swipe out of recents leaves behind — it sends no quit and waits anyway.
 * 2. Raising the quit by hand is not enough either. Since Qt 6, `QCoreApplication::quit()`
 *    asks every top-level window to close first and abandons the quit if any refuses, and
 *    `Main.qml`'s `onClosing` always refuses: declining the close is how a window dismissal
 *    becomes `moveToBackground()` so a decode session survives the user leaving the app.
 *
 * So [DsdNative.nativeQuitUi] uses `exit()`, which leaves the event loop without consulting
 * windows. See the JNI side in `android/decoder_host_android.cpp`.
 *
 * The deadlock is invisible in a plain Qt app: with nothing holding the process up, Android
 * reaps it as an empty process and the hang goes unnoticed. This app keeps a foreground
 * service for the decoder, so the process is retained, the block persists, and everything
 * queued behind the main thread — the service's own callbacks included — times out into
 * "DSD-neo isn't responding".
 */
class DsdNeoActivity : QtActivity() {

    override fun onDestroy() {
        // Not on a configuration change: those destroy and immediately recreate the
        // Activity, and Qt keeps the process across them (it skips its own teardown for
        // exactly this case). Quitting there would take the UI down on a rotation.
        if (!isChangingConfigurations) {
            // Signalled directly rather than through DecoderService.stopDecoder(): that
            // posts an intent, and intents are delivered on this very thread, which is
            // about to block inside super.onDestroy() and then never come back — the stop
            // would sit in the queue until System.exit(0) threw it away. nativeStop() only
            // raises the engine's stop flag, so it is safe to call from here.
            //
            // Best effort, not a guarantee: nothing waits for the engine to finish
            // unwinding, because the only thread that could wait is the one whose blocking
            // this override exists to prevent. It buys the decode loop a chance to close
            // its files before the process goes.
            DsdNative.nativeStop()

            // Withdraws the service's outstanding start request, and it has to happen
            // before the process goes. The engine thread is what normally calls stopSelf(),
            // once nativeRun() returns — but Qt's teardown ends in System.exit(0) and will
            // not wait for that. A process that dies with a start request still standing is
            // a *crashed* service to ActivityManager, which duly restarts it:
            //
            //   Process ... has died: fg +50 FGS
            //   Scheduling restart of crashed service .DecoderService for start-requested
            //   Start proc ... for service {.DecoderService}
            //
            // — a decoder respawned with no UI and no way to have asked for it, holding a
            // notification the user just swiped away. stopService() is an ActivityManager
            // call rather than a queued callback, so it lands even though this thread is
            // about to block.
            stopService(Intent(this, DecoderService::class.java))

            DsdNative.nativeQuitUi()
        }
        super.onDestroy()
    }
}
