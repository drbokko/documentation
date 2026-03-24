/*
 * stream2_stats.c - Statistics tracking and reporting implementation
 */
#include "stream2_stats.h"
#include "stream2_image_buffer.h"

void stream2_stats_init(struct stream2_stats* s) {
    memset(s, 0, sizeof(*s));
    clock_gettime(CLOCK_MONOTONIC, &s->last_report);
}

void stream2_stats_add_image(struct stream2_stats* s, size_t msg_size) {
    s->images_total++;
    s->images_window++;
    s->bytes_total += msg_size;
    s->bytes_window += msg_size;
}

void stream2_stats_add_bytes(struct stream2_stats* s, size_t msg_size) {
    s->bytes_total += msg_size;
    s->bytes_window += msg_size;
}

void stream2_stats_report(struct stream2_stats* s,
                          const struct stream2_buffer_ctx* buf,
                          int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = stream2_time_diff_sec(&now, &s->last_report);
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

void stream2_stats_report_simple(struct stream2_stats* s, int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = stream2_time_diff_sec(&now, &s->last_report);
    if (!force && elapsed < 3.0)
        return;

    double gigabytes_total = s->bytes_total / 1e9;
    printf("\rimages: %" PRIu64 "  received: %.3f GB", s->images_total,
           gigabytes_total);
    if (force)
        printf("\n");
    fflush(stdout);

    s->last_report = now;
}
