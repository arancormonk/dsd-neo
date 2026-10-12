// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "file_compat_internal.h"
#include "write_frames_internal.h"

int
dsd_fileno(FILE* fp) {
    return fp ? fileno(fp) : -1;
}

int
dsd_isatty(int fd) {
    return isatty(fd);
}

int
dsd_set_binary_mode(int fd) {
    return fcntl(fd, F_GETFL) < 0 ? -1 : 0;
}

int
dsd_dup(int oldfd) {
    return dup(oldfd);
}

int
dsd_dup2(int oldfd, int newfd) {
    return dup2(oldfd, newfd);
}

int
dsd_close(int fd) {
    return close(fd);
}

int
dsd_fsync(int fd) {
    return fsync(fd);
}

int
dsd_fstat(int fd, dsd_stat_t* st) {
    return fstat(fd, st);
}

int
dsd_stat_path(const char* path, dsd_stat_t* st) {
    return stat(path, st);
}

int
dsd_stat_is_regular(const dsd_stat_t* st) {
    return st && S_ISREG(st->st_mode);
}

int
dsd_fchmod(int fd, int mode) {
    return fchmod(fd, (mode_t)mode);
}

static int
dsd_private_open_flags(const char* mode) {
    if (!mode || mode[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    int flags = 0;
    switch (mode[0]) {
        case 'w': flags = O_CREAT | O_TRUNC; break;
        case 'a': flags = O_CREAT | O_APPEND; break;
        default: errno = EINVAL; return -1;
    }

    flags |= (strchr(mode, '+') != NULL) ? O_RDWR : O_WRONLY;
    if (strchr(mode, 'x') != NULL) {
        /* C11's exclusive create: only a "w" mode creates, and an existing file is left untouched (EEXIST). */
        if (mode[0] != 'w') {
            errno = EINVAL;
            return -1;
        }
        flags |= O_EXCL;
    }
    return flags;
}

/* The mode for the stream over an opened descriptor: fdopen() has no use for 'x', the descriptor already exists, and
   not every C library accepts it there. */
static int
dsd_private_stream_mode(const char* mode, char* out, size_t out_size) {
    size_t n = 0;
    for (const char* p = mode; *p != '\0'; ++p) {
        if (*p == 'x') {
            continue;
        }
        if (n + 1U >= out_size) {
            errno = EINVAL;
            return -1;
        }
        out[n++] = *p;
    }
    out[n] = '\0';
    return 0;
}

FILE*
dsd_fopen_private_ex(const char* path, const char* mode, int* out_opened) {
    if (out_opened) {
        *out_opened = 0;
    }
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }
    if (mode[0] == 'r') {
        FILE* fp = fopen(path, mode);
        if (out_opened) {
            *out_opened = fp != NULL;
        }
        return fp;
    }

    int flags = dsd_private_open_flags(mode);
    if (flags < 0) {
        return NULL;
    }
    char stream_mode[8];
    if (dsd_private_stream_mode(mode, stream_mode, sizeof stream_mode) != 0) {
        return NULL;
    }

    int fd = open(path, flags, (mode_t)0600);
    if (fd < 0) {
        return NULL;
    }
    /* The file is created or emptied now, whatever becomes of the stream over it. */
    if (out_opened) {
        *out_opened = 1;
    }

    FILE* fp = fdopen(fd, stream_mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
    }
    return fp;
}

FILE*
dsd_fopen_private(const char* path, const char* mode) {
    return dsd_fopen_private_ex(path, mode, NULL);
}

FILE*
dsd_fopen_existing_regular_file(const char* path, const char* mode) {
    if (!path || path[0] == '\0' || !mode || mode[0] != 'r' || strchr(mode, '+') != NULL) {
        errno = EINVAL;
        return NULL;
    }

    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        return NULL;
    }

    dsd_stat_t st;
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    if (!dsd_stat_is_regular(&st)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }

    FILE* fp = fdopen(fd, mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
    }
    return fp;
}

static void
dsd_set_cloexec_best_effort(int fd) {
#ifdef FD_CLOEXEC
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
#else
    (void)fd;
#endif
}

