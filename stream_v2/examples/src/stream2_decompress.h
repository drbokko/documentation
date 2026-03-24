/*
 * stream2_decompress.h - Decompression helpers for Stream V2 examples
 */
#ifndef STREAM2_DECOMPRESS_H
#define STREAM2_DECOMPRESS_H

#include "stream2.h"
#include "stream2_common.h"

/*
 * Decode compressed bytes. If uncompressed, returns pointer to original data.
 * If compressed, allocates and returns decompressed buffer (caller must free
 * via decompress_buffer).
 */
enum stream2_result stream2_decode_bytes(const struct stream2_bytes* bytes,
                                         const unsigned char** decoded,
                                         size_t* decoded_len,
                                         void** decompress_buffer);

/*
 * Decode a typed array, returning element count and size.
 * If data was decompressed, caller must free decompress_buffer.
 */
enum stream2_result stream2_decode_typed_array(
        const struct stream2_typed_array* array,
        const unsigned char** data,
        size_t* len,
        size_t* elem_size,
        void** decompress_buffer);

#endif /* STREAM2_DECOMPRESS_H */
