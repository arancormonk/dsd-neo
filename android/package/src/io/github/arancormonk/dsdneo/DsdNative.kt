// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Thin binding to the JNI lifecycle surface in `android/dsdneo_jni.cpp`.
 *
 * The library loaded here is the very same one Qt loads for the UI, so the service
 * and the UI share one instance of the engine's globals. Loading it from a companion
 * object matters: the service can be started with no QtActivity ever created, and
 * relying on Qt's activity bootstrap to have loaded the library ends in
 * UnsatisfiedLinkError.
 */
object DsdNative {
    const val STATUS_OK = 0
    const val STATUS_ERROR = -1
    const val STATUS_BAD_STATE = -2
    const val STATUS_CONFIG_EXIT = 1

    init {
        System.loadLibrary("dsd-neo-app_arm64-v8a")
    }

    external fun nativeInit(configDir: String, cacheDir: String): Int

    external fun nativeConfigure(args: Array<String>): Int

    external fun nativeRun(): Int

    external fun nativeStop(): Int

    external fun nativeDestroy(): Int

    external fun nativeIsRunning(): Boolean
}
