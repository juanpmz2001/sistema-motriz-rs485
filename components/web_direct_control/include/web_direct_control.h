#ifndef WEB_DIRECT_CONTROL_H
#define WEB_DIRECT_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "motion_application_service.h"
#include "motion_status_port.h"
#include "svd48_workspace_port.h"
#include "web_direct_control_model.h"

typedef struct web_direct_control_t *web_direct_control_handle_t;

typedef bool (*web_direct_control_admission_gate_fn_t)(void *context,
                                                       char *detail,
                                                       size_t detail_size);

typedef struct {
    motion_application_service_handle_t motion_application;
    motion_status_port_t *motion_status;
    svd48_workspace_port_t *svd48_workspace;
    float max_vx_mps;
    float max_wz_radps;
    web_direct_control_admission_gate_fn_t admission_gate;
    void *admission_context;
} web_direct_control_config_t;

esp_err_t web_direct_control_init(const web_direct_control_config_t *config,
                                  web_direct_control_handle_t *out_handle);
esp_err_t web_direct_control_start(web_direct_control_handle_t handle);
void web_direct_control_deinit(web_direct_control_handle_t handle);

#endif
