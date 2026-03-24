#ifndef _WIN32
#error "DectrisStream2Demo_windows is Windows-only"
#endif

/*
 * DectrisStream2Demo_windows.c
 *
 * Windows UI example that:
 *  - shows the same stats you'd see on the terminal
 *  - displays a RAM usage progress bar
 *  - keeps receiving while saving TIFFs (spawned worker threads)
 *  - lets you save TIFFs (same format as DectrisStream2Receiver_linux) and free RAM
 *  - exit button
 */

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_  /* Prevent windows.h from including winsock.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <iphlpapi.h>
#include <shlobj.h>

#include "stream2.h"
#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "tiff_writer.h"
#include <zmq.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#define IDC_IP       1000
#define IDC_PORT     1001
#define IDC_THREADS  1002
#define IDC_STATUS   1003
#define IDC_STATS    1004
#define IDC_PROGRESS 1005
#define IDC_SAVE     1006
#define IDC_EXIT     1007
#define IDC_START    1008
#define IDC_STOP     1009
#define IDC_FLUSH    1010
#define IDC_SAVE_PROGRESS 1011
#define IDC_NET_STATS 1012
#define IDC_OUTPUT_FOLDER 1013
#define IDC_BROWSE_FOLDER 1014
#define IDC_AUTOSAVE 1015
#define WM_APP_SAVE_DONE    (WM_APP + 1)
#define WM_APP_RECV_STATE   (WM_APP + 2)
#define WM_APP_SAVE_PROGRESS (WM_APP + 3)
#define MAX_RECEIVERS 50
#define DEFAULT_RECEIVERS 10

struct net_stats {
    uint64_t rx_packets;
    uint64_t rx_errors;
    uint64_t rx_dropped;
    uint64_t tx_errors;
    uint64_t tx_dropped;
    char iface_name[64];
    ULONG iface_index;
};

struct app_ctx {
    HWND hwnd;
    HWND hIp;
    HWND hPort;
    HWND hThreads;
    HWND hStatus;
    HWND hStats;
    HWND hProgress;
    HWND hSave;
    HWND hExit;
    HWND hStart;
    HWND hStop;
    HWND hFlush;
    HWND hSaveProgress;
    HWND hNetStats;
    HWND hOutputFolder;
    HWND hBrowseFolder;
    HWND hAutosave;

    volatile LONG saving;
    volatile LONG autosave_enabled;
    volatile LONG64 last_autosaved_index;
    volatile LONG receiving;
    volatile LONG recv_stop;
    volatile LONG64 save_done;
    volatile LONG64 save_total;
    CRITICAL_SECTION buf_cs;
    CRITICAL_SECTION stats_cs;
    CRITICAL_SECTION net_cs;

    struct stream2_stats stats;
    struct stream2_buffer_ctx buf;
    uint64_t bytes_limit;

    void* zmq_ctx;
    HANDLE hRecvThreads[MAX_RECEIVERS];
    int num_active_receivers;
    int num_receivers;

    struct net_stats net_start;
    struct net_stats net_current;
    int have_net_stats;

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

    /* Update save progress */
    LONG64 done = InterlockedCompareExchange64(&ctx->save_done, 0, 0);
    LONG64 total = InterlockedCompareExchange64(&ctx->save_total, 0, 0);
    if (total > 0) {
        double save_pct = (100.0 * (double)done / (double)total);
        if (save_pct > 100.0) save_pct = 100.0;
        SendMessage(ctx->hSaveProgress, PBM_SETRANGE32, 0, 1000);
        SendMessage(ctx->hSaveProgress, PBM_SETPOS, (WPARAM)(save_pct * 10.0), 0);
    } else {
        SendMessage(ctx->hSaveProgress, PBM_SETPOS, 0, 0);
    }
}

