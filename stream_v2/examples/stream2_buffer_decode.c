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

#include "compression/src/compression.h"
#include "stream2.h"

struct msg_owner {
    zmq_msg_t msg;
    size_t refs;
};

struct buffered_image {
    char* channel;
    uint64_t image_id;
    uint64_t width;
    uint64_t height;
    enum stream2_typed_array_tag tag;
    size_t elem_size;
    const void* data;
    size_t data_size;
    struct msg_owner* owner;  // non-NULL when zero-copy frame is retained
    char* compression_alg;    // NULL if uncompressed
    size_t compression_elem_size;
};

struct buffer_ctx {
    struct buffered_image* items;
    size_t len;
    size_t cap;
    uint64_t total_bytes;
    uint64_t bytes_limit;
    int warned_limit;
};

struct stats {
    uint64_t images_total;
    uint64_t bytes_total;
    uint64_t images_window;
    uint64_t bytes_window;
    struct timespec last_report;
};

static volatile sig_atomic_t g_stop = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static enum stream2_result decode_bytes(const struct stream2_bytes* bytes,
                                        const unsigned char** decoded,
                                        size_t* decoded_len,
                                        void** decompress_buffer) {
    const struct stream2_compression compression = bytes->compression;

    if (compression.algorithm == NULL) {
        *decoded = (const unsigned char*)bytes->ptr;
        *decoded_len = bytes->len;
        *decompress_buffer = NULL;
        return STREAM2_OK;
    }

    CompressionAlgorithm algorithm;
    if (strcmp(compression.algorithm, "bslz4") == 0) {
        algorithm = COMPRESSION_BSLZ4;
    } else if (strcmp(compression.algorithm, "lz4") == 0) {
        algorithm = COMPRESSION_LZ4;
    } else {
        return STREAM2_ERROR_NOT_IMPLEMENTED;
    }

    const size_t len = compression_decompress_buffer(
            algorithm, NULL, 0, (const char*)bytes->ptr, bytes->len,
            compression.elem_size);
    if (len == COMPRESSION_ERROR)
        return STREAM2_ERROR_DECODE;

    void* buffer = malloc(len);
    if (!buffer)
        return STREAM2_ERROR_OUT_OF_MEMORY;

    if (compression_decompress_buffer(algorithm, (char*)buffer, len,
                                      (const char*)bytes->ptr, bytes->len,
                                      compression.elem_size) != len)
    {
        free(buffer);
        return STREAM2_ERROR_DECODE;
    }

    *decoded = (const unsigned char*)buffer;
    *decoded_len = len;
    *decompress_buffer = buffer;
    return STREAM2_OK;
}

static enum stream2_result decode_typed_array(
        const struct stream2_typed_array* array,
        const unsigned char** data,
        size_t* len,
        size_t* elem_size,
        void** decompress_buffer) {
    enum stream2_result r;

    uint64_t elem_size64;
    if ((r = stream2_typed_array_elem_size(array, &elem_size64)))
        return r;

    if (elem_size64 > SIZE_MAX)
        return STREAM2_ERROR_NOT_IMPLEMENTED;

    *elem_size = elem_size64;

    if ((r = decode_bytes(&array->data, data, len, decompress_buffer)))
        return r;

    *len /= *elem_size;

    return STREAM2_OK;
}

