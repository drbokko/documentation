/* stream2_eiger_acquire - EIGER DCU configure + stream buffer + TIFF (see --help in main) */
#include "eiger_client.h"
#include "stream2.h"
#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "stream2_tiff.h"
#include <zmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>

#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define MAX_RECV_THREADS 32
#define DEFAULT_RECV_THREADS 10

struct recv_shared {
    void* zmq_ctx;
    char zmq_address[160];
    volatile int recv_stop;
    struct stream2_buffer_ctx buf;
#ifdef _WIN32
    CRITICAL_SECTION buf_cs;
    CRITICAL_SECTION stats_cs;
#else
    pthread_mutex_t buf_mu;
    pthread_mutex_t stats_mu;
#endif
    struct stream2_stats stats;
    int image_frames;   /* under stats lock */
    int parse_errors;   /* under stats lock */
    volatile int buffer_errors; /* set from recv threads if buffer fails */
};

struct recv_thread_arg {
    struct recv_shared* shared;
};

static void lock_buf(struct recv_shared* s) {
#ifdef _WIN32
    EnterCriticalSection(&s->buf_cs);
#else
    pthread_mutex_lock(&s->buf_mu);
#endif
}

static void unlock_buf(struct recv_shared* s) {
#ifdef _WIN32
    LeaveCriticalSection(&s->buf_cs);
#else
    pthread_mutex_unlock(&s->buf_mu);
#endif
}

static void lock_stats(struct recv_shared* s) {
#ifdef _WIN32
    EnterCriticalSection(&s->stats_cs);
#else
    pthread_mutex_lock(&s->stats_mu);
#endif
}

static void unlock_stats(struct recv_shared* s) {
#ifdef _WIN32
    LeaveCriticalSection(&s->stats_cs);
#else
    pthread_mutex_unlock(&s->stats_mu);
#endif
}

/* Periodic receiver line (same style as stream2_stats_report); main thread only. */
static void print_recv_status(struct recv_shared* sh,
                               int nimages,
                               struct timespec* last_t,
                               uint64_t* last_img,
                               uint64_t* last_bytes,
                               int force_nl) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = stream2_time_diff_sec(&now, last_t);
    if (!force_nl && elapsed < 3.0)
        return;

    lock_stats(sh);
    uint64_t img = sh->stats.images_total;
    uint64_t bt = sh->stats.bytes_total;
    int frames = sh->image_frames;
    int perr = sh->parse_errors;
    unlock_stats(sh);
    lock_buf(sh);
    size_t blen = sh->buf.len;
    double gb_buf = (double)sh->buf.total_bytes / 1e9;
    double gb_cap = (double)sh->buf.bytes_limit / 1e9;
    unlock_buf(sh);

    double win_el = elapsed > 1e-9 ? elapsed : 1e-9;
    uint64_t dbytes = bt - *last_bytes;
    double gbps = (dbytes * 8.0) / (win_el * 1e9);

    printf("\rimages: %" PRIu64 "  frames: %d/%d  received: %.3f GB  rate: %.3f Gbit/s"
           "  buffer: %zu img / %.3f GB (cap %.1f GB)  parse_err: %d",
           img, frames, nimages, bt / 1e9, gbps, blen, gb_buf, gb_cap, perr);
    if (force_nl)
        printf("\n");
    fflush(stdout);

    *last_img = img;
    *last_bytes = bt;
    *last_t = now;
}

/* Request lines are printed by eiger_client (eiger_set_http_trace) using the same path/body as sent. */
static int eiger_cmd_status(const char* host, int port, const char* command) {
    int r = eiger_send_command(host, port, command);
    printf("[eiger]   -> %s\n", r == 0 ? "OK" : "FAILED");
    fflush(stdout);
    return r;
}

static int eiger_get_status_status(const char* host, int port, const char* path,
                                   char* response, size_t response_size) {
    int r = eiger_get_status(host, port, path, response, response_size);
    if (r == 0 && response && response[0])
        printf("[eiger]   <- body: %s\n", response);
    else
        printf("[eiger]   -> %s\n", r == 0 ? "OK (empty body)" : "FAILED");
    fflush(stdout);
    return r;
}