static int get_interface_for_address(const char* address, char* iface_name, ULONG* iface_index) {
    /* Extract IP from address (tcp://IP:PORT) */
    char ip_str[128] = {0};
    if (strncmp(address, "tcp://", 6) == 0) {
        const char* ip_start = address + 6;
        const char* colon = strchr(ip_start, ':');
        if (colon) {
            size_t len = colon - ip_start;
            if (len < sizeof(ip_str)) {
                memcpy(ip_str, ip_start, len);
                ip_str[len] = '\0';
            }
        }
    }

    struct sockaddr_in target;
    if (inet_pton(AF_INET, ip_str, &target.sin_addr) != 1) {
        return -1;
    }

    ULONG target_ip = ntohl(target.sin_addr.S_un.S_addr);
    ULONG mask = 0xFFFFFF00u; /* /24 */
    if ((target_ip & 0x80000000u) == 0) {
        mask = 0xFF000000u; /* Class A */
    } else if ((target_ip & 0xC0000000u) == 0x80000000u) {
        mask = 0xFFFF0000u; /* Class B */
    }

    /* Use GetAdaptersAddresses to find interface */
    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = NULL;
    DWORD dwRetVal = 0;
    int found = -1;

    for (int i = 0; i < 3; i++) {
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
        if (!pAddresses) return -1;

        dwRetVal = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen);
        if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
            free(pAddresses);
            pAddresses = NULL;
            continue;
        }
        break;
    }

    if (dwRetVal != NO_ERROR || !pAddresses) {
        if (pAddresses) free(pAddresses);
        return -1;
    }

    PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
    while (pCurrAddresses) {
        if (pCurrAddresses->IfType != IF_TYPE_ETHERNET_CSMACD && 
            pCurrAddresses->IfType != IF_TYPE_IEEE80211) {
            pCurrAddresses = pCurrAddresses->Next;
            continue;
        }

        PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurrAddresses->FirstUnicastAddress;
        while (pUnicast) {
            if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in* sin = (struct sockaddr_in*)pUnicast->Address.lpSockaddr;
                ULONG iface_ip = ntohl(sin->sin_addr.S_un.S_addr);
                if ((iface_ip & mask) == (target_ip & mask)) {
                    strncpy(iface_name, (char*)pCurrAddresses->FriendlyName, 63);
                    iface_name[63] = '\0';
                    *iface_index = pCurrAddresses->IfIndex;
                    found = 0;
                    break;
                }
            }
            pUnicast = pUnicast->Next;
        }
        if (found == 0) break;
        pCurrAddresses = pCurrAddresses->Next;
    }

    free(pAddresses);
    return found;
}

static int read_net_stats(ULONG iface_index, struct net_stats* st) {
    PMIB_IF_ROW2 pRow = (PMIB_IF_ROW2)malloc(sizeof(MIB_IF_ROW2));
    if (!pRow) return -1;
    pRow->InterfaceIndex = iface_index;
    DWORD dwRetVal = GetIfEntry2(pRow);
    if (dwRetVal != NO_ERROR) {
        free(pRow);
        return -1;
    }

    st->rx_packets = pRow->InUcastPkts + pRow->InNUcastPkts;
    st->rx_errors = pRow->InErrors;
    st->rx_dropped = pRow->InDiscards;
    st->tx_errors = pRow->OutErrors;
    st->tx_dropped = pRow->OutDiscards;

    free(pRow);
    return 0;
}

static void format_stats_text(struct app_ctx* ctx, char* out, size_t out_sz) {
    EnterCriticalSection(&ctx->stats_cs);
    struct stream2_stats s = ctx->stats;
    LeaveCriticalSection(&ctx->stats_cs);

    EnterCriticalSection(&ctx->buf_cs);
    struct stream2_buffer_ctx b = ctx->buf;
    LeaveCriticalSection(&ctx->buf_cs);

    // Also print to console to mirror terminal output
    stream2_stats_report(&s, &b, 0);

    // Compose a concise status line for the UI
    double gigabytes_total = s.bytes_total / 1e9;
    double gigabytes_buffer = b.total_bytes / 1e9;
    double gigabytes_cap = b.bytes_limit / 1e9;
    LONG64 save_done = InterlockedCompareExchange64(&ctx->save_done, 0, 0);
    LONG64 save_total = InterlockedCompareExchange64(&ctx->save_total, 0, 0);
    
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

    /* Update network stats display */
    EnterCriticalSection(&ctx->net_cs);
    if (ctx->have_net_stats) {
        struct net_stats delta = {0};
        delta.rx_errors = ctx->net_current.rx_errors - ctx->net_start.rx_errors;
        delta.rx_dropped = ctx->net_current.rx_dropped - ctx->net_start.rx_dropped;
        delta.tx_errors = ctx->net_current.tx_errors - ctx->net_start.tx_errors;
        delta.tx_dropped = ctx->net_current.tx_dropped - ctx->net_start.tx_dropped;
        
        char net_buf[256];
        _snprintf(net_buf, sizeof(net_buf),
                  "Interface: %s\n"
                  "RX Errors: %llu  RX Drops: %llu\n"
                  "TX Errors: %llu  TX Drops: %llu",
                  ctx->net_current.iface_name,
                  (unsigned long long)delta.rx_errors,
                  (unsigned long long)delta.rx_dropped,
                  (unsigned long long)delta.tx_errors,
                  (unsigned long long)delta.tx_dropped);
        SetWindowText(ctx->hNetStats, net_buf);
    }
    LeaveCriticalSection(&ctx->net_cs);
}

