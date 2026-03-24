# eiger_client – EIGER detector REST API C library

Small C library that implements the REST API operations used by the **start_stream** script (Python `start_stream_eigerclient.py` / C `start_stream_eigerclient.c` and the session recorded in `start_stream.json`).

## Functionality

- **Status**: `eiger_get_status(host, port, path, response_buf, size)` – GET `/detector/api/1.8.0/status/{path}` (e.g. `state`, `temperature`, `humidity`, `high_voltage/state`).
- **Commands**: `eiger_send_command(host, port, command)` – PUT `/detector/api/1.8.0/command/{command}` (`disarm`, `initialize`, `arm`, `trigger`).
- **Detector config**: `eiger_set_detector_config(host, port, param, value_json)` – PUT config with `{"value": value_json}`.
- **Stream / monitor / filewriter config**: `eiger_set_stream_config`, `eiger_set_monitor_config`, `eiger_set_filewriter_config` – same pattern for `/stream/`, `/monitor/`, `/filewriter/` APIs.

The session segment in URLs defaults to **1.8.0** (same as `start_stream.json`). If your DCU uses another version (e.g. **1.8.0**), set the environment variable **`EIGER_API_VERSION`** before running the client.

On **Windows**, JSON request bodies are sent as **UTF-8** bytes (not UTF-16); older builds that converted the body to wide characters caused PUT commands to fail against the detector.

Failed requests print a line to **stderr** with the HTTP status code (or Win32 error) and the path.

Call **eiger_set_http_trace(stdout)** (or any FILE*) so **eiger_http_request** logs each outgoing line: HTTP method, full URL http://host:port + path, and for PUT the **exact body bytes** sent (same buffer as WinHTTP/curl). Pass **NULL** to disable (default).

## Build

The library is built as part of the stream_v2 examples CMake build:

- **Windows**: uses WinHTTP (no extra dependency).
- **Linux/macOS**: if `find_package(CURL)` finds libcurl, the library is built with HTTP support; otherwise the library still builds but HTTP calls return failure.

The demo program `eiger_client_demo` is only added when HTTP is available (Windows or CURL found).

## Usage example

```c
#include "eiger_client.h"

char response[EIGER_CLIENT_RESPONSE_MAX];
const char *host = "172.31.1.1";
int port = 80;

eiger_send_command(host, port, "disarm");
eiger_get_status(host, port, "state", response, sizeof(response));
eiger_set_detector_config(host, port, "counting_mode", "\"normal\"");
eiger_set_stream_config(host, port, "mode", "\"enabled\"");
eiger_send_command(host, port, "arm");
eiger_send_command(host, port, "trigger");
eiger_send_command(host, port, "disarm");
```

For a full flow (disarm, optional init, status, configure, arm, trigger, disarm) see `eiger_client_demo.c`.
