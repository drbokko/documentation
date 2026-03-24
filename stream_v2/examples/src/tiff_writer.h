/*
 * tiff_writer.h - TIFF writing for buffered detector images
 *
 * Operates on types from stream2_image_buffer.h (no stream2 parser API).
 */
#ifndef TIFF_WRITER_H
#define TIFF_WRITER_H

#include "stream2_image_buffer.h"

/* Write a buffered image to a TIFF file */
int tiff_writer_write(const char* path,
                      const struct stream2_buffered_image* img);

/* Set custom output path for TIFF files (NULL or empty string to use default) */
void tiff_writer_set_output_path(const char* path);

/* Format a TIFF filename for an image (creates series folder if needed) */
void tiff_writer_format_path(char* dst,
                             size_t dst_size,
                             const char* channel,
                             uint64_t image_id,
                             uint64_t series_id);

/* Write one image from buffer, with decompression if needed */
void tiff_writer_write_one_image(struct stream2_buffer_ctx* buf,
                                 size_t idx,
                                 uint64_t* compressed_bytes,
                                 uint64_t* decompressed_bytes);

/* Flush entire buffer to TIFF files (single-threaded) */
void tiff_writer_flush_buffer(struct stream2_buffer_ctx* buf);

/* Flush entire buffer to TIFF files (multi-threaded) */
void tiff_writer_flush_buffer_mt(struct stream2_buffer_ctx* buf,
                                 int num_threads);

#endif /* TIFF_WRITER_H */
