// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <dsd-neo/protocol/tetra/tetra_acelp.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <dsd-neo/core/dsd_time.h>
#include <sndfile.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <poll.h>
#  include <signal.h>
#  include <time.h>
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <pthread.h>
#endif

/* EN 300 395-2 V1.3.1 Table 4 bit-position tables for TCH/FS — from osmo-tetra tch_reordering.c */

/* 51 Class-0 (unprotected) bit positions, 1-indexed */
static const uint8_t class0_positions[] = {
    35, 36, 37, 38, 39, 40, 41, 42, 43, 47, 48,
    56, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 74,
    75, 83, 88, 89, 90, 91, 92, 93, 94, 95, 96,
    97, 101, 102, 110, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 124, 128, 129, 137
};

/* 56 Class-1 bit positions, 1-indexed */
static const uint8_t class1_positions[] = {
    58, 85, 112,
    54, 81, 108, 135,
    50, 77,
    104, 131,
    45, 72, 99, 126,
    55, 82, 109, 136,
    5, 13, 34,
    8, 16, 17, 22, 23, 24, 25, 26,
    6, 14, 7, 15,
    60, 87, 114,
    46,
    73, 100, 127,
    44, 71, 98, 125,
    33, 49,
    76, 103, 130,
    59, 86, 113,
    57, 84, 111
};

/* 30 Class-2 (least protected) bit positions, 1-indexed */
static const uint8_t class2_positions[] = {
    18, 19, 20, 21,
    31, 32,
    53, 80, 107, 134,
    1, 2, 3, 4,
    9, 10, 11, 12,
    27, 28, 29, 30,
    52, 79, 106, 133,
    51, 78, 105, 132
};
/* Compute counts from arrays to avoid mismatches between defines and initializers */
enum {
    NUM_ACELP_CLASS0_BITS = sizeof(class0_positions) / sizeof(class0_positions[0]),
    NUM_ACELP_CLASS1_BITS = sizeof(class1_positions) / sizeof(class1_positions[0]),
    NUM_ACELP_CLASS2_BITS = sizeof(class2_positions) / sizeof(class2_positions[0])
};
#define NUM_ACELP_BITS (NUM_ACELP_CLASS0_BITS + NUM_ACELP_CLASS1_BITS + NUM_ACELP_CLASS2_BITS)

/* TETRA TCH/FS speech frame parameters (ETSI EN 300 395-2) */
#define TETRA_TCH_FRAME_BITS     137  /* encoded bits in one ACELP codec frame       */
#define TETRA_TCH_FRAME_SAMPLES  240  /* decoded PCM16 samples (30 ms @ 8 kHz)        */
#define TETRA_VOCODER_READ_TIMEOUT_MS 2000

typedef enum {
    TETRA_VOC_STATUS_UNKNOWN = 0,
    TETRA_VOC_STATUS_OPEN,
    TETRA_VOC_STATUS_MISSING_CMD,
    TETRA_VOC_STATUS_OPEN_FAILED
} tetra_voc_status_t;

static int s_voc_last_timeout = 0;

/* Convert decoded TYPE2 bits (at least 274 bits) into two consecutive codec frames.
 * Each frame occupies TETRA_TCH_FRAME_BITS (137) bytes in the output buffer.
 * The output buffer must be at least 2 * TETRA_TCH_FRAME_BITS = 274 bytes.
 *
 * The class-reordered TYPE2 layout is (ETSI EN 300 395-2 Table 4):
 *   [class0_frame0, class0_frame1, class0_frame0, class0_frame1, ...]  × 51 pairs
 *   [class1 interleaved]  × 56 pairs
 *   [class2 interleaved]  × 30 pairs
 * Total: 2 × (51+56+30) = 274 input bits consumed.
 */
