// SPDX-License-Identifier: GPL-3.0-or-later
package android.system
import java.io.FileDescriptor
import java.io.FileOutputStream
import java.util.Collections

class ErrnoException(val functionName: String, val errno: Int) : Exception("$functionName failed: errno $errno")

// fsync and ftruncate on the JVM's own descriptors. A test can make fsync fail (or wait) through [fsyncHook], and
// sees each descriptor synced.
object Os {
    @Volatile
    var fsyncHook: (FileDescriptor) -> Unit = {}
    val synced: MutableList<FileDescriptor> = Collections.synchronizedList(mutableListOf())

    @JvmStatic
    fun fsync(fd: FileDescriptor) {
        fsyncHook(fd)
        fd.sync()
        synced += fd
    }

    @JvmStatic
    fun ftruncate(fd: FileDescriptor, length: Long) {
        FileOutputStream(fd).channel.truncate(length)
    }
}
