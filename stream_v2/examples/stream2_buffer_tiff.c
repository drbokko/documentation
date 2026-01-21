/*
 * stream2_buffer_tiff.c - Multi-threaded TIFF writer
 *
 * Like stream2_buffer_decode, but writes buffered images to TIFF files on disk
 * using multi-threaded parallel writing. Thread count configurable via
 * STREAM2_TIFF_THREADS environment variable.
 */
#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "stream2_tiff.h"
#include "stream2.h"
#include <zmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifndef _WIN32
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#endif

struct iface_stats {
    uint64_t rx_packets;
    uint64_t rx_errs;
    uint64_t rx_drop;
    uint64_t rx_frame;
};

static void handle_msg(struct stream2_msg* msg,
                       size_t msg_size,
                       struct stream2_stats* s,
                       struct stream2_buffer_ctx* buf,
                       struct stream2_msg_owner** owner_slot,
                       zmq_msg_t* src_msg) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        stream2_stats_add_image(s, msg_size);
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                stream2_buffer_image(&d->data, im->image_id, im->series_id,
                                     d->channel, buf, owner_slot, src_msg);
            }
        }
    } else {
        stream2_stats_add_bytes(s, msg_size);
    }
}

static enum stream2_result parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stream2_stats* s,
                                     struct stream2_buffer_ctx* buf,
                                     struct stream2_msg_owner** owner_slot,
                                     zmq_msg_t* src_msg) {
    enum stream2_result r;

    struct stream2_msg* msg;
    if ((r = stream2_parse_msg(msg_data, msg_size, &msg))) {
        fprintf(stderr, "error: error %i parsing message\n", (int)r);
        return r;
    }

    handle_msg(msg, msg_size, s, buf, owner_slot, src_msg);

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

#ifndef _WIN32
    char iface[64] = {0};
    const char* env_iface = getenv("STREAM2_NET_IFACE");
    if (env_iface && *env_iface) {
        strncpy(iface, env_iface, sizeof(iface) - 1);
    } else if (select_iface_for_host(argv[1], iface, sizeof(iface)) != 0 &&
               pick_default_iface(iface, sizeof(iface)) != 0) {
        fprintf(stderr, "warn: could not auto-detect network interface\n");
    }

    struct iface_stats net_start = {0};
    struct iface_stats net_end = {0};
    int have_iface_stats = iface[0] != '\0' &&
            read_iface_stats(iface, &net_start) == 0;
    if (have_iface_stats) {
        fprintf(stderr,
                "net iface %s start: rx_drop=%" PRIu64 " rx_err=%" PRIu64
                " rx_frame=%" PRIu64 "\n",
                iface, net_start.rx_drop, net_start.rx_errs, net_start.rx_frame);
    }
#endif

    void* ctx = zmq_ctx_new();
    void* socket = zmq_socket(ctx, ZMQ_PULL);

    int hwm = 10000;
    zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    int rcvbuf = 16 * 1024 * 1024;
    zmq_setsockopt(socket, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    zmq_connect(socket, address);
    int rcv_timeout_ms = 500;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    stream2_install_signal_handler();

    struct stream2_stats s = {0};
    stream2_stats_init(&s);

    uint64_t buffer_limit = stream2_parse_buffer_limit_gb(20);
    struct stream2_buffer_ctx buf;
    stream2_buffer_init(&buf, buffer_limit);

    for (;;) {
        if (g_stop)
            break;

        struct stream2_msg_owner* owner_slot = NULL;

        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EAGAIN) {
                stream2_stats_report(&s, &buf, 0);
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

        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);

        enum stream2_result r;
        if ((r = parse_msg(msg_data, msg_size, &s, &buf, &owner_slot, &msg)))
            break;

        stream2_stats_report(&s, &buf, 0);
    }

#ifndef _WIN32
    stream2_stats_report(&s, &buf, 1);
    stream2_flush_buffer_to_tiff_mt(&buf, 10);
    if (have_iface_stats) {
        if (read_iface_stats(iface, &net_end) == 0) {
            fprintf(stderr,
                    "net iface %s end:   rx_drop=%" PRIu64 " rx_err=%" PRIu64
                    " rx_frame=%" PRIu64 " (delta drop=%" PRIu64
                    " err=%" PRIu64 " frame=%" PRIu64 ")\n",
                    iface, net_end.rx_drop, net_end.rx_errs, net_end.rx_frame,
                    net_end.rx_drop - net_start.rx_drop,
                    net_end.rx_errs - net_start.rx_errs,
                    net_end.rx_frame - net_start.rx_frame);
        } else {
            fprintf(stderr,
                    "warn: failed to read final network stats for %s\n", iface);
        }
    }
#else
    stream2_stats_report(&s, &buf, 1);
    stream2_flush_buffer_to_tiff_mt(&buf, 10);
#endif
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&buf);
    return EXIT_FAILURE;
}
