/*
 * tiff_writer.c - TIFF writing implementation
 */
#ifdef __linux__
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "tiff_writer.h"
#include "compression/src/compression.h"
#include <errno.h>
#include <string.h>

#ifndef _WIN32
#include <sys/statvfs.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#define TIFF_WRITER_THREADS_SUPPORTED 1
#define TIFF_WRITER_THREADS_WIN 0
#else
#include <direct.h>
    /* Windows: no statvfs; skip disk-space check */
#define TIFF_WRITER_THREADS_SUPPORTED 1
#define TIFF_WRITER_THREADS_WIN 1
#endif

/* Custom output path (NULL means use default) */
static char g_output_path[512] = {0};

static uint16_t sample_format_for_tag(enum stream2_typed_array_tag tag) {
    switch (tag) {
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN:
            return 3; /* IEEE floating point */
        default:
            return 1; /* unsigned int */
    }
}

static inline int fwrite_checked(const void* ptr,
                                 size_t size,
                                 size_t nmemb,
                                 FILE* f) {
    if (fwrite(ptr, size, nmemb, f) != nmemb) {
        if (errno == ENOSPC)
            g_out_of_space = 1;
        return -1;
    }
    return 0;
}

#define WRITE_OR_FAIL(ptr, size, nmemb) \
    do {                                \
        if (fwrite_checked(ptr, size, nmemb, f)) \
            goto io_fail;               \
    } while (0)

int tiff_writer_write(const char* path,
                       const struct stream2_buffered_image* img) {
    if (img->data_size > UINT32_MAX)
        return -1; /* classic TIFF can't handle >4GB */

    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;

    uint16_t magic = 42;
    uint32_t ifd_offset = 8;
    WRITE_OR_FAIL("II", 1, 2);
    WRITE_OR_FAIL(&magic, sizeof(magic), 1);
    WRITE_OR_FAIL(&ifd_offset, sizeof(ifd_offset), 1);

    uint16_t entry_count = 11;
    uint32_t image_data_offset = 8 + 2 + entry_count * 12 + 4;

    uint16_t sample_format = sample_format_for_tag(img->tag);
    uint16_t samples_per_pixel = 1;

    uint16_t bits_tag = (uint16_t)(img->elem_size * 8);
    switch (img->tag) {
        case STREAM2_TYPED_ARRAY_UINT8:
            bits_tag = 8;
            break;
        case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN:
            bits_tag = 16;
            break;
        case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN:
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN:
            bits_tag = 32;
            break;
        default:
            break;
    }
    if (bits_tag != 8 && bits_tag != 16 && bits_tag != 32)
        bits_tag = 16;

    WRITE_OR_FAIL(&entry_count, sizeof(entry_count), 1);

    uint16_t tag = 256, type = 4;
    uint32_t count = 1;
    uint32_t val32;
    uint16_t val16;
    uint32_t offset;
    uint16_t pad16 = 0;

    /* ImageWidth */
    val32 = (uint32_t)img->width;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val32, 4, 1);

    /* ImageLength */
    tag = 257;
    val32 = (uint32_t)img->height;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val32, 4, 1);

    /* BitsPerSample */
    tag = 258;
    type = 3;
    count = 1;
    val16 = bits_tag;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val16, 2, 1);
    WRITE_OR_FAIL(&pad16, 2, 1);

    /* Compression (1 = none) */
    tag = 259;
    type = 3;
    count = 1;
    val16 = 1;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val16, 2, 1);
    WRITE_OR_FAIL(&pad16, 2, 1);

    /* Photometric (1 = min is black) */
    tag = 262;
    type = 3;
    count = 1;
    val16 = 1;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val16, 2, 1);
    WRITE_OR_FAIL(&pad16, 2, 1);

    /* StripOffsets */
    tag = 273;
    type = 4;
    count = 1;
    offset = image_data_offset;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&offset, 4, 1);

    /* RowsPerStrip */
    tag = 278;
    type = 4;
    count = 1;
    val32 = (uint32_t)img->height;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val32, 4, 1);

    /* StripByteCounts */
    tag = 279;
    type = 4;
    count = 1;
    val32 = (uint32_t)img->data_size;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val32, 4, 1);

    /* SamplesPerPixel */
    tag = 277;
    type = 3;
    count = 1;
    val16 = samples_per_pixel;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val16, 2, 1);
    WRITE_OR_FAIL(&pad16, 2, 1);

    /* PlanarConfiguration (1 = chunky) */
    tag = 284;
    type = 3;
    count = 1;
    val16 = 1;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val16, 2, 1);
    WRITE_OR_FAIL(&pad16, 2, 1);

    /* SampleFormat */
    tag = 339;
    type = 3;
    count = 1;
    val16 = sample_format;
    WRITE_OR_FAIL(&tag, 2, 1);
    WRITE_OR_FAIL(&type, 2, 1);
    WRITE_OR_FAIL(&count, 4, 1);
    WRITE_OR_FAIL(&val16, 2, 1);
    WRITE_OR_FAIL(&pad16, 2, 1);

    /* next IFD offset = 0 */
    uint32_t zero = 0;
    WRITE_OR_FAIL(&zero, sizeof(zero), 1);

    /* Image data */
    if (fwrite_checked(img->data, 1, img->data_size, f) != 0)
        goto io_fail;

    fclose(f);
    return 0;

