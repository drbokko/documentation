#ifndef _WIN32
#error "stream2_buffer_tiff_ui_win is Windows-only"
#endif

/*
 * stream2_buffer_tiff_ui_win.c
 *
 * Windows UI example that:
 *  - shows the same stats you'd see on the terminal
 *  - displays a RAM usage progress bar
 *  - keeps receiving while saving TIFFs (spawned worker threads)
 *  - lets you save TIFFs (same format as stream2_buffer_tiff) and free RAM
 *  - exit button
 */

#include <windows.h>
#include <commctrl.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>

#include "stream2.h"
#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "stream2_tiff.h"
#include <zmq.h>

#define IDC_IP       1000
#define IDC_PORT     1001
#define IDC_STATUS   1002
#define IDC_STATS    1003
#define IDC_PROGRESS 1004
#define IDC_SAVE     1005
#define IDC_EXIT     1006
#define IDC_START    1007
#define IDC_STOP     1008
#define IDC_FLUSH    1009
#define WM_APP_SAVE_DONE    (WM_APP + 1)
#define WM_APP_RECV_STATE   (WM_APP + 2)

struct app_ctx {
    HWND hwnd;
    HWND hIp;
    HWND hPort;
    HWND hStatus;
    HWND hStats;
    HWND hProgress;
    HWND hSave;
    HWND hExit;
    HWND hStart;
    HWND hStop;
    HWND hFlush;

    volatile LONG saving;
    volatile LONG receiving;
    volatile LONG recv_stop;
    CRITICAL_SECTION buf_cs;
    CRITICAL_SECTION stats_cs;

    struct stream2_stats stats;
    struct stream2_buffer_ctx buf;
    uint64_t bytes_limit;

    void* zmq_ctx;
    HANDLE hRecvThread;

    char address[128];
};

static void update_progress(struct app_ctx* ctx) {
    EnterCriticalSection(&ctx->buf_cs);
    uint64_t used = ctx->buf.total_bytes;
    uint64_t cap = ctx->bytes_limit;
    LeaveCriticalSection(&ctx->buf_cs);

    double pct = (cap > 0) ? (100.0 * (double)used / (double)cap) : 0.0;
    if (pct > 100.0) pct = 100.0;
    SendMessage(ctx->hProgress, PBM_SETRANGE32, 0, 1000);
    SendMessage(ctx->hProgress, PBM_SETPOS, (WPARAM)(pct * 10.0), 0);
}

static void format_stats_text(struct app_ctx* ctx, char* out, size_t out_sz) {
    EnterCriticalSection(&ctx->stats_cs);
    struct stream2_stats s = ctx->stats;
    LeaveCriticalSection(&ctx->stats_cs);

    EnterCriticalSection(&ctx->buf_cs);
    struct stream2_buffer_ctx b = ctx->buf;
    LeaveCriticalSection(&ctx->buf_cs);

    double gbps = 0.0;
    double elapsed = stream2_time_diff_sec(&(struct timespec){0}, &(struct timespec){0});
    (void)elapsed;  // quiet warning; we reformat below using stream2_stats_report for console

    // Also print to console to mirror terminal output
    stream2_stats_report(&s, &b, 0);

    // Compose a concise status line for the UI
    double gigabytes_total = s.bytes_total / 1e9;
    double gigabytes_buffer = b.total_bytes / 1e9;
    double gigabytes_cap = b.bytes_limit / 1e9;
    int len = _snprintf(out, out_sz,
                        "Images: %llu  Received: %.3f GB\n"
                        "Buffer: %.3f / %.3f GB  (%.1f%%)\n"
                        "Saving: %s",
                        (unsigned long long)s.images_total,
                        gigabytes_total,
                        gigabytes_buffer, gigabytes_cap,
                        (gigabytes_cap > 0.0)
                            ? (100.0 * gigabytes_buffer / gigabytes_cap)
                            : 0.0,
                        (InterlockedCompareExchange(&ctx->saving, 0, 0) ? "yes" : "no"));
    if (len < 0 || (size_t)len >= out_sz) {
        out[out_sz - 1] = '\0';
    }
}

