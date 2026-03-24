/*
 * stream2_image_buffer.h - Image buffering for Stream V2 examples
 */
#ifndef STREAM2_IMAGE_BUFFER_H
#define STREAM2_IMAGE_BUFFER_H

#include "stream2.h"
#include "stream2_common.h"
#include <zmq.h>

/* ZMQ message owner for zero-copy support */
struct stream2_msg_owner {
    zmq_msg_t msg;
    size_t refs;
};

/* Buffered image structure */
struct stream2_buffered_image {
    char* channel;
    uint64_t image_id;
    uint64_t series_id;
    uint64_t width;
    uint64_t height;
    enum stream2_typed_array_tag tag;
    size_t elem_size;
    const void* data;
    size_t data_size;
    struct stream2_msg_owner* owner; /* non-NULL when zero-copy frame retained */
    char* compression_alg;           /* NULL if uncompressed */
    size_t compression_elem_size;
};

/* Buffer context */
struct stream2_buffer_ctx {
    struct stream2_buffered_image* items;
    size_t len;
    size_t cap;
    uint64_t total_bytes;
    uint64_t bytes_limit;
    int warned_limit;
};

/* Initialize buffer context */
void stream2_buffer_init(struct stream2_buffer_ctx* buf, uint64_t bytes_limit);

/* Free all buffer contents */
void stream2_buffer_free(struct stream2_buffer_ctx* buf);

/* Buffer an image from multidim array data (with zero-copy support) */
enum stream2_result stream2_buffer_image(
        const struct stream2_multidim_array* md,
        uint64_t image_id,
        uint64_t series_id,
        const char* channel,
        struct stream2_buffer_ctx* buf,
        struct stream2_msg_owner** owner_slot,
        zmq_msg_t* src_msg);

/* Buffer an image (simpler version without zero-copy) */
enum stream2_result stream2_buffer_image_copy(
        const struct stream2_multidim_array* md,
        uint64_t image_id,
        uint64_t series_id,
        const char* channel,
        struct stream2_buffer_ctx* buf);

#endif /* STREAM2_IMAGE_BUFFER_H */