static int buffer_image_data(const struct stream2_multidim_array* md,
                             uint64_t image_id,
                             const char* channel,
                             struct buffer_ctx* buf,
                             struct msg_owner** owner_slot,
                             zmq_msg_t* src_msg) {
    enum stream2_result r;
    const unsigned char* data = NULL;
    size_t len_elems = 0;
    size_t elem_size = 0;
    void* decompress_buffer = NULL;

    const bool no_compression = (md->array.data.compression.algorithm == NULL);
    const bool compressed = !no_compression;
    const bool expected_shape = (md->dim[0] == 4364 && md->dim[1] == 4150);
    const bool expected_type =
            (md->array.tag == STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN);

    size_t data_size = 0;

    if (compressed) {
        data = (const unsigned char*)md->array.data.ptr;
        data_size = md->array.data.len;
        elem_size = md->array.data.compression.elem_size ?
                    md->array.data.compression.elem_size : 1;
    } else if (expected_shape && expected_type) {
        data = (const unsigned char*)md->array.data.ptr;
        elem_size = 2;
        len_elems = md->array.data.len / elem_size;
    } else {
        if ((r = decode_typed_array(&md->array, &data, &len_elems, &elem_size,
                                    &decompress_buffer)))
        {
            return r;
        }
    }

    uint64_t w = md->dim[1];
    uint64_t h = md->dim[0];
    if (w == 0 || h == 0) {
        free(decompress_buffer);
        return STREAM2_ERROR_PARSE;
    }

    if (!compressed) {
        uint64_t total_elems = w * h;
        if (total_elems != len_elems) {
            free(decompress_buffer);
            return STREAM2_ERROR_PARSE;
        }

        uint64_t data_size_u64 = total_elems * elem_size;
        if (data_size_u64 > SIZE_MAX) {
            free(decompress_buffer);
            return STREAM2_ERROR_OUT_OF_MEMORY;
        }
        data_size = (size_t)data_size_u64;
    }

    if (buf->total_bytes + data_size > buf->bytes_limit) {
        if (!buf->warned_limit) {
            fprintf(stderr, "buffer limit reached (%" PRIu64 " bytes); "
                            "skipping further images\n",
                    buf->bytes_limit);
            buf->warned_limit = 1;
        }
        free(decompress_buffer);
        return STREAM2_OK;
    }

    if (buf->len == buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 64;
        void* new_items = realloc(buf->items, new_cap * sizeof(*buf->items));
        if (!new_items) {
            free(decompress_buffer);
            return STREAM2_ERROR_OUT_OF_MEMORY;
        }
        buf->items = new_items;
        buf->cap = new_cap;
    }

    struct buffered_image* bi = &buf->items[buf->len++];
    bi->channel = channel ? strdup(channel) : NULL;
    bi->image_id = image_id;
    bi->width = w;
    bi->height = h;
    bi->tag = md->array.tag;
    bi->elem_size = elem_size;
    bi->owner = NULL;
    bi->compression_alg = NULL;
    bi->compression_elem_size = md->array.data.compression.elem_size;

    if (compressed) {
        if (*owner_slot == NULL) {
            *owner_slot = calloc(1, sizeof(**owner_slot));
            if (!*owner_slot) {
                return STREAM2_ERROR_OUT_OF_MEMORY;
            }
            zmq_msg_init(&(*owner_slot)->msg);
            zmq_msg_move(&(*owner_slot)->msg, src_msg);  // transfer frame
        }
        (*owner_slot)->refs++;
        bi->owner = *owner_slot;
        bi->data = data;
        bi->data_size = data_size;
        if (md->array.data.compression.algorithm)
            bi->compression_alg = strdup(md->array.data.compression.algorithm);
    } else if (no_compression && expected_shape && expected_type) {
        if (*owner_slot == NULL) {
            *owner_slot = calloc(1, sizeof(**owner_slot));
            if (!*owner_slot) {
                return STREAM2_ERROR_OUT_OF_MEMORY;
            }
            zmq_msg_init(&(*owner_slot)->msg);
            zmq_msg_move(&(*owner_slot)->msg, src_msg);  // transfer frame
        }
        (*owner_slot)->refs++;
        bi->owner = *owner_slot;
        bi->data = data;
        bi->data_size = data_size;
    } else {
        bi->data = malloc(data_size);
        if (!bi->data) {
            free(decompress_buffer);
            return STREAM2_ERROR_OUT_OF_MEMORY;
        }
        memcpy((void*)bi->data, data, data_size);
        bi->data_size = data_size;
    }

    buf->total_bytes += data_size;

    free(decompress_buffer);
    return STREAM2_OK;
}

static double time_diff_sec(const struct timespec* a,
                            const struct timespec* b) {
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

static void report_stats(struct stats* s, struct buffer_ctx* buf, int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = time_diff_sec(&now, &s->last_report);
    if (!force && elapsed < 3.0)
        return;

    if (elapsed <= 0.0)
        elapsed = 1e-9;

    double gbps = (s->bytes_window * 8.0) / (elapsed * 1e9);
    double gigabytes_total = s->bytes_total / 1e9;
    double gigabytes_buffer = buf ? (double)buf->total_bytes / 1e9 : 0.0;
    double gigabytes_cap = buf ? (double)buf->bytes_limit / 1e9 : 0.0;
    size_t buffered_images = buf ? buf->len : 0;

    printf("\rimages: %" PRIu64 "  received: %.3f GB  rate: %.3f Gbit/s"
           "  buffer: %zu img / %.3f GB (cap %.1f GB)",
           s->images_total, gigabytes_total, gbps, buffered_images,
           gigabytes_buffer, gigabytes_cap);
    if (force)
        printf("\n");
    fflush(stdout);

    s->images_window = 0;
    s->bytes_window = 0;
    s->last_report = now;
}

static void handle_msg(struct stream2_msg* msg,
                       size_t msg_size,
                       struct stats* s,
                       struct buffer_ctx* buf,
                       struct msg_owner** owner_slot,
                       zmq_msg_t* src_msg) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        s->images_total++;
        s->images_window++;
        s->bytes_total += msg_size;
        s->bytes_window += msg_size;
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                buffer_image_data(&d->data, im->image_id, d->channel, buf,
                                  owner_slot, src_msg);
            }
        }
    } else {
        s->bytes_total += msg_size;
        s->bytes_window += msg_size;
    }
}

