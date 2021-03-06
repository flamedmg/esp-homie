/*
 * This code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 *
 * Obtained from $IDF_PATH/examples/system/ota/native_ota_example
 */

#include <string.h>
#include <freertos/FreeRTOS.h>

#include <freertos/task.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_flash_partitions.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>

/* ignore if not esp-idf 4.x */
#if HELPER_TARGET_VERSION >= HELPER_TARGET_VERSION_ESP32_V4

#include "esp_idf_lib_helpers.h"
#include "task_ota.h"
#include "semver.c"

#define BUFFSIZE (1024)
#define HASH_SHA256_LEN (32)
#define UPDATE_CHECK_INTERVAL_SEC (60)
#define MUTEX_DO_NOT_BLOCK (0)

static const char *TAG = "task_ota";
extern SemaphoreHandle_t mutex_ota;

/* an ota data write buffer ready to write to the flash */
static char ota_write_data[BUFFSIZE + 1] = { 0 };
extern const uint8_t ca_cert_ota_pem_start[] asm("_binary_ca_cert_ota_pem_start");
extern const uint8_t ca_cert_ota_pem_end[] asm("_binary_ca_cert_ota_pem_end");

/**
 * @brief compare two Semantic Version strings
 *
 * @param[in] compare Version string to be compared
 * @param[in] current Current version string
 * @param[out] result Result of the comparison.
 *                    0 if both versions are same.
 *                    -1 if compare is lower (older) than current,
 *                    1 if compare is higer (newer) than current
 * @return ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t compare_version(const char *compare, const char *current, int *result)
{
    esp_err_t err;
    semver_t compare_version = {};
    semver_t current_version = {};
    if (semver_parse(current, &current_version) || semver_parse(compare, &compare_version)) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "Invalid semantic version string: compare: `%s` current: `%s`", compare, current);
        goto fail;
    }
    *result = semver_compare(compare_version, current_version);
    err = ESP_OK;
fail:
    semver_free(&compare_version);
    semver_free(&current_version);
    return err;
}

static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void do_ota(void *pvParameter)
{
    bool got_mutex = false;
    bool image_header_was_checked = false;
    int binary_file_length = 0;
    const esp_partition_t *update_partition = NULL;
    const esp_partition_t *configured = NULL;
    const esp_partition_t *running = NULL;
    esp_err_t err;
    esp_http_client_config_t *config = (esp_http_client_config_t *)pvParameter;
    esp_http_client_handle_t client = NULL;

    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;

    ESP_LOGI(TAG, "Starting OTA");

    config = (esp_http_client_config_t *)pvParameter;
    if (xSemaphoreTake(mutex_ota, (TickType_t) MUTEX_DO_NOT_BLOCK) != pdTRUE) {
        ESP_LOGW(TAG, "Another OTA is in progress");
        goto fail;
    }
    got_mutex = true;
    printf("URL: %s", config->url);

    /* deal with all receive packet */
    configured = esp_ota_get_boot_partition();
    running = esp_ota_get_running_partition();
    if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    client = esp_http_client_init(config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        err = ESP_FAIL;
        goto fail;
    }

    ESP_LOGI(TAG, "Fetching the update");
    printf("firmware URL: %s\n", config->url);

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        goto fail;
    }
    esp_http_client_fetch_headers(client);

    update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);
    assert(update_partition != NULL);

    while (1) {
        int data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            err = ESP_FAIL;
            goto fail;
        } else if (data_read > 0) {
            if (image_header_was_checked == false) {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    // check current version with downloading
                    memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
                    }

                    const esp_partition_t *last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                    }

                    int result;
                    // check current version with last invalid partition
                    if (last_invalid_app != NULL) {
                        if (compare_version(invalid_app_info.version, new_app_info.version, &result) != ESP_OK) {
                            ESP_LOGE(TAG, "compare_version()");
                            goto fail;
                        }
                        if (result == 0) {
                            ESP_LOGW(TAG, "New version is the same as invalid version.");
                            ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                            ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                            err = ESP_FAIL;
                            goto fail;
                        }
                    }

                    if (compare_version(new_app_info.version, running_app_info.version, &result) != ESP_OK) {
                        ESP_LOGE(TAG, "compare_version()");
                        goto fail;
                    }
                    if (result == 0) {
                        ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
                        goto no_need_to_update;
                    }
                    else if (result < 0) {
                        ESP_LOGW(TAG, "Current running version is newer than new firmware version. We will not continue the update.");
                        goto no_need_to_update;
                    } else {
                        ESP_LOGI(TAG, "Current running version is older than new firmware version. Starting the OTA");
                    }

                    image_header_was_checked = true;

                    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                        goto fail;
                    }
                    ESP_LOGI(TAG, "esp_ota_begin succeeded");
                } else {
                    ESP_LOGE(TAG, "received package is not fit len");
                    err = ESP_FAIL;
                    goto fail;
                }
            }
            err = esp_ota_write( update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                goto fail;
            }
            binary_file_length += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_length);
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Connection closed");
            break;
        }
    }
    ESP_LOGI(TAG, "Total Write binary data length: %d", binary_file_length);
    if (esp_http_client_is_complete_data_received(client) != true) {
        ESP_LOGE(TAG, "Error in receiving complete file");
        err = ESP_FAIL;
        goto fail;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        goto fail;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        goto fail;
    }
    ESP_LOGI(TAG, "Prepare to restart system!");
    esp_restart();
    while (1) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    /* NOT REACHED */
no_need_to_update:
fail:
    if (got_mutex) {
        xSemaphoreGive(mutex_ota);
    }
    if (client != NULL) {
        http_cleanup(client);
    }
    vTaskDelete(NULL);
}

esp_err_t start_ota(esp_http_client_config_t config)
{
    ESP_LOGI(TAG, "Starting OTA");
    if (xTaskCreate(&do_ota, "do_ota", configMINIMAL_STACK_SIZE * 20, (void *)&config, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate() failed");
        goto fail;
    }
    return ESP_OK;
fail:
    return ESP_FAIL;
}

#endif // HELPER_TARGET_VERSION >= HELPER_TARGET_VERSION_ESP32_V4
