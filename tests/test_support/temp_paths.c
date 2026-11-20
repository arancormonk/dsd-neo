// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The temp paths a test process created, removed if the test leaves them behind (temp_paths.h). Every test target
 * links this through the test support include folder.
 *
 * A test removes what it creates. This catches the paths it misses: when the process ends normally, and when a failed
 * assert() aborts it before its own cleanup runs. At a normal end a folder goes with everything in it, which covers the
 * files the code under test wrote there, and the test fails, so a miss on a passing path is fixed in the test, not left
 * to this. The abort handler may make only calls that are safe in a signal handler, which cannot list a folder: it
 * removes the tracked files and the tracked folders that are empty by then, and names any folder it has to leave. Nothing here runs until the first path is tracked, so a test that makes no temp path is
 * untouched. Each path belongs to the process that created it: a forked child that exits removes only its own.
 *
 * Plain C and the OS calls only: it links into every test executable, including those that link nothing else.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>

#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if DSD_PLATFORM_WIN_NATIVE
#include <io.h>
#include <process.h>
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "temp_paths.h"

#define TEMP_PATHS_MAX       256
#define TEMP_PATHS_LEN       1024
#define TEMP_PATHS_MAX_DEPTH 32
/* A folder whose listing missed entries removed during the walk is walked again, this many times at most. */
#define TEMP_PATHS_PASSES    4

typedef struct {
    char path[TEMP_PATHS_LEN];
    long pid;
} temp_path_entry;

static temp_path_entry g_paths[TEMP_PATHS_MAX];
static size_t g_count;
static int g_installed;
static int g_full_reported;
static volatile sig_atomic_t g_cleaning;
static void (*g_prev_abort)(int);

static long
current_pid(void) {
#if DSD_PLATFORM_WIN_NATIVE
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

typedef enum { TEMP_PATH_ABSENT, TEMP_PATH_FILE, TEMP_PATH_DIR } temp_path_kind;

#if DSD_PLATFORM_WIN_NATIVE

/* @p path as UTF-16 when it is well-formed UTF-8, else in the ANSI code page, as the platform helpers decide. */
static int
widen(const char* path, wchar_t* out, int out_len) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, out_len) > 0) {
        return 0;
    }
    return MultiByteToWideChar(CP_ACP, 0, path, -1, out, out_len) > 0 ? 0 : -1;
}

static temp_path_kind
kind_of_wide(const wchar_t* path, DWORD* attrs) {
    *attrs = GetFileAttributesW(path);
    if (*attrs == INVALID_FILE_ATTRIBUTES) {
        return TEMP_PATH_ABSENT;
    }
    /* A junction or a link to a folder is removed itself, never walked into. */
    if ((*attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U && (*attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0U) {
        return TEMP_PATH_DIR;
    }
    return TEMP_PATH_FILE;
}

static void remove_tree_wide(const wchar_t* path, int depth);

static int
remove_children_wide(const wchar_t* dir, int depth) {
    wchar_t pattern[TEMP_PATHS_LEN];
    if (_snwprintf_s(pattern, TEMP_PATHS_LEN, _TRUNCATE, L"%ls\\*", dir) < 0) {
        return 0;
    }
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int removed = 0;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) {
            continue;
        }
        wchar_t child[TEMP_PATHS_LEN];
        if (_snwprintf_s(child, TEMP_PATHS_LEN, _TRUNCATE, L"%ls\\%ls", dir, found.cFileName) >= 0) {
            remove_tree_wide(child, depth + 1);
            removed = 1;
        }
    } while (FindNextFileW(find, &found));
    (void)FindClose(find);
    return removed;
}

static void
remove_tree_wide(const wchar_t* path, int depth) {
    DWORD attrs = 0;
    const temp_path_kind kind = kind_of_wide(path, &attrs);
    if (kind == TEMP_PATH_ABSENT) {
        return;
    }
    if ((attrs & FILE_ATTRIBUTE_READONLY) != 0U) {
        (void)SetFileAttributesW(path, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    }
    if (kind == TEMP_PATH_FILE) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            (void)RemoveDirectoryW(path);
        } else {
            (void)DeleteFileW(path);
        }
        return;
    }
    for (int pass = 0; depth < TEMP_PATHS_MAX_DEPTH && pass < TEMP_PATHS_PASSES; pass++) {
        if (!remove_children_wide(path, depth)) {
            break;
        }
    }
    (void)RemoveDirectoryW(path);
}

static int
path_exists(const char* path) {
    wchar_t wide[TEMP_PATHS_LEN];
    DWORD attrs = 0;
    return widen(path, wide, TEMP_PATHS_LEN) == 0 && kind_of_wide(wide, &attrs) != TEMP_PATH_ABSENT;
}

static void
remove_tree(const char* path) {
    wchar_t wide[TEMP_PATHS_LEN];
    if (widen(path, wide, TEMP_PATHS_LEN) == 0) {
        remove_tree_wide(wide, 0);
    }
}

/* One path, not walked into: a file, or a folder only when it is empty. */
static void
remove_one(const char* path) {
    wchar_t wide[TEMP_PATHS_LEN];
    DWORD attrs = 0;
    if (widen(path, wide, TEMP_PATHS_LEN) != 0) {
        return;
    }
    const temp_path_kind kind = kind_of_wide(wide, &attrs);
    if (kind == TEMP_PATH_DIR || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        (void)RemoveDirectoryW(wide);
    } else if (kind == TEMP_PATH_FILE) {
        (void)DeleteFileW(wide);
    }
}

