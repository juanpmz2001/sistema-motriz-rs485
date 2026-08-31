#ifndef PPM_MOTION_SOURCE_H
#define PPM_MOTION_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ibus_receiver.h"
#include "motion_application_service.h"
#include "robot_profile.h"

typedef struct ppm_motion_source_t *ppm_motion_source_handle_t;
typedef bool (*ppm_motion_source_priority_gate_t)(void *context);

#define PPM_MOTION_SOURCE_DEFAULT_PERIOD_MS 20U
#define PPM_MOTION_SOURCE_DEFAULT_TASK_PRIORITY 8U

typedef struct {
    const robot_profile_t *profile;
    ibus_receiver_handle_t receiver;
    motion_application_service_handle_t motion_application;
    uint32_t period_ms;
    uint32_t task_priority;
    /* Source selection is owned by robot_safety. This gate prevents a raw
     * unconfirmed CH5 sample from directly becoming RC authority. */
    ppm_motion_source_priority_gate_t priority_confirmed;
    void *priority_context;
} ppm_motion_source_config_t;

esp_err_t ppm_motion_source_init(const ppm_motion_source_config_t *config,
                                 ppm_motion_source_handle_t *out_handle);
esp_err_t ppm_motion_source_start(ppm_motion_source_handle_t handle);
void ppm_motion_source_deinit(ppm_motion_source_handle_t handle);

#endif
