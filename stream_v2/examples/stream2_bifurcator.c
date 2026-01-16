/*
 * stream2_bifurcator.c - High-performance stream relay with buffering
 *
 * Receives Stream V2 data on one network interface, buffers images in RAM,
 * and simultaneously re-broadcasts the raw messages on another interface
 * without modification.
 *
 * Optimized for high-performance NICs (Mellanox ConnectX-6/7):
 * - Large socket buffers for burst handling
 * - Zero-copy message forwarding
 * - Optional CPU affinity for reduced latency
 * - Busy-polling support for lowest latency
 *
 * Usage: stream2_bifurcator <source_host> <publish_interface> [publish_port]
 *
 * Example:
 *   stream2_bifurcator 192.168.1.100 192.168.2.1 31002
 *
 * Environment variables:
 *   STREAM2_BUFFER_GB      - Buffer limit in GB (default: 20)
 *   STREAM2_RCVBUF_MB      - Receive buffer size in MB (default: 256)
 *   STREAM2_SNDBUF_MB      - Send buffer size in MB (default: 256)
 *   STREAM2_CPU_AFFINITY   - CPU core to pin to (optional)
 *   STREAM2_BUSY_POLL_US   - Busy poll timeout in microseconds (default: 0)
 *   STREAM2_IO_THREADS     - ZMQ I/O threads (default: 2)
 *
 * For best performance with ConnectX-6/7:
 *   - Enable busy polling: STREAM2_BUSY_POLL_US=50
 *   - Pin to a CPU near the NIC: STREAM2_CPU_AFFINITY=0
 *   - Use large buffers: STREAM2_RCVBUF_MB=512 STREAM2_SNDBUF_MB=512
 *   - Ensure IRQ affinity is configured for the NIC
 */
#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "stream2.h"
#include <zmq.h>

#ifndef _WIN32
#include <sched.h>
#include <sys/resource.h>
#endif

/* Extended stats for bifurcator */
struct bifurcator_stats {
    struct stream2_stats base;
    uint64_t msgs_forwarded;
    uint64_t bytes_forwarded;
    uint64_t forward_errors;
    uint64_t zero_copy_forwards;
};

static void bifurcator_stats_init(struct bifurcator_stats* s) {
    stream2_stats_init(&s->base);
    s->msgs_forwarded = 0;
    s->bytes_forwarded = 0;
    s->forward_errors = 0;
    s->zero_copy_forwards = 0;
}

static void bifurcator_report(struct bifurcator_stats* s,
                              const struct stream2_buffer_ctx* buf,
                              int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = stream2_time_diff_sec(&now, &s->base.last_report);
    if (!force && elapsed < 3.0)
        return;

    if (elapsed <= 0.0)
        elapsed = 1e-9;

    double gbps_in = (s->base.bytes_window * 8.0) / (elapsed * 1e9);
    double gb_total = s->base.bytes_total / 1e9;
    double gb_buffer = buf ? (double)buf->total_bytes / 1e9 : 0.0;
    double gb_cap = buf ? (double)buf->bytes_limit / 1e9 : 0.0;
    size_t buffered = buf ? buf->len : 0;
    double gb_fwd = s->bytes_forwarded / 1e9;

    printf("\rimages: %" PRIu64 "  in: %.2f GB @ %.2f Gbps  "
           "fwd: %" PRIu64 " msgs / %.2f GB  "
           "buf: %zu / %.1f GB (cap %.0f GB)",
           s->base.images_total, gb_total, gbps_in, s->msgs_forwarded, gb_fwd,
           buffered, gb_buffer, gb_cap);

    if (s->forward_errors > 0)
        printf("  [ERR: %" PRIu64 "]", s->forward_errors);

    if (force)
        printf("\n");
    fflush(stdout);

    s->base.images_window = 0;
    s->base.bytes_window = 0;
    s->base.last_report = now;
}

static void handle_msg(struct stream2_msg* msg,
                       size_t msg_size,
                       struct bifurcator_stats* s,
                       struct stream2_buffer_ctx* buf,
                       struct stream2_msg_owner** owner_slot,
                       zmq_msg_t* src_msg) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        stream2_stats_add_image(&s->base, msg_size);
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                stream2_buffer_image(&d->data, im->image_id, d->channel, buf,
                                     owner_slot, src_msg);
            }
        }
    } else {
        stream2_stats_add_bytes(&s->base, msg_size);
    }
}