static void safe_buffer_reset(struct stream2_buffer_ctx* buf) {
    buf->items = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->total_bytes = 0;
    buf->warned_limit = 0;
}

static DWORD WINAPI save_thread(LPVOID param) {
    struct app_ctx* ctx = (struct app_ctx*)param;

    struct stream2_buffer_ctx snapshot = {0};
    EnterCriticalSection(&ctx->buf_cs);
    snapshot = ctx->buf;
    safe_buffer_reset(&ctx->buf);
    snapshot.bytes_limit = ctx->bytes_limit;
    LeaveCriticalSection(&ctx->buf_cs);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threads = (int)si.dwNumberOfProcessors;
    if (threads < 2) threads = 2;
    if (threads > 16) threads = 16;

    stream2_flush_buffer_to_tiff_mt(&snapshot, threads);
    stream2_buffer_free(&snapshot);

    InterlockedExchange(&ctx->saving, 0);
    PostMessage(ctx->hwnd, WM_APP_SAVE_DONE, 0, 0);
    return 0;
}

static DWORD WINAPI recv_thread(LPVOID param) {
    struct app_ctx* ctx = (struct app_ctx*)param;
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    stream2_install_signal_handler();

    void* socket = zmq_socket(ctx->zmq_ctx, ZMQ_PULL);
    if (!socket) {
        InterlockedExchange(&ctx->receiving, 0);
        zmq_msg_close(&msg);
        return 0;
    }
    int hwm = 10000;
    zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    int rcvbuf = 16 * 1024 * 1024;
    zmq_setsockopt(socket, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int rcv_timeout_ms = 500;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));

    if (zmq_connect(socket, ctx->address) != 0) {
        fprintf(stderr, "zmq_connect failed: %s\n", zmq_strerror(zmq_errno()));
        zmq_close(socket);
        InterlockedExchange(&ctx->receiving, 0);
        zmq_msg_close(&msg);
        return 0;
    }

    InterlockedExchange(&ctx->receiving, 1);
    if (IsWindow(ctx->hwnd))
        PostMessage(ctx->hwnd, WM_APP_RECV_STATE, 1, 0); /* receiver state change */

    while (!g_stop && !InterlockedCompareExchange(&ctx->recv_stop, 0, 0)) {
        struct stream2_msg_owner* owner_slot = NULL;
        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            int err = zmq_errno();
            if (err == EAGAIN) continue;
            if (err == EINTR && (g_stop || InterlockedCompareExchange(&ctx->recv_stop, 0, 0))) break;
            if (err == EINTR) continue;
            fprintf(stderr, "zmq_msg_recv error: %s\n", zmq_strerror(err));
            break;
        }

        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);

        struct stream2_msg* m = NULL;
        enum stream2_result r = stream2_parse_msg(msg_data, msg_size, &m);
        if (r) {
            fprintf(stderr, "parse error %d\n", (int)r);
            zmq_msg_close(&msg);
            break;
        }

        if (m->type == STREAM2_MSG_IMAGE) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)m;
            EnterCriticalSection(&ctx->stats_cs);
            stream2_stats_add_image(&ctx->stats, msg_size);
            LeaveCriticalSection(&ctx->stats_cs);

            EnterCriticalSection(&ctx->buf_cs);
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                enum stream2_result rbuf = stream2_buffer_image(
                        &d->data, im->image_id, im->series_id,
                        d->channel, &ctx->buf, &owner_slot, &msg);
                if (rbuf != STREAM2_OK) {
                    /* Stop receiving on hard error (likely OOM) */
                    fprintf(stderr, "buffer error %d, stopping receive\n", (int)rbuf);
                    InterlockedExchange(&ctx->recv_stop, 1);
                    break;
                }
            }
            LeaveCriticalSection(&ctx->buf_cs);
        } else {
            EnterCriticalSection(&ctx->stats_cs);
            stream2_stats_add_bytes(&ctx->stats, msg_size);
            LeaveCriticalSection(&ctx->stats_cs);
        }

        stream2_free_msg(m);
    }

    zmq_close(socket);
    zmq_msg_close(&msg);
    InterlockedExchange(&ctx->receiving, 0);
    if (IsWindow(ctx->hwnd))
        PostMessage(ctx->hwnd, WM_APP_RECV_STATE, 0, 0); /* receiver state change */
    return 0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    struct app_ctx* ctx = (struct app_ctx*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);
        break;
    }
    case WM_COMMAND:
        if (!ctx) break;
        switch (LOWORD(wParam)) {
        case IDC_EXIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        case IDC_SAVE:
            if (!InterlockedCompareExchange(&ctx->saving, 1, 0)) {
                HANDLE h = CreateThread(NULL, 0, save_thread, ctx, 0, NULL);
                if (h) CloseHandle(h);
                else InterlockedExchange(&ctx->saving, 0);
            }
            break;
        case IDC_FLUSH: {
            EnterCriticalSection(&ctx->buf_cs);
            stream2_buffer_free(&ctx->buf);
            stream2_buffer_init(&ctx->buf, ctx->bytes_limit);
            LeaveCriticalSection(&ctx->buf_cs);
            break;
        }
        case IDC_START: {
            if (InterlockedCompareExchange(&ctx->receiving, 0, 0)) break; /* already running */
            char ip[128] = {0};
            char port[16] = {0};
            GetWindowText(ctx->hIp, ip, sizeof(ip));
            GetWindowText(ctx->hPort, port, sizeof(port));
            if (ip[0] == '\0') lstrcpyA(ip, "127.0.0.1");
            if (port[0] == '\0') lstrcpyA(port, "31001");
            _snprintf(ctx->address, sizeof(ctx->address), "tcp://%s:%s", ip, port);
            InterlockedExchange(&ctx->recv_stop, 0);
            ctx->hRecvThread = CreateThread(NULL, 0, recv_thread, ctx, 0, NULL);
            if (!ctx->hRecvThread) {
                InterlockedExchange(&ctx->receiving, 0);
                PostMessage(ctx->hwnd, WM_APP_RECV_STATE, 0, 0);
            }
            break;
        }
        case IDC_STOP:
            if (InterlockedCompareExchange(&ctx->receiving, 0, 0)) {
                InterlockedExchange(&ctx->recv_stop, 1);
                WaitForSingleObject(ctx->hRecvThread, INFINITE);
                CloseHandle(ctx->hRecvThread);
                ctx->hRecvThread = NULL;
                if (IsWindow(ctx->hwnd))
                    PostMessage(ctx->hwnd, WM_APP_RECV_STATE, 0, 0);
            }
            break;
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (ctx && wParam == 1) {
            char buf[256];
            format_stats_text(ctx, buf, sizeof(buf));
            SetWindowText(ctx->hStats, buf);
            update_progress(ctx);
        }
        break;
    case WM_APP_SAVE_DONE:
        MessageBox(hwnd, "Save complete", "Info", MB_OK | MB_ICONINFORMATION);
        break;
    case WM_APP_RECV_STATE:
        if (ctx) {
            const char* state = (wParam ? "running" : "stopped");
            char buf[128];
            _snprintf(buf, sizeof(buf), "Receiver: %s", state);
            SetWindowText(ctx->hStatus, buf);
        }
        break;
    case WM_CLOSE:
        g_stop = 1;
        if (ctx && InterlockedCompareExchange(&ctx->receiving, 0, 0)) {
            InterlockedExchange(&ctx->recv_stop, 1);
        }
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int main(int argc, char** argv) {
    HINSTANCE hInst = GetModuleHandle(NULL);

    struct app_ctx ctx = {0};
    InitializeCriticalSection(&ctx.buf_cs);
    InitializeCriticalSection(&ctx.stats_cs);
    stream2_stats_init(&ctx.stats);

    ctx.bytes_limit = stream2_parse_buffer_limit_gb(20);
    /* Clamp buffer size for 32-bit builds to avoid exhausting address space */
    if (sizeof(void*) == 4) {
        const uint64_t clamp_bytes = 1500ULL * 1024ULL * 1024ULL; /* ~1.5 GB */
        if (ctx.bytes_limit > clamp_bytes) ctx.bytes_limit = clamp_bytes;
    }
    stream2_buffer_init(&ctx.buf, ctx.bytes_limit);

    ctx.zmq_ctx = zmq_ctx_new();

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "Stream2UIClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    ctx.hwnd = CreateWindowEx(0, wc.lpszClassName, "Stream2 Buffered TIFF (Windows UI)",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              520, 240, NULL, NULL, hInst, NULL);
    if (!ctx.hwnd) return EXIT_FAILURE;
    SetWindowLongPtr(ctx.hwnd, GWLP_USERDATA, (LONG_PTR)&ctx);

    ctx.hIp = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "127.0.0.1",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             10, 10, 200, 24, ctx.hwnd, (HMENU)IDC_IP, hInst, NULL);
    ctx.hPort = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "31001",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               220, 10, 80, 24, ctx.hwnd, (HMENU)IDC_PORT, hInst, NULL);

    ctx.hStart = CreateWindowEx(0, "BUTTON", "Start",
                                WS_CHILD | WS_VISIBLE,
                                310, 10, 80, 24, ctx.hwnd, (HMENU)IDC_START, hInst, NULL);
    ctx.hStop = CreateWindowEx(0, "BUTTON", "Stop",
                               WS_CHILD | WS_VISIBLE,
                               400, 10, 80, 24, ctx.hwnd, (HMENU)IDC_STOP, hInst, NULL);

    ctx.hStatus = CreateWindowEx(0, "STATIC", "Receiver: stopped",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 10, 44, 480, 20, ctx.hwnd, (HMENU)IDC_STATUS, hInst, NULL);
    ctx.hStats = CreateWindowEx(0, "STATIC", "",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                10, 66, 480, 60, ctx.hwnd, (HMENU)IDC_STATS, hInst, NULL);
    ctx.hProgress = CreateWindowEx(0, PROGRESS_CLASS, NULL,
                                   WS_CHILD | WS_VISIBLE,
                                   10, 130, 480, 20, ctx.hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);
    ctx.hSave = CreateWindowEx(0, "BUTTON", "Save TIFFs",
                               WS_CHILD | WS_VISIBLE,
                               10, 158, 120, 28, ctx.hwnd, (HMENU)IDC_SAVE, hInst, NULL);
    ctx.hFlush = CreateWindowEx(0, "BUTTON", "Flush Buffer",
                                WS_CHILD | WS_VISIBLE,
                                140, 158, 120, 28, ctx.hwnd, (HMENU)IDC_FLUSH, hInst, NULL);
    ctx.hExit = CreateWindowEx(0, "BUTTON", "Exit",
                               WS_CHILD | WS_VISIBLE,
                               270, 158, 80, 28, ctx.hwnd, (HMENU)IDC_EXIT, hInst, NULL);

    ShowWindow(ctx.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(ctx.hwnd);

    SetTimer(ctx.hwnd, 1, 500, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_stop = 1;
    if (InterlockedCompareExchange(&ctx.receiving, 0, 0)) {
        InterlockedExchange(&ctx.recv_stop, 1);
    }
    if (ctx.hRecvThread) {
        WaitForSingleObject(ctx.hRecvThread, INFINITE);
        CloseHandle(ctx.hRecvThread);
        ctx.hRecvThread = NULL;
    }

    zmq_ctx_term(ctx.zmq_ctx);

    EnterCriticalSection(&ctx.buf_cs);
    stream2_buffer_free(&ctx.buf);
    LeaveCriticalSection(&ctx.buf_cs);
    DeleteCriticalSection(&ctx.buf_cs);
    DeleteCriticalSection(&ctx.stats_cs);
    return 0;
}
