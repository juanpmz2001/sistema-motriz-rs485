#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "config_manager.h"
#include "esp_err.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_MANAGER_VERSION_MAX 32
#define OTA_MANAGER_URL_MAX 256
#define OTA_MANAGER_FILENAME_MAX 96
#define OTA_MANAGER_SHA256_HEX_LEN 64
#define OTA_MANAGER_DETAIL_MAX 64
#define OTA_MANAGER_PARTITION_LABEL_MAX 17

typedef struct ota_manager_t *ota_manager_handle_t;

typedef enum {
    OTA_MANAGER_CHECK_STATUS_UNKNOWN = 0,
    OTA_MANAGER_CHECK_STATUS_UP_TO_DATE,
    OTA_MANAGER_CHECK_STATUS_UPDATE_AVAILABLE,
} ota_manager_check_status_t;

typedef enum {
    OTA_MANAGER_ROLLBACK_TEST_NONE = 0,
    OTA_MANAGER_ROLLBACK_TEST_NO_CONFIRM_ONCE = 1,
    OTA_MANAGER_ROLLBACK_TEST_SELF_TEST_FAIL_ONCE = 2,
} ota_manager_rollback_test_mode_t;

typedef struct {
    config_manager_handle_t config_manager;
    const char *current_project;
    const char *current_target;
    uint32_t current_build_number;
} ota_manager_config_t;

typedef struct {
    char partition_label[OTA_MANAGER_PARTITION_LABEL_MAX];
    esp_ota_img_states_t state;
    esp_err_t state_error;
    bool state_known;
    bool pending_verify;
    bool rollback_possible;
} ota_manager_boot_state_t;

typedef struct {
    ota_manager_check_status_t status;
    char project[CONFIG_MANAGER_OTA_HOST_MAX];
    char target[CONFIG_MANAGER_OTA_HOST_MAX];
    char version[OTA_MANAGER_VERSION_MAX];
    uint32_t current_build_number;
    uint32_t build_number;
    uint32_t min_supported_build;
    uint32_t size;
    char filename[OTA_MANAGER_FILENAME_MAX];
    char url[OTA_MANAGER_URL_MAX];
    char sha256[OTA_MANAGER_SHA256_HEX_LEN + 1];
    char detail[OTA_MANAGER_DETAIL_MAX];
} ota_manager_check_result_t;

typedef struct {
    char partition_label[OTA_MANAGER_PARTITION_LABEL_MAX];
    uint32_t partition_size;
    uint32_t manifest_size;
    uint32_t bytes_written;
    char sha256[OTA_MANAGER_SHA256_HEX_LEN + 1];
    char detail[OTA_MANAGER_DETAIL_MAX];
} ota_manager_download_result_t;

esp_err_t ota_manager_init(const ota_manager_config_t *config, ota_manager_handle_t *out_handle);
void ota_manager_deinit(ota_manager_handle_t handle);

esp_err_t ota_manager_check(ota_manager_handle_t handle, ota_manager_check_result_t *result);
esp_err_t ota_manager_download_to_inactive(ota_manager_handle_t handle, ota_manager_download_result_t *result);
esp_err_t ota_manager_download_test(ota_manager_handle_t handle, ota_manager_download_result_t *result);
esp_err_t ota_manager_set_boot_partition(const char *partition_label);
esp_err_t ota_manager_get_boot_state(ota_manager_boot_state_t *state);
esp_err_t ota_manager_mark_app_valid(void);
esp_err_t ota_manager_mark_app_invalid_and_rollback(void);
esp_err_t ota_manager_get_rollback_test_mode(ota_manager_rollback_test_mode_t *mode);
esp_err_t ota_manager_set_rollback_test_mode(ota_manager_rollback_test_mode_t mode);
esp_err_t ota_manager_consume_rollback_test_mode(ota_manager_rollback_test_mode_t *mode);
const char *ota_manager_check_status_to_string(ota_manager_check_status_t status);
const char *ota_manager_image_state_to_string(esp_ota_img_states_t state);
const char *ota_manager_rollback_test_mode_to_string(ota_manager_rollback_test_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif // OTA_MANAGER_H
