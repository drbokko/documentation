/*
 * stream2_tiff.h - TIFF writing for Stream V2 examples
 */
#ifndef STREAM2_TIFF_H
#define STREAM2_TIFF_H

#include "stream2_image_buffer.h"

/* Write a buffered image to a TIFF file */
int stream2_write_tiff(const char* path,
                       const struct stream2_buffered_image* img);

/* Format a TIFF filename for an image */
void stream2_format_tiff_path(char* dst,
                              size_t dst_size,
                              const char* channel,
                              uint64_t image_id);

/* Write one image from buffer, with decompression if needed */
void stream2_write_one_image(struct stream2_buffer_ctx* buf,
                             size_t idx,
                             uint64_t* compressed_bytes,
                             uint64_t* decompressed_bytes);

/* Flush entire buffer to TIFF files (single-threaded) */
void stream2_flush_buffer_to_tiff(struct stream2_buffer_ctx* buf);

/* Flush entire buffer to TIFF files (multi-threaded) */
void stream2_flush_buffer_to_tiff_mt(struct stream2_buffer_ctx* buf,
                                     int num_threads);

#endif /* STREAM2_TIFF_H */
