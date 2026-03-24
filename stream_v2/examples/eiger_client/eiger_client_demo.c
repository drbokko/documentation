/*
 * eiger_client_demo.c - Demo using libeiger_client for the start_stream flow
 *
 * Same sequence as start_stream_eigerclient: disarm, (optional init),
 * status checks, configure detector/stream/monitor/filewriter, arm, trigger, disarm.
 */

#include "eiger_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void configure_detector(const char *host, int port, int threshold,
                               int nimages, double exposure_time, double sleep_time,
                               const char *monitor, const char *filewriter, const char *stream) {
    char buf[64];

    printf("Configuring detector...\n");
    eiger_set_detector_config(host, port, "countrate_correction_applied", "false");
    eiger_set_detector_config(host, port, "retrigger", "false");
    eiger_set_detector_config(host, port, "counting_mode", "\"normal\"");
    eiger_set_detector_config(host, port, "virtual_pixel_correction_applied", "true");
    eiger_set_detector_config(host, port, "mask_to_zero", "true");
    eiger_set_detector_config(host, port, "test_image_mode", "\"\"");
    eiger_set_detector_config(host, port, "flatfield_correction_applied", "false");

    printf("Setting thresholds and acquisition...\n");
    snprintf(buf, sizeof(buf), "%d", threshold);
    eiger_set_detector_config(host, port, "threshold/1/mode", "\"enabled\"");
    eiger_set_detector_config(host, port, "threshold/1/energy", buf);
    eiger_set_detector_config(host, port, "threshold/2/mode", "\"disabled\"");

    snprintf(buf, sizeof(buf), "%.6f", exposure_time);
    eiger_set_detector_config(host, port, "count_time", buf);
    snprintf(buf, sizeof(buf), "%.6f", exposure_time + sleep_time);
    eiger_set_detector_config(host, port, "frame_time", buf);
    snprintf(buf, sizeof(buf), "%d", nimages);
    eiger_set_detector_config(host, port, "nimages", buf);
    eiger_set_detector_config(host, port, "auto_summation", "false");

    printf("Configuring interfaces...\n");
    eiger_set_monitor_config(host, port, "mode", monitor);
    eiger_set_filewriter_config(host, port, "mode", filewriter);
    eiger_set_stream_config(host, port, "mode", stream);
    eiger_set_stream_config(host, port, "format", "\"cbor\"");
    eiger_set_stream_config(host, port, "header_detail", "\"all\"");
    printf("Configuration complete.\n");
}

int main(int argc, char **argv) {
    const char *host = "172.31.1.1";
    int port = 80;
    int threshold = 45000;
    int nimages = 6000;
    double exposure_time = 0.001;
    double sleep_time = 0.0;
    int force_init = 0;
    const char *monitor = "disabled";
    const char *filewriter = "disabled";
    const char *stream = "enabled";

    char response[EIGER_CLIENT_RESPONSE_MAX];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nimages") == 0 && i + 1 < argc) {
            nimages = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            exposure_time = atof(argv[++i]);
        } else if (strcmp(argv[i], "--sleep") == 0 && i + 1 < argc) {
            sleep_time = atof(argv[++i]);
        } else if (strcmp(argv[i], "--force-init") == 0) {
            force_init = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--ip IP] [--threshold EV] [--nimages N] [--exposure SEC] [--sleep SEC] [--force-init]\n", argv[0]);
            return 0;
        }
    }

    printf("EIGER client demo (libeiger_client)\n");
    printf("Host: %s:%d  threshold=%d eV  nimages=%d  exposure=%.6f s\n\n", host, port, threshold, nimages, exposure_time);

    printf("Disarming...\n");
    if (eiger_send_command(host, port, "disarm") != 0) {
        fprintf(stderr, "Warning: disarm failed\n");
    }

    {
        int st = eiger_get_status(host, port, "state", response, sizeof(response));
        if (st == 0 && response[0])
            printf("State: %s\n", response);
        int idle_or_ready =
                (st == 0 && response[0] &&
                 (strstr(response, "\"idle\"") != NULL || strstr(response, "\"ready\"") != NULL));

        if (force_init || !idle_or_ready) {
            if (force_init)
                printf("Initializing (--force-init)...\n");
            else if (st != 0 || !response[0])
                printf("State query failed or empty; initializing...\n");
            else
                printf("State not idle/ready; initializing...\n");
            if (eiger_send_command(host, port, "initialize") != 0) {
                fprintf(stderr, "Error: initialize failed\n");
                return 1;
            }
        }
    }
    if (eiger_get_status(host, port, "high_voltage/state", response, sizeof(response)) == 0 && response[0])
        printf("HV: %s\n", response);
    if (eiger_get_status(host, port, "temperature", response, sizeof(response)) == 0 && response[0])
        printf("Temperature: %s\n", response);
    if (eiger_get_status(host, port, "humidity", response, sizeof(response)) == 0 && response[0])
        printf("Humidity: %s\n", response);

    configure_detector(host, port, threshold, nimages, exposure_time, sleep_time, monitor, filewriter, stream);

    printf("\nArming...\n");
    if (eiger_send_command(host, port, "arm") != 0) {
        fprintf(stderr, "Error: arm failed\n");
        return 1;
    }
    printf("Triggering...\n");
    if (eiger_send_command(host, port, "trigger") != 0) {
        fprintf(stderr, "Error: trigger failed\n");
        eiger_send_command(host, port, "disarm");
        return 1;
    }
    printf("Disarming...\n");
    if (eiger_send_command(host, port, "disarm") != 0) {
        fprintf(stderr, "Warning: disarm failed\n");
    } else {
        printf("Done.\n");
    }
    return 0;
}
