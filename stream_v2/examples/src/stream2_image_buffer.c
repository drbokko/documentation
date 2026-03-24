/*
 * stream2_image_buffer.c - Image buffering implementation
 */
#ifdef __linux__
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "stream2_image_buffer.h"
#include "stream2_decompress.h"
#include <string.h>

void stream2_buffer_init(struct stream2_buffer_ctx* buf, uint64_t bytes_limit) {
    buf->items = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->total_bytes = 0;
    buf->bytes_limit = bytes_limit;
    buf->warned_limit = 0;
}

static void free_owner(struct stream2_msg_owner* owner) {
    if (!owner)
        return;
    zmq_msg_close(&owner->msg);
    free(owner);
}

void stream2_buffer_free(struct stream2_buffer_ctx* buf) {
    /* First pass: free channel names, compression alg, and decrement refs */
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
    
    /* Second pass: free owners that have no more refs.
     * Track which owners we've already freed to avoid double-free
     * when multiple images share the same owner (multi-channel messages). */
    for (size_t i = 0; i < buf->len; i++) {
        struct stream2_msg_owner* owner = buf->items[i].owner;
        if (owner && owner->refs == 0) {
            /* Check if we've already seen this exact owner pointer earlier in the loop */
            int already_seen = 0;
            for (size_t j = 0; j < i; j++) {
                if (buf->items[j].owner == owner) {
                    already_seen = 1;
                    break;
                }
            }
            if (!already_seen) {
                /* This is the first time we see this owner, free it and
                 * set all references to this owner to NULL */
                struct stream2_msg_owner* owner_to_free = owner;
                for (size_t j = 0; j < buf->len; j++) {
                    if (buf->items[j].owner == owner_to_free) {
                        buf->items[j].owner = NULL;
                    }
                }
                free_owner(owner_to_free);
            }
        }
    }
    free(buf->items);
    buf->items = NULL;
    buf->len = buf->cap = 0;
    buf->total_bytes = 0;
}

static enum stream2_result ensure_capacity(struct stream2_buffer_ctx* buf) {
    if (buf->len < buf->cap)
        return STREAM2_OK;

    size_t new_cap = buf->cap ? buf->cap * 2 : 64;
    void* new_items = realloc(buf->items, new_cap * sizeof(*buf->items));
    if (!new_items)
        return STREAM2_ERROR_OUT_OF_MEMORY;
    buf->items = new_items;
    buf->cap = new_cap;
    return STREAM2_OK;
}

enum stream2_result stream2_buffer_image(
        const struct stream2_multidim_array* md,
        uint64_t image_id,
        uint64_t series_id,
        const char* channel,
        struct stream2_buffer_ctx* buf,
        struct stream2_msg_owner** owner_slot,
        zmq_msg_t* src_msg) {
    enum stream2_result r;
    const unsigned char* data = NULL;
    size_t len_elems = 0;
    size_t elem_size = 0;
    void* decompress_buffer = NULL;

    const int no_compression = (md->array.data.compression.algorithm == NULL);
    const int compressed = !no_compression;
    const int expected_shape = (md->dim[0] == 4364 && md->dim[1] == 4150);
    const int expected_type =
            (md->array.tag == STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN);

    size_t data_size = 0;

    if (compressed) {
        data = (const unsigned char*)md->array.data.ptr;
        data_size = md->array.data.len;
        elem_size = md->array.data.compression.elem_size
                            ? md->array.data.compression.elem_size
                            : 1;
    } else if (expected_shape && expected_type) {
        data = (const unsigned char*)md->array.data.ptr;
        elem_size = 2;
        len_elems = md->array.data.len / elem_size;
    } else {
        if ((r = stream2_decode_typed_array(&md->array, &data, &len_elems,
                                            &elem_size, &decompress_buffer))) {
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
            fprintf(stderr,
                    "buffer limit reached (%" PRIu64
                    " bytes); skipping further images\n",
                    buf->bytes_limit);
            buf->warned_limit = 1;
        }
        free(decompress_buffer);
        return STREAM2_OK;
    }

    if ((r = ensure_capacity(buf))) {
        free(decompress_buffer);
        return r;
    }

    struct stream2_buffered_image* bi = &buf->items[buf->len++];
    bi->channel = channel ? strdup(channel) : NULL;
    bi->image_id = image_id;
    bi->series_id = series_id;
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
            if (!*owner_slot)
                return STREAM2_ERROR_OUT_OF_MEMORY;
            zmq_msg_init(&(*owner_slot)->msg);
            zmq_msg_move(&(*owner_slot)->msg, src_msg);
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
            if (!*owner_slot)
                return STREAM2_ERROR_OUT_OF_MEMORY;
            zmq_msg_init(&(*owner_slot)->msg);
            zmq_msg_move(&(*owner_slot)->msg, src_msg);
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

enum stream2_result stream2_buffer_image_copy(
        const struct stream2_multidim_array* md,
        uint64_t image_id,
        uint64_t series_id,
        const char* channel,
        struct stream2_buffer_ctx* buf) {
    enum stream2_result r;
    const unsigned char* data;
    size_t len_elems;
    size_t elem_size;
    void* decompress_buffer;

    if ((r = stream2_decode_typed_array(&md->array, &data, &len_elems,
                                        &elem_size, &decompress_buffer))) {
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
            fprintf(stderr,
                    "buffer limit reached (%" PRIu64
                    " bytes); skipping further images\n",
                    buf->bytes_limit);
            buf->warned_limit = 1;
        }
        free(decompress_buffer);
        return STREAM2_OK;
    }

    if ((r = ensure_capacity(buf))) {
        free(decompress_buffer);
        return r;
    }

    struct stream2_buffered_image* bi = &buf->items[buf->len++];
    bi->channel = channel ? strdup(channel) : NULL;
    bi->image_id = image_id;
    bi->series_id = series_id;
    bi->width = w;
    bi->height = h;
    bi->tag = md->array.tag;
    bi->elem_size = elem_size;
    bi->data_size = data_size;
    bi->owner = NULL;
    bi->compression_alg = NULL;
    bi->compression_elem_size = 0;

    bi->data = malloc(data_size);
    if (!bi->data) {
        free(decompress_buffer);
        return STREAM2_ERROR_OUT_OF_MEMORY;
    }
    memcpy((void*)bi->data, data, data_size);

    buf->total_bytes += data_size;

    free(decompress_buffer);
    return STREAM2_OK;
}
