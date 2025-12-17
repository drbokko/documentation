#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <zmq.h>
#include <time.h>

#include "compression/src/compression.h"
#include "stream2.h"
#include "tinycbor/src/cbor.h"

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

static void print_multidim_array(
        const struct stream2_multidim_array* multidim) {
    enum stream2_result r;
    const unsigned char* data;
    size_t len;
    size_t elem_size;
    void* buffer;
    if ((r = decode_typed_array(&multidim->array, &data, &len, &elem_size,
                                &buffer)))
    {
        printf("error %i\n", (int)r);
        return;
    }
    printf("dim [%" PRIu64 " %" PRIu64 "] type ", multidim->dim[0],
           multidim->dim[1]);
    print_typed_array_type(&multidim->array);
    printf("\n");

    uint64_t ce = multidim->dim[1];
    int c_width;
    switch (multidim->array.tag) {
        default:
        case STREAM2_TYPED_ARRAY_UINT8:
            ce = multidim->dim[1] * elem_size;
            c_width = 2;
            break;
        case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN:
            c_width = 4;
            break;
        case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN:
            c_width = 8;
            break;
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN:
            c_width = 5;
            break;
    }
    const int COLS = 80;
    const uint64_t c_mid = ((COLS - 3) / (c_width + 1) & ~1) / 2;
    const uint64_t r_mid = 20 / 2;
    for (uint64_t ri = 0, re = multidim->dim[0]; ri < re; ri++, printf("\n")) {
        if (ri == r_mid && re > r_mid * 2) {
            ri = re - r_mid - 1;
            for (uint64_t ci = 0; ci < ce; ci++) {
                if (ci == c_mid && ce > c_mid * 2) {
                    ci = ce - c_mid - 1;
                    printf(":: ");
                    continue;
                }
                printf(":%*s: ", c_width - 2, "");
            }
            continue;
        }
        for (uint64_t ci = 0; ci < ce; ci++) {
            if (ci == c_mid && ce > c_mid * 2) {
                ci = ce - c_mid - 1;
                printf(".. ");
                continue;
            }
            switch (multidim->array.tag) {
                default:
                case STREAM2_TYPED_ARRAY_UINT8:
                    printf("%02" PRIx8 " ", data[ri * ce + ci]);
                    break;
                case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN: {
                    uint16_t v;
                    memcpy(&v, data + (ri * ce + ci) * elem_size, sizeof(v));
                    printf("%04" PRIx16 " ", v);
                    break;
                }
                case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN: {
                    uint32_t v;
                    memcpy(&v, data + (ri * ce + ci) * elem_size, sizeof(v));
                    printf("%08" PRIx32 " ", v);
                    break;
                }
                case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN: {
                    float v;
                    memcpy(&v, data + (ri * ce + ci) * elem_size, sizeof(v));
                    if (v >= 0.f && v < 10.f)
                        printf("%.3f ", v != 0.f ? v : 0.f);
                    else
                        printf("##### ");
                    break;
                }
            }
        }
    }
    free(buffer);
}

static void print_user_data(struct stream2_user_data* user_data) {
    if (user_data->ptr != NULL) {
        CborParser parser;
        CborValue it;
        CborError e;
        if ((e = cbor_parser_init(user_data->ptr, user_data->len, 0, &parser,
                                  &it)) ||
            (e = cbor_value_to_pretty(stdout, &it)))
        {
            printf("error: %s\n", cbor_error_string(e));
            return;
        }
    }
    printf("\n");
}

struct stats {
    uint64_t images_total;
    uint64_t bytes_total;
    uint64_t images_window;
    uint64_t bytes_window;
    struct timespec last_report;
    int first_image_printed;
};

struct buffered_image {
    char* channel;
    uint64_t image_id;
    uint64_t width;
    uint64_t height;
    enum stream2_typed_array_tag tag;
    size_t elem_size;
    void* data;
    size_t data_size;
};