FILE*
dsd_fopen_private_temp_for_replace(const char* final_path, char* tmp_path, size_t tmp_path_size, const char* mode) {
    if (!final_path || final_path[0] == '\0' || !tmp_path || tmp_path_size == 0 || !mode || mode[0] == 'r') {
        errno = EINVAL;
        return NULL;
    }

    int n = DSD_SNPRINTF(tmp_path, tmp_path_size, "%s.tmp.XXXXXX", final_path);
    if (n < 0 || (size_t)n >= tmp_path_size) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        return NULL;
    }
    dsd_set_cloexec_best_effort(fd);
    (void)fchmod(fd, (mode_t)0600);

    FILE* fp = fdopen(fd, mode);
    if (!fp) {
        int saved_errno = errno ? errno : EINVAL;
        close(fd);
        (void)remove(tmp_path);
        errno = saved_errno;
        return NULL;
    }
    return fp;
}

static void
dsd_fsync_parent_dir_best_effort(const char* path) {
    if (!path || path[0] == '\0') {
        return;
    }

    const char* slash = strrchr(path, '/');
    char dir_buf[4096];
    const char* dir = ".";
    if (slash && slash != path) {
        size_t len = (size_t)(slash - path);
        if (len >= sizeof(dir_buf)) {
            return;
        }
        DSD_MEMCPY(dir_buf, path, len);
        dir_buf[len] = '\0';
        dir = dir_buf;
    } else if (slash == path) {
        dir = "/";
    }

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(dir, flags);
    if (fd >= 0) {
        (void)fsync(fd);
        (void)close(fd);
    }
}

int
dsd_replace_file_with_temp(const char* tmp_path, const char* final_path) {
    if (!tmp_path || tmp_path[0] == '\0' || !final_path || final_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (rename(tmp_path, final_path) != 0) {
        return -1;
    }
    dsd_fsync_parent_dir_best_effort(final_path);
    return 0;
}

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

/* The filesystem cannot do what was asked (a rename flag, a hard link), as opposed to refusing this rename. */
static int
dsd_rename_unsupported_errno(int err) {
    if (err == EINVAL || err == ENOSYS || err == ENOTSUP) {
        return 1;
    }
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    if (err == EOPNOTSUPP) {
        return 1;
    }
#endif
    return 0;
}

int
dsd_rename_noreplace_by_reservation(const char* from, const char* to) {
    /* An exclusive create claims the name: of every creator, exactly one gets it, and one that finds it taken fails
       with EEXIST. The rename then replaces only that claim, the empty file this call just made. */
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = open(to, flags, 0600);
    if (fd < 0) {
        return -1;
    }
    (void)close(fd);
    if (rename(from, to) != 0) {
        const int err = errno;
        (void)unlink(to);
        errno = err;
        return -1;
    }
    return 0;
}

int
dsd_rename_noreplace_by_link(const char* from, const char* to, int* out_source_left) {
    if (out_source_left) {
        *out_source_left = 0;
    }
    if (link(from, to) == 0) {
        /* @p to names the file now; a source name that will not go names it too, and the caller is told. */
        if (unlink(from) != 0 && out_source_left) {
            *out_source_left = 1;
        }
        return 0;
    }
    const int err = errno;
    /* EPERM is how Linux FAT and exFAT refuse a hard link; anything else refused this link, not links. */
    if (err != EPERM && !dsd_rename_unsupported_errno(err)) {
        errno = err;
        return -1;
    }
    /* No hard links here either. */
    return dsd_rename_noreplace_by_reservation(from, to);
}

int
dsd_rename_noreplace(const char* from, const char* to, int* out_source_left) {
    if (out_source_left) {
        *out_source_left = 0;
    }
    if (!from || from[0] == '\0' || !to || to[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
#if defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, (unsigned int)RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (!dsd_rename_unsupported_errno(errno)) {
        return -1;
    }
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    if (renamex_np(from, to, RENAME_EXCL) == 0) {
        return 0;
    }
    if (!dsd_rename_unsupported_errno(errno)) {
        return -1;
    }
#endif
    return dsd_rename_noreplace_by_link(from, to, out_source_left);
}

static int
dsd_local_file_name_is_unsafe(const char* requested) {
    return requested == NULL || requested[0] == '\0' || requested[0] == '/' || strchr(requested, '/') != NULL
           || strchr(requested, '\\') != NULL || strstr(requested, "..") != NULL;
}

static int
dsd_copy_resolved_local_name(const char* name, char* out, size_t out_size) {
    int n = DSD_SNPRINTF(out, out_size, "%s", name);
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static FILE*
dsd_open_resolved_local_entry(const char* name, char* out, size_t out_size, int* saved_errno) {
    if (dsd_copy_resolved_local_name(name, out, out_size) != 0) {
        *saved_errno = errno;
        return NULL;
    }

    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(out, flags);
    if (fd < 0) {
        *saved_errno = errno ? errno : EINVAL;
        return NULL;
    }

    dsd_stat_t st;
    if (fstat(fd, &st) != 0) {
        *saved_errno = errno ? errno : EINVAL;
        close(fd);
        return NULL;
    }
    if (!dsd_stat_is_regular(&st)) {
        *saved_errno = EINVAL;
        close(fd);
        return NULL;
    }

    FILE* result = fdopen(fd, "r");
    if (!result) {
        *saved_errno = errno ? errno : EINVAL;
        close(fd);
        return NULL;
    }
    *saved_errno = 0;
    return result;
}

FILE*
dsd_fopen_existing_local_file(const char* requested, char* out, size_t out_size) {
    if (!out || out_size == 0 || dsd_local_file_name_is_unsafe(requested)) {
        errno = EINVAL;
        return NULL;
    }
    out[0] = '\0';

    DIR* dir = opendir(".");
    if (!dir) {
        return NULL;
    }

    FILE* result = NULL;
    int saved_errno = ENOENT;
    int found = 0;
    errno = 0;
    const struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, requested) != 0) {
            continue;
        }
        found = 1;
        result = dsd_open_resolved_local_entry(ent->d_name, out, out_size, &saved_errno);
        break;
    }
    if (!found && errno != 0) {
        saved_errno = errno;
    }
    if (closedir(dir) != 0 && result) {
        int close_errno = errno;
        fclose(result);
        errno = close_errno;
        return NULL;
    }
    errno = saved_errno;
    return result;
}