void tetra_acelp_reorder(const uint8_t* in, uint8_t* out, int len) {
    if (!in || !out || len < 2 * NUM_ACELP_BITS)
        return;
    const uint8_t *in_cur = in;
    int bit, frame;

    /* Class0 */
    for (bit = 0; bit < NUM_ACELP_CLASS0_BITS; bit++) {
        for (frame = 0; frame < 2; frame++)
            out[frame * TETRA_TCH_FRAME_BITS + class0_positions[bit] - 1] = in_cur[2*bit + frame];
    }
    in_cur += 2*NUM_ACELP_CLASS0_BITS;

    /* Class1 */
    for (bit = 0; bit < NUM_ACELP_CLASS1_BITS; bit++) {
        for (frame = 0; frame < 2; frame++)
            out[frame * TETRA_TCH_FRAME_BITS + class1_positions[bit] - 1] = in_cur[2*bit + frame];
    }
    in_cur += 2*NUM_ACELP_CLASS1_BITS;

    /* Class2 */
    for (bit = 0; bit < NUM_ACELP_CLASS2_BITS; bit++) {
        for (frame = 0; frame < 2; frame++)
            out[frame * TETRA_TCH_FRAME_BITS + class2_positions[bit] - 1] = in_cur[2*bit + frame];
    }
}

/* =========================================================================
 * Persistent external vocoder subprocess
 *
 * The TETRA ACELP speech codec is proprietary (ETSI ETS 300 395-2) and has
 * no freely-available open-source implementation.  dsd-neo supports an
 * external command-line vocoder via the TETRA_VOCODER_CMD environment
 * variable.  The subprocess must implement this synchronous protocol:
 *
 *   → stdin  : TETRA_TCH_FRAME_BITS raw bytes (one byte per coded bit, 0/1)
 *   ← stdout : TETRA_TCH_FRAME_SAMPLES × sizeof(int16_t) bytes of PCM16LE
 *              (8 kHz, mono, little-endian)
 *   repeat indefinitely until stdin is closed.
 *
 * Do not vendor ETSI ACELP sources into this repository. The top-level
 * build does not compile a speech codec. tools/tetra/acelp_adapter is a
 * separate CMake project that the developer points at a local C-CODE tree.
 * tools/tetra/vocoder_stub.py is the silence stub used by tests.
 * Linux and macOS launch TETRA_VOCODER_CMD with sh -c. Windows uses
 * cmd.exe /C. The byte protocol above is the same on both.
 * ========================================================================= */

#ifdef _WIN32
/* ---- Windows implementation ---- */
static struct {
    HANDLE h_write;   /* parent writes bits  → child stdin  */
    HANDLE h_read;    /* parent reads  PCM   ← child stdout */
    HANDLE h_proc;
    HANDLE h_thread;
    HANDLE h_job;
    int    open;
} s_voc;

static tetra_voc_status_t s_voc_status = TETRA_VOC_STATUS_UNKNOWN;

static int
build_windows_shell_command(const char *cmd, char *out, size_t out_sz) {
    const char *comspec;

    if (!cmd || !cmd[0] || !out || out_sz < 16)
        return 0;

    comspec = getenv("ComSpec");
    if (!comspec || !comspec[0])
        comspec = "C:\\Windows\\System32\\cmd.exe";

    /*
     * Run through cmd.exe so TETRA_VOCODER_CMD can be a normal Windows
     * command line: .exe with args, .cmd/.bat wrappers, or interpreter-based
     * commands such as "py -3 path\\to\\vocoder.py".
     */
    return snprintf(out, out_sz, "\"%s\" /D /S /C \"%s\"", comspec, cmd) < (int)out_sz;
}

