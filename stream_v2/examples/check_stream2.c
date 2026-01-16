#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zmq.h>

#include "stream2.h"

static volatile sig_atomic_t g_stop = 0;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
/* Use QueryPerformanceCounter as a monotonic source */
static int clock_gettime_monotonic(struct timespec* tp) {
    LARGE_INTEGER freq, ctr;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr))
        return -1;
    tp->tv_sec = (time_t)(ctr.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)((ctr.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}
#define clock_gettime(id, tp) clock_gettime_monotonic(tp)

/* Map Ctrl+C to our stop flag */
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}
#endif

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

struct stats {
    uint64_t images_total;
    uint64_t bytes_total;
    struct timespec last_report;
};

static double time_diff_sec(const struct timespec* a,
                            const struct timespec* b) {
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

static void report_stats(struct stats* s, int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = time_diff_sec(&now, &s->last_report);
    if (!force && elapsed < 3.0)
        return;

    double gigabytes_total = s->bytes_total / 1e9;
    printf("\rimages: %" PRIu64 "  received: %.3f GB", s->images_total,
           gigabytes_total);
    if (force)
        printf("\n");
    fflush(stdout);

    s->last_report = now;
}

static enum stream2_result parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stats* s) {
    enum stream2_result r;

    struct stream2_msg* msg;
    if ((r = stream2_parse_msg(msg_data, msg_size, &msg))) {
        fprintf(stderr, "error: %i parsing message\n", (int)r);
        return r;
    }

    s->bytes_total += msg_size;
    if (msg->type == STREAM2_MSG_IMAGE)
        s->images_total++;

    stream2_free_msg(msg);

    return STREAM2_OK;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s HOST\n", argv[0]);
        return EXIT_FAILURE;
    }

    char address[100];
    sprintf(address, "tcp://%s:31001", argv[1]);

    void* ctx = zmq_ctx_new();
    void* socket = zmq_socket(ctx, ZMQ_PULL);

    // Keep stats responsive even when idle.
    int rcv_timeout_ms = 500;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));

    zmq_connect(socket, address);
    zmq_msg_t msg;
    zmq_msg_init(&msg);

#ifndef _WIN32
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
#else
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

    struct stats s = {0};
    clock_gettime(CLOCK_MONOTONIC, &s.last_report);

    for (;;) {
        if (g_stop) {
            report_stats(&s, 1);
            break;
        }

        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EAGAIN) {
                report_stats(&s, 0);
                if (g_stop)
                    break;
                continue;
            }
            if (errno == EINTR) {
                if (g_stop) {
                    report_stats(&s, 1);
                    break;
                }
                continue;
            }
            perror("zmq_msg_recv");
            break;
        }

        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);

        enum stream2_result r;
        if ((r = parse_msg(msg_data, msg_size, &s)))
            break;

        report_stats(&s, 0);

        if (g_stop)
            break;
    }

    report_stats(&s, 1);
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    return EXIT_FAILURE;
}