static void
say(const char* text) {
    (void)_write(2, text, (unsigned int)strlen(text));
}

#else

static temp_path_kind
kind_of(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return TEMP_PATH_ABSENT;
    }
    /* lstat() reports a link as itself: it is removed, never followed. */
    return S_ISDIR(st.st_mode) ? TEMP_PATH_DIR : TEMP_PATH_FILE;
}

static void remove_tree_at(const char* path, int depth);

static int
remove_children(const char* dir, int depth) {
    DIR* d = opendir(dir);
    if (!d) {
        return 0;
    }
    int removed = 0;
    const struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[TEMP_PATHS_LEN];
        const int n = DSD_SNPRINTF(child, sizeof child, "%s/%s", dir, entry->d_name);
        if (n > 0 && (size_t)n < sizeof child) {
            remove_tree_at(child, depth + 1);
            removed = 1;
        }
    }
    (void)closedir(d);
    return removed;
}

static void
remove_tree_at(const char* path, int depth) {
    const temp_path_kind kind = kind_of(path);
    if (kind == TEMP_PATH_FILE) {
        (void)unlink(path);
        return;
    }
    if (kind != TEMP_PATH_DIR) {
        return;
    }
    for (int pass = 0; depth < TEMP_PATHS_MAX_DEPTH && pass < TEMP_PATHS_PASSES; pass++) {
        if (!remove_children(path, depth)) {
            break;
        }
    }
    (void)rmdir(path);
}

static int
path_exists(const char* path) {
    return kind_of(path) != TEMP_PATH_ABSENT;
}

static void
remove_tree(const char* path) {
    remove_tree_at(path, 0);
}

/* One path, not walked into: a file, or a folder only when it is empty. Calls only what a signal handler may. */
static void
remove_one(const char* path) {
    const temp_path_kind kind = kind_of(path);
    if (kind == TEMP_PATH_FILE) {
        (void)unlink(path);
    } else if (kind == TEMP_PATH_DIR) {
        (void)rmdir(path);
    }
}

static void
say(const char* text) {
    const ssize_t written = write(2, text, strlen(text));
    (void)written;
}

#endif

/* Remove this process's tracked paths that still exist, newest first, so a file goes before the folder it is in.
   @return how many there were. */
static size_t
remove_left_behind(const char* why) {
    if (g_cleaning) {
        return 0;
    }
    g_cleaning = 1;
    const long pid = current_pid();
    size_t found = 0;
    for (size_t i = g_count; i-- > 0;) {
        if (g_paths[i].pid != pid || !path_exists(g_paths[i].path)) {
            continue;
        }
        found++;
        remove_tree(g_paths[i].path);
        DSD_FPRINTF(stderr, "test temp cleanup: removed %s, %s%s\n", g_paths[i].path, why,
                    path_exists(g_paths[i].path) ? " (it could not be removed: an open file in it?)" : "");
    }
    g_count = 0;
    return found;
}

/* A test that ends normally with a path still there missed it in its own cleanup: it fails, whatever it returned, so
   the miss is fixed in the test rather than left to this. The streams are flushed first, since _Exit() skips that. */
static void
remove_at_exit(void) {
    if (remove_left_behind("which the test left behind") == 0U) {
        return;
    }
    DSD_FPRINTF(stderr, "test temp cleanup: failing the test, which must remove its own temp paths\n");
    (void)fflush(NULL);
    _Exit(EXIT_FAILURE);
}

/* After a failed assert(): this process's tracked files, and its tracked folders that are empty by then, newest
   first, through calls a signal handler may make (no folder listing, no formatted output). */
static void
remove_on_abort(int sig) {
    if (!g_cleaning) {
        g_cleaning = 1;
        const long pid = current_pid();
        for (size_t i = g_count; i-- > 0;) {
            const char* path = g_paths[i].path;
            if (g_paths[i].pid != pid || !path_exists(path)) {
                continue;
            }
            remove_one(path);
            const int left = path_exists(path);
            say(left ? "test temp cleanup: left " : "test temp cleanup: removed ");
            say(path);
            say(left ? " after the test aborted: it is not empty\n" : ", after the test aborted\n");
        }
    }
    void (*prev)(int) = g_prev_abort;
    (void)signal(sig, (prev == SIG_ERR || prev == SIG_IGN || prev == NULL) ? SIG_DFL : prev);
    (void)raise(sig);
}

/* Drop the entries whose paths are gone: the test removed them itself. */
static void
compact(void) {
    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        if (path_exists(g_paths[i].path)) {
            if (kept != i) {
                g_paths[kept] = g_paths[i];
            }
            kept++;
        }
    }
    g_count = kept;
}

static void
install(void) {
    if (g_installed) {
        return;
    }
    g_installed = 1;
    (void)atexit(remove_at_exit);
    g_prev_abort = signal(SIGABRT, remove_on_abort);
}

void
dsd_test_temp_track(const char* path) {
    if (!path || path[0] == '\0' || strlen(path) >= TEMP_PATHS_LEN) {
        return;
    }
    install();
    if (g_count == TEMP_PATHS_MAX) {
        compact();
    }
    if (g_count == TEMP_PATHS_MAX) {
        if (!g_full_reported) {
            g_full_reported = 1;
            DSD_FPRINTF(stderr, "test temp cleanup: %d temp paths exist at once; %s and later ones are not tracked\n",
                        TEMP_PATHS_MAX, path);
        }
        return;
    }
    DSD_SNPRINTF(g_paths[g_count].path, sizeof g_paths[g_count].path, "%s", path);
    g_paths[g_count].pid = current_pid();
    g_count++;
}