static int voc_open(const char *cmd) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE pipe_stdin_r,  pipe_stdin_w;
    HANDLE pipe_stdout_r, pipe_stdout_w;
    if (!CreatePipe(&pipe_stdin_r,  &pipe_stdin_w,  &sa, 0)) return 0;
    if (!CreatePipe(&pipe_stdout_r, &pipe_stdout_w, &sa, 0)) {
        CloseHandle(pipe_stdin_r); CloseHandle(pipe_stdin_w);
        return 0;
    }
    /* make the parent-side handles non-inheritable */
    SetHandleInformation(pipe_stdin_w,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(pipe_stdout_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput  = pipe_stdin_r;
    si.hStdOutput = pipe_stdout_w;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags    = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    char cmd_buf[2048];
    DWORD last_error;
    HANDLE job = NULL;

    if (!build_windows_shell_command(cmd, cmd_buf, sizeof(cmd_buf))) {
        fprintf(stderr, "[TETRA] vocoder command too long or invalid\n");
        CloseHandle(pipe_stdin_r);  CloseHandle(pipe_stdin_w);
        CloseHandle(pipe_stdout_r); CloseHandle(pipe_stdout_w);
        return 0;
    }

    job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        ZeroMemory(&limits, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            CloseHandle(job);
            job = NULL;
        }
    }

    if (!job || !CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE, CREATE_SUSPENDED,
                                NULL, NULL, &si, &pi)) {
        last_error = GetLastError();
        fprintf(stderr, "[TETRA] failed to start vocoder command '%s' (CreateProcess=%lu)\n",
                cmd, (unsigned long)last_error);
        if (job) CloseHandle(job);
        CloseHandle(pipe_stdin_r);  CloseHandle(pipe_stdin_w);
        CloseHandle(pipe_stdout_r); CloseHandle(pipe_stdout_w);
        return 0;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        last_error = GetLastError();
        fprintf(stderr, "[TETRA] failed to contain vocoder process tree (Win32=%lu)\n",
                (unsigned long)last_error);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(job);
        CloseHandle(pipe_stdin_r);  CloseHandle(pipe_stdin_w);
        CloseHandle(pipe_stdout_r); CloseHandle(pipe_stdout_w);
        return 0;
    }
    ResumeThread(pi.hThread);
    /* close child-side handles in parent */
    CloseHandle(pipe_stdin_r);
    CloseHandle(pipe_stdout_w);

    s_voc.h_write  = pipe_stdin_w;
    s_voc.h_read   = pipe_stdout_r;
    s_voc.h_proc   = pi.hProcess;
    s_voc.h_thread = pi.hThread;
    s_voc.h_job    = job;
    s_voc.open     = 1;
    s_voc_status   = TETRA_VOC_STATUS_OPEN;
    fprintf(stderr, "[TETRA] vocoder subprocess started\n");
    return 1;
}

static int voc_send_recv(const uint8_t *bits, int nbits, int16_t *pcm, int nsamples) {
    DWORD nwritten, nread;
    s_voc_last_timeout = 0;
    if (!WriteFile(s_voc.h_write, bits, (DWORD)nbits, &nwritten, NULL)
            || (int)nwritten != nbits)
        return 0;
    int pcm_bytes = nsamples * (int)sizeof(int16_t);
    DWORD got = 0;
    uint8_t *p = (uint8_t *)pcm;
    ULONGLONG deadline = GetTickCount64() + TETRA_VOCODER_READ_TIMEOUT_MS;
    while ((int)got < pcm_bytes) {
        DWORD available = 0;
        if (!PeekNamedPipe(s_voc.h_read, NULL, 0, NULL, &available, NULL))
            break;
        if (available == 0) {
            if (WaitForSingleObject(s_voc.h_proc, 0) == WAIT_OBJECT_0)
                break;
            if (GetTickCount64() >= deadline) {
                s_voc_last_timeout = 1;
                break;
            }
            Sleep(5);
            continue;
        }
        if (!ReadFile(s_voc.h_read, p + got, (DWORD)(pcm_bytes - (int)got), &nread, NULL)
                || nread == 0)
            break;
        got += nread;
    }
    return (int)(got / sizeof(int16_t));
}

void tetra_vocoder_close(void) {
    if (!s_voc.open) return;
    CloseHandle(s_voc.h_write);
    CloseHandle(s_voc.h_read);
    if (WaitForSingleObject(s_voc.h_proc, 200) == WAIT_TIMEOUT) {
        TerminateJobObject(s_voc.h_job, 1);
        WaitForSingleObject(s_voc.h_proc, 2000);
    }
    CloseHandle(s_voc.h_proc);
    CloseHandle(s_voc.h_thread);
    CloseHandle(s_voc.h_job);
    memset(&s_voc, 0, sizeof(s_voc));
}

#else
/* ---- POSIX implementation ---- */
static struct {
    int    fd_write;   /* parent writes bits  → child stdin  */
    int    fd_read;    /* parent reads  PCM   ← child stdout */
    pid_t  pid;
    int    open;
} s_voc;

static tetra_voc_status_t s_voc_status = TETRA_VOC_STATUS_UNKNOWN;

/* A closed child stdin must be a recoverable codec error, not a SIGPIPE that
 * terminates the decoder. Block SIGPIPE only in this thread and consume the
 * signal generated by this write before restoring the caller's mask. */
