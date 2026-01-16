/*
 * check_stream2.c - Simple stream checker that counts images and bytes
 */
#include "stream2_common.h"
#include "stream2_stats.h"
#include "stream2.h"
#include <zmq.h>

static enum stream2_result parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stream2_stats* s) {
    enum stream2_result r;

    struct stream2_msg* msg;
    if ((r = stream2_parse_msg(msg_data, msg_size, &msg))) {
        fprintf(stderr, "error: %i parsing message\n", (int)r);
        return r;
    }

    if (msg->type == STREAM2_MSG_IMAGE)
        stream2_stats_add_image(s, msg_size);
    else
        stream2_stats_add_bytes(s, msg_size);

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

    int rcv_timeout_ms = 500;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));

    zmq_connect(socket, address);
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    stream2_install_signal_handler();

    struct stream2_stats s = {0};
    stream2_stats_init(&s);

    for (;;) {
        if (g_stop) {
            stream2_stats_report_simple(&s, 1);
            break;
        }

        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EAGAIN) {
                stream2_stats_report_simple(&s, 0);
                if (g_stop)
                    break;
                continue;
            }
            if (errno == EINTR) {
                if (g_stop) {
                    stream2_stats_report_simple(&s, 1);
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

        stream2_stats_report_simple(&s, 0);

        if (g_stop)
            break;
    }

    stream2_stats_report_simple(&s, 1);
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    return EXIT_FAILURE;
}