static void safe_buffer_reset(struct stream2_buffer_ctx* buf) {
    buf->items = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->total_bytes = 0;
    buf->warned_limit = 0;
}

static void autosave_images(struct app_ctx* ctx) {
    if (!InterlockedCompareExchange(&ctx->autosave_enabled, 0, 0)) {
        return; /* Autosave disabled */
    }
    
    if (InterlockedCompareExchange(&ctx->saving, 0, 0)) {
        return; /* Manual save in progress, skip autosave */
    }
    
    /* Get output folder from UI */
    char output_folder[512] = {0};
    GetWindowText(ctx->hOutputFolder, output_folder, sizeof(output_folder));
    tiff_writer_set_output_path(output_folder);
    
    EnterCriticalSection(&ctx->buf_cs);
    size_t current_len = ctx->buf.len;
    LONG64 last_saved = InterlockedCompareExchange64(&ctx->last_autosaved_index, 0, 0);
    size_t start_idx = (size_t)last_saved;
    
    if (start_idx >= current_len) {
        LeaveCriticalSection(&ctx->buf_cs);
        return; /* No new images to save */
    }
    
    /* Save new images (from start_idx to current_len) */
    for (size_t i = start_idx; i < current_len; i++) {
        if (i >= ctx->buf.len) {
            break; /* Buffer changed during save */
        }
        /* Write the image directly from buffer */
        uint64_t cbytes = 0, dbytes = 0;
        tiff_writer_write_one_image(&ctx->buf, i, &cbytes, &dbytes);
    }
    
    /* Update last saved index */
    InterlockedExchange64(&ctx->last_autosaved_index, (LONG64)current_len);
    LeaveCriticalSection(&ctx->buf_cs);
}

static int browse_for_folder(HWND hwnd, char* path, size_t path_size) {
    BROWSEINFO bi = {0};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select Output Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    /* Set initial directory if path is provided */
    if (path && path[0] != '\0') {
        LPITEMIDLIST pidl = ILCreateFromPathA(path);
        if (pidl) {
            bi.pidlRoot = pidl;
        }
    }
    
    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
    
    /* Clean up initial directory if set */
    if (bi.pidlRoot) {
        ILFree((LPITEMIDLIST)bi.pidlRoot);
    }
    
    if (pidl) {
        if (SHGetPathFromIDListA(pidl, path)) {
            ILFree(pidl);
            return 0; /* Success */
        }
        ILFree(pidl);
    }
    return -1; /* User cancelled or error */
}

/* Multi-threaded save with progress reporting */
struct save_write_ctx {
    struct stream2_buffer_ctx* buf;
    volatile LONG64 next;
    volatile LONG64 done;
    HWND hwnd;
    CRITICAL_SECTION stats_cs;
};

static DWORD WINAPI save_writer_thread(LPVOID arg) {
    struct save_write_ctx* ctx = (struct save_write_ctx*)arg;
    for (;;) {
        LONG64 idx64 = InterlockedIncrement64(&ctx->next) - 1;
        size_t idx = (size_t)idx64;
        if (idx >= ctx->buf->len || g_out_of_space)
            break;

        uint64_t cbytes = 0, dbytes = 0;
        tiff_writer_write_one_image(ctx->buf, idx, &cbytes, &dbytes);

        LONG64 done = InterlockedIncrement64(&ctx->done);
        if (IsWindow(ctx->hwnd)) {
            PostMessage(ctx->hwnd, WM_APP_SAVE_PROGRESS, (WPARAM)done, (LPARAM)ctx->buf->len);
        }
        if (g_out_of_space)
            break;
    }
    return 0;
}

