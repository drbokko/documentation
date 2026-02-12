/*
 * start_stream_eigerclient.c
 *
 * C version of start_stream_eigerclient.py
 * Configures EIGER detector for streaming data acquisition
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <unistd.h>
/* Linux version would need libcurl or similar HTTP library */
#endif

struct eiger_config {
    char ip[128];
    int threshold;
    int nimages;
    double exposure_time;
    double sleep_time;
    int force_initialization;
    char monitor[32];
    char filewriter[32];
    char stream[32];
};

static int eiger_http_request(const char* host, int port, const char* method,
                              const char* path, const char* data, char* response, size_t response_size) {
#ifdef _WIN32
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    int result = -1;
    DWORD dwStatusCode = 0;
    DWORD dwStatusCodeSize = sizeof(dwStatusCode);
    
    hSession = WinHttpOpen(L"EigerClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        fprintf(stderr, "Error: WinHttpOpen failed\n");
        return -1;
    }
    
    WCHAR wHost[256];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wHost, sizeof(wHost)/sizeof(WCHAR));
    
    hConnect = WinHttpConnect(hSession, wHost, port, 0);
    if (!hConnect) {
        fprintf(stderr, "Error: WinHttpConnect failed for %s:%d\n", host, port);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    
    WCHAR wPath[512];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, sizeof(wPath)/sizeof(WCHAR));
    
    hRequest = WinHttpOpenRequest(hConnect,
                                 (strcmp(method, "GET") == 0) ? L"GET" : L"PUT",
                                 wPath, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        fprintf(stderr, "Error: WinHttpOpenRequest failed for %s\n", path);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    
    BOOL bResults = FALSE;
    if (data && strlen(data) > 0) {
        WCHAR wContentType[] = L"Content-Type: application/json\r\n";
        WinHttpAddRequestHeaders(hRequest, wContentType, -1, WINHTTP_ADDREQ_FLAG_ADD);
        
        WCHAR wData[2048];
        MultiByteToWideChar(CP_UTF8, 0, data, -1, wData, sizeof(wData)/sizeof(WCHAR));
        bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     wData, wcslen(wData) * sizeof(WCHAR),
                                     wcslen(wData) * sizeof(WCHAR), 0);
    } else {
        /* For PUT requests without data, send empty JSON object */
        if (strcmp(method, "PUT") == 0) {
            WCHAR wContentType[] = L"Content-Type: application/json\r\n";
            WinHttpAddRequestHeaders(hRequest, wContentType, -1, WINHTTP_ADDREQ_FLAG_ADD);
            const char* empty_json = "{}";
            WCHAR wData[16];
            MultiByteToWideChar(CP_UTF8, 0, empty_json, -1, wData, sizeof(wData)/sizeof(WCHAR));
            bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         wData, wcslen(wData) * sizeof(WCHAR),
                                         wcslen(wData) * sizeof(WCHAR), 0);
        } else {
            bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        }
    }
    
    if (!bResults) {
        DWORD dwError = GetLastError();
        fprintf(stderr, "Error: WinHttpSendRequest failed for %s %s (error: %lu)\n", method, path, dwError);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    
    bResults = WinHttpReceiveResponse(hRequest, NULL);
    if (!bResults) {
        DWORD dwError = GetLastError();
        fprintf(stderr, "Error: WinHttpReceiveResponse failed for %s %s (error: %lu)\n", method, path, dwError);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    
    /* Check HTTP status code */
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwStatusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        fprintf(stderr, "Error: Failed to get HTTP status code for %s %s\n", method, path);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    
    if (dwStatusCode < 200 || dwStatusCode >= 300) {
        fprintf(stderr, "Error: HTTP %lu for %s %s\n", dwStatusCode, method, path);
        /* Read error response if available */
        if (response && response_size > 0) {
            DWORD dwSize = 0;
            DWORD dwDownloaded = 0;
            response[0] = '\0';
            do {
                dwSize = 0;
                if (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                    char* pszOutBuffer = (char*)malloc(dwSize + 1);
                    if (pszOutBuffer) {
                        ZeroMemory(pszOutBuffer, dwSize + 1);
                        if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
                            if (strlen(response) + dwDownloaded < response_size - 1) {
                                strncat(response, pszOutBuffer, dwDownloaded);
                            }
                        }
                        free(pszOutBuffer);
                    }
                }
            } while (dwSize > 0);
            if (strlen(response) > 0) {
                fprintf(stderr, "Response: %s\n", response);
            }
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }
    
    /* Read response body if requested */
    if (response && response_size > 0) {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        response[0] = '\0';
        
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
                break;
            }
            
            if (dwSize == 0) break;
            
            char* pszOutBuffer = (char*)malloc(dwSize + 1);
            if (!pszOutBuffer) break;
            
            ZeroMemory(pszOutBuffer, dwSize + 1);
            
            if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
                if (strlen(response) + dwDownloaded < response_size - 1) {
                    strncat(response, pszOutBuffer, dwDownloaded);
                }
            }
            
            free(pszOutBuffer);
        } while (dwSize > 0);
    }
    
    result = 0;
    
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    
    return result;
