#ifndef BLUETOOTH_CONTROLLER_H
#define BLUETOOTH_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Estructura para datos del joystick (basada en el código Arduino)
typedef struct {
    int16_t x;
    int16_t y;
    bool valid;
    uint32_t timestamp;
} joystick_data_t;

// Estados de conexión Bluetooth
typedef enum {
    BT_STATE_DISCONNECTED,
    BT_STATE_ADVERTISING,
    BT_STATE_CONNECTED
} bluetooth_state_t;

// Callback para cuando se reciben datos del joystick
typedef void (*bluetooth_joystick_callback_t)(const joystick_data_t *data, void *user_data);

// Callback para cambios de estado de conexión
typedef void (*bluetooth_connection_callback_t)(bluetooth_state_t state, void *user_data);

// Configuración del controlador Bluetooth
typedef struct {
    const char *device_name;
    bluetooth_joystick_callback_t joystick_callback;
    bluetooth_connection_callback_t connection_callback;
    void *user_data;
    uint32_t connection_timeout_ms;  // Tiempo máximo para intentar conexión
} bluetooth_controller_config_t;

// Handle del controlador Bluetooth
typedef struct bluetooth_controller_t* bluetooth_controller_handle_t;

/**
 * @brief Inicializar el controlador Bluetooth
 * @param config Configuración del controlador
 * @return Handle del controlador o NULL si falla
 */
bluetooth_controller_handle_t bluetooth_controller_init(const bluetooth_controller_config_t *config);

/**
 * @brief Liberar recursos del controlador Bluetooth
 * @param handle Handle del controlador
 */
void bluetooth_controller_deinit(bluetooth_controller_handle_t handle);

/**
 * @brief Verificar si hay un dispositivo conectado
 * @param handle Handle del controlador
 * @return true si hay un dispositivo conectado, false en caso contrario
 */
bool bluetooth_controller_is_connected(bluetooth_controller_handle_t handle);

/**
 * @brief Obtener el estado actual de Bluetooth
 * @param handle Handle del controlador
 * @return Estado actual de Bluetooth
 */
bluetooth_state_t bluetooth_controller_get_state(bluetooth_controller_handle_t handle);

/**
 * @brief Obtener los últimos datos del joystick recibidos
 * @param handle Handle del controlador
 * @param data Puntero donde almacenar los datos del joystick
 * @return true si exitoso, false si falla
 */
bool bluetooth_controller_get_joystick_data(bluetooth_controller_handle_t handle, joystick_data_t *data);

/**
 * @brief Verificar si el tiempo de conexión ha expirado
 * @param handle Handle del controlador
 * @return true si ha expirado el tiempo de conexión, false en caso contrario
 */
bool bluetooth_controller_is_connection_timeout(bluetooth_controller_handle_t handle);

/**
 * @brief Detener el intento de conexión Bluetooth
 * @param handle Handle del controlador
 */
void bluetooth_controller_stop_advertising(bluetooth_controller_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // BLUETOOTH_CONTROLLER_H 