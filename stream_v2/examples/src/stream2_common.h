/*
 * stream2_common.h - Platform compatibility layer for Stream V2 examples
 *
 * Provides cross-platform support for:
 * - clock_gettime (Windows polyfill)
 * - Signal handling (SIGINT / Ctrl+C)
 * - Time difference calculations
 */
#ifndef STREAM2_COMMON_H
#define STREAM2_COMMON_H

#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sys/time.h>
#endif

/* Windows doesn't have CLOCK_MONOTONIC or clockid_t */
#ifdef _WIN32
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
typedef int clockid_t;
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

/* Global stop flag - set by signal handlers */
extern volatile sig_atomic_t g_stop;
/* Set when disk I/O detects out-of-space to halt writers */
extern volatile sig_atomic_t g_out_of_space;

#ifdef _WIN32
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* Windows clock_gettime polyfill using QueryPerformanceCounter */
static inline int clock_gettime_monotonic(struct timespec* tp) {
    LARGE_INTEGER freq, ctr;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr))
        return -1;
    tp->tv_sec = (time_t)(ctr.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)((ctr.QuadPart % freq.QuadPart) * 1000000000LL /
                         freq.QuadPart);
    return 0;
}
#define clock_gettime(id, tp) clock_gettime_monotonic(tp)

/* Windows Ctrl+C handler */
static inline BOOL WINAPI stream2_console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}

#define stream2_install_signal_handler() \
    SetConsoleCtrlHandler(stream2_console_ctrl_handler, TRUE)

#else /* POSIX */

/* POSIX SIGINT handler */
static inline void stream2_handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

#define stream2_install_signal_handler()           \
    do {                                           \
        struct sigaction sa = {0};                 \
        sa.sa_handler = stream2_handle_sigint;     \
        sigaction(SIGINT, &sa, NULL);              \
    } while (0)

#endif /* _WIN32 */

/* Time difference in seconds */
static inline double stream2_time_diff_sec(const struct timespec* a,
                                           const struct timespec* b) {
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

/* Time difference in nanoseconds */
static inline long long stream2_time_diff_ns(const struct timespec* a,
                                             const struct timespec* b) {
    return (a->tv_sec - b->tv_sec) * 1000000000LL + (a->tv_nsec - b->tv_nsec);
}

/* Parse buffer limit from STREAM2_BUFFER_GB environment variable */
static inline uint64_t stream2_parse_buffer_limit_gb(uint64_t default_gb) {
    const char* env_limit = getenv("STREAM2_BUFFER_GB");
    if (env_limit && *env_limit) {
        char* endp = NULL;
        errno = 0;
        unsigned long long gb = strtoull(env_limit, &endp, 10);
        if (errno == 0 && endp && *endp == '\0' && gb > 0) {
            return gb * 1024ULL * 1024ULL * 1024ULL;
        } else {
            fprintf(stderr,
                    "WARN: STREAM2_BUFFER_GB invalid, using default %" PRIu64
                    "GB\n",
                    default_gb);
        }
    }
    return default_gb * 1024ULL * 1024ULL * 1024ULL;
}

#endif /* STREAM2_COMMON_H */