int
dsd_resolve_existing_local_file(const char* requested, char* out, size_t out_size) {
    FILE* fp = dsd_fopen_existing_local_file(requested, out, out_size);
    if (!fp) {
        return -1;
    }
    return fclose(fp);
}

// Split out of dsd_dir_list() to keep it under the CCN ceiling tools/lizard.sh
// enforces; it carries every per-entry rejection the walk applies.
static int
dsd_dir_entry_is_regular_file(const char* dir, const char* name) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }

    char path[1024];
    int n = DSD_SNPRINTF(path, sizeof path, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof path) {
        return 0; // cannot build a path for it, so it cannot be classified
    }

    // No ent->d_type: it is not portable off glibc and appears nowhere else
    // in this tree. dsd_stat_path()/dsd_stat_is_regular() are the project
    // classifiers and also drop symlinks-to-directories and device nodes.
    dsd_stat_t st;
    return (dsd_stat_path(path, &st) == 0 && dsd_stat_is_regular(&st)) ? 1 : 0;
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
int
dsd_dir_list(const char* dir, dsd_dir_list_cb cb, void* user) {
    if (!dir || dir[0] == '\0' || !cb) {
        errno = EINVAL;
        return -1;
    }

    DIR* d = opendir(dir);
    if (!d) {
        return -1;
    }

    int list_errno = 0;
    for (;;) {
        // Cleared per iteration so a dsd_stat_path() failure inside the
        // classifier cannot be mistaken for a readdir() failure on the next pass.
        errno = 0;
        const struct dirent* ent = readdir(d);
        if (!ent) {
            list_errno = errno; // 0 means a clean end of directory
            break;
        }
        if (!dsd_dir_entry_is_regular_file(dir, ent->d_name)) {
            continue;
        }
        if (cb(ent->d_name, user) != 0) {
            break;
        }
    }

    if (closedir(d) != 0 && list_errno == 0) {
        list_errno = errno;
    }
    errno = list_errno;
    return list_errno == 0 ? 0 : -1;
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

ssize_t
dsd_read(int fd, void* buf, size_t count) {
    return read(fd, buf, count);
}

ssize_t
dsd_write(int fd, const void* buf, size_t count) {
    return write(fd, buf, count);
}

static ssize_t
// cppcheck-suppress constParameterCallback -- signature fixed by dsd_write_frames_ops
write_frames_posix_write(void* ctx, const void* buf, size_t count) {
    return write(*(const int*)ctx, buf, count);
}

/* Wait for room, at most @p timeout_ms in all: an interrupted poll() waits only for what is left, so a stream of
   signals cannot stretch the wait. */
static int
// cppcheck-suppress constParameterCallback -- signature fixed by dsd_write_frames_ops
write_frames_posix_wait_writable(void* ctx, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = *(const int*)ctx;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    const uint64_t deadline_ns = dsd_time_monotonic_ns() + ((uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ULL);
    int wait_ms = timeout_ms > 0 ? timeout_ms : 0;
    for (;;) {
        const int rc = poll(&pfd, 1, wait_ms);
        if (rc < 0 && errno == EINTR) {
            const uint64_t now_ns = dsd_time_monotonic_ns();
            if (now_ns >= deadline_ns) {
                return 0;
            }
            wait_ms = (int)((deadline_ns - now_ns + 999999ULL) / 1000000ULL);
            continue;
        }
        if (rc < 0) {
            return -1;
        }
        /* POLLERR/POLLHUP count as ready: the next write reports the error itself. */
        return rc > 0 ? 1 : 0;
    }
}

#if !defined(F_SETNOSIGPIPE)
/* Take back the SIGPIPE the calling thread's write raised, which it holds blocked and pending. Linux and the BSDs that
   have it raise a write's SIGPIPE on the writing thread, so it is there to take; sigtimedwait() never waits for it. A
   system without sigtimedwait() takes it with sigwait() once sigpending() shows it, which cannot block for the same
   reason. */
static void
write_frames_consume_own_sigpipe(const sigset_t* pipe_set) {
#if defined(__linux__) || defined(__ANDROID__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    const struct timespec no_wait = {0, 0};
    while (sigtimedwait(pipe_set, NULL, &no_wait) < 0 && errno == EINTR) {}
#else
    sigset_t pending;
    int sig = 0;
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        (void)sigwait(pipe_set, &sig);
    }
#endif
}
#endif

dsd_write_frames_status
dsd_write_frames(int fd, const void* buf, size_t bytes, size_t frame_bytes, size_t* written) {
    int fd_copy = fd;
    const dsd_write_frames_ops ops = {write_frames_posix_write, write_frames_posix_wait_writable, &fd_copy};
#if defined(F_SETNOSIGPIPE)
    /* Darwin raises a write's SIGPIPE on the process, not the thread, so blocking it here would not keep it from
       another thread; the descriptor's own flag stops it being raised at all. */
    (void)fcntl(fd, F_SETNOSIGPIPE, 1);
    return dsd_write_frames_loop(&ops, buf, bytes, frame_bytes, written);
#else
    /* A write to a pipe or socket nobody reads raises SIGPIPE on the writing thread, which kills the process unless it
       ignores the signal. Block it in this thread for the write and consume the one the write raised; a SIGPIPE that
       was already pending is someone else's and stays. */
    sigset_t pipe_set;
    sigset_t old_mask;
    sigset_t pending;
    (void)sigemptyset(&pipe_set);
    (void)sigaddset(&pipe_set, SIGPIPE);
    const int masked = pthread_sigmask(SIG_BLOCK, &pipe_set, &old_mask) == 0;
    const int was_pending = masked && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;

    const dsd_write_frames_status status = dsd_write_frames_loop(&ops, buf, bytes, frame_bytes, written);
    const int saved_errno = errno;

    if (masked) {
        if (status == DSD_WRITE_FRAMES_ERROR && saved_errno == EPIPE && !was_pending) {
            write_frames_consume_own_sigpipe(&pipe_set);
        }
        (void)pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    }
    errno = saved_errno;
    return status;
#endif
}

const char*
dsd_null_device(void) {
    return "/dev/null";
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