static int parse_env_int(const char* name, int default_val) {
    const char* val = getenv(name);
    if (val && *val) {
        char* endp;
        long v = strtol(val, &endp, 10);
        if (endp && *endp == '\0' && v > 0)
            return (int)v;
    }
    return default_val;
}

#ifndef _WIN32
static void set_cpu_affinity(int cpu) {
    if (cpu < 0)
        return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        printf("  CPU affinity: pinned to core %d\n", cpu);
    } else {
        fprintf(stderr, "  Warning: could not set CPU affinity to core %d\n",
                cpu);
    }
}

static void set_realtime_priority(void) {
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);

    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
        printf("  Scheduler: SCHED_FIFO (realtime)\n");
    } else {
        /* Try SCHED_RR as fallback */
        param.sched_priority = sched_get_priority_max(SCHED_RR);
        if (sched_setscheduler(0, SCHED_RR, &param) == 0) {
            printf("  Scheduler: SCHED_RR (realtime)\n");
        }
        /* Silently continue with default scheduler if neither works */
    }
}
#endif

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr,
                "usage: %s <source_host> <publish_interface> [publish_port]\n\n"
                "Receives stream from source_host:31001 and re-publishes on\n"
                "publish_interface:publish_port (default port: 31002)\n\n"
                "Example:\n"
                "  %s 192.168.1.100 192.168.2.1 31002\n\n"
                "Environment variables:\n"
                "  STREAM2_BUFFER_GB    - Buffer limit in GB (default: 20)\n"
                "  STREAM2_RCVBUF_MB    - Receive buffer MB (default: 256)\n"
                "  STREAM2_SNDBUF_MB    - Send buffer MB (default: 256)\n"
                "  STREAM2_CPU_AFFINITY - CPU core to pin to (optional)\n"
                "  STREAM2_BUSY_POLL_US - Busy poll us (default: 0)\n"
                "  STREAM2_IO_THREADS   - ZMQ I/O threads (default: 2)\n"
                "  STREAM2_REALTIME     - Set 1 for realtime priority\n\n"
                "For ConnectX-6/7 performance:\n"
                "  STREAM2_BUSY_POLL_US=50 STREAM2_CPU_AFFINITY=0 \\\n"
                "  STREAM2_RCVBUF_MB=512 %s host1 eth1_ip 31002\n",
                argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char* source_host = argv[1];
    const char* publish_interface = argv[2];
    int publish_port = (argc > 3) ? atoi(argv[3]) : 31002;

    if (publish_port <= 0 || publish_port > 65535) {
        fprintf(stderr, "error: invalid port %d\n", publish_port);
        return EXIT_FAILURE;
    }

    /* Parse configuration from environment */
    int rcvbuf_mb = parse_env_int("STREAM2_RCVBUF_MB", 256);
    int sndbuf_mb = parse_env_int("STREAM2_SNDBUF_MB", 256);
    int io_threads = parse_env_int("STREAM2_IO_THREADS", 2);
    int busy_poll_us = parse_env_int("STREAM2_BUSY_POLL_US", 0);
    int cpu_affinity = parse_env_int("STREAM2_CPU_AFFINITY", -1);
    int realtime = parse_env_int("STREAM2_REALTIME", 0);

    char source_addr[128];
    char publish_addr[128];
    snprintf(source_addr, sizeof(source_addr), "tcp://%s:31001", source_host);
    snprintf(publish_addr, sizeof(publish_addr), "tcp://%s:%d",
             publish_interface, publish_port);

    printf("Stream Bifurcator (High-Performance)\n");
    printf("  Source:  %s\n", source_addr);
    printf("  Publish: %s\n", publish_addr);
    printf("Configuration:\n");
    printf("  Receive buffer: %d MB\n", rcvbuf_mb);
    printf("  Send buffer:    %d MB\n", sndbuf_mb);
    printf("  I/O threads:    %d\n", io_threads);
    if (busy_poll_us > 0)
        printf("  Busy polling:   %d us\n", busy_poll_us);

