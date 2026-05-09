#ifndef PPM_DECODER_H
#define PPM_DECODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPM_MAX_CHANNELS 8

// Configuración del decodificador PPM
typedef struct {
    int ppm_pin;
    uint8_t num_channels;
    uint32_t sync_threshold_us;  // Umbral para detectar pulso de sincronización (microsegundos)
} ppm_decoder_config_t;

// Handle del decodificador PPM
typedef struct ppm_decoder_t* ppm_decoder_handle_t;

/**
 * @brief Inicializar el decodificador PPM
 * @param config Configuración del decodificador
 * @return Handle del decodificador o NULL si falla
 */
ppm_decoder_handle_t ppm_decoder_init(const ppm_decoder_config_t *config);

/**
 * @brief Liberar recursos del decodificador
 * @param handle Handle del decodificador
 */
void ppm_decoder_deinit(ppm_decoder_handle_t handle);

/**
 * @brief Obtener el valor de un canal específico
 * @param handle Handle del decodificador
 * @param channel Número de canal (0-based)
 * @param value Puntero donde almacenar el valor del canal (microsegundos)
 * @return true si exitoso, false si falla
 */
bool ppm_decoder_get_channel(ppm_decoder_handle_t handle, uint8_t channel, uint16_t *value);

/**
 * @brief Obtener todos los valores de canales
 * @param handle Handle del decodificador
 * @param values Array donde almacenar los valores (debe tener al menos num_channels elementos)
 * @param num_channels Número de canales a leer
 * @return true si exitoso, false si falla
 */
bool ppm_decoder_get_all_channels(ppm_decoder_handle_t handle, uint16_t *values, uint8_t num_channels);

/**
 * @brief Verificar si se están recibiendo señales PPM válidas
 * @param handle Handle del decodificador
 * @return true si se reciben señales válidas, false en caso contrario
 */
bool ppm_decoder_is_signal_valid(ppm_decoder_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // PPM_DECODER_H 