/* EIGER status/state JSON body: value string "idle" or "ready" means no initialize needed. */
static int eiger_state_response_is_idle_or_ready(const char* response) {
    if (!response || !response[0])
        return 0;
    return strstr(response, "\"idle\"") != NULL || strstr(response, "\"ready\"") != NULL;
}

static void recv_loop(struct recv_shared* sh) {
    zmq_msg_t msg;
    zmq_msg_init(&msg);
    void* socket = zmq_socket(sh->zmq_ctx, ZMQ_PULL);
    if (!socket) {
        zmq_msg_close(&msg);
        return;
    }
    int hwm = 10000;
    zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    int rcvbuf = 16 * 1024 * 1024;
    zmq_setsockopt(socket, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int rcv_timeout_ms = 500;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));
    if (zmq_connect(socket, sh->zmq_address) != 0) {
        zmq_close(socket);
        zmq_msg_close(&msg);
        return;
    }
    while (!sh->recv_stop && !g_stop) {
        struct stream2_msg_owner* owner_slot = NULL;
        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            int err = zmq_errno();
            if (err == EAGAIN || err == EINTR)
                continue;
            break;
        }
        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);
        struct stream2_msg* m = NULL;
        if (stream2_parse_msg(msg_data, msg_size, &m) != STREAM2_OK) {
            lock_stats(sh);
            sh->parse_errors++;
            unlock_stats(sh);
            continue;
        }
        if (m->type == STREAM2_MSG_IMAGE) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)m;
            lock_stats(sh);
            stream2_stats_add_image(&sh->stats, msg_size);
            unlock_stats(sh);
            int buf_fail = 0;
            lock_buf(sh);
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                if (stream2_buffer_image(&d->data, im->image_id, im->series_id,
                        d->channel, &sh->buf, &owner_slot, &msg) != STREAM2_OK) {
                    buf_fail = 1;
                    break;
                }
            }
            unlock_buf(sh);
            if (buf_fail) {
                sh->buffer_errors = 1;
                sh->recv_stop = 1;
            } else {
                lock_stats(sh);
                sh->image_frames++;
                unlock_stats(sh);
            }
        } else {
            lock_stats(sh);
            stream2_stats_add_bytes(&sh->stats, msg_size);
            unlock_stats(sh);
        }
        stream2_free_msg(m);
    }
    zmq_close(socket);
    zmq_msg_close(&msg);
}

#ifdef _WIN32
static unsigned int __stdcall recv_thread_win(void* param) {
    struct recv_thread_arg* a = (struct recv_thread_arg*)param;
    recv_loop(a->shared);
    free(a);
    return 0;
}
#else
static void* recv_thread_posix(void* param) {
    struct recv_thread_arg* a = (struct recv_thread_arg*)param;
    recv_loop(a->shared);
    free(a);
    return NULL;
}
#endif