static enum stream2_result parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stats* s,
                                     struct buffer_ctx* buf,
                                     struct msg_owner** owner_slot,
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

static void free_owner(struct msg_owner* owner) {
    if (!owner)
        return;
    zmq_msg_close(&owner->msg);
    free(owner);
}

static void free_buffer(struct buffer_ctx* buf) {
    for (size_t i = 0; i < buf->len; i++) {
        free(buf->items[i].channel);
        free(buf->items[i].compression_alg);
        if (buf->items[i].owner) {
            if (buf->items[i].owner->refs > 0)
                buf->items[i].owner->refs--;
        } else {
            free((void*)buf->items[i].data);
        }
    }
    for (size_t i = 0; i < buf->len; i++) {
        if (buf->items[i].owner && buf->items[i].owner->refs == 0) {
            free_owner(buf->items[i].owner);
            buf->items[i].owner = NULL;
        }
    }
    free(buf->items);
    buf->items = NULL;
    buf->len = buf->cap = 0;
    buf->total_bytes = 0;
}

static void report_compression(const struct buffer_ctx* buf) {
    uint64_t compressed_bytes = 0;
    uint64_t decompressed_bytes = 0;
    uint64_t compressed_images = 0;

    for (size_t i = 0; i < buf->len; i++) {
        const struct buffered_image* bi = &buf->items[i];
        if (bi->compression_alg == NULL) {
            compressed_bytes += bi->data_size;
            decompressed_bytes += bi->data_size;
            continue;
        }
        CompressionAlgorithm algorithm;
        if (strcmp(bi->compression_alg, "bslz4") == 0) {
            algorithm = COMPRESSION_BSLZ4;
        } else if (strcmp(bi->compression_alg, "lz4") == 0) {
            algorithm = COMPRESSION_LZ4;
        } else {
            fprintf(stderr, "unknown compression '%s' for image %" PRIu64 "\n",
                    bi->compression_alg, bi->image_id);
            continue;
        }
        const size_t out_len = compression_decompress_buffer(
                algorithm, NULL, 0, (const char*)bi->data, bi->data_size,
                bi->compression_elem_size);
        if (out_len == COMPRESSION_ERROR) {
            fprintf(stderr, "decompress size error for image %" PRIu64 "\n",
                    bi->image_id);
            continue;
        }
        void* tmp = malloc(out_len);
        if (!tmp) {
            fprintf(stderr, "OOM decompressing image %" PRIu64 "\n", bi->image_id);
            continue;
        }
        const size_t decoded = compression_decompress_buffer(
                algorithm, (char*)tmp, out_len, (const char*)bi->data,
                bi->data_size, bi->compression_elem_size);
        if (decoded != out_len) {
            fprintf(stderr, "decode mismatch for image %" PRIu64 "\n",
                    bi->image_id);
            free(tmp);
            continue;
        }
        free(tmp);
        compressed_images++;
        compressed_bytes += bi->data_size;
        decompressed_bytes += out_len;
    }

    printf("\nCompression summary:\n");
    printf("  images buffered: %zu\n", buf->len);
    printf("  compressed images: %" PRIu64 "\n", compressed_images);
    printf("  compressed bytes: %.3f GB\n", compressed_bytes / 1e9);
    printf("  decompressed bytes: %.3f GB\n", decompressed_bytes / 1e9);
    if (compressed_bytes > 0) {
        double ratio = (double)decompressed_bytes / (double)compressed_bytes;
        printf("  avg compression ratio: %.3f\n", ratio);
    } else {
        printf("  avg compression ratio: n/a\n");
    }
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

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    struct stats s = {0};
    uint64_t buffer_limit_bytes = 20ULL * 1024ULL * 1024ULL * 1024ULL;  // default 20 GB
    const char* env_limit = getenv("STREAM2_BUFFER_GB");
    if (env_limit && *env_limit) {
        char* endp = NULL;
        errno = 0;
        unsigned long long gb = strtoull(env_limit, &endp, 10);
        if (errno == 0 && endp && *endp == '\0' && gb > 0) {
            buffer_limit_bytes = gb * 1024ULL * 1024ULL * 1024ULL;
        } else {
            fprintf(stderr, "WARN: STREAM2_BUFFER_GB invalid, using default 20GB\n");
        }
    }
    struct buffer_ctx buf = {
        .items = NULL,
        .len = 0,
        .cap = 0,
        .total_bytes = 0,
        .bytes_limit = buffer_limit_bytes,
        .warned_limit = 0,
    };
    clock_gettime(CLOCK_MONOTONIC, &s.last_report);

    for (;;) {
        if (g_stop)
            break;

        struct msg_owner* owner_slot = NULL;

        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EAGAIN) {
                report_stats(&s, &buf, 0);
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

        report_stats(&s, &buf, 0);
    }

    report_stats(&s, &buf, 1);
    report_compression(&buf);
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    free_buffer(&buf);
    return EXIT_FAILURE;
}