#else
    /* Linux version using libcurl - simplified for now */
    /* Note: This requires libcurl to be installed */
    fprintf(stderr, "Linux version not yet implemented. Please use Windows or implement libcurl support.\n");
    return -1;
#endif
}

static int eiger_set_config(const char* host, int port, const char* param, const char* value) {
    char path[256];
    char json_data[256];
    
    snprintf(path, sizeof(path), "/detector/api/v1.0/config/%s", param);
    
    if (value[0] == '"' || strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        snprintf(json_data, sizeof(json_data), "%s", value);
    } else {
        snprintf(json_data, sizeof(json_data), "%s", value);
    }
    
    return eiger_http_request(host, port, "PUT", path, json_data, NULL, 0);
}

static int eiger_set_stream_config(const char* host, int port, const char* param, const char* value) {
    char path[256];
    char json_data[256];
    
    snprintf(path, sizeof(path), "/stream/api/v1.0/config/%s", param);
    
    if (value[0] == '"' || strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        snprintf(json_data, sizeof(json_data), "%s", value);
    } else {
        snprintf(json_data, sizeof(json_data), "%s", value);
    }
    
    return eiger_http_request(host, port, "PUT", path, json_data, NULL, 0);
}

static int eiger_set_monitor_config(const char* host, int port, const char* param, const char* value) {
    char path[256];
    char json_data[256];
    
    snprintf(path, sizeof(path), "/monitor/api/v1.0/config/%s", param);
    snprintf(json_data, sizeof(json_data), "\"%s\"", value);
    
    return eiger_http_request(host, port, "PUT", path, json_data, NULL, 0);
}

static int eiger_set_filewriter_config(const char* host, int port, const char* param, const char* value) {
    char path[256];
    char json_data[256];
    
    snprintf(path, sizeof(path), "/filewriter/api/v1.0/config/%s", param);
    
    if (value[0] == '"' || strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
        snprintf(json_data, sizeof(json_data), "%s", value);
    } else {
        snprintf(json_data, sizeof(json_data), "\"%s\"", value);
    }
    
    return eiger_http_request(host, port, "PUT", path, json_data, NULL, 0);
}

static int eiger_get_status(const char* host, int port, const char* param, char* response, size_t response_size) {
    char path[256];
    snprintf(path, sizeof(path), "/detector/api/v1.0/status/%s", param);
    return eiger_http_request(host, port, "GET", path, NULL, response, response_size);
}

static int eiger_send_command(const char* host, int port, const char* command) {
    char path[256];
    char response[1024] = {0};
    snprintf(path, sizeof(path), "/detector/api/v1.0/command/%s", command);
    int result = eiger_http_request(host, port, "PUT", path, NULL, response, sizeof(response));
    if (result == 0 && strlen(response) > 0) {
        printf("Command response: %s\n", response);
    }
    return result;
}

static void configure_detector(const char* host, int port, const struct eiger_config* config) {
    printf("Configuring detector settings...\n");
    
    eiger_set_config(host, port, "countrate_correction_applied", "false");
    eiger_set_config(host, port, "retrigger", "false");
    eiger_set_config(host, port, "counting_mode", "\"normal\"");
    eiger_set_config(host, port, "virtual_pixel_correction_applied", "true");
    eiger_set_config(host, port, "mask_to_zero", "true");
    eiger_set_config(host, port, "test_image_mode", "\"\"");
    eiger_set_config(host, port, "flatfield_correction_applied", "false");
    
    printf("Setting thresholds...\n");
    char threshold_str[32];
    snprintf(threshold_str, sizeof(threshold_str), "%d", config->threshold);
    eiger_set_config(host, port, "threshold/1/mode", "\"enabled\"");
    eiger_set_config(host, port, "threshold/1/energy", threshold_str);
    eiger_set_config(host, port, "threshold/2/mode", "\"disabled\"");
    
    printf("Setting acquisition parameters...\n");
    char exp_str[64];
    snprintf(exp_str, sizeof(exp_str), "%.6f", config->exposure_time);
    eiger_set_config(host, port, "count_time", exp_str);
    
    double frame_time = config->exposure_time + config->sleep_time;
    snprintf(exp_str, sizeof(exp_str), "%.6f", frame_time);
    eiger_set_config(host, port, "frame_time", exp_str);
    
    char nimg_str[32];
    snprintf(nimg_str, sizeof(nimg_str), "%d", config->nimages);
    eiger_set_config(host, port, "nimages", nimg_str);
    eiger_set_config(host, port, "auto_summation", "false");
    
    printf("Configuring data acquisition interfaces...\n");
    eiger_set_monitor_config(host, port, "mode", config->monitor);
    eiger_set_filewriter_config(host, port, "mode", config->filewriter);
    
    if (strcmp(config->filewriter, "enabled") == 0) {
        eiger_set_filewriter_config(host, port, "compression_enabled", "false");
        eiger_set_filewriter_config(host, port, "format", "hdf5 nexus v2024.2 nxmx");
    }
    
    eiger_set_stream_config(host, port, "mode", config->stream);
    eiger_set_stream_config(host, port, "format", "\"cbor\"");
    eiger_set_stream_config(host, port, "header_detail", "\"all\"");
    
    printf("Configuration complete.\n");
}

