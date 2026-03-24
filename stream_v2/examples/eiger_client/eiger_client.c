/*
 * eiger_client.c - EIGER detector REST API client implementation
 */

#include "eiger_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#if !defined(_WIN32) && defined(USE_LIBCURL) && (USE_LIBCURL)
#include <curl/curl.h>

struct eiger_curl_mem {
    char *memory;
    size_t size;
    size_t capacity;
};

static size_t eiger_curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct eiger_curl_mem *mem = (struct eiger_curl_mem *)userp;
    if (mem->size + realsize > mem->capacity)
        realsize = mem->capacity - mem->size;
    if (realsize) {
        memcpy(mem->memory + mem->size, contents, realsize);
        mem->size += realsize;
        mem->memory[mem->size] = '\0';
    }
    return size * nmemb;
}
#endif

const char *eiger_client_api_version(void) {
    const char *e = getenv("EIGER_API_VERSION");
    return (e && e[0]) ? e : "1.8.0";
}

static FILE *g_eiger_http_trace;

void eiger_set_http_trace(FILE *fp) {
    g_eiger_http_trace = fp;
}

static int build_value_body(char *buf, size_t buf_size, const char *value_json) {
    int n = snprintf(buf, buf_size, "{\"value\": %s}", value_json);
    return (n >= 0 && (size_t)n < buf_size) ? 0 : -1;
}

int eiger_http_request(const char *host, int port,
                       const char *method, const char *path,
                       const char *data,
                       char *response, size_t response_size) {
    const char *send_body = NULL;
    size_t send_len = 0;

    if (strcmp(method, "PUT") == 0) {
        if (data && data[0] != '\0') {
            send_body = data;
            send_len = strlen(data);
        } else {
            send_body = "{}";
            send_len = 2;
        }
    }

    if (g_eiger_http_trace) {
        fprintf(g_eiger_http_trace, "[eiger] %s http://%s:%d%s", method, host, port, path);
        if (strcmp(method, "PUT") == 0)
            fprintf(g_eiger_http_trace, "  body: %.*s", (int)send_len, send_body);
        fprintf(g_eiger_http_trace, "\n");
        fflush(g_eiger_http_trace);
    }

#ifdef _WIN32
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    int result = -1;
    DWORD dwStatusCode = 0;
    DWORD dwStatusCodeSize = sizeof(dwStatusCode);

    hSession = WinHttpOpen(L"EigerClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    WCHAR wHost[256];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wHost, (int)(sizeof(wHost) / sizeof(WCHAR)));

    hConnect = WinHttpConnect(hSession, wHost, (INTERNET_PORT)port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return -1;
    }

    WCHAR wPath[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, (int)(sizeof(wPath) / sizeof(WCHAR)));

    hRequest = WinHttpOpenRequest(hConnect,
                                  (strcmp(method, "GET") == 0) ? L"GET" : L"PUT",
                                  wPath, NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    {
        if (strcmp(method, "PUT") == 0 && send_body && send_len > 0) {
            /*
             * Entity body must be UTF-8 JSON bytes. Do NOT send UTF-16 (WCHAR):
             * converting the body with MultiByteToWideChar and passing those
             * bytes breaks the API (detector expects ASCII/UTF-8 JSON).
             */
            WCHAR wContentType[] = L"Content-Type: application/json; charset=utf-8\r\n";
            WinHttpAddRequestHeaders(hRequest, wContentType, -1, WINHTTP_ADDREQ_FLAG_ADD);
            if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   (LPVOID)send_body, (DWORD)send_len, (DWORD)send_len, 0)) {
                fprintf(stderr, "eiger_client: WinHttpSendRequest failed %s %s (err=%lu)\n",
                        method, path, (unsigned long)GetLastError());
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return -1;
            }
        } else if (strcmp(method, "GET") == 0) {
            if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return -1;
            }
        }
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwStatusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    if (dwStatusCode < 200 || dwStatusCode >= 300) {
        fprintf(stderr,
                "eiger_client: HTTP %lu %s %s (check EIGER_API_VERSION, default is 1.8.0)\n",
                (unsigned long)dwStatusCode, method, path);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    if (response && response_size > 0) {
        response[0] = '\0';
        size_t offset = 0;
        DWORD dwSize, dwDownloaded;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
            if (offset + (size_t)dwSize >= response_size) dwSize = (DWORD)(response_size - offset - 1);
            if (dwSize == 0) break;
            if (!WinHttpReadData(hRequest, response + offset, dwSize, &dwDownloaded)) break;
            offset += (size_t)dwDownloaded;
            response[offset] = '\0';
        } while (dwSize > 0);
    }

    result = 0;
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;

#else
#if defined(USE_LIBCURL) && (USE_LIBCURL)
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char url[1024];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, send_body);
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json; charset=utf-8");
    if (strcmp(method, "PUT") == 0)
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (response && response_size > 0) {
        response[0] = '\0';
        struct eiger_curl_mem chunk = {
            .memory = response,
            .size = 0,
            .capacity = response_size - 1
        };
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, eiger_curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    }

    CURLcode res = curl_easy_perform(curl);
    if (headers) curl_slist_free_all(headers);
    long code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || code < 200 || code >= 300) {
        fprintf(stderr,
                "eiger_client: HTTP %ld %s %s (curl=%d, check EIGER_API_VERSION)\n",
                code, method, path, (int)res);
        return -1;
    }
    return 0;
