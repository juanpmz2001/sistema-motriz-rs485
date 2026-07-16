#ifndef IBUS_RECEIVER_H
#define IBUS_RECEIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IBUS_RECEIVER_CHANNELS 14
#define IBUS_RECEIVER_RAW_SAMPLE_SIZE 64
#define IBUS_RECEIVER_PPM_EDGE_CAPACITY 96

typedef struct ibus_receiver_t *ibus_receiver_handle_t;

typedef enum {
    IBUS_RECEIVER_MODE_IBUS = 0,
    IBUS_RECEIVER_MODE_IBUS_INVERTED,
    IBUS_RECEIVER_MODE_IBUS_8N2,
    IBUS_RECEIVER_MODE_IBUS_INVERTED_8N2,
    IBUS_RECEIVER_MODE_SBUS,
    IBUS_RECEIVER_MODE_SBUS_NON_INVERTED,
} ibus_receiver_mode_t;

typedef struct {
    uart_port_t uart_port;
    int rx_pin;
    int tx_pin;
    uint32_t baud_rate;
    uint32_t stale_timeout_ms;
    bool invert_rx;
    ibus_receiver_mode_t mode;
} ibus_receiver_config_t;

typedef struct {
    bool signal_valid;
    ibus_receiver_mode_t mode;
    uart_port_t uart_port;
    int rx_pin;
    uint32_t baud_rate;
    uint32_t stale_timeout_ms;
    uint32_t last_frame_age_ms;
    uint32_t bytes_received;
    uint32_t frames_seen;
    uint32_t valid_frames;
    uint32_t bad_header_frames;
    uint32_t bad_checksum_frames;
    uint16_t channels[IBUS_RECEIVER_CHANNELS];
    uint8_t raw_sample[IBUS_RECEIVER_RAW_SAMPLE_SIZE];
    uint8_t raw_sample_count;
} ibus_receiver_status_t;

typedef struct {
    int rx_pin;
    uint32_t samples;
    uint32_t high_count;
    uint32_t low_count;
    uint32_t transitions;
    int last_level;
} ibus_receiver_pin_sample_t;

typedef struct {
    uint32_t time_us;
    uint32_t duration_since_previous_us;
    int level;
} ibus_receiver_pin_edge_t;

typedef struct {
    int rx_pin;
    uint32_t requested_duration_us;
    uint32_t elapsed_us;
    uint32_t interval_us;
    uint32_t samples;
    uint32_t high_count;
    uint32_t low_count;
    uint32_t transitions;
    uint32_t rising_edges;
    uint32_t falling_edges;
    int initial_level;
    int last_level;
    uint8_t edge_count;
    bool edge_overflow;
    ibus_receiver_pin_edge_t edges[IBUS_RECEIVER_PPM_EDGE_CAPACITY];
} ibus_receiver_ppm_capture_t;

esp_err_t ibus_receiver_init(const ibus_receiver_config_t *config, ibus_receiver_handle_t *out_handle);
void ibus_receiver_deinit(ibus_receiver_handle_t handle);
esp_err_t ibus_receiver_set_mode(ibus_receiver_handle_t handle, ibus_receiver_mode_t mode);
esp_err_t ibus_receiver_get_status(ibus_receiver_handle_t handle, ibus_receiver_status_t *status);
esp_err_t ibus_receiver_sample_pin(ibus_receiver_handle_t handle,
                                   uint32_t sample_count,
                                   uint32_t interval_us,
                                   ibus_receiver_pin_sample_t *sample);
esp_err_t ibus_receiver_capture_ppm(ibus_receiver_handle_t handle,
                                    uint32_t duration_us,
                                    uint32_t interval_us,
                                    ibus_receiver_ppm_capture_t *capture);
const char *ibus_receiver_mode_to_string(ibus_receiver_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif // IBUS_RECEIVER_H
