// SPDX-License-Identifier: GPL-3.0-or-later
package android.system
import java.io.FileDescriptor
import java.io.FileInputStream
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

    @JvmField
    val ELOOP = 40

    // open(2)'s flags as Linux numbers them. The stub honours O_NOFOLLOW only.
    @JvmField
    val O_RDONLY = 0

    @JvmField
    val O_NONBLOCK = 0x800

    @JvmField
    val O_NOFOLLOW = 0x20000

    @JvmField
    val O_CLOEXEC = 0x80000

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
    fun lstat(path: String): StructStat = statOf("lstat", path)

    // Each descriptor open() gave, with its stream (which keeps it open) and the file as it was opened, which fstat()
    // gives: the JDK reads no attributes of a descriptor, and a name replaced after the open leaves these as they were.
    private class Opened(val stream: FileInputStream?, val stat: StructStat)

    private val opened: MutableMap<FileDescriptor, Opened> = Collections.synchronizedMap(HashMap())

    // A read-only open. O_NOFOLLOW refuses a link at the name with ELOOP; a FIFO must be opened with O_NONBLOCK (as
    // Android's open() returns at once only then), and fstat() then reports it as no regular file. Missing, no access:
    // ENOENT, EACCES.
    @JvmStatic
    fun open(path: String, flags: Int, mode: Int): FileDescriptor {
        // A link at the name: refused with O_NOFOLLOW, followed without it, as open(2) does; what is opened is then the
        // target, and fstat() reports the target.
        val link = java.nio.file.Files.isSymbolicLink(java.nio.file.Paths.get(path))
        if (link && flags and OsConstants.O_NOFOLLOW != 0) {
            throw ErrnoException("open", OsConstants.ELOOP)
        }
        val stat = statOf("open", path, follow = link)
        if (!OsConstants.S_ISREG(stat.st_mode)) {
            // A FIFO: on Android an open without O_NONBLOCK waits for a writer that never comes, so the stub refuses
            // to stand in for it (the test fails instead of hanging).
            val other = java.nio.file.Files.readAttributes(
                java.nio.file.Paths.get(path),
                java.nio.file.attribute.BasicFileAttributes::class.java,
                java.nio.file.LinkOption.NOFOLLOW_LINKS,
            ).isOther
            check(!other || flags and OsConstants.O_NONBLOCK != 0) { "a FIFO opened without O_NONBLOCK: $path" }
            // Nothing to read here, and nothing waits: a descriptor whose fstat() says what the name is.
            val none = FileDescriptor()
            opened[none] = Opened(null, stat)
            return none
        }
        val stream = try {
            FileInputStream(path)
        } catch (e: java.io.FileNotFoundException) {
            throw ErrnoException("open", OsConstants.ENOENT)
        }
        opened[stream.fd] = Opened(stream, stat)
        return stream.fd
    }

    @JvmStatic
    fun fstat(fd: FileDescriptor): StructStat = opened[fd]?.stat ?: throw ErrnoException("fstat", 9)

    @JvmStatic
    fun close(fd: FileDescriptor) {
        val held = opened.remove(fd) ?: throw ErrnoException("close", 9)
        held.stream?.close()
    }

    // How many descriptors open() gave are still open: a test sees each one closed.
    fun openCount(): Int = opened.size

    private fun statOf(call: String, path: String, follow: Boolean = false): StructStat {
        val options = if (follow) emptyArray() else arrayOf(java.nio.file.LinkOption.NOFOLLOW_LINKS)
        val attributes = try {
            java.nio.file.Files.readAttributes(
                java.nio.file.Paths.get(path),
                java.nio.file.attribute.BasicFileAttributes::class.java,
                *options,
            )
        } catch (e: java.nio.file.NoSuchFileException) {
            throw ErrnoException(call, OsConstants.ENOENT)
        } catch (e: java.nio.file.AccessDeniedException) {
            throw ErrnoException(call, OsConstants.EACCES)
        }
        val modified = attributes.lastModifiedTime().toInstant()
        return StructStat(
            if (attributes.isRegularFile) OsConstants.S_IFREG_BITS else 0,
            attributes.size(),
            StructTimespec(modified.epochSecond, modified.nano.toLong()),
        )
    }
}
