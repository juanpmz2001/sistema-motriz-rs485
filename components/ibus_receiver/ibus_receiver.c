#include "ibus_receiver.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ibus_receiver";

#define IBUS_DEFAULT_BAUD_RATE 115200
#define IBUS_DEFAULT_STALE_TIMEOUT_MS 100
#define IBUS_UART_RX_BUFFER_SIZE 512
#define IBUS_FRAME_LENGTH 32
#define IBUS_FRAME_HEADER_LENGTH 0x20
#define IBUS_FRAME_COMMAND_CHANNELS 0x40
#define IBUS_TASK_STACK 4096
#define IBUS_TASK_PRIORITY 5
#define IBUS_LOCK_TIMEOUT_MS 100

struct ibus_receiver_t {
    ibus_receiver_config_t config;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool running;
    uint32_t last_frame_ms;
    uint32_t bytes_received;
    uint32_t frames_seen;
    uint32_t valid_frames;
    uint32_t bad_header_frames;
    uint32_t bad_checksum_frames;
    uint16_t channels[IBUS_RECEIVER_CHANNELS];
    uint8_t raw_sample[IBUS_RECEIVER_RAW_SAMPLE_SIZE];
    uint8_t raw_sample_index;
    uint8_t raw_sample_count;
};

const char *ibus_receiver_mode_to_string(ibus_receiver_mode_t mode)
{
    switch (mode) {
    case IBUS_RECEIVER_MODE_IBUS:
        return "IBUS";
    case IBUS_RECEIVER_MODE_IBUS_INVERTED:
        return "IBUS_INV";
    case IBUS_RECEIVER_MODE_IBUS_8N2:
        return "IBUS_8N2";
    case IBUS_RECEIVER_MODE_IBUS_INVERTED_8N2:
        return "IBUS_INV_8N2";
    case IBUS_RECEIVER_MODE_SBUS:
        return "SBUS";
    case IBUS_RECEIVER_MODE_SBUS_NON_INVERTED:
        return "SBUS_NOINV";
    default:
        return "UNKNOWN";
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t take_lock(ibus_receiver_handle_t handle)
{
    return xSemaphoreTake(handle->lock, pdMS_TO_TICKS(IBUS_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static uint16_t ibus_checksum(const uint8_t frame[IBUS_FRAME_LENGTH])
{
    uint16_t checksum = 0xFFFF;
    for (uint8_t i = 0; i < IBUS_FRAME_LENGTH - 2; i++) {
        checksum -= frame[i];
    }
    return checksum;
}

static void reset_runtime_state(ibus_receiver_handle_t handle)
{
    handle->last_frame_ms = 0;
    handle->bytes_received = 0;
    handle->frames_seen = 0;
    handle->valid_frames = 0;
    handle->bad_header_frames = 0;
    handle->bad_checksum_frames = 0;
    memset(handle->channels, 0, sizeof(handle->channels));
    memset(handle->raw_sample, 0, sizeof(handle->raw_sample));
    handle->raw_sample_index = 0;
    handle->raw_sample_count = 0;
}

static esp_err_t apply_uart_mode(ibus_receiver_handle_t handle, ibus_receiver_mode_t mode)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uint32_t inverse_mask = UART_SIGNAL_INV_DISABLE;

    switch (mode) {
    case IBUS_RECEIVER_MODE_IBUS:
        uart_config.baud_rate = 115200;
        uart_config.stop_bits = UART_STOP_BITS_1;
        inverse_mask = UART_SIGNAL_INV_DISABLE;
        break;
    case IBUS_RECEIVER_MODE_IBUS_INVERTED:
        uart_config.baud_rate = 115200;
        uart_config.stop_bits = UART_STOP_BITS_1;
        inverse_mask = UART_SIGNAL_RXD_INV;
        break;
    case IBUS_RECEIVER_MODE_IBUS_8N2:
        uart_config.baud_rate = 115200;
        uart_config.stop_bits = UART_STOP_BITS_2;
        inverse_mask = UART_SIGNAL_INV_DISABLE;
        break;
    case IBUS_RECEIVER_MODE_IBUS_INVERTED_8N2:
        uart_config.baud_rate = 115200;
        uart_config.stop_bits = UART_STOP_BITS_2;
        inverse_mask = UART_SIGNAL_RXD_INV;
        break;
    case IBUS_RECEIVER_MODE_SBUS:
        uart_config.baud_rate = 100000;
        uart_config.parity = UART_PARITY_EVEN;
        uart_config.stop_bits = UART_STOP_BITS_2;
        inverse_mask = UART_SIGNAL_RXD_INV;
        break;
    case IBUS_RECEIVER_MODE_SBUS_NON_INVERTED:
        uart_config.baud_rate = 100000;
        uart_config.parity = UART_PARITY_EVEN;
        uart_config.stop_bits = UART_STOP_BITS_2;
        inverse_mask = UART_SIGNAL_INV_DISABLE;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = uart_param_config(handle->config.uart_port, &uart_config);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_line_inverse(handle->config.uart_port, inverse_mask);
    if (err != ESP_OK) {
        return err;
    }
    uart_flush_input(handle->config.uart_port);

    handle->config.mode = mode;
    handle->config.baud_rate = (uint32_t)uart_config.baud_rate;
    handle->config.invert_rx = inverse_mask == UART_SIGNAL_RXD_INV;
    return ESP_OK;
}

static void record_bad_header(ibus_receiver_handle_t handle)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    handle->bad_header_frames++;
    xSemaphoreGive(handle->lock);
}

static void process_frame(ibus_receiver_handle_t handle, const uint8_t frame[IBUS_FRAME_LENGTH])
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }

    handle->frames_seen++;
    if (frame[0] != IBUS_FRAME_HEADER_LENGTH || frame[1] != IBUS_FRAME_COMMAND_CHANNELS) {
        handle->bad_header_frames++;
        xSemaphoreGive(handle->lock);
        return;
    }

    uint16_t expected = ibus_checksum(frame);
    uint16_t received = (uint16_t)frame[IBUS_FRAME_LENGTH - 2] |
                        ((uint16_t)frame[IBUS_FRAME_LENGTH - 1] << 8);
    if (received != expected) {
        handle->bad_checksum_frames++;
        xSemaphoreGive(handle->lock);
        return;
    }

    for (uint8_t i = 0; i < IBUS_RECEIVER_CHANNELS; i++) {
        uint8_t offset = 2 + (i * 2);
        handle->channels[i] = (uint16_t)frame[offset] | ((uint16_t)frame[offset + 1] << 8);
    }
    handle->valid_frames++;
    handle->last_frame_ms = now_ms();
    xSemaphoreGive(handle->lock);
}

static void note_bytes_received(ibus_receiver_handle_t handle, const uint8_t *bytes, uint32_t count)
{
    if (take_lock(handle) != ESP_OK) {
        return;
    }
    handle->bytes_received += count;
    for (uint32_t i = 0; bytes && i < count; i++) {
        handle->raw_sample[handle->raw_sample_index] = bytes[i];
        handle->raw_sample_index = (uint8_t)((handle->raw_sample_index + 1U) % IBUS_RECEIVER_RAW_SAMPLE_SIZE);
        if (handle->raw_sample_count < IBUS_RECEIVER_RAW_SAMPLE_SIZE) {
            handle->raw_sample_count++;
        }
    }
    xSemaphoreGive(handle->lock);
}

static void ibus_rx_task(void *arg)
{
    ibus_receiver_handle_t handle = (ibus_receiver_handle_t)arg;
    uint8_t bytes[64];
    uint8_t frame[IBUS_FRAME_LENGTH];
    uint8_t frame_index = 0;

    while (handle->running) {
        int read_len = uart_read_bytes(handle->config.uart_port,
                                       bytes,
                                       sizeof(bytes),
                                       pdMS_TO_TICKS(20));
        if (read_len <= 0) {
            continue;
        }

        note_bytes_received(handle, bytes, (uint32_t)read_len);
        for (int i = 0; i < read_len; i++) {
            uint8_t byte = bytes[i];

            if (frame_index == 0) {
                if (byte != IBUS_FRAME_HEADER_LENGTH) {
                    continue;
                }
                frame[frame_index++] = byte;
                continue;
            }

            if (frame_index == 1 && byte != IBUS_FRAME_COMMAND_CHANNELS) {
                record_bad_header(handle);
                frame_index = byte == IBUS_FRAME_HEADER_LENGTH ? 1 : 0;
                if (frame_index == 1) {
                    frame[0] = byte;
                }
                continue;
            }

            frame[frame_index++] = byte;
            if (frame_index == IBUS_FRAME_LENGTH) {
                process_frame(handle, frame);
                frame_index = 0;
            }
        }
    }

    vTaskDelete(NULL);
}

esp_err_t ibus_receiver_init(const ibus_receiver_config_t *config, ibus_receiver_handle_t *out_handle)
{
    if (!config || !out_handle || config->rx_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    ibus_receiver_handle_t handle = calloc(1, sizeof(struct ibus_receiver_t));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    if (handle->config.baud_rate == 0) {
        handle->config.baud_rate = IBUS_DEFAULT_BAUD_RATE;
    }
    if (handle->config.stale_timeout_ms == 0) {
        handle->config.stale_timeout_ms = IBUS_DEFAULT_STALE_TIMEOUT_MS;
    }
    if (handle->config.tx_pin == 0) {
        handle->config.tx_pin = UART_PIN_NO_CHANGE;
    }

    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = uart_driver_install(handle->config.uart_port,
                                        IBUS_UART_RX_BUFFER_SIZE,
                                        0,
                                        0,
                                        NULL,
                                        0);
    if (err != ESP_OK) {
        ibus_receiver_deinit(handle);
        return err;
    }

    err = uart_set_pin(handle->config.uart_port,
                       handle->config.tx_pin,
                       handle->config.rx_pin,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ibus_receiver_deinit(handle);
        return err;
    }
    (void)gpio_set_pull_mode((gpio_num_t)handle->config.rx_pin, GPIO_PULLUP_ONLY);

    ibus_receiver_mode_t mode = config->mode;
    if (mode == IBUS_RECEIVER_MODE_IBUS && config->invert_rx) {
        mode = IBUS_RECEIVER_MODE_IBUS_INVERTED;
    }
    err = apply_uart_mode(handle, mode);
    if (err != ESP_OK) {
        ibus_receiver_deinit(handle);
        return err;
    }

    handle->running = true;
    BaseType_t task_ok = xTaskCreate(ibus_rx_task,
                                     "ibus_rx",
                                     IBUS_TASK_STACK,
                                     handle,
                                     IBUS_TASK_PRIORITY,
                                     &handle->task);
    if (task_ok != pdPASS) {
        handle->running = false;
        ibus_receiver_deinit(handle);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "i-BUS receiver ready on UART%d RX GPIO%d baud=%lu",
             handle->config.uart_port,
             handle->config.rx_pin,
             (unsigned long)handle->config.baud_rate);
    *out_handle = handle;
    return ESP_OK;
}

void ibus_receiver_deinit(ibus_receiver_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->running = false;
    if (handle->task) {
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    (void)uart_driver_delete(handle->config.uart_port);
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}

esp_err_t ibus_receiver_set_mode(ibus_receiver_handle_t handle, ibus_receiver_mode_t mode)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = apply_uart_mode(handle, mode);
    if (err == ESP_OK) {
        reset_runtime_state(handle);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t ibus_receiver_get_status(ibus_receiver_handle_t handle, ibus_receiver_status_t *status)
{
    if (!handle || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }

    memset(status, 0, sizeof(*status));
    status->mode = handle->config.mode;
    status->uart_port = handle->config.uart_port;
    status->rx_pin = handle->config.rx_pin;
    status->baud_rate = handle->config.baud_rate;
    status->stale_timeout_ms = handle->config.stale_timeout_ms;
    status->bytes_received = handle->bytes_received;
    status->frames_seen = handle->frames_seen;
    status->valid_frames = handle->valid_frames;
    status->bad_header_frames = handle->bad_header_frames;
    status->bad_checksum_frames = handle->bad_checksum_frames;
    memcpy(status->channels, handle->channels, sizeof(status->channels));
    status->raw_sample_count = handle->raw_sample_count;
    uint8_t raw_start = handle->raw_sample_count == IBUS_RECEIVER_RAW_SAMPLE_SIZE ?
                        handle->raw_sample_index : 0;
    for (uint8_t i = 0; i < status->raw_sample_count; i++) {
        status->raw_sample[i] = handle->raw_sample[(raw_start + i) % IBUS_RECEIVER_RAW_SAMPLE_SIZE];
    }

    uint32_t last_frame_ms = handle->last_frame_ms;
    uint32_t timestamp = now_ms();
    status->last_frame_age_ms = last_frame_ms == 0 ? UINT32_MAX : timestamp - last_frame_ms;
    status->signal_valid = last_frame_ms != 0 &&
                           status->last_frame_age_ms <= handle->config.stale_timeout_ms;

    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t ibus_receiver_sample_pin(ibus_receiver_handle_t handle,
                                   uint32_t sample_count,
                                   uint32_t interval_us,
                                   ibus_receiver_pin_sample_t *sample)
{
    if (!handle || !sample || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    int rx_pin = handle->config.rx_pin;
    xSemaphoreGive(handle->lock);

    memset(sample, 0, sizeof(*sample));
    sample->rx_pin = rx_pin;
    sample->samples = sample_count;
    sample->last_level = -1;

    int previous_level = -1;
    for (uint32_t i = 0; i < sample_count; i++) {
        int level = gpio_get_level((gpio_num_t)rx_pin);
        if (level) {
            sample->high_count++;
        } else {
            sample->low_count++;
        }
        if (previous_level >= 0 && level != previous_level) {
            sample->transitions++;
        }
        previous_level = level;
        sample->last_level = level;
        if (interval_us > 0) {
            esp_rom_delay_us(interval_us);
        }
    }

    return ESP_OK;
}

esp_err_t ibus_receiver_capture_ppm(ibus_receiver_handle_t handle,
                                    uint32_t duration_us,
                                    uint32_t interval_us,
                                    ibus_receiver_ppm_capture_t *capture)
{
    if (!handle || !capture || duration_us == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = take_lock(handle);
    if (err != ESP_OK) {
        return err;
    }
    int rx_pin = handle->config.rx_pin;
    xSemaphoreGive(handle->lock);

    memset(capture, 0, sizeof(*capture));
    capture->rx_pin = rx_pin;
    capture->requested_duration_us = duration_us;
    capture->interval_us = interval_us;
    capture->initial_level = gpio_get_level((gpio_num_t)rx_pin);
    capture->last_level = capture->initial_level;

    int previous_level = capture->initial_level;
    uint64_t start_us = esp_timer_get_time();
    uint64_t previous_edge_us = start_us;

    while (true) {
        uint64_t now_us = esp_timer_get_time();
        uint32_t elapsed_us = (uint32_t)(now_us - start_us);
        if (elapsed_us >= duration_us) {
            capture->elapsed_us = elapsed_us;
            break;
        }

        int level = gpio_get_level((gpio_num_t)rx_pin);
        capture->samples++;
        if (level) {
            capture->high_count++;
        } else {
            capture->low_count++;
        }

        if (level != previous_level) {
            capture->transitions++;
            if (level) {
                capture->rising_edges++;
            } else {
                capture->falling_edges++;
            }

            if (capture->edge_count < IBUS_RECEIVER_PPM_EDGE_CAPACITY) {
                ibus_receiver_pin_edge_t *edge = &capture->edges[capture->edge_count++];
                edge->time_us = elapsed_us;
                edge->duration_since_previous_us = (uint32_t)(now_us - previous_edge_us);
                edge->level = level;
            } else {
                capture->edge_overflow = true;
            }
            previous_edge_us = now_us;
            previous_level = level;
        }
        capture->last_level = level;

        if (interval_us > 0) {
            esp_rom_delay_us(interval_us);
        }
    }

    return ESP_OK;
}