struct buffer_ctx {
    struct buffered_image* items;
    size_t len;
    size_t cap;
    uint64_t total_bytes;
    uint64_t bytes_limit;
    int warned_limit;
};

static volatile sig_atomic_t g_stop = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static double time_diff_sec(const struct timespec* a,
                            const struct timespec* b) {
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

static uint16_t sample_format_for_tag(enum stream2_typed_array_tag tag) {
    switch (tag) {
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN:
            return 3;  // IEEE floating point
        default:
            return 1;  // unsigned int
    }
}

static int write_tiff(const char* path, const struct buffered_image* img) {
    // Classic TIFF, little endian, single strip, one sample.
    if (img->data_size > UINT32_MAX)
        return -1;  // classic TIFF can't handle >4GB

    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;

    uint16_t magic = 42;
    uint32_t ifd_offset = 8;
    fwrite("II", 1, 2, f);
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ifd_offset, sizeof(ifd_offset), 1, f);

    uint16_t entry_count = 11;
    uint32_t image_data_offset = 8 + 2 + entry_count * 12 + 4;  // header + count + entries + next-ifd

    uint16_t sample_format = sample_format_for_tag(img->tag);
    uint16_t samples_per_pixel = 1;

    uint16_t bits_tag = (uint16_t)(img->elem_size * 8);
    switch (img->tag) {
        case STREAM2_TYPED_ARRAY_UINT8: bits_tag = 8; break;
        case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN: bits_tag = 16; break;
        case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN:
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN: bits_tag = 32; break;
        default: break;
    }
    if (bits_tag != 8 && bits_tag != 16 && bits_tag != 32)
        bits_tag = 16;  // clamp to common depths for viewer compatibility

    // IFD entries
    fwrite(&entry_count, sizeof(entry_count), 1, f);

    // ImageWidth
    uint16_t tag = 256, type = 4; uint32_t count = 1; uint32_t val32;
    val32 = (uint32_t)img->width;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f); fwrite(&val32,4,1,f);
    // ImageLength
    tag = 257; val32 = (uint32_t)img->height;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f); fwrite(&val32,4,1,f);
    // BitsPerSample (value fits in place)
    tag = 258; type = 3; count = 1; uint16_t val16 = bits_tag; uint32_t offset = 0;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f);
    fwrite(&val16,2,1,f); uint16_t pad16 = 0; fwrite(&pad16,2,1,f);
    // Compression (1 = none)
    tag = 259; type = 3; count = 1; val16 = 1;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f);
    fwrite(&val16,2,1,f); pad16 = 0; fwrite(&pad16,2,1,f);
    // Photometric (1 = min is black)
    tag = 262; type = 3; count = 1; val16 = 1;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f);
    fwrite(&val16,2,1,f); fwrite(&pad16,2,1,f);
    // StripOffsets
    tag = 273; type = 4; count = 1; offset = image_data_offset;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f); fwrite(&offset,4,1,f);
    // RowsPerStrip
    tag = 278; type = 4; count = 1; val32 = (uint32_t)img->height;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f); fwrite(&val32,4,1,f);
    // StripByteCounts
    tag = 279; type = 4; count = 1; val32 = (uint32_t)img->data_size;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f); fwrite(&val32,4,1,f);
    // SamplesPerPixel
    tag = 277; type = 3; count = 1; val16 = samples_per_pixel;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f);
    fwrite(&val16,2,1,f); fwrite(&pad16,2,1,f);
    // PlanarConfiguration (1 = chunky)
    tag = 284; type = 3; count = 1; val16 = 1;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f);
    fwrite(&val16,2,1,f); fwrite(&pad16,2,1,f);
    // SampleFormat
    tag = 339; type = 3; count = 1; val16 = sample_format;
    fwrite(&tag,2,1,f); fwrite(&type,2,1,f); fwrite(&count,4,1,f);
    fwrite(&val16,2,1,f); fwrite(&pad16,2,1,f);

    // next IFD offset = 0
    uint32_t zero = 0;
    fwrite(&zero, sizeof(zero), 1, f);

    // Image data
    if (fwrite(img->data, 1, img->data_size, f) != img->data_size) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static int buffer_image_data(const struct stream2_multidim_array* md,
                             uint64_t image_id,
                             const char* channel,
                             struct buffer_ctx* buf) {
    enum stream2_result r;
    const unsigned char* data;
    size_t len_elems;
    size_t elem_size;
    void* decompress_buffer;
    if ((r = decode_typed_array(&md->array, &data, &len_elems, &elem_size,
                                &decompress_buffer)))
    {
        return r;
    }

    uint64_t w = md->dim[1];
    uint64_t h = md->dim[0];
    if (w == 0 || h == 0) {
        free(decompress_buffer);
        return STREAM2_ERROR_PARSE;
    }

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
    size_t data_size = (size_t)data_size_u64;

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
    bi->data_size = data_size;
    bi->data = malloc(data_size);
    if (!bi->data) {
        free(decompress_buffer);
        return STREAM2_ERROR_OUT_OF_MEMORY;
    }
    memcpy(bi->data, data, data_size);

    buf->total_bytes += data_size;

    free(decompress_buffer);
    return STREAM2_OK;
}

