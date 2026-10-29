// SPDX-License-Identifier: GPL-3.0-or-later
// Intentional rule fixtures; run with semgrep --test, never compile.

import android.system.Os

fun environmentWrites(name: String) {
    // ruleid: dsd-neo.no-environment-write-kotlin
    Os.setenv(name, "1", true)
    // ruleid: dsd-neo.no-environment-write-kotlin
    Os.unsetenv(name)
    // ruleid: dsd-neo.no-environment-write-kotlin
    android.system.Os.setenv(name, "1", true)
    // Reading is not writing.
    // ok: dsd-neo.no-environment-write-kotlin
    val value = Os.getenv(name)
    // ok: dsd-neo.no-environment-write-kotlin
    DsdNative.nativeInit(name)
}