#ifndef _WIN32
    /* Set CPU affinity if requested */
    if (cpu_affinity >= 0)
        set_cpu_affinity(cpu_affinity);

    /* Set realtime priority if requested */
    if (realtime)
        set_realtime_priority();
#endif

    printf("\n");

    /* Create ZMQ context with multiple I/O threads for better throughput */
    void* ctx = zmq_ctx_new();
    zmq_ctx_set(ctx, ZMQ_IO_THREADS, io_threads);

    /* Receiver socket (PULL) */
    void* receiver = zmq_socket(ctx, ZMQ_PULL);

    int hwm = 100000; /* High water mark for high-speed streams */
    zmq_setsockopt(receiver, ZMQ_RCVHWM, &hwm, sizeof(hwm));

    int rcvbuf = rcvbuf_mb * 1024 * 1024;
    zmq_setsockopt(receiver, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int rcv_timeout_ms = 100; /* Shorter timeout for responsiveness */
    zmq_setsockopt(receiver, ZMQ_RCVTIMEO, &rcv_timeout_ms,
                   sizeof(rcv_timeout_ms));

    /* Enable TCP keepalive for long-running connections */
    int tcp_keepalive = 1;
    zmq_setsockopt(receiver, ZMQ_TCP_KEEPALIVE, &tcp_keepalive,
                   sizeof(tcp_keepalive));
    int tcp_keepalive_idle = 30;
    zmq_setsockopt(receiver, ZMQ_TCP_KEEPALIVE_IDLE, &tcp_keepalive_idle,
                   sizeof(tcp_keepalive_idle));

    if (zmq_connect(receiver, source_addr) != 0) {
        fprintf(stderr, "error: cannot connect to %s: %s\n", source_addr,
                zmq_strerror(errno));
        zmq_close(receiver);
        zmq_ctx_term(ctx);
        return EXIT_FAILURE;
    }

    /* Publisher socket (PUB) for re-broadcasting */
    void* publisher = zmq_socket(ctx, ZMQ_PUB);

    int sndhwm = 100000;
    zmq_setsockopt(publisher, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));

    int sndbuf = sndbuf_mb * 1024 * 1024;
    zmq_setsockopt(publisher, ZMQ_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* Non-blocking send - drop if subscribers can't keep up */
    int snd_timeout_ms = 0;
    zmq_setsockopt(publisher, ZMQ_SNDTIMEO, &snd_timeout_ms,
                   sizeof(snd_timeout_ms));

    /* Enable TCP_NODELAY for lower latency */
    int tcp_nodelay = 1;
    zmq_setsockopt(publisher, ZMQ_TCP_NODELAY, &tcp_nodelay,
                   sizeof(tcp_nodelay));

    /* Enable TCP keepalive */
    zmq_setsockopt(publisher, ZMQ_TCP_KEEPALIVE, &tcp_keepalive,
                   sizeof(tcp_keepalive));

    if (zmq_bind(publisher, publish_addr) != 0) {
        fprintf(stderr, "error: cannot bind to %s: %s\n", publish_addr,
                zmq_strerror(errno));
        zmq_close(receiver);
        zmq_close(publisher);
        zmq_ctx_term(ctx);
        return EXIT_FAILURE;
    }

    zmq_msg_t msg;
    zmq_msg_init(&msg);

    stream2_install_signal_handler();

    struct bifurcator_stats stats;
    bifurcator_stats_init(&stats);

    uint64_t buffer_limit = stream2_parse_buffer_limit_gb(20);
    struct stream2_buffer_ctx buf;
    stream2_buffer_init(&buf, buffer_limit);

    printf("Waiting for data... (Ctrl+C to stop)\n");

    /* Main loop with optional busy polling */
    struct timespec poll_start, poll_now;
    int use_busy_poll = (busy_poll_us > 0);

    for (;;) {
        if (g_stop)
            break;

        struct stream2_msg_owner* owner_slot = NULL;

        int rc;

        if (use_busy_poll) {
            /* Busy poll loop for lowest latency */
            clock_gettime(CLOCK_MONOTONIC, &poll_start);
            for (;;) {
                rc = zmq_msg_recv(&msg, receiver, ZMQ_DONTWAIT);
                if (rc >= 0)
                    break; /* Got message */
                if (errno != EAGAIN) {
                    if (errno == EINTR && g_stop)
                        goto done;
                    if (errno == EINTR)
                        continue;
                    perror("zmq_msg_recv");
                    goto done;
                }
                /* Check if we've exceeded busy poll timeout */
                clock_gettime(CLOCK_MONOTONIC, &poll_now);
                long long elapsed_us =
                        stream2_time_diff_ns(&poll_now, &poll_start) / 1000;
                if (elapsed_us >= busy_poll_us) {
                    /* Fall back to blocking receive */
                    rc = zmq_msg_recv(&msg, receiver, 0);
                    break;
                }
                /* Yield to other threads/processes occasionally */
#ifndef _WIN32
                sched_yield();
#endif
            }
        } else {
            rc = zmq_msg_recv(&msg, receiver, 0);
        }

        if (rc == -1) {
            if (errno == EAGAIN) {
                bifurcator_report(&stats, &buf, 0);
                if (g_stop)
                    break;
                continue;
            }
            if (errno == EINTR && g_stop)
                break;
            if (errno == EINTR)
                continue;
            perror("zmq_msg_recv");
            break;
        }

        size_t msg_size = zmq_msg_size(&msg);

        /*
         * Zero-copy forward: Use zmq_msg_send with ZMQ_SNDMORE=0
         * to transfer message ownership. We need to copy for buffering
         * anyway, but we can avoid one copy for the forward path.
         *
         * Strategy: Copy the data for buffering first, then forward
         * the original message using zero-copy if possible.
         */
        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);

        /* Parse and buffer first (needs the data) */
        enum stream2_result r;
        struct stream2_msg* parsed;
        if ((r = stream2_parse_msg(msg_data, msg_size, &parsed))) {
            fprintf(stderr, "error: error %i parsing message\n", (int)r);
            /* Still forward even if we can't parse */
        } else {
            handle_msg(parsed, msg_size, &stats, &buf, &owner_slot, &msg);
            stream2_free_msg(parsed);
        }

        /*
         * Forward the message. If the buffer took ownership via zero-copy,
         * we need to make a copy. Otherwise we can potentially forward
         * with zero-copy.
         */
        int fwd_rc;
        if (owner_slot != NULL) {
            /* Buffer took ownership, need to copy for forward */
            zmq_msg_t fwd_msg;
            zmq_msg_init_size(&fwd_msg, msg_size);
            memcpy(zmq_msg_data(&fwd_msg), msg_data, msg_size);
            fwd_rc = zmq_msg_send(&fwd_msg, publisher, ZMQ_DONTWAIT);
            if (fwd_rc == -1)
                zmq_msg_close(&fwd_msg);
        } else {
            /* Try zero-copy forward by moving the message */
            zmq_msg_t fwd_msg;
            zmq_msg_init(&fwd_msg);
            zmq_msg_move(&fwd_msg, &msg);
            fwd_rc = zmq_msg_send(&fwd_msg, publisher, ZMQ_DONTWAIT);
            if (fwd_rc == -1) {
                zmq_msg_close(&fwd_msg);
            } else {
                stats.zero_copy_forwards++;
            }
            /* Reinitialize msg for next receive */
            zmq_msg_init(&msg);
        }

        if (fwd_rc == -1) {
            if (errno != EAGAIN) {
                stats.forward_errors++;
            }
        } else {
            stats.msgs_forwarded++;
            stats.bytes_forwarded += msg_size;
        }

        bifurcator_report(&stats, &buf, 0);
    }

done:
    bifurcator_report(&stats, &buf, 1);

    printf("\nSummary:\n");
    printf("  Total received:  %" PRIu64 " images, %.3f GB\n",
           stats.base.images_total, stats.base.bytes_total / 1e9);
    printf("  Total forwarded: %" PRIu64 " msgs, %.3f GB\n",
           stats.msgs_forwarded, stats.bytes_forwarded / 1e9);
    printf("  Zero-copy fwds:  %" PRIu64 "\n", stats.zero_copy_forwards);
    printf("  Forward errors:  %" PRIu64 "\n", stats.forward_errors);
    printf("  Buffered:        %zu images, %.3f GB\n", buf.len,
           (double)buf.total_bytes / 1e9);

    zmq_msg_close(&msg);
    zmq_close(receiver);
    zmq_close(publisher);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&buf);

    return EXIT_SUCCESS;
}