io_fail:
    if (errno == ENOSPC) {
        fprintf(stderr, "disk full while writing %s\n", path);
    } else {
        fprintf(stderr, "I/O error writing %s: %s\n", path, strerror(errno));
    }
    fclose(f);
    return -1;
}

void tiff_writer_set_output_path(const char* path) {
    if (path && path[0] != '\0') {
        size_t len = strlen(path);
        strncpy(g_output_path, path, sizeof(g_output_path) - 1);
        g_output_path[sizeof(g_output_path) - 1] = '\0';
        
        /* Remove trailing slashes/backslashes */
        len = strlen(g_output_path);
        while (len > 0 && (g_output_path[len - 1] == '/' || g_output_path[len - 1] == '\\')) {
            g_output_path[len - 1] = '\0';
            len--;
        }
    } else {
        g_output_path[0] = '\0';
    }
}

void tiff_writer_format_path(char* dst,
                              size_t dst_size,
                              const char* channel,
                              uint64_t image_id,
                              uint64_t series_id) {
    const char* base;
    char series_dir[256];
    char full_path[512];
    
    /* Use custom path if set, otherwise use default */
    if (g_output_path[0] != '\0') {
        base = g_output_path;
    } else {
#ifdef _WIN32
        base = "Z:/";
#else
        base = "/dev/shm";
#endif
    }
    
#ifdef _WIN32
    snprintf(series_dir, sizeof(series_dir), "%s\\serie_%06" PRIu64, base, series_id);
    snprintf(full_path, sizeof(full_path), "%s\\stream2_%s_%06" PRIu64 ".tiff",
             series_dir, channel ? channel : "data", image_id);
#else
    snprintf(series_dir, sizeof(series_dir), "%s/serie_%06" PRIu64, base, series_id);
    snprintf(full_path, sizeof(full_path), "%s/stream2_%s_%06" PRIu64 ".tiff",
             series_dir, channel ? channel : "data", image_id);
#endif
    
    /* Create base directory if it doesn't exist (for custom paths) */
    if (g_output_path[0] != '\0') {
#ifdef _WIN32
        _mkdir(base);
#else
        struct stat st = {0};
        if (stat(base, &st) == -1) {
            mkdir(base, 0755);
        }
#endif
    }
    
    /* Create series directory if it doesn't exist */
#ifdef _WIN32
    /* On Windows, _mkdir returns -1 if directory exists, so ignore errors */
    _mkdir(series_dir);
#else
    /* Check if directory exists, create if not */
    struct stat st = {0};
    if (stat(series_dir, &st) == -1) {
        mkdir(series_dir, 0755);
    }
#endif
    
    strncpy(dst, full_path, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void tiff_writer_write_one_image(struct stream2_buffer_ctx* buf,
                             size_t idx,
                             uint64_t* compressed_bytes,
                             uint64_t* decompressed_bytes) {
    struct stream2_buffered_image* bi = &buf->items[idx];
    const void* out_data = bi->data;
    size_t out_size = bi->data_size;
    void* tmp = NULL;

    if (compressed_bytes)
        *compressed_bytes = 0;
    if (decompressed_bytes)
        *decompressed_bytes = 0;

    if (bi->compression_alg) {
        CompressionAlgorithm algorithm;
        if (strcmp(bi->compression_alg, "bslz4") == 0) {
            algorithm = COMPRESSION_BSLZ4;
        } else if (strcmp(bi->compression_alg, "lz4") == 0) {
            algorithm = COMPRESSION_LZ4;
        } else {
            fprintf(stderr, "unknown compression '%s' for image %" PRIu64 "\n",
                    bi->compression_alg, bi->image_id);
            return;
        }
        const size_t out_len = compression_decompress_buffer(
                algorithm, NULL, 0, (const char*)bi->data, bi->data_size,
                bi->compression_elem_size);
        if (out_len == COMPRESSION_ERROR) {
            fprintf(stderr, "decompress size error for image %" PRIu64 "\n",
                    bi->image_id);
            return;
        }
        tmp = malloc(out_len);
        if (!tmp) {
            fprintf(stderr, "OOM decompressing image %" PRIu64 "\n",
                    bi->image_id);
            return;
        }
        const size_t decoded = compression_decompress_buffer(
                algorithm, (char*)tmp, out_len, (const char*)bi->data,
                bi->data_size, bi->compression_elem_size);
        if (decoded != out_len) {
            fprintf(stderr, "decode mismatch for image %" PRIu64 "\n",
                    bi->image_id);
            free(tmp);
            return;
        }
        out_data = tmp;
        out_size = out_len;
    }

    if (out_size > UINT32_MAX) {
        fprintf(stderr,
                "image %" PRIu64 " too large for TIFF (>4GB), skipping\n",
                bi->image_id);
        free(tmp);
        return;
    }

    if (compressed_bytes)
        *compressed_bytes = bi->data_size;
    if (decompressed_bytes)
        *decompressed_bytes = out_size;

    struct stream2_buffered_image out_img = *bi;
    out_img.data = out_data;
    out_img.data_size = out_size;
    out_img.tag = bi->compression_elem_size == 4
                          ? STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN
                          : bi->tag;

    char filename[256];
    tiff_writer_format_path(filename, sizeof(filename), bi->channel,
                             bi->image_id, bi->series_id);

#ifndef _WIN32
    /* Preflight space check (POSIX only): avoid partial writes. */
    struct statvfs svfs;
    if (statvfs(filename, &svfs) == 0) {
        unsigned long long free_bytes =
                (unsigned long long)svfs.f_bavail * (unsigned long long)svfs.f_frsize;
        const unsigned long long needed = out_size + 1 * 1024 * 1024ULL; /* +1MiB buffer */
        if (free_bytes < needed) {
            fprintf(stderr,
                    "no space left to write %s (need %llu bytes, free %llu bytes)\n",
                    filename, needed, free_bytes);
            g_out_of_space = 1;
            free(tmp);
            return;
        }
    }
#endif

    if (tiff_writer_write(filename, &out_img) != 0) {
        fprintf(stderr, "failed to write %s\n", filename);
    }

    free(tmp);
}

void tiff_writer_flush_buffer(struct stream2_buffer_ctx* buf) {
    for (size_t i = 0; i < buf->len; i++) {
        struct stream2_buffered_image* bi = &buf->items[i];
        if (bi->data_size > UINT32_MAX) {
            fprintf(stderr,
                    "image %" PRIu64
                    " too large for classic TIFF (>4GB), skipping\n",
                    bi->image_id);
            continue;
        }
        char filename[256];
        tiff_writer_format_path(filename, sizeof(filename), bi->channel,
                                 bi->image_id, bi->series_id);
        if (tiff_writer_write(filename, bi) != 0) {
            fprintf(stderr, "failed to write %s\n", filename);
        }
    }
}

/* Multi-threaded TIFF writing */
#if TIFF_WRITER_THREADS_WIN
struct write_ctx {
    struct stream2_buffer_ctx* buf;
    volatile LONG64 next;
    volatile LONG64 done;
    struct timespec start;
    long long total_ns;
    uint64_t compressed_total;
    uint64_t decompressed_total;
    CRITICAL_SECTION stats_cs;
};

static DWORD WINAPI writer_thread_win(LPVOID arg) {
    struct write_ctx* ctx = (struct write_ctx*)arg;
    for (;;) {
        LONG64 idx64 = InterlockedIncrement64(&ctx->next) - 1;
        size_t idx = (size_t)idx64;
        if (idx >= ctx->buf->len || g_out_of_space)
            break;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t cbytes = 0, dbytes = 0;
        tiff_writer_write_one_image(ctx->buf, idx, &cbytes, &dbytes);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        EnterCriticalSection(&ctx->stats_cs);
        ctx->total_ns += stream2_time_diff_ns(&t1, &t0);
        ctx->done++;
        ctx->compressed_total += cbytes;
        ctx->decompressed_total += dbytes;
        uint64_t done_u = (uint64_t)ctx->done;
        size_t total = ctx->buf->len;
        double pct = total ? (100.0 * (double)done_u / (double)total) : 100.0;
        printf("\rWriting TIFFs: %" PRIu64 "/%zu (%.1f%%)", done_u, total, pct);
        fflush(stdout);
        LeaveCriticalSection(&ctx->stats_cs);
        if (g_out_of_space)
            break;
    }
    return 0;
}

#elif TIFF_WRITER_THREADS_SUPPORTED

struct write_ctx {
    struct stream2_buffer_ctx* buf;
    pthread_mutex_t mu;
    size_t next;
    size_t done;
    struct timespec start;
    long long total_ns;
    uint64_t compressed_total;
    uint64_t decompressed_total;
};

static void* writer_thread(void* arg) {
    struct write_ctx* ctx = (struct write_ctx*)arg;
    for (;;) {
        size_t idx;
        pthread_mutex_lock(&ctx->mu);
        idx = ctx->next++;
        pthread_mutex_unlock(&ctx->mu);
        if (idx >= ctx->buf->len || g_out_of_space)
            break;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t cbytes = 0, dbytes = 0;
        tiff_writer_write_one_image(ctx->buf, idx, &cbytes, &dbytes);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        pthread_mutex_lock(&ctx->mu);
        ctx->total_ns += stream2_time_diff_ns(&t1, &t0);
        ctx->done++;
        ctx->compressed_total += cbytes;
        ctx->decompressed_total += dbytes;
        size_t total = ctx->buf->len;
        double pct =
                total ? (100.0 * (double)ctx->done / (double)total) : 100.0;
        printf("\rWriting TIFFs: %zu/%zu (%.1f%%)", ctx->done, total, pct);
        fflush(stdout);
        pthread_mutex_unlock(&ctx->mu);
        if (g_out_of_space)
            break;
    }
    return NULL;
}
#endif

void tiff_writer_flush_buffer_mt(struct stream2_buffer_ctx* buf,
                                     int num_threads) {
    if (buf->len == 0)
        return;

    /* Ensure out-of-space flag is clear before writing. */
    g_out_of_space = 0;

    int threads = TIFF_WRITER_THREADS_SUPPORTED ? num_threads : 1;
    if (threads <= 0)
        threads = 10;

    if (TIFF_WRITER_THREADS_SUPPORTED) {
        const char* env_threads = getenv("STREAM2_TIFF_THREADS");
        if (env_threads && *env_threads) {
            char* endp = NULL;
            long t = strtol(env_threads, &endp, 10);
            if (endp && *endp == '\0' && t > 0 && t < 256)
                threads = (int)t;
        }
    }

#if TIFF_WRITER_THREADS_WIN
    struct write_ctx ctx = {
            .buf = buf,
            .next = -1,
            .done = 0,
            .total_ns = 0,
            .compressed_total = 0,
            .decompressed_total = 0,
    };
    clock_gettime(CLOCK_MONOTONIC, &ctx.start);
    InitializeCriticalSection(&ctx.stats_cs);

    HANDLE* tids = calloc((size_t)threads, sizeof(HANDLE));
    if (!tids) {
        fprintf(stderr,
                "WARN: cannot allocate threads array, falling back to "
                "single-thread\n");
        threads = 1;
    }

    if (threads <= 1 || !tids) {
        uint64_t cbytes = 0, dbytes = 0;
        for (size_t i = 0; i < buf->len; i++) {
            tiff_writer_write_one_image(buf, i, &cbytes, &dbytes);
            ctx.compressed_total += cbytes;
            ctx.decompressed_total += dbytes;
            ctx.done++;
            double pct = (buf->len > 0) ? (100.0 * ctx.done / (double)buf->len)
                                        : 100.0;
            printf("\rWriting TIFFs: %" PRIu64 "/%zu (%.1f%%)", ctx.done,
                   buf->len, pct);
            fflush(stdout);
        }
    } else {
        for (int i = 0; i < threads; i++) {
            tids[i] = CreateThread(NULL, 0, writer_thread_win, &ctx, 0, NULL);
            if (!tids[i]) {
                fprintf(stderr,
                        "WARN: CreateThread failed, reducing thread count\n");
                threads = i;
                break;
            }
        }
        if (threads > 0) {
            WaitForMultipleObjects((DWORD)threads, tids, TRUE, INFINITE);
            for (int i = 0; i < threads; i++) {
                if (tids[i])
                    CloseHandle(tids[i]);
            }
        }
    }
    free(tids);

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double wall_sec = stream2_time_diff_ns(&end, &ctx.start) / 1e9;
    double cpu_sec = ctx.total_ns / 1e9;
    printf("\nTIFF flush done: images=%zu threads=%d wall=%.3fs cpu=%.3fs\n",
           buf->len, threads, wall_sec, cpu_sec);
    if (ctx.compressed_total > 0) {
        double ratio =
                (double)ctx.decompressed_total / (double)ctx.compressed_total;
        printf("Compression ratio (decompressed/compressed): %.3f "
               "(compressed %.3f GB, decompressed %.3f GB)\n",
               ratio, ctx.compressed_total / 1e9,
               ctx.decompressed_total / 1e9);
    }
    DeleteCriticalSection(&ctx.stats_cs);

#elif TIFF_WRITER_THREADS_SUPPORTED

    struct write_ctx ctx = {
            .buf = buf,
            .next = 0,
            .done = 0,
            .total_ns = 0,
            .compressed_total = 0,
            .decompressed_total = 0,
    };
    pthread_mutex_init(&ctx.mu, NULL);
    clock_gettime(CLOCK_MONOTONIC, &ctx.start);

    pthread_t* tids = calloc((size_t)threads, sizeof(pthread_t));
    if (!tids) {
        fprintf(stderr,
                "WARN: cannot allocate threads array, falling back to "
                "single-thread\n");
        threads = 1;
    }

    if (threads <= 1 || !tids) {
        uint64_t cbytes = 0, dbytes = 0;
        for (size_t i = 0; i < buf->len; i++) {
            tiff_writer_write_one_image(buf, i, &cbytes, &dbytes);
            ctx.compressed_total += cbytes;
            ctx.decompressed_total += dbytes;
            ctx.done++;
            double pct = (buf->len > 0) ? (100.0 * ctx.done / (double)buf->len)
                                        : 100.0;
            printf("\rWriting TIFFs: %zu/%zu (%.1f%%)", ctx.done, buf->len,
                   pct);
            fflush(stdout);
        }
    } else {
        for (int i = 0; i < threads; i++) {
            if (pthread_create(&tids[i], NULL, writer_thread, &ctx) != 0) {
                fprintf(stderr,
                        "WARN: pthread_create failed, reducing thread count\n");
                threads = i;
                break;
            }
        }
        for (int i = 0; i < threads; i++) {
            pthread_join(tids[i], NULL);
        }
    }
    free(tids);

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double wall_sec = stream2_time_diff_ns(&end, &ctx.start) / 1e9;
    double cpu_sec = ctx.total_ns / 1e9;
    printf("\nTIFF flush done: images=%zu threads=%d wall=%.3fs cpu=%.3fs\n",
           buf->len, threads, wall_sec, cpu_sec);
    if (ctx.compressed_total > 0) {
        double ratio =
                (double)ctx.decompressed_total / (double)ctx.compressed_total;
        printf("Compression ratio (decompressed/compressed): %.3f "
               "(compressed %.3f GB, decompressed %.3f GB)\n",
               ratio, ctx.compressed_total / 1e9,
               ctx.decompressed_total / 1e9);
    }
    pthread_mutex_destroy(&ctx.mu);

#else
    /* No threading support */
    uint64_t compressed_total = 0, decompressed_total = 0;
    for (size_t i = 0; i < buf->len; i++) {
        uint64_t cbytes = 0, dbytes = 0;
        tiff_writer_write_one_image(buf, i, &cbytes, &dbytes);
        compressed_total += cbytes;
        decompressed_total += dbytes;
        double pct =
                (buf->len > 0) ? (100.0 * (i + 1) / (double)buf->len) : 100.0;
        printf("\rWriting TIFFs: %zu/%zu (%.1f%%)", i + 1, buf->len, pct);
        fflush(stdout);
    }
    printf("\nTIFF flush done: images=%zu threads=1\n", buf->len);
    if (compressed_total > 0) {
        double ratio = (double)decompressed_total / (double)compressed_total;
        printf("Compression ratio (decompressed/compressed): %.3f\n", ratio);
    }
#endif
}
