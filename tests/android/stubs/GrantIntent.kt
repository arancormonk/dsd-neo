// SPDX-License-Identifier: GPL-3.0-or-later
package android.content

// Only the grant modes, for builds whose sources release persisted grants and start no activity.
class Intent {
    companion object {
        const val FLAG_GRANT_READ_URI_PERMISSION = 0x00000001
        const val FLAG_GRANT_WRITE_URI_PERMISSION = 0x00000002
    }
}