static void configure_detector(const char* host, int http_port, int threshold_ev,
        int nimages, double count_time, double frame_time) {
    char buf[64];
    printf("[eiger] --- detector / monitor / filewriter / stream configuration ---\n");
    fflush(stdout);

#define PUT_DET(K, V)                         \
    do {                                      \
        int _r = eiger_set_detector_config(host, http_port, (K), (V)); \
        printf("[eiger]   -> %s\n", _r == 0 ? "OK" : "FAILED"); \
        fflush(stdout);                       \
    } while (0)
#define PUT_STR(K, V)                         \
    do {                                      \
        int _r = eiger_set_stream_config(host, http_port, (K), (V)); \
        printf("[eiger]   -> %s\n", _r == 0 ? "OK" : "FAILED"); \
        fflush(stdout);                       \
    } while (0)
#define PUT_MON(K, V)                         \
    do {                                      \
        int _r = eiger_set_monitor_config(host, http_port, (K), (V)); \
        printf("[eiger]   -> %s\n", _r == 0 ? "OK" : "FAILED"); \
        fflush(stdout);                       \
    } while (0)
#define PUT_FW(K, V)                          \
    do {                                      \
        int _r = eiger_set_filewriter_config(host, http_port, (K), (V)); \
        printf("[eiger]   -> %s\n", _r == 0 ? "OK" : "FAILED"); \
        fflush(stdout);                       \
    } while (0)

    PUT_DET("countrate_correction_applied", "false");
    PUT_DET("retrigger", "false");
    PUT_DET("counting_mode", "\"normal\"");
    PUT_DET("virtual_pixel_correction_applied", "true");
    PUT_DET("mask_to_zero", "true");
    PUT_DET("test_image_mode", "\"\"");
    PUT_DET("flatfield_correction_applied", "false");
    snprintf(buf, sizeof(buf), "%d", threshold_ev);
    PUT_DET("threshold/1/mode", "\"enabled\"");
    PUT_DET("threshold/1/energy", buf);
    PUT_DET("threshold/2/mode", "\"disabled\"");
    snprintf(buf, sizeof(buf), "%.9f", count_time);
    PUT_DET("count_time", buf);
    snprintf(buf, sizeof(buf), "%.9f", frame_time);
    PUT_DET("frame_time", buf);
    snprintf(buf, sizeof(buf), "%d", nimages);
    PUT_DET("nimages", buf);
    PUT_DET("auto_summation", "false");
    /* monitor API uses plain string in JSON, not JSON-escaped value */
    PUT_MON("mode", "disabled");
    PUT_FW("mode", "\"disabled\"");
    PUT_STR("mode", "\"enabled\"");
    PUT_STR("format", "\"cbor\"");
    PUT_STR("header_detail", "\"all\"");

#undef PUT_DET
#undef PUT_STR
#undef PUT_MON
#undef PUT_FW
}

static void usage(const char* p) {
    fprintf(stderr,
            "Usage: %s --host HOST --out DIR --nimages N --frame-time SEC --threshold EV\n"
            "  [--count-time SEC] [--http-port P] [--stream-port P] [--recv-threads N]\n"
            "  [--tiff-threads N] [--force-init]\n"
            "\n"
            "TIFF files are written under DIR in subfolders serie_<series_id>/ as:\n"
            "  stream2_<channel>_<image_id>.tiff\n"
            "  (not in DIR itself — look inside serie_* .)\n",
            p);
}

int main(int argc, char** argv) {
    const char* host = NULL;
    const char* out_dir = NULL;
    int nimages = 0;
    double frame_time = 0.0;
    double count_time = -1.0;
    int threshold_ev = 0;
    int http_port = 80;
    int stream_port = 31001;
    int recv_threads = DEFAULT_RECV_THREADS;
    int tiff_threads = 0;
    int force_init = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out_dir = argv[++i];
        else if (strcmp(argv[i], "--nimages") == 0 && i + 1 < argc)
            nimages = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frame-time") == 0 && i + 1 < argc)
            frame_time = atof(argv[++i]);
        else if (strcmp(argv[i], "--count-time") == 0 && i + 1 < argc)
            count_time = atof(argv[++i]);
        else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc)
            threshold_ev = atoi(argv[++i]);
        else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc)
            http_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--stream-port") == 0 && i + 1 < argc)
            stream_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--recv-threads") == 0 && i + 1 < argc)
            recv_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--tiff-threads") == 0 && i + 1 < argc)
            tiff_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--force-init") == 0)
            force_init = 1;
        else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!host || !out_dir || nimages <= 0 || frame_time <= 0.0 || threshold_ev <= 0) {
        usage(argv[0]);
        return 2;
    }
    if (count_time < 0.0)
        count_time = frame_time;
    if (count_time > frame_time) {
        fprintf(stderr, "count_time must be <= frame_time\n");
        return 2;
    }
    if (recv_threads < 1)
        recv_threads = 1;
    if (recv_threads > MAX_RECV_THREADS)
        recv_threads = MAX_RECV_THREADS;

    stream2_install_signal_handler();

    struct recv_shared sh = {0};
    sh.zmq_ctx = zmq_ctx_new();
    if (!sh.zmq_ctx)
        return 1;
    snprintf(sh.zmq_address, sizeof(sh.zmq_address), "tcp://%s:%d", host, stream_port);
