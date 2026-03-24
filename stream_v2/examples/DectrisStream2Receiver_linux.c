/*
 * DectrisStream2Receiver_linux.c - Multi-threaded TIFF writer
 *
 * Like stream2_buffer_decode, but writes buffered images to TIFF files on disk
 * using multi-threaded parallel writing. Thread count configurable via
 * command-line option (default 10).
 */
#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "tiff_writer.h"
#include "stream2.h"
#include <zmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
/* IFF_LOOPBACK may not be exposed on all platforms */
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8
#endif
#endif

struct iface_stats {
    uint64_t rx_packets;
    uint64_t rx_errs;
    uint64_t rx_drop;
    uint64_t rx_frame;
    uint64_t tx_errs;
    uint64_t tx_drop;
};

#ifndef _WIN32
static int parse_iface_line(const char* line,
                            const char* iface,
                            struct iface_stats* st) {
    char name[64] = {0};
    unsigned long long rx_bytes, rx_packets, rx_errs, rx_drop, rx_fifo,
            rx_frame, rx_compressed, rx_multicast;
    unsigned long long tx_bytes, tx_packets, tx_errs, tx_drop, tx_fifo,
            tx_colls, tx_carrier, tx_compressed;
    int n = sscanf(line,
                   " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu "
                   "%llu %llu %llu %llu %llu %llu",
                   name, &rx_bytes, &rx_packets, &rx_errs, &rx_drop, &rx_fifo,
                   &rx_frame, &rx_compressed, &rx_multicast, &tx_bytes,
                   &tx_packets, &tx_errs, &tx_drop, &tx_fifo, &tx_colls,
                   &tx_carrier);
    if (n != 16)
        return -1;
    (void)rx_bytes;
    (void)rx_fifo;
    (void)rx_compressed;
    (void)rx_multicast;
    (void)tx_bytes;
    (void)tx_packets;
    (void)tx_errs;
    (void)tx_drop;
    (void)tx_fifo;
    (void)tx_colls;
    (void)tx_carrier;
    (void)tx_compressed;
    if (strcmp(name, iface) != 0)
        return 1;
    st->rx_packets = rx_packets;
    st->rx_errs = rx_errs;
    st->rx_drop = rx_drop;
    st->rx_frame = rx_frame;
    st->tx_drop = tx_drop;
    st->tx_errs = tx_errs;
    return 0;
}

static int read_iface_stats(const char* iface, struct iface_stats* st) {
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f)
        return -1;
    char line[512];
    int found = 0;
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        int r = parse_iface_line(line, iface, st);
        if (r == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static int pick_default_iface(char* dst, size_t dst_size) {
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f)
        return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[64] = {0};
        if (sscanf(line, " %63[^:]:", name) == 1) {
            if (strcmp(name, "lo") == 0)
                continue;
            strncpy(dst, name, dst_size - 1);
            dst[dst_size - 1] = '\0';
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int select_iface_for_host(const char* host, char* dst, size_t dst_size) {
    struct in_addr target;
    if (inet_pton(AF_INET, host, &target) != 1)
        return -1;

    uint32_t ip = ntohl(target.s_addr);
    uint32_t mask = 0xFFFFFF00u; /* default /24 */
    if ((ip & 0x80000000u) == 0) {         /* Class A */
        mask = 0xFF000000u;
    } else if ((ip & 0xC0000000u) == 0x80000000u) { /* Class B */
        mask = 0xFFFF0000u;
    } /* else Class C / default /24 */

    struct ifaddrs* ifs = NULL;
    if (getifaddrs(&ifs) != 0)
        return -1;
    int found = -1;
    for (struct ifaddrs* it = ifs; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
            continue;
        if (it->ifa_flags & IFF_LOOPBACK)
            continue;
        struct sockaddr_in* sin = (struct sockaddr_in*)it->ifa_addr;
        uint32_t iface_ip = ntohl(sin->sin_addr.s_addr);
        if ((iface_ip & mask) == (ip & mask)) {
            strncpy(dst, it->ifa_name, dst_size - 1);
            dst[dst_size - 1] = '\0';
            found = 0;
            break;
        }
    }
    freeifaddrs(ifs);
    return found;
}
#endif /* !_WIN32 */

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
    int num_threads = 10;  /* default thread count */
    const char* host = NULL;

    /* Parse command-line arguments */
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s HOST [--threads N]\n", argv[0]);
        fprintf(stderr, "  HOST: target host address\n");
        fprintf(stderr, "  --threads N: number of threads for receiver (default: 10)\n");
        return EXIT_FAILURE;
    }

    host = argv[1];

    /* Parse optional thread count */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0) {
            if (i + 1 < argc) {
                char* endp = NULL;
                long t = strtol(argv[i + 1], &endp, 10);
                if (endp && *endp == '\0' && t > 0 && t <= 255) {
                    num_threads = (int)t;
                    i++; /* skip the number */
                } else {
                    fprintf(stderr, "error: invalid thread count '%s'\n", argv[i + 1]);
                    return EXIT_FAILURE;
                }
            } else {
                fprintf(stderr, "error: --threads requires a number\n");
                return EXIT_FAILURE;
            }
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    char address[100];
    sprintf(address, "tcp://%s:31001", host);

#ifndef _WIN32
    char iface[64] = {0};
    const char* env_iface = getenv("STREAM2_NET_IFACE");
    if (env_iface && *env_iface) {
        strncpy(iface, env_iface, sizeof(iface) - 1);
    } else if (select_iface_for_host(host, iface, sizeof(iface)) != 0 &&
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
                " rx_frame=%" PRIu64 " tx_drop=%" PRIu64 " tx_err=%" PRIu64
                "\n",
                iface, net_start.rx_drop, net_start.rx_errs, net_start.rx_frame,
                net_start.tx_drop, net_start.tx_errs);
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
    tiff_writer_flush_buffer_mt(&buf, num_threads);
    if (have_iface_stats) {
        if (read_iface_stats(iface, &net_end) == 0) {
            fprintf(stderr,
                    "net iface %s end:   rx_drop=%" PRIu64 " rx_err=%" PRIu64
                    " rx_frame=%" PRIu64 " tx_drop=%" PRIu64 " tx_err=%" PRIu64
                    "\n",
                    iface, net_end.rx_drop, net_end.rx_errs, net_end.rx_frame,
                    net_end.tx_drop, net_end.tx_errs);
            fprintf(stderr,
                    "net iface %s delta: rx_drop=%" PRIu64 " rx_err=%" PRIu64
                    " rx_frame=%" PRIu64 " tx_drop=%" PRIu64 " tx_err=%" PRIu64
                    "\n",
                    iface,
                    net_end.rx_drop - net_start.rx_drop,
                    net_end.rx_errs - net_start.rx_errs,
                    net_end.rx_frame - net_start.rx_frame,
                    net_end.tx_drop - net_start.tx_drop,
                    net_end.tx_errs - net_start.tx_errs);
        } else {
            fprintf(stderr,
                    "warn: failed to read final network stats for %s\n", iface);
        }
    }
#else
    stream2_stats_report(&s, &buf, 1);
    tiff_writer_flush_buffer_mt(&buf, num_threads);
#endif
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&buf);
    return EXIT_SUCCESS;
}
