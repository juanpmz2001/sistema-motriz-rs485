#include "ppm_decoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ppm_decoder_model.h"

static const char *TAG = "ppm_decoder";

struct ppm_decoder_t {
    ppm_decoder_config_t config;
    ppm_decoder_model_t model;
    portMUX_TYPE mux;
    bool initialized;
};

static void ppm_isr_handler(void *arg)
{
    ppm_decoder_handle_t handle = (ppm_decoder_handle_t)arg;
    if (!handle) {
        return;
    }

    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL_ISR(&handle->mux);
    (void)ppm_decoder_model_feed_rising_edge(&handle->model, now_us);
    portEXIT_CRITICAL_ISR(&handle->mux);
}

ppm_decoder_handle_t ppm_decoder_init(const ppm_decoder_config_t *config)
{
    if (!config || !GPIO_IS_VALID_GPIO(config->ppm_pin) || config->stale_timeout_ms == 0) {
        return NULL;
    }

    ppm_decoder_handle_t handle = calloc(1, sizeof(struct ppm_decoder_t));
    if (!handle) {
        return NULL;
    }

    handle->config = *config;
    handle->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    const ppm_decoder_model_config_t model_config = {
        .channel_count = config->channel_count,
        .min_frame_channels = config->min_frame_channels,
        .sync_threshold_us = config->sync_threshold_us,
        .min_pulse_us = config->min_pulse_us,
        .max_pulse_us = config->max_pulse_us,
    };
    if (!ppm_decoder_model_init(&handle->model, &model_config)) {
        free(handle);
        return NULL;
    }

    const gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << config->ppm_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d configuration failed: %s", config->ppm_pin, esp_err_to_name(err));
        free(handle);
        return NULL;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR service failed: %s", esp_err_to_name(err));
        free(handle);
        return NULL;
    }
    err = gpio_isr_handler_add((gpio_num_t)config->ppm_pin, ppm_isr_handler, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d ISR handler failed: %s", config->ppm_pin, esp_err_to_name(err));
        free(handle);
        return NULL;
    }

    handle->initialized = true;
    ESP_LOGI(TAG,
             "PPM ready GPIO%d channels=%u sync=%luus pulse=%u..%uus stale=%lums",
             config->ppm_pin,
             config->channel_count,
             (unsigned long)config->sync_threshold_us,
             config->min_pulse_us,
             config->max_pulse_us,
             (unsigned long)config->stale_timeout_ms);
    return handle;
}

void ppm_decoder_deinit(ppm_decoder_handle_t handle)
{
    if (!handle) {
        return;
    }
    if (handle->initialized) {
        (void)gpio_isr_handler_remove((gpio_num_t)handle->config.ppm_pin);
    }
    free(handle);
}

bool ppm_decoder_get_status(ppm_decoder_handle_t handle, ppm_decoder_status_t *status)
{
    if (!handle || !handle->initialized || !status) {
        return false;
    }

    ppm_decoder_model_status_t model_status;
    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&handle->mux);
    const bool ok = ppm_decoder_model_snapshot(&handle->model, now_us, &model_status);
    portEXIT_CRITICAL(&handle->mux);
    if (!ok) {
        return false;
    }

    memset(status, 0, sizeof(*status));
    status->last_frame_age_ms = model_status.last_frame_age_us == UINT32_MAX
                                  ? UINT32_MAX
                                  : model_status.last_frame_age_us / 1000U;
    status->signal_valid = status->last_frame_age_ms != UINT32_MAX &&
                           status->last_frame_age_ms <= handle->config.stale_timeout_ms;
    status->edges_seen = model_status.edges_seen;
    status->sync_gaps = model_status.sync_gaps;
    status->valid_frames = model_status.valid_frames;
    status->incomplete_frames = model_status.incomplete_frames;
    status->invalid_pulses = model_status.invalid_pulses;
    status->overflow_pulses = model_status.overflow_pulses;
    status->rejected_frames = model_status.rejected_frames;
    status->channel_count = model_status.channel_count;
    memcpy(status->channels, model_status.channels, sizeof(status->channels));
    return true;
}

bool ppm_decoder_get_channel(ppm_decoder_handle_t handle, uint8_t channel, uint16_t *value)
{
    ppm_decoder_status_t status;
    if (!value || !ppm_decoder_get_status(handle, &status) || channel >= status.channel_count) {
        return false;
    }
    *value = status.channels[channel];
    return true;
}

bool ppm_decoder_get_all_channels(ppm_decoder_handle_t handle,
                                  uint16_t *values,
                                  uint8_t channel_count)
{
    ppm_decoder_status_t status;
    if (!values || !ppm_decoder_get_status(handle, &status) || channel_count > status.channel_count) {
        return false;
    }
    memcpy(values, status.channels, (size_t)channel_count * sizeof(values[0]));
    return true;
}

bool ppm_decoder_is_signal_valid(ppm_decoder_handle_t handle)
{
    ppm_decoder_status_t status;
    return ppm_decoder_get_status(handle, &status) && status.signal_valid;
}
