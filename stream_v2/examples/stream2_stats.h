/*
 * stream2_stats.h - Statistics tracking and reporting for Stream V2 examples
 */
#ifndef STREAM2_STATS_H
#define STREAM2_STATS_H

#include "stream2_common.h"

/* Forward declaration for buffer context */
struct stream2_buffer_ctx;

/* Statistics tracking structure */
struct stream2_stats {
    uint64_t images_total;
    uint64_t bytes_total;
    uint64_t images_window;
    uint64_t bytes_window;
    struct timespec last_report;
    int first_image_printed;
};

/* Initialize stats structure */
void stream2_stats_init(struct stream2_stats* s);

/* Update stats for an image message */
void stream2_stats_add_image(struct stream2_stats* s, size_t msg_size);

/* Update stats for a non-image message */
void stream2_stats_add_bytes(struct stream2_stats* s, size_t msg_size);

/* Report stats to stdout (with optional buffer info) */
void stream2_stats_report(struct stream2_stats* s,
                          const struct stream2_buffer_ctx* buf,
                          int force);

/* Simpler report without buffer info */
void stream2_stats_report_simple(struct stream2_stats* s, int force);

#endif /* STREAM2_STATS_H */