#ifdef _WIN32
    InitializeCriticalSection(&sh.buf_cs);
    InitializeCriticalSection(&sh.stats_cs);
#else
    pthread_mutex_init(&sh.buf_mu, NULL);
    pthread_mutex_init(&sh.stats_mu, NULL);
#endif
    stream2_buffer_init(&sh.buf, stream2_parse_buffer_limit_gb(20));
    stream2_stats_init(&sh.stats);
    stream2_set_output_path(out_dir);

#ifdef _WIN32
    HANDLE handles[MAX_RECV_THREADS];
#else
    pthread_t handles[MAX_RECV_THREADS];
#endif
    int started = 0;
    for (; started < recv_threads; started++) {
        struct recv_thread_arg* arg =
                (struct recv_thread_arg*)malloc(sizeof(struct recv_thread_arg));
        if (!arg)
            break;
        arg->shared = &sh;
#ifdef _WIN32
        {
            uintptr_t h = _beginthreadex(NULL, 0, recv_thread_win, arg, 0, NULL);
            if (h == 0) {
                free(arg);
                break;
            }
            handles[started] = (HANDLE)h;
        }
#else
        if (pthread_create(&handles[started], NULL, recv_thread_posix, arg) != 0) {
            free(arg);
            break;
        }
#endif
    }
    if (started == 0) {
        stream2_buffer_free(&sh.buf);
        zmq_ctx_term(sh.zmq_ctx);
#ifdef _WIN32
        DeleteCriticalSection(&sh.buf_cs);
        DeleteCriticalSection(&sh.stats_cs);
#else
        pthread_mutex_destroy(&sh.buf_mu);
        pthread_mutex_destroy(&sh.stats_mu);
#endif
        return 1;
    }

    printf("Receiver: ZMQ PULL %s  (%d thread%s)\n", sh.zmq_address, started,
           started == 1 ? "" : "s");
    printf("TIFF base path: %s  (files go in serie_<id>/ subfolders, not here)\n",
           out_dir);
    printf("Acquisition: nimages=%d  frame_time=%.9f s  count_time=%.9f s  "
           "threshold=%d eV\n",
           nimages, frame_time, count_time, threshold_ev);
    fflush(stdout);

#ifdef _WIN32
    Sleep(200);
#else
    usleep(200000);
#endif

    {
        char response[EIGER_CLIENT_RESPONSE_MAX];
        eiger_set_http_trace(stdout);
        printf("[eiger] HTTP %s:%d (each line below is the exact request)\n", host, http_port);
        fflush(stdout);
        eiger_cmd_status(host, http_port, "disarm");

        /* Always query state; initialize unless idle or ready (or user forces init). */
        {
            int st = eiger_get_status_status(host, http_port, "state", response,
                                             sizeof(response));
            int idle_or_ready =
                    (st == 0 && eiger_state_response_is_idle_or_ready(response));

            if (force_init || !idle_or_ready) {
                if (force_init)
                    printf("[eiger]   -> initialize (forced)\n");
                else if (st != 0 || !response[0])
                    printf("[eiger]   -> initialize (state query failed or empty)\n");
                else
                    printf("[eiger]   -> initialize (state is not idle or ready)\n");
                fflush(stdout);
                if (eiger_cmd_status(host, http_port, "initialize") != 0)
                    goto fail;
            }
        }
        configure_detector(host, http_port, threshold_ev, nimages, count_time, frame_time);
        if (eiger_cmd_status(host, http_port, "arm") != 0)
            goto fail;
        if (eiger_cmd_status(host, http_port, "trigger") != 0) {
            eiger_cmd_status(host, http_port, "disarm");
            goto fail;
        }
    }

    printf("Receiving stream (stats every ~3 s, same style as stream2_buffer)...\n");
    fflush(stdout);

    {
        double timeout_sec = (double)nimages * frame_time + 120.0;
        if (timeout_sec < 60.0)
            timeout_sec = 60.0;
        struct timespec t0, t1;
        struct timespec last_report;
        uint64_t last_img = 0;
        uint64_t last_bytes = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        clock_gettime(CLOCK_MONOTONIC, &last_report);
        for (;;) {
            if (g_stop || sh.buffer_errors)
                break;
            lock_stats(&sh);
            int got = sh.image_frames;
            unlock_stats(&sh);
            if (got >= nimages)
                break;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (stream2_time_diff_sec(&t1, &t0) > timeout_sec)
                break;
            print_recv_status(&sh, nimages, &last_report, &last_img, &last_bytes, 0);
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50000);
#endif
        }
        print_recv_status(&sh, nimages, &last_report, &last_img, &last_bytes, 1);
    }

    sh.recv_stop = 1;