static DWORD WINAPI save_thread(LPVOID param) {
    struct app_ctx* ctx = (struct app_ctx*)param;

    /* Get output folder from UI */
    char output_folder[512] = {0};
    GetWindowText(ctx->hOutputFolder, output_folder, sizeof(output_folder));
    tiff_writer_set_output_path(output_folder);

    struct stream2_buffer_ctx snapshot = {0};
    EnterCriticalSection(&ctx->buf_cs);
    snapshot = ctx->buf;
    safe_buffer_reset(&ctx->buf);
    snapshot.bytes_limit = ctx->bytes_limit;
    size_t total_images = snapshot.len;
    LeaveCriticalSection(&ctx->buf_cs);

    if (total_images == 0) {
        InterlockedExchange(&ctx->saving, 0);
        PostMessage(ctx->hwnd, WM_APP_SAVE_DONE, 0, 0);
        return 0;
    }

    InterlockedExchange64(&ctx->save_total, (LONG64)total_images);
    InterlockedExchange64(&ctx->save_done, 0);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threads = (int)si.dwNumberOfProcessors;
    if (threads < 2) threads = 2;
    if (threads > 16) threads = 16;

    /* Use multi-threaded save with progress */
    struct save_write_ctx write_ctx = {
        .buf = &snapshot,
        .next = -1,
        .done = 0,
        .hwnd = ctx->hwnd,
    };
    InitializeCriticalSection(&write_ctx.stats_cs);

    HANDLE* tids = calloc((size_t)threads, sizeof(HANDLE));
    if (!tids || threads <= 1) {
        /* Fallback to single-threaded */
        for (size_t i = 0; i < snapshot.len; i++) {
            uint64_t cbytes = 0, dbytes = 0;
            tiff_writer_write_one_image(&snapshot, i, &cbytes, &dbytes);
            InterlockedExchange64(&ctx->save_done, (LONG64)(i + 1));
            if (IsWindow(ctx->hwnd)) {
                PostMessage(ctx->hwnd, WM_APP_SAVE_PROGRESS, (WPARAM)(i + 1), (LPARAM)total_images);
            }
        }
    } else {
        for (int i = 0; i < threads; i++) {
            tids[i] = CreateThread(NULL, 0, save_writer_thread, &write_ctx, 0, NULL);
            if (!tids[i]) {
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
    DeleteCriticalSection(&write_ctx.stats_cs);

    stream2_buffer_free(&snapshot);

    InterlockedExchange(&ctx->saving, 0);
    InterlockedExchange64(&ctx->save_total, 0);
    InterlockedExchange64(&ctx->save_done, 0);
    PostMessage(ctx->hwnd, WM_APP_SAVE_DONE, 0, 0);
    return 0;
}

struct recv_thread_param {
    struct app_ctx* ctx;
    int thread_id;
};

static DWORD WINAPI recv_thread(LPVOID param) {
    struct recv_thread_param* p = (struct recv_thread_param*)param;
    struct app_ctx* ctx = p->ctx;
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    stream2_install_signal_handler();

    void* socket = zmq_socket(ctx->zmq_ctx, ZMQ_PULL);
    if (!socket) {
        zmq_msg_close(&msg);
        free(p);
        return 0;
    }
    int hwm = 10000;
    zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    int rcvbuf = 16 * 1024 * 1024;
    zmq_setsockopt(socket, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int rcv_timeout_ms = 500;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));

    if (zmq_connect(socket, ctx->address) != 0) {
        fprintf(stderr, "zmq_connect failed (thread %d): %s\n", p->thread_id, zmq_strerror(zmq_errno()));
        zmq_close(socket);
        zmq_msg_close(&msg);
        free(p);
        return 0;
    }

    while (!g_stop && !InterlockedCompareExchange(&ctx->recv_stop, 0, 0)) {
        struct stream2_msg_owner* owner_slot = NULL;
        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            int err = zmq_errno();
            if (err == EAGAIN) continue;
            if (err == EINTR && (g_stop || InterlockedCompareExchange(&ctx->recv_stop, 0, 0))) break;
            if (err == EINTR) continue;
            fprintf(stderr, "zmq_msg_recv error (thread %d): %s\n", p->thread_id, zmq_strerror(err));
            break;
        }

        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);

        struct stream2_msg* m = NULL;
        enum stream2_result r = stream2_parse_msg(msg_data, msg_size, &m);
        if (r) {
            fprintf(stderr, "parse error %d (thread %d)\n", (int)r, p->thread_id);
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
    free(p);
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
        case IDC_BROWSE_FOLDER: {
            char folder_path[MAX_PATH] = {0};
            char current_path[MAX_PATH] = {0};
            GetWindowText(ctx->hOutputFolder, current_path, sizeof(current_path));
            if (current_path[0] != '\0') {
                strncpy(folder_path, current_path, sizeof(folder_path) - 1);
            }
            if (browse_for_folder(hwnd, folder_path, sizeof(folder_path)) == 0) {
                SetWindowText(ctx->hOutputFolder, folder_path);
            }
            break;
        }
        case IDC_AUTOSAVE: {
            LRESULT checked = SendMessage(ctx->hAutosave, BM_GETCHECK, 0, 0);
            InterlockedExchange(&ctx->autosave_enabled, (checked == BST_CHECKED) ? 1 : 0);
            if (checked == BST_CHECKED) {
                /* Reset autosave index when enabling */
                InterlockedExchange64(&ctx->last_autosaved_index, 0);
            }
            break;
        }
        case IDC_START: {
            if (InterlockedCompareExchange(&ctx->receiving, 0, 0)) break; /* already running */
            char ip[128] = {0};
            char port[16] = {0};
            char threads_str[16] = {0};
            GetWindowText(ctx->hIp, ip, sizeof(ip));
            GetWindowText(ctx->hPort, port, sizeof(port));
            GetWindowText(ctx->hThreads, threads_str, sizeof(threads_str));
            if (ip[0] == '\0') lstrcpyA(ip, "127.0.0.1");
            if (port[0] == '\0') lstrcpyA(port, "31001");
            
            /* Parse thread count */
            int num_threads = DEFAULT_RECEIVERS;
            if (threads_str[0] != '\0') {
                char* endp = NULL;
                long t = strtol(threads_str, &endp, 10);
                if (endp && *endp == '\0' && t > 0 && t <= MAX_RECEIVERS) {
                    num_threads = (int)t;
                }
            }
            ctx->num_receivers = num_threads;
            
            _snprintf(ctx->address, sizeof(ctx->address), "tcp://%s:%s", ip, port);
            InterlockedExchange(&ctx->recv_stop, 0);

            /* Get network interface stats */
            EnterCriticalSection(&ctx->net_cs);
            if (get_interface_for_address(ctx->address, ctx->net_start.iface_name, &ctx->net_start.iface_index) == 0) {
                if (read_net_stats(ctx->net_start.iface_index, &ctx->net_start) == 0) {
                    ctx->net_current = ctx->net_start;
                    ctx->have_net_stats = 1;
                }
            }
            LeaveCriticalSection(&ctx->net_cs);

            /* Disable thread count edit while receiving */
            EnableWindow(ctx->hThreads, FALSE);

            /* Spawn multiple receiver threads */
            ctx->num_active_receivers = 0;
            for (int i = 0; i < ctx->num_receivers; i++) {
                struct recv_thread_param* p = (struct recv_thread_param*)malloc(sizeof(struct recv_thread_param));
                if (!p) break;
                p->ctx = ctx;
                p->thread_id = i;
                ctx->hRecvThreads[i] = CreateThread(NULL, 0, recv_thread, p, 0, NULL);
                if (ctx->hRecvThreads[i]) {
                    ctx->num_active_receivers++;
                } else {
                    free(p);
                }
            }

            if (ctx->num_active_receivers == 0) {
                InterlockedExchange(&ctx->receiving, 0);
                EnableWindow(ctx->hThreads, TRUE);
                PostMessage(ctx->hwnd, WM_APP_RECV_STATE, 0, 0);
            } else {
                InterlockedExchange(&ctx->receiving, 1);
                /* Reset autosave index when starting acquisition */
                InterlockedExchange64(&ctx->last_autosaved_index, 0);
                PostMessage(ctx->hwnd, WM_APP_RECV_STATE, 1, 0);
            }
            break;
        }
        case IDC_STOP:
            if (InterlockedCompareExchange(&ctx->receiving, 0, 0)) {
                InterlockedExchange(&ctx->recv_stop, 1);
                for (int i = 0; i < ctx->num_active_receivers; i++) {
                    if (ctx->hRecvThreads[i]) {
                        WaitForSingleObject(ctx->hRecvThreads[i], INFINITE);
                        CloseHandle(ctx->hRecvThreads[i]);
                        ctx->hRecvThreads[i] = NULL;
                    }
                }
                ctx->num_active_receivers = 0;
                InterlockedExchange(&ctx->receiving, 0); /* Reset receiving flag */

                /* Re-enable thread count edit */
                EnableWindow(ctx->hThreads, TRUE);

                /* Read final network stats */
                EnterCriticalSection(&ctx->net_cs);
                if (ctx->have_net_stats) {
                    read_net_stats(ctx->net_start.iface_index, &ctx->net_current);
                }
                LeaveCriticalSection(&ctx->net_cs);

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

            /* Update network stats periodically */
            if (ctx->have_net_stats && InterlockedCompareExchange(&ctx->receiving, 0, 0)) {
                EnterCriticalSection(&ctx->net_cs);
                read_net_stats(ctx->net_start.iface_index, &ctx->net_current);
                LeaveCriticalSection(&ctx->net_cs);
            }
            
            /* Autosave images if enabled */
            if (InterlockedCompareExchange(&ctx->autosave_enabled, 0, 0) &&
                InterlockedCompareExchange(&ctx->receiving, 0, 0)) {
                autosave_images(ctx);
            }
        }
        break;
    case WM_APP_SAVE_DONE:
        MessageBox(hwnd, "Save complete", "Info", MB_OK | MB_ICONINFORMATION);
        if (ctx) {
            SendMessage(ctx->hSaveProgress, PBM_SETPOS, 0, 0);
        }
        break;
    case WM_APP_SAVE_PROGRESS:
        if (ctx) {
            LONG64 done = (LONG64)wParam;
            LONG64 total = (LONG64)lParam;
            if (total > 0) {
                double pct = (100.0 * (double)done / (double)total);
                if (pct > 100.0) pct = 100.0;
                SendMessage(ctx->hSaveProgress, PBM_SETRANGE32, 0, 1000);
                SendMessage(ctx->hSaveProgress, PBM_SETPOS, (WPARAM)(pct * 10.0), 0);
            }
        }
        break;
    case WM_APP_RECV_STATE:
        if (ctx) {
            const char* state = (wParam ? "running" : "stopped");
            char buf[128];
            _snprintf(buf, sizeof(buf), "Receivers: %s (%d threads)", state, ctx->num_active_receivers);
            SetWindowText(ctx->hStatus, buf);
        }
        break;
    case WM_ERASEBKGND:
        /* Let Windows handle background erasing to prevent black background */
        return DefWindowProc(hwnd, msg, wParam, lParam);
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
    InitializeCriticalSection(&ctx.net_cs);
    stream2_stats_init(&ctx.stats);
    ctx.num_active_receivers = 0;
    ctx.num_receivers = DEFAULT_RECEIVERS;
    ctx.autosave_enabled = 0;
    ctx.last_autosaved_index = 0;
    for (int i = 0; i < MAX_RECEIVERS; i++) {
        ctx.hRecvThreads[i] = NULL;
    }

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
    wc.lpszClassName = "DectrisStream2DemoClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    ctx.hwnd = CreateWindowEx(0, wc.lpszClassName, "DectrisStream2Demo_windows",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              520, 440, NULL, NULL, hInst, NULL);
    if (!ctx.hwnd) return EXIT_FAILURE;
    SetWindowLongPtr(ctx.hwnd, GWLP_USERDATA, (LONG_PTR)&ctx);

    ctx.hIp = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "127.0.0.1",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             10, 10, 150, 24, ctx.hwnd, (HMENU)IDC_IP, hInst, NULL);
    ctx.hPort = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "31001",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               170, 10, 60, 24, ctx.hwnd, (HMENU)IDC_PORT, hInst, NULL);
    
    CreateWindowEx(0, "STATIC", "Threads:",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   240, 13, 50, 16, ctx.hwnd, NULL, hInst, NULL);
    ctx.hThreads = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "10",
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                  295, 10, 50, 24, ctx.hwnd, (HMENU)IDC_THREADS, hInst, NULL);

    ctx.hStart = CreateWindowEx(0, "BUTTON", "Start",
                                WS_CHILD | WS_VISIBLE,
                                355, 10, 70, 24, ctx.hwnd, (HMENU)IDC_START, hInst, NULL);
    ctx.hStop = CreateWindowEx(0, "BUTTON", "Stop",
                               WS_CHILD | WS_VISIBLE,
                               435, 10, 70, 24, ctx.hwnd, (HMENU)IDC_STOP, hInst, NULL);

    ctx.hStatus = CreateWindowEx(0, "STATIC", "Receiver: stopped",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 10, 44, 480, 20, ctx.hwnd, (HMENU)IDC_STATUS, hInst, NULL);
    ctx.hStats = CreateWindowEx(0, "STATIC", "",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                10, 66, 480, 60, ctx.hwnd, (HMENU)IDC_STATS, hInst, NULL);
    
    CreateWindowEx(0, "STATIC", "Buffer Progress:",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   10, 130, 200, 16, ctx.hwnd, NULL, hInst, NULL);
    ctx.hProgress = CreateWindowEx(0, PROGRESS_CLASS, NULL,
                                   WS_CHILD | WS_VISIBLE,
                                   10, 150, 480, 20, ctx.hwnd, (HMENU)IDC_PROGRESS, hInst, NULL);
    
    CreateWindowEx(0, "STATIC", "Save Progress:",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   10, 175, 200, 16, ctx.hwnd, NULL, hInst, NULL);
    ctx.hSaveProgress = CreateWindowEx(0, PROGRESS_CLASS, NULL,
                                       WS_CHILD | WS_VISIBLE,
                                       10, 195, 480, 20, ctx.hwnd, (HMENU)IDC_SAVE_PROGRESS, hInst, NULL);
    
    ctx.hNetStats = CreateWindowEx(0, "STATIC", "",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    10, 220, 480, 60, ctx.hwnd, (HMENU)IDC_NET_STATS, hInst, NULL);
    
    CreateWindowEx(0, "STATIC", "Output Folder:",
                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                   10, 288, 100, 16, ctx.hwnd, NULL, hInst, NULL);
    ctx.hBrowseFolder = CreateWindowEx(0, "BUTTON", "Browse...",
                                       WS_CHILD | WS_VISIBLE,
                                       10, 308, 60, 24, ctx.hwnd, (HMENU)IDC_BROWSE_FOLDER, hInst, NULL);
    ctx.hOutputFolder = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                       80, 308, 430, 24, ctx.hwnd, (HMENU)IDC_OUTPUT_FOLDER, hInst, NULL);
    
    ctx.hAutosave = CreateWindowEx(0, "BUTTON", "Autosave",
                                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    10, 340, 120, 20, ctx.hwnd, (HMENU)IDC_AUTOSAVE, hInst, NULL);
    
    ctx.hSave = CreateWindowEx(0, "BUTTON", "Save TIFFs",
                               WS_CHILD | WS_VISIBLE,
                               10, 368, 120, 28, ctx.hwnd, (HMENU)IDC_SAVE, hInst, NULL);
    ctx.hFlush = CreateWindowEx(0, "BUTTON", "Flush Buffer",
                                WS_CHILD | WS_VISIBLE,
                                140, 368, 120, 28, ctx.hwnd, (HMENU)IDC_FLUSH, hInst, NULL);
    ctx.hExit = CreateWindowEx(0, "BUTTON", "Exit",
                               WS_CHILD | WS_VISIBLE,
                               270, 368, 80, 28, ctx.hwnd, (HMENU)IDC_EXIT, hInst, NULL);

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
    for (int i = 0; i < ctx.num_active_receivers; i++) {
        if (ctx.hRecvThreads[i]) {
            WaitForSingleObject(ctx.hRecvThreads[i], INFINITE);
            CloseHandle(ctx.hRecvThreads[i]);
            ctx.hRecvThreads[i] = NULL;
        }
    }

    zmq_ctx_term(ctx.zmq_ctx);

    EnterCriticalSection(&ctx.buf_cs);
    stream2_buffer_free(&ctx.buf);
    LeaveCriticalSection(&ctx.buf_cs);
    DeleteCriticalSection(&ctx.buf_cs);
    DeleteCriticalSection(&ctx.stats_cs);
    DeleteCriticalSection(&ctx.net_cs);
    return 0;
}
