/*
 * stream2_writer_single_threaded.c - Single-threaded TIFF writer
 *
 * Receives stream data, buffers images, prints first image metadata,
 * and writes TIFF files sequentially. Simpler implementation without
 * threading complexity.
 */
#include "stream2_common.h"
#include "stream2_decompress.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "stream2_tiff.h"
#include "stream2.h"
#include "tinycbor/src/cbor.h"
#include <zmq.h>

static int print_typed_array_type(const struct stream2_typed_array* array) {
    switch (array->tag) {
        case STREAM2_TYPED_ARRAY_UINT8:
            return printf("uint8 (tag %" PRIu64 ")", array->tag);
        case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN:
            return printf("uint16 (tag %" PRIu64 ")", array->tag);
        case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN:
            return printf("uint32 (tag %" PRIu64 ")", array->tag);
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN:
            return printf("float32 (tag %" PRIu64 ")", array->tag);
        default:
            return printf("tag %" PRIu64, array->tag);
    }
}

static void print_first_image_metadata(struct stream2_image_msg* msg) {
    printf("\nFIRST IMAGE METADATA\n");
    printf("series_id: %" PRIu64 "  series_unique_id: %s\n", msg->series_id,
           msg->series_unique_id ? msg->series_unique_id : "");
    printf("image_id: %" PRIu64 "\n", msg->image_id);
    printf("series_date: %s\n", msg->series_date ? msg->series_date : "");
    printf("start_time: %" PRIu64 "/%" PRIu64 "  stop_time: %" PRIu64
           "/%" PRIu64 "  real_time: %" PRIu64 "/%" PRIu64 "\n",
           msg->start_time[0], msg->start_time[1], msg->stop_time[0],
           msg->stop_time[1], msg->real_time[0], msg->real_time[1]);
    printf("user_data bytes: %zu\n", msg->user_data.len);
    printf("data blocks: %zu\n", msg->data.len);
    for (size_t i = 0; i < msg->data.len; i++) {
        struct stream2_image_data* d = &msg->data.ptr[i];
        const struct stream2_multidim_array* md = &d->data;
        printf("  channel \"%s\": dims [%" PRIu64 " x %" PRIu64 "] type ",
               d->channel, md->dim[0], md->dim[1]);
        print_typed_array_type(&md->array);
        if (md->array.data.compression.algorithm) {
            printf(" (compressed: %s)", md->array.data.compression.algorithm);
        }
        printf("\n");
    }
    fflush(stdout);
}

static void handle_msg(struct stream2_msg* msg,
                       size_t msg_size,
                       struct stream2_stats* s,
                       struct stream2_buffer_ctx* buf) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        if (!s->first_image_printed) {
            print_first_image_metadata((struct stream2_image_msg*)msg);
            s->first_image_printed = 1;
        }
        stream2_stats_add_image(s, msg_size);
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                stream2_buffer_image_copy(&d->data, im->image_id, im->series_id,
                                          d->channel, buf);
            }
        }
    }
}

static enum stream2_result parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stream2_stats* s,
                                     struct stream2_buffer_ctx* buf) {
    enum stream2_result r;

    struct stream2_msg* msg;
    if ((r = stream2_parse_msg(msg_data, msg_size, &msg))) {
        fprintf(stderr, "error: error %i parsing message\n", (int)r);
        return r;
    }

    handle_msg(msg, msg_size, s, buf);

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
        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EINTR && g_stop)
                break;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                stream2_stats_report(&s, &buf, 0);
                if (g_stop)
                    break;
                continue;
            }
            perror("zmq_msg_recv");
            break;
        }

        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);

        enum stream2_result r;
        if ((r = parse_msg(msg_data, msg_size, &s, &buf)))
            break;

        stream2_stats_report(&s, &buf, 0);

        if (g_stop)
            break;
    }

    stream2_stats_report(&s, &buf, 1);
    stream2_flush_buffer_to_tiff(&buf);
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&buf);
    return EXIT_FAILURE;
}