#else
    (void)host;
    (void)port;
    (void)method;
    (void)path;
    (void)data;
    (void)send_body;
    (void)send_len;
    (void)response;
    (void)response_size;
    fprintf(stderr, "eiger_client: HTTP not implemented on this platform (use Windows or build with USE_LIBCURL and link libcurl)\n");
    return -1;
#endif
#endif
}

int eiger_get_status(const char *host, int port, const char *path,
                     char *response, size_t response_size) {
    char api_path[512];
    snprintf(api_path, sizeof(api_path), "/detector/api/%s/status/%s",
             eiger_client_api_version(), path);
    return eiger_http_request(host, port, "GET", api_path, NULL, response, response_size);
}

int eiger_send_command(const char *host, int port, const char *command) {
    char api_path[512];
    snprintf(api_path, sizeof(api_path), "/detector/api/%s/command/%s",
             eiger_client_api_version(), command);
    return eiger_http_request(host, port, "PUT", api_path, "{}", NULL, 0);
}

static int eiger_set_config(const char *host, int port, const char *api_prefix,
                            const char *param, const char *body) {
    char api_path[512];
    snprintf(api_path, sizeof(api_path), "/%s/api/%s/config/%s", api_prefix,
             eiger_client_api_version(), param);
    return eiger_http_request(host, port, "PUT", api_path, body, NULL, 0);
}

int eiger_set_detector_config(const char *host, int port,
                             const char *param, const char *value_json) {
    char body[512];
    if (build_value_body(body, sizeof(body), value_json) != 0) return -1;
    return eiger_set_config(host, port, "detector", param, body);
}

int eiger_set_stream_config(const char *host, int port,
                            const char *param, const char *value_json) {
    char body[512];
    if (build_value_body(body, sizeof(body), value_json) != 0) return -1;
    return eiger_set_config(host, port, "stream", param, body);
}

int eiger_set_monitor_config(const char *host, int port,
                             const char *param, const char *value) {
    char body[256];
    snprintf(body, sizeof(body), "{\"value\": \"%s\"}", value);
    return eiger_set_config(host, port, "monitor", param, body);
}

int eiger_set_filewriter_config(const char *host, int port,
                                const char *param, const char *value_json) {
    char body[512];
    if (build_value_body(body, sizeof(body), value_json) != 0) return -1;
    return eiger_set_config(host, port, "filewriter", param, body);
}
