#ifndef MOTOR_CONTROLLER_H
#define MOTOR_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configuración del controlador de motores
typedef struct {
    uart_port_t uart_port;
    int tx_pin;
    int rx_pin;
    uint32_t baud_rate;
    uint8_t device_address;
} motor_controller_config_t;

// Handle del controlador de motores
typedef struct motor_controller_t* motor_controller_handle_t;

// Comandos de control
typedef enum {
    MOTOR_CMD_STOP = 0,
    MOTOR_CMD_START = 1,
    MOTOR_CMD_CLEAR_ALARM = 2
} motor_control_cmd_t;

/**
 * @brief Inicializar el controlador de motores
 * @param config Configuración del controlador
 * @return Handle del controlador o NULL si falla
 */
motor_controller_handle_t motor_controller_init(const motor_controller_config_t *config);

/**
 * @brief Liberar recursos del controlador
 * @param handle Handle del controlador
 */
void motor_controller_deinit(motor_controller_handle_t handle);

/**
 * @brief Establecer comando de control para motor 1
 * @param handle Handle del controlador
 * @param cmd Comando de control
 * @return true si exitoso, false si falla
 */
bool motor_controller_set_m1_control_command(motor_controller_handle_t handle, motor_control_cmd_t cmd);

/**
 * @brief Establecer velocidad para motor 1
 * @param handle Handle del controlador
 * @param speed Velocidad (-100 a 100)
 * @return true si exitoso, false si falla
 */
bool motor_controller_set_m1_speed(motor_controller_handle_t handle, int16_t speed);

/**
 * @brief Establecer comando de control para motor 2
 * @param handle Handle del controlador
 * @param cmd Comando de control
 * @return true si exitoso, false si falla
 */
bool motor_controller_set_m2_control_command(motor_controller_handle_t handle, motor_control_cmd_t cmd);

/**
 * @brief Establecer velocidad para motor 2
 * @param handle Handle del controlador
 * @param speed Velocidad (-100 a 100)
 * @return true si exitoso, false si falla
 */
bool motor_controller_set_m2_speed(motor_controller_handle_t handle, int16_t speed);

/**
 * @brief Leer velocidad actual del motor 1
 * @param handle Handle del controlador
 * @param speed Puntero donde almacenar la velocidad leída
 * @return true si exitoso, false si falla
 */
bool motor_controller_get_m1_speed(motor_controller_handle_t handle, int16_t *speed);

/**
 * @brief Leer velocidad actual del motor 2
 * @param handle Handle del controlador
 * @param speed Puntero donde almacenar la velocidad leída
 * @return true si exitoso, false si falla
 */
bool motor_controller_get_m2_speed(motor_controller_handle_t handle, int16_t *speed);

#ifdef __cplusplus
}
#endif

#endif // MOTOR_CONTROLLER_H 
