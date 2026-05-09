#include "ppm_decoder.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ppm_decoder";

// Estructura interna del decodificador
struct ppm_decoder_t {
    int ppm_pin;
    uint8_t num_channels;
    uint32_t sync_threshold_us;
    volatile uint16_t channel_values[PPM_MAX_CHANNELS];
    volatile uint8_t current_channel;
    volatile uint64_t last_rise_time;
    volatile uint64_t last_valid_frame_time;
    bool initialized;
};

// Variable global para acceder desde la ISR
static ppm_decoder_handle_t g_ppm_handle = NULL;

// Prototipo de la ISR
static void IRAM_ATTR ppm_isr_handler(void *arg);

ppm_decoder_handle_t ppm_decoder_init(const ppm_decoder_config_t *config)
{
    if (!config || config->num_channels > PPM_MAX_CHANNELS) {
        ESP_LOGE(TAG, "Invalid configuration");
        return NULL;
    }

    if (g_ppm_handle != NULL) {
        ESP_LOGE(TAG, "PPM decoder already initialized");
        return NULL;
    }

    ppm_decoder_handle_t handle = malloc(sizeof(struct ppm_decoder_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory for PPM decoder");
        return NULL;
    }

    handle->ppm_pin = config->ppm_pin;
    handle->num_channels = config->num_channels;
    handle->sync_threshold_us = config->sync_threshold_us;
    handle->current_channel = 0;
    handle->last_rise_time = 0;
    handle->last_valid_frame_time = 0;
    handle->initialized = false;

    // Inicializar valores de canales con valores neutros (1500 us)
    for (int i = 0; i < PPM_MAX_CHANNELS; i++) {
        handle->channel_values[i] = 1500;
    }

    // Configurar GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << config->ppm_pin),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    // Instalar ISR
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    // Asignar handle global para la ISR
    g_ppm_handle = handle;

    ret = gpio_isr_handler_add(config->ppm_pin, ppm_isr_handler, handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        g_ppm_handle = NULL;
        free(handle);
        return NULL;
    }

    handle->initialized = true;
    ESP_LOGI(TAG, "PPM decoder initialized on pin %d with %d channels", config->ppm_pin, config->num_channels);

    return handle;
}

void ppm_decoder_deinit(ppm_decoder_handle_t handle)
{
    if (!handle) return;

    if (handle->initialized) {
        gpio_isr_handler_remove(handle->ppm_pin);
        g_ppm_handle = NULL;
    }

    free(handle);
    ESP_LOGI(TAG, "PPM decoder deinitialized");
}

bool ppm_decoder_get_channel(ppm_decoder_handle_t handle, uint8_t channel, uint16_t *value)
{
    if (!handle || !handle->initialized || !value || channel >= handle->num_channels) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

    // Deshabilitar interrupciones temporalmente para leer el valor
    portDISABLE_INTERRUPTS();
    *value = handle->channel_values[channel];
    portENABLE_INTERRUPTS();

    return true;
}

bool ppm_decoder_get_all_channels(ppm_decoder_handle_t handle, uint16_t *values, uint8_t num_channels)
{
    if (!handle || !handle->initialized || !values || num_channels > handle->num_channels) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

    // Deshabilitar interrupciones temporalmente para leer todos los valores
    portDISABLE_INTERRUPTS();
    for (uint8_t i = 0; i < num_channels; i++) {
        values[i] = handle->channel_values[i];
    }
    portENABLE_INTERRUPTS();

    return true;
}

bool ppm_decoder_is_signal_valid(ppm_decoder_handle_t handle)
{
    if (!handle || !handle->initialized) {
        return false;
    }

    uint64_t current_time = esp_timer_get_time();
    uint64_t last_frame_time;

    portDISABLE_INTERRUPTS();
    last_frame_time = handle->last_valid_frame_time;
    portENABLE_INTERRUPTS();

    // Considerar señal válida si hemos recibido un frame en los últimos 100ms
    return (current_time - last_frame_time) < 100000; // 100ms en microsegundos
}

// ISR para manejar las interrupciones PPM
static void IRAM_ATTR ppm_isr_handler(void *arg)
{
    ppm_decoder_handle_t handle = (ppm_decoder_handle_t)arg;
    if (!handle) return;

    uint64_t now = esp_timer_get_time();
    uint64_t pulse_width = now - handle->last_rise_time;
    handle->last_rise_time = now;

    if (pulse_width > handle->sync_threshold_us) {
        // Pulso largo detectado - reiniciar secuencia de canales
        handle->current_channel = 0;
        handle->last_valid_frame_time = now;
    } else {
        // Pulso de canal normal
        if (handle->current_channel < handle->num_channels) {
            // Limitar valores a rango típico de PPM (800-2200 us)
            if (pulse_width >= 800 && pulse_width <= 2200) {
                handle->channel_values[handle->current_channel] = (uint16_t)pulse_width;
            }
            handle->current_channel++;
        }
    }
} 