int main(int argc, char** argv) {
    struct eiger_config config = {
        .ip = "172.31.1.1",
        .threshold = 15000,
        .nimages = 2000,
        .exposure_time = 0.002,
        .sleep_time = 0.0,
        .force_initialization = 0,
        .monitor = "disabled",
        .filewriter = "disabled",
        .stream = "enabled"
    };
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            strncpy(config.ip, argv[++i], sizeof(config.ip) - 1);
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            config.threshold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nimages") == 0 && i + 1 < argc) {
            config.nimages = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            config.exposure_time = atof(argv[++i]);
        } else if (strcmp(argv[i], "--sleep") == 0 && i + 1 < argc) {
            config.sleep_time = atof(argv[++i]);
        } else if (strcmp(argv[i], "--force-init") == 0) {
            config.force_initialization = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --ip IP              Detector IP address (default: 172.31.1.1)\n");
            printf("  --threshold EV       Energy threshold in eV (default: 15000)\n");
            printf("  --nimages N          Number of images (default: 2000)\n");
            printf("  --exposure SEC       Exposure time in seconds (default: 0.002)\n");
            printf("  --sleep SEC          Sleep time between frames (default: 0.0)\n");
            printf("  --force-init         Force detector initialization\n");
            printf("  --help               Show this help message\n");
            return 0;
        }
    }
    
    printf("EIGER Detector Stream Acquisition\n");
    printf("==================================\n");
    printf("IP: %s\n", config.ip);
    printf("Threshold: %d eV\n", config.threshold);
    printf("Number of images: %d\n", config.nimages);
    printf("Exposure time: %.6f s\n", config.exposure_time);
    printf("Sleep time: %.6f s\n", config.sleep_time);
    printf("\n");
    
    /* Connect to detector */
    printf("Connecting to detector at %s...\n", config.ip);
    
    char status_response[1024] = {0};
    if (eiger_get_status(config.ip, 80, "state", status_response, sizeof(status_response)) == 0) {
        printf("Detector status retrieved\n");
    }
    
    /* Disarm detector */
    printf("Disarming detector...\n");
    eiger_send_command(config.ip, 80, "disarm");
    printf("Detector disarmed\n");
    
    /* Check if initialization is needed */
    char state_response[1024] = {0};
    int needs_init = config.force_initialization;
    if (!needs_init && eiger_get_status(config.ip, 80, "state", state_response, sizeof(state_response)) == 0) {
        if (strstr(state_response, "\"idle\"") == NULL) {
            needs_init = 1;
        }
    }
    
    if (needs_init) {
        printf("Initializing detector...\n");
        eiger_send_command(config.ip, 80, "initialize");
        printf("Detector initialized\n");
    }
    
    /* Get detector status */
    char hv_response[1024] = {0};
    char temp_response[1024] = {0};
    char hum_response[1024] = {0};
    
    if (eiger_get_status(config.ip, 80, "high_voltage/state", hv_response, sizeof(hv_response)) == 0) {
        printf("High voltage status retrieved\n");
    }
    if (eiger_get_status(config.ip, 80, "temperature", temp_response, sizeof(temp_response)) == 0) {
        printf("Temperature retrieved\n");
    }
    if (eiger_get_status(config.ip, 80, "humidity", hum_response, sizeof(hum_response)) == 0) {
        printf("Humidity retrieved\n");
    }
    
    /* Configure detector */
    configure_detector(config.ip, 80, &config);
    
    /* Run acquisition */
    printf("\nStarting data acquisition...\n");
    printf("Arming detector...\n");
    if (eiger_send_command(config.ip, 80, "arm") != 0) {
        fprintf(stderr, "Error: Failed to arm detector\n");
        return 1;
    }
    printf("Detector armed\n");
    
    /* Small delay to ensure detector is ready */
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100000);
#endif
    
    printf("Triggering acquisition...\n");
    if (eiger_send_command(config.ip, 80, "trigger") != 0) {
        fprintf(stderr, "Error: Failed to trigger acquisition\n");
        eiger_send_command(config.ip, 80, "disarm");
        return 1;
    }
    printf("Acquisition triggered\n");
    
    /* Wait a moment before disarming to allow acquisition to start */
#ifdef _WIN32
    Sleep(500);
#else
    usleep(500000);
#endif
    
    printf("Disarming detector...\n");
    if (eiger_send_command(config.ip, 80, "disarm") != 0) {
        fprintf(stderr, "Warning: Failed to disarm detector\n");
    } else {
        printf("Detector disarmed - acquisition complete\n");
    }
    
    return 0;
}
