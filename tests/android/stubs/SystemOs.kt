// SPDX-License-Identifier: GPL-3.0-or-later
package android.system
import java.io.FileDescriptor
import java.io.FileOutputStream
import java.util.Collections

class ErrnoException(val functionName: String, val errno: Int) : Exception("$functionName failed: errno $errno")

// The errno values these tests meet, as Linux numbers them, and the file-type test lstat()'s mode takes.
object OsConstants {
    @JvmField
    val ENOENT = 2

    @JvmField
    val ENOTDIR = 20

    @JvmField
    val EACCES = 13

    @JvmField
    val ENOSPC = 28

    private const val S_IFMT = 0xF000
    private const val S_IFREG = 0x8000

    // The mode lstat() gives a regular file (the type bits only; the stub keeps no permissions).
    const val S_IFREG_BITS = S_IFREG

    @JvmStatic
    fun S_ISREG(mode: Int) = (mode and S_IFMT) == S_IFREG
}

class StructTimespec(@JvmField val tv_sec: Long, @JvmField val tv_nsec: Long)

class StructStat(@JvmField val st_mode: Int, @JvmField val st_size: Long, @JvmField val st_mtim: StructTimespec)

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

    // The name itself, never a link's target, read with the JDK's basic attributes (every platform's JDK has them):
    // the regular-file mode bit, size and the modification time to the nanosecond where the file system keeps it.
    @JvmStatic
    fun lstat(path: String): StructStat {
        val attributes = try {
            java.nio.file.Files.readAttributes(
                java.nio.file.Paths.get(path),
                java.nio.file.attribute.BasicFileAttributes::class.java,
                java.nio.file.LinkOption.NOFOLLOW_LINKS,
            )
        } catch (e: java.nio.file.NoSuchFileException) {
            throw ErrnoException("lstat", OsConstants.ENOENT)
        } catch (e: java.nio.file.AccessDeniedException) {
            throw ErrnoException("lstat", OsConstants.EACCES)
        }
        val modified = attributes.lastModifiedTime().toInstant()
        return StructStat(
            if (attributes.isRegularFile) OsConstants.S_IFREG_BITS else 0,
            attributes.size(),
            StructTimespec(modified.epochSecond, modified.nano.toLong()),
        )
    }
}