static void flush_buffer_to_tiff(struct buffer_ctx* buf) {
    for (size_t i = 0; i < buf->len; i++) {
        struct buffered_image* bi = &buf->items[i];
        if (bi->data_size > UINT32_MAX) {
            fprintf(stderr,
                    "image %" PRIu64 " too large for classic TIFF (>4GB), skipping\n",
                    bi->image_id);
            continue;
        }
        char filename[256];
        snprintf(filename, sizeof(filename), "/dev/shm/stream2_%s_%06" PRIu64 ".tiff",
                 bi->channel ? bi->channel : "data",
                 bi->image_id);
        if (write_tiff(filename, bi) != 0) {
            fprintf(stderr, "failed to write %s\n", filename);
        }
    }

    for (size_t i = 0; i < buf->len; i++) {
        free(buf->items[i].channel);
        free(buf->items[i].data);
    }
    free(buf->items);
    buf->items = NULL;
    buf->len = buf->cap = 0;
    buf->total_bytes = 0;
}

static void print_first_image_metadata(struct stream2_image_msg* msg) {
    printf("\nFIRST IMAGE METADATA\n");
    printf("series_id: %" PRIu64 "  series_unique_id: %s\n", msg->series_id,
           msg->series_unique_id ? msg->series_unique_id : "");
    printf("image_id: %" PRIu64 "\n", msg->image_id);
    printf("series_date: %s\n", msg->series_date ? msg->series_date : "");
    printf("start_time: %" PRIu64 "/%" PRIu64 "  stop_time: %" PRIu64 "/%"
           PRIu64 "  real_time: %" PRIu64 "/%" PRIu64 "\n",
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

static void report_stats(struct stats* s, struct buffer_ctx* buf, int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = time_diff_sec(&now, &s->last_report);
    if (!force && elapsed < 3.0)
        return;

    if (elapsed <= 0.0)
        elapsed = 1e-9;  // avoid division by zero if clocks are equal

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
                       struct buffer_ctx* buf) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        if (!s->first_image_printed) {
            print_first_image_metadata((struct stream2_image_msg*)msg);
            s->first_image_printed = 1;
        }
        s->images_total++;
        s->images_window++;
        s->bytes_total += msg_size;
        s->bytes_window += msg_size;
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                buffer_image_data(&d->data, im->image_id, d->channel, buf);
            }
        }
    }
}

static enum stream2_result parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stats* s,
                                     struct buffer_ctx* buf) {
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

    // Raise buffers and high-water mark to avoid dropping bursts while printing.
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
        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EINTR && g_stop)
                break;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                report_stats(&s, &buf, 0);
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

        report_stats(&s, &buf, 0);

        if (g_stop)
            break;
    }
    report_stats(&s, &buf, 1);
    flush_buffer_to_tiff(&buf);
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    return EXIT_FAILURE;
}