#ifdef _WIN32
    for (i = 0; i < started; i++) {
        WaitForSingleObject(handles[i], INFINITE);
        CloseHandle(handles[i]);
    }
#else
    for (i = 0; i < started; i++)
        pthread_join(handles[i], NULL);
#endif

    eiger_cmd_status(host, http_port, "disarm");
    {
        size_t nbuf = 0;
        lock_buf(&sh);
        nbuf = sh.buf.len;
        unlock_buf(&sh);
        if (nbuf == 0) {
            fprintf(stderr,
                    "\nNo images buffered — no TIFFs written.\n"
#ifdef _WIN32
                    "  • After a successful run, look under: %s\\serie_*\\\n"
                    "    (example: serie_000001\\stream2_data_000001.tiff)\n"
#else
                    "  • After a successful run, look under: %s/serie_*/\n"
                    "    (example: serie_000001/stream2_data_000001.tiff)\n"
#endif
                    "  • If there are no serie_* folders: check ZMQ %s (reachable, firewall),\n"
                    "    stream enabled, and the parse_err counter in the status line.\n",
                    out_dir, sh.zmq_address);
        } else {
#ifdef _WIN32
            printf("\nFlushing %zu image(s) under %s\\serie_*\\ ...\n", nbuf, out_dir);
#else
            printf("\nFlushing %zu image(s) under %s/serie_*/ ...\n", nbuf, out_dir);
#endif
            fflush(stdout);
        }
    }
    stream2_flush_buffer_to_tiff_mt(&sh.buf, tiff_threads);
    stream2_buffer_free(&sh.buf);

    {
        int buf_err = sh.buffer_errors;
        lock_stats(&sh);
        int got = sh.image_frames;
        unlock_stats(&sh);
        int exit_code = (buf_err || got < nimages) ? 1 : 0;
        zmq_ctx_term(sh.zmq_ctx);
#ifdef _WIN32
        DeleteCriticalSection(&sh.buf_cs);
        DeleteCriticalSection(&sh.stats_cs);
#else
        pthread_mutex_destroy(&sh.buf_mu);
        pthread_mutex_destroy(&sh.stats_mu);
#endif
        return exit_code;
    }

fail:
    sh.recv_stop = 1;
#ifdef _WIN32
    for (i = 0; i < started; i++) {
        WaitForSingleObject(handles[i], INFINITE);
        CloseHandle(handles[i]);
    }
#else
    for (i = 0; i < started; i++)
        pthread_join(handles[i], NULL);
#endif
    stream2_buffer_free(&sh.buf);
    zmq_ctx_term(sh.zmq_ctx);
#ifdef _WIN32
    DeleteCriticalSection(&sh.buf_cs);
    DeleteCriticalSection(&sh.stats_cs);
#else
    pthread_mutex_destroy(&sh.buf_mu);
    pthread_mutex_destroy(&sh.stats_mu);
#endif
    return 1;
}