static int
voc_write_all(int fd, const uint8_t *data, size_t len)
{
    sigset_t block, oldmask, pending;
    int had_pending = 0;
    size_t total = 0;

    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &block, &oldmask) != 0)
        return 0;
    if (sigpending(&pending) == 0)
        had_pending = sigismember(&pending, SIGPIPE);

    while (total < len) {
        ssize_t written = write(fd, data + total, len - total);
        if (written > 0) {
            total += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        break;
    }

    if (total != len && !had_pending && sigpending(&pending) == 0
            && sigismember(&pending, SIGPIPE)) {
        int consumed_signal;
        (void)sigwait(&block, &consumed_signal);
    }
    (void)pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    return total == len;
}

static int voc_open(const char *cmd) {
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0) return 0;
    if (pipe(from_child) != 0) {
        close(to_child[0]); close(to_child[1]);
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return 0;
    }
    if (pid == 0) {
        /* child */
        /* Give the shell and any codec it starts a private process group so
         * timeout recovery can terminate the whole command tree. */
        if (setpgid(0, 0) != 0)
            _exit(127);
        dup2(to_child[0],   STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execlp("sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    /* parent */
    close(to_child[0]);
    close(from_child[1]);
    s_voc.fd_write = to_child[1];
    s_voc.fd_read  = from_child[0];
    s_voc.pid      = pid;
    s_voc.open     = 1;
    s_voc_status   = TETRA_VOC_STATUS_OPEN;
    fprintf(stderr, "[TETRA] vocoder subprocess started\n");
    return 1;
}

static int64_t
voc_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int voc_send_recv(const uint8_t *bits, int nbits, int16_t *pcm, int nsamples) {
    s_voc_last_timeout = 0;
    if (!voc_write_all(s_voc.fd_write, bits, (size_t)nbits)) return 0;
    int pcm_bytes = nsamples * (int)sizeof(int16_t);
    int total = 0;
    uint8_t *p = (uint8_t *)pcm;
    int64_t deadline = voc_monotonic_ms() + TETRA_VOCODER_READ_TIMEOUT_MS;
    while (total < pcm_bytes) {
        struct pollfd ready = {s_voc.fd_read, POLLIN, 0};
        int poll_rc;
        int64_t remaining = deadline - voc_monotonic_ms();
        if (remaining <= 0) {
            s_voc_last_timeout = 1;
            break;
        }
        do {
            poll_rc = poll(&ready, 1, (int)remaining);
        } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc == 0)
            s_voc_last_timeout = 1;
        if (poll_rc <= 0 || (ready.revents & (POLLERR | POLLNVAL)))
            break;
        ssize_t r = read(s_voc.fd_read, p + total, (size_t)(pcm_bytes - total));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        total += (int)r;
    }
    return total / (int)sizeof(int16_t);
}

void tetra_vocoder_close(void) {
    if (!s_voc.open) return;
    close(s_voc.fd_write);
    close(s_voc.fd_read);
    int status = 0;
    int exited = 0;
    for (int i = 0; i < 20; i++) {
        pid_t rc = waitpid(s_voc.pid, &status, WNOHANG);
        if (rc == s_voc.pid || (rc < 0 && errno == ECHILD)) {
            exited = 1;
            break;
        }
        struct timespec pause = {0, 10000000L}; /* 10 ms, at most 200 ms */
        nanosleep(&pause, NULL);
    }
    /* The shell may have exited while a codec descendant still holds our
     * pipe open. Close the private process group as well as reaping the child. */
    kill(-s_voc.pid, SIGTERM);
    if (!exited) {
        while (waitpid(s_voc.pid, &status, 0) < 0 && errno == EINTR) {}
    }
    memset(&s_voc, 0, sizeof(s_voc));
}
#endif /* _WIN32 / POSIX */

/* Ensure the vocoder subprocess is running.  Returns 1 if open. */
static int voc_ensure_open(void) {
    if (s_voc.open) return 1;
    const char *cmd = getenv("TETRA_VOCODER_CMD");
    if (!cmd || !cmd[0]) {
        s_voc_status = TETRA_VOC_STATUS_MISSING_CMD;
        return 0;
    }
    if (!voc_open(cmd)) {
        s_voc_status = TETRA_VOC_STATUS_OPEN_FAILED;
        return 0;
    }
    return 1;
}

static const char *
tetra_audio_backend_name(const dsd_opts *opts) {
    if (!opts)
        return "unknown";

    switch (opts->audio_out_type) {
        case 0: return "pulse";
        case 1: return "file/stdout";
        case 8: return "udp";
        default: return "other";
    }
}

static int
tetra_has_live_audio_route(const dsd_opts *opts) {
    if (!opts)
        return 0;
    if (opts->slot1_on != 1)
        return 0;

    if (opts->audio_out_type == 0)
        return opts->audio_out_stream != NULL;
    if (opts->audio_out_type == 1)
        return opts->audio_out_fd >= 0;
    if (opts->audio_out_type == 8)
        return 1;
    return 0;
}

static int
tetra_has_any_audio_sink(const dsd_opts *opts) {
    if (!opts)
        return 0;
    if (tetra_has_live_audio_route(opts))
        return 1;
    if (opts->wav_out_f != NULL && (opts->dmr_stereo_wav == 1 || opts->static_wav_file == 1))
        return 1;
    return 0;
}

/* tetra_acelp_decode() kept for backward ABI compat; no longer does anything.
 * Callers should use tetra_acelp_process_tch() instead.
 */
void tetra_acelp_decode(const uint8_t* bits, int len) {
    (void)bits; (void)len;
}

/* -------------------------------------------------------------------------
 * tetra_acelp_process_tch()
 *
 * Full TCH/FS voice pipeline for the 274 speech bits decoded
 * from both NDB blocks:
 *
 *   type-2 bits → class reorder → 2 × ACELP codec frames
 *               → external vocoder subprocess (TETRA_VOCODER_CMD)
 *               → PCM16 routing (PA / UDP / raw FD / WAV)
 *
 * The reorder produces two 137-bit ACELP frames from all 274
 * type-2 bits (102 class-0 + 112 class-1 + 60 class-2, interleaved
 * for two frames).  Each frame is sent to the vocoder independently.
 *
 * @type2_bits : decoded sensitivity-ordered bits (at least 274 required)
 * @type2_len  : number of valid bits
 * @block_idx  : 0 for combined TCH/FS (informational)
 * @opts/state : standard dsd-neo context
 * ------------------------------------------------------------------------- */
void tetra_acelp_process_tch(const uint8_t *type2_bits, int type2_len,
                              int block_idx,
                              dsd_opts *opts, dsd_state *state)
{
    static int warned_gate = 0;
    static int warned_missing_cmd = 0;
    static int warned_open_failed = 0;
    static int warned_no_sink = 0;
    static int warned_control_channel = 0;

    if (!type2_bits || !opts) {
        fprintf(stderr, "[TETRA] cannot process TCH without bits and decoder options\n");
        return;
    }

    if (!tetra_acelp_channel_gate_passes(opts)) {
        if (!warned_control_channel) {
            fprintf(stderr, "[TETRA] waiting for a traffic-channel grant; control-channel bursts are not speech\n");
            warned_control_channel = 1;
        }
        return;
    }

    /* Phase 12 floor-grant gate. */
    if (!tetra_acelp_slot_gate_passes(block_idx ? block_idx : 1, state)) {
        if (!warned_gate) {
            fprintf(stderr,
                "[TETRA] audio suppressed by slot gate: grant_valid=%u vc_slot=%u block=%d\n",
                state ? (unsigned)state->tetra_tx_granted_valid : 0u,
                state ? (unsigned)state->tetra_vc_slot : 0u,
                block_idx ? block_idx : 1);
            warned_gate = 1;
        }
        return;
    }

    if (type2_len < 274) {
        fprintf(stderr,
            "[TETRA TCH] B%d: only %d type-2 bits (need 274), skipping\n",
            block_idx, type2_len);
        return;
    }

    /* Reception activity is independent of codec availability. Do not let
     * generic no-carrier handling use a stale pre-call activity timestamp. */
    dsd_mark_vc_sync(state);

    /* —— Step 1: class reorder → 2 codec frames —— */
    uint8_t codec[2 * TETRA_TCH_FRAME_BITS];  /* 274 bytes */
    memset(codec, 0, sizeof(codec));
    tetra_acelp_reorder(type2_bits, codec, type2_len);

    /* —— Step 2: vocoder + audio routing for each frame —— */
    if (!voc_ensure_open()) {
        if (s_voc_status == TETRA_VOC_STATUS_MISSING_CMD) {
            if (state) state->tetra_vocoder_status = TETRA_VOCODER_STATUS_COMMAND_MISSING;
            if (!warned_missing_cmd) {
                fprintf(stderr,
                    "[TETRA] audio disabled: TETRA_VOCODER_CMD is not set. "
                    "Example: TETRA_VOCODER_CMD=\"python tools/tetra/vocoder_stub.py --tone\"\n");
                warned_missing_cmd = 1;
            }
        } else if (s_voc_status == TETRA_VOC_STATUS_OPEN_FAILED) {
            if (state) {
                state->tetra_vocoder_status = TETRA_VOCODER_STATUS_START_FAILED;
                state->tetra_vocoder_errors++;
            }
            if (!warned_open_failed) {
                const char *cmd = getenv("TETRA_VOCODER_CMD");
                fprintf(stderr,
                    "[TETRA] audio disabled: failed to start vocoder command: %s\n",
                    (cmd && cmd[0]) ? cmd : "(unset)");
                warned_open_failed = 1;
            }
        }
        return;
    }

    if (state) state->tetra_vocoder_status = TETRA_VOCODER_STATUS_READY;

    if (!tetra_has_any_audio_sink(opts)) {
        if (!warned_no_sink) {
            fprintf(stderr,
                "[TETRA] vocoder is producing PCM but no audio sink is active: "
                "audio_out=%d backend=%s slot1_on=%d stream=%s fd=%d wav=%s static_wav=%d\n",
                opts ? opts->audio_out : -1,
                tetra_audio_backend_name(opts),
                opts ? opts->slot1_on : -1,
                (opts && opts->audio_out_stream) ? "yes" : "no",
                opts ? opts->audio_out_fd : -1,
                (opts && opts->wav_out_f) ? "yes" : "no",
                opts ? opts->static_wav_file : 0);
            warned_no_sink = 1;
        }
    }

    for (int f = 0; f < 2; f++) {
        const uint8_t *frame = codec + f * TETRA_TCH_FRAME_BITS;

        int16_t pcm[TETRA_TCH_FRAME_SAMPLES];
        memset(pcm, 0, sizeof(pcm));
        int nsamples = voc_send_recv(frame, TETRA_TCH_FRAME_BITS,
                                     pcm, TETRA_TCH_FRAME_SAMPLES);
        if (nsamples != TETRA_TCH_FRAME_SAMPLES) {
            if (state) {
                state->tetra_vocoder_status = s_voc_last_timeout
                                                ? TETRA_VOCODER_STATUS_TIMEOUT
                                                : TETRA_VOCODER_STATUS_SHORT_OUTPUT;
                state->tetra_vocoder_errors++;
            }
            fprintf(stderr,
                    "[TETRA] vocoder returned a short PCM frame (%d/%d samples); resetting\n",
                    nsamples, TETRA_TCH_FRAME_SAMPLES);
            tetra_vocoder_close();
            return;
        }
        if (state) state->tetra_vocoder_frames++;

        /* —— Step 3: audio routing (same pattern as M17 / Codec2) —— */
        if (opts->slot1_on == 1) {
            if (opts->audio_out_type == 0 && opts->audio_out_stream)
                dsd_audio_write(opts->audio_out_stream, pcm, (size_t)nsamples);
            if (opts->audio_out_type == 8)
                dsd_udp_audio_hook_blast(opts, state,
                                         (size_t)nsamples * sizeof(int16_t), pcm);
            if (opts->audio_out_type == 1 && opts->audio_out_fd >= 0)
                dsd_write(opts->audio_out_fd, pcm,
                          (size_t)nsamples * sizeof(int16_t));
        }

        if (opts->wav_out_f != NULL && opts->dmr_stereo_wav == 1)
            sf_write_short(opts->wav_out_f, pcm, nsamples);

        if (opts->wav_out_f != NULL && opts->static_wav_file == 1) {
            short ss[TETRA_TCH_FRAME_SAMPLES * 2];
            for (int i = 0; i < nsamples && i < TETRA_TCH_FRAME_SAMPLES; i++) {
                ss[i * 2 + 0] = pcm[i];
                ss[i * 2 + 1] = pcm[i];
            }
            sf_write_short(opts->wav_out_f, ss, nsamples * 2);
        }
    }
}
