/*
 * eiger_client.h - Simple C library for EIGER detector REST API
 *
 * Covers the functionalities used by start_stream_eigerclient:
 * - GET status (state, temperature, humidity, high_voltage, config, etc.)
 * - PUT detector commands (disarm, initialize, arm, trigger)
 * - PUT detector / stream / monitor / filewriter config
 *
 * API base paths follow the session format (e.g. /detector/api/1.8.0/...).
 */

#ifndef EIGER_CLIENT_H
#define EIGER_CLIENT_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EIGER_CLIENT_RESPONSE_MAX 4096

/*
 * Session API segment in URLs (e.g. 1.8.0). Default "1.8.0" (matches
 * start_stream.json). Override with environment variable EIGER_API_VERSION
 * (e.g. 1.6.0) if the DCU reports a different version.
 */
const char *eiger_client_api_version(void);

/*
 * When non-NULL, every eiger_http_request logs the exact method, URL
 * (http://host:port + path), and PUT body bytes that are sent (same as wire).
 * Set to NULL to disable (default).
 */
void eiger_set_http_trace(FILE *fp);

/*
 * Perform HTTP request. Returns 0 on success, -1 on failure.
 * For GET, data may be NULL. For PUT, data is JSON body (e.g. "{\"value\": 1}").
 * If response buffer is non-NULL and response_size > 0, body is written there (null-terminated).
 */
int eiger_http_request(const char *host, int port,
                       const char *method, const char *path,
                       const char *data,
                       char *response, size_t response_size);

/*
 * GET /detector/api/1.8.0/status/{path}
 * e.g. path = "state", "temperature", "humidity", "high_voltage/state"
 */
int eiger_get_status(const char *host, int port, const char *path,
                     char *response, size_t response_size);

/*
 * PUT /detector/api/1.8.0/command/{command}
 * e.g. command = "disarm", "initialize", "arm", "trigger"
 */
int eiger_send_command(const char *host, int port, const char *command);

/*
 * PUT /detector/api/1.8.0/config/{param} with body {"value": value_json}
 * value_json: string that becomes the JSON value (number, string, bool).
 * For string use e.g. "\"enabled\"" or "\"normal\""; for number "45000" or "0.001".
 */
int eiger_set_detector_config(const char *host, int port,
                              const char *param, const char *value_json);

/*
 * PUT /stream/api/1.8.0/config/{param}
 */
int eiger_set_stream_config(const char *host, int port,
                            const char *param, const char *value_json);

/*
 * PUT /monitor/api/1.8.0/config/{param}
 */
int eiger_set_monitor_config(const char *host, int port,
                             const char *param, const char *value_json);

/*
 * PUT /filewriter/api/1.8.0/config/{param}
 */
int eiger_set_filewriter_config(const char *host, int port,
                                const char *param, const char *value_json);

#ifdef __cplusplus
}
#endif

#endif /* EIGER_CLIENT_H */
