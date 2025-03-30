#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs_config.h"
#include "nvs_device.h"

#include "connect.h"
#include "global_state.h"

// Developer Notes:
// This static constant defines the logging tag for the NVS device module, used with ESP_LOG macros to identify log
// messages related to NVS initialization and configuration parsing. It aids in debugging and monitoring the storage
// and retrieval of device settings critical to mining operations.
static const char * TAG = "nvs_device";

// Developer Notes:
// This static constant represents the total nonce space available for a 32-bit nonce (2^32 = 4,294,967,296). It’s used
// in calculating the ASIC job frequency (commented out in the code), which determines how often new mining jobs are
// sent to the ASIC based on its frequency and core count. Although currently unused in favor of hardcoded values, it
// reflects the theoretical maximum number of nonces an ASIC can process per job, a key parameter in mining performance.
static const double NONCE_SPACE = 4294967296.0; //  2^32

// Developer Notes:
// This function initializes the NVS flash storage system, which stores persistent configuration data for the mining
// device (e.g., Wi-Fi credentials, ASIC settings). It attempts to initialize NVS and, if it fails due to no free pages
// (ESP_ERR_NVS_NO_FREE_PAGES), erases the flash and retries. It returns an error code if initialization fails after
// the retry, ensuring the system has a usable NVS partition. This is a critical startup step, as subsequent functions
// rely on NVS to load device configuration, and it’s called once during system boot.
esp_err_t NVSDevice_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

// Developer Notes:
// This function retrieves Wi-Fi credentials (SSID and password) and the device hostname from NVS, storing them in the
// provided pointers and copying the SSID to the global state’s SYSTEM_MODULE. It uses nvs_config_get_string to fetch
// values with defaults (WIFI_SSID, WIFI_PASS, HOSTNAME) if not set in NVS. The SSID is truncated to fit the global state’s
// buffer, ensuring null-termination. It returns ESP_OK, assuming NVS operations succeed. This function is essential for
// configuring network connectivity, enabling the device to join a Wi-Fi network and communicate with a mining pool.
esp_err_t NVSDevice_get_wifi_creds(GlobalState * GLOBAL_STATE, char ** wifi_ssid, char ** wifi_pass, char ** hostname) {
    *wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, WIFI_SSID);
    *wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS, WIFI_PASS);
    *hostname  = nvs_config_get_string(NVS_CONFIG_HOSTNAME, HOSTNAME);

    strncpy(GLOBAL_STATE->SYSTEM_MODULE.ssid, *wifi_ssid, sizeof(GLOBAL_STATE->SYSTEM_MODULE.ssid));
    GLOBAL_STATE->SYSTEM_MODULE.ssid[sizeof(GLOBAL_STATE->SYSTEM_MODULE.ssid)-1] = 0;

    return ESP_OK;
}

// Developer Notes:
// This function parses the device configuration from NVS, populating the global state with settings like ASIC frequency,
// device model, board version, and ASIC model. It sets:
// - frequency_value: ASIC operating frequency from NVS (default CONFIG_ASIC_FREQUENCY).
// - device_model: Maps a string (e.g., "max", "ultra") to an enum (DEVICE_MAX, etc.), setting asic_count and voltage_domain.
// - board_version: Converts a string to an integer (default "000").
// - asic_model: Maps a string (e.g., "BM1366") to an enum (ASIC_BM1366, etc.), assigning function pointers for ASIC
//   operations (init, work processing, etc.), job frequency, and difficulty.
// It logs the retrieved values and returns ESP_FAIL for invalid device or ASIC models, otherwise ESP_OK. This function
// is crucial for configuring the system at startup, tailoring behavior to the specific hardware and ASIC type detected.
esp_err_t NVSDevice_parse_config(GlobalState * GLOBAL_STATE) {

    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);
    ESP_LOGI(TAG, "NVS_CONFIG_ASIC_FREQ %f", (float)GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value);

    GLOBAL_STATE->device_model_str = nvs_config_get_string(NVS_CONFIG_DEVICE_MODEL, "");
    if (strcmp(GLOBAL_STATE->device_model_str, "max") == 0) {
        ESP_LOGI(TAG, "DEVICE: Max");
        GLOBAL_STATE->device_model = DEVICE_MAX;
        GLOBAL_STATE->asic_count = 1;
        GLOBAL_STATE->voltage_domain = 1;
    } else if (strcmp(GLOBAL_STATE->device_model_str, "ultra") == 0) {
        ESP_LOGI(TAG, "DEVICE: Ultra");
        GLOBAL_STATE->device_model = DEVICE_ULTRA;
        GLOBAL_STATE->asic_count = 1;
        GLOBAL_STATE->voltage_domain = 1;
    } else if (strcmp(GLOBAL_STATE->device_model_str, "supra") == 0) {
        ESP_LOGI(TAG, "DEVICE: Supra");
        GLOBAL_STATE->device_model = DEVICE_SUPRA;
        GLOBAL_STATE->asic_count = 1;
        GLOBAL_STATE->voltage_domain = 1;
        } else if (strcmp(GLOBAL_STATE->device_model_str, "gamma") == 0) {
        ESP_LOGI(TAG, "DEVICE: Gamma");
        GLOBAL_STATE->device_model = DEVICE_GAMMA;
        GLOBAL_STATE->asic_count = 1;
        GLOBAL_STATE->voltage_domain = 1;
    } else {
        ESP_LOGE(TAG, "Invalid DEVICE model");
        GLOBAL_STATE->device_model = DEVICE_UNKNOWN;
        GLOBAL_STATE->asic_count = -1;
        GLOBAL_STATE->voltage_domain = 1;

        return ESP_FAIL;
    }

    GLOBAL_STATE->board_version = atoi(nvs_config_get_string(NVS_CONFIG_BOARD_VERSION, "000"));
    ESP_LOGI(TAG, "Found Device Model: %s", GLOBAL_STATE->device_model_str);
    ESP_LOGI(TAG, "Found Board Version: %d", GLOBAL_STATE->board_version);

    GLOBAL_STATE->asic_model_str = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL, "");
    if (strcmp(GLOBAL_STATE->asic_model_str, "BM1366") == 0) {
        ESP_LOGI(TAG, "ASIC: %dx BM1366 (%" PRIu64 " cores)", GLOBAL_STATE->asic_count, BM1366_CORE_COUNT);
        GLOBAL_STATE->asic_model = ASIC_BM1366;
        AsicFunctions ASIC_functions = {.init_fn = BM1366_init,
                                        .receive_result_fn = BM1366_proccess_work,
                                        .set_max_baud_fn = BM1366_set_max_baud,
                                        .set_difficulty_mask_fn = BM1366_set_job_difficulty_mask,
                                        .send_work_fn = BM1366_send_work,
                                        .set_version_mask = BM1366_set_version_mask};
        GLOBAL_STATE->asic_job_frequency_ms = 2000; //ms
        GLOBAL_STATE->ASIC_difficulty = BM1366_ASIC_DIFFICULTY;

        GLOBAL_STATE->ASIC_functions = ASIC_functions;
        } else if (strcmp(GLOBAL_STATE->asic_model_str, "BM1370") == 0) {
        ESP_LOGI(TAG, "ASIC: %dx BM1370 (%" PRIu64 " cores)", GLOBAL_STATE->asic_count, BM1370_CORE_COUNT);
        GLOBAL_STATE->asic_model = ASIC_BM1370;
        AsicFunctions ASIC_functions = {.init_fn = BM1370_init,
                                        .receive_result_fn = BM1370_proccess_work,
                                        .set_max_baud_fn = BM1370_set_max_baud,
                                        .set_difficulty_mask_fn = BM1370_set_job_difficulty_mask,
                                        .send_work_fn = BM1370_send_work,
                                        .set_version_mask = BM1370_set_version_mask};
        GLOBAL_STATE->asic_job_frequency_ms = 500; //ms
        GLOBAL_STATE->ASIC_difficulty = BM1370_ASIC_DIFFICULTY;

        GLOBAL_STATE->ASIC_functions = ASIC_functions;
    } else if (strcmp(GLOBAL_STATE->asic_model_str, "BM1368") == 0) {
        ESP_LOGI(TAG, "ASIC: %dx BM1368 (%" PRIu64 " cores)", GLOBAL_STATE->asic_count, BM1368_CORE_COUNT);
        GLOBAL_STATE->asic_model = ASIC_BM1368;
        AsicFunctions ASIC_functions = {.init_fn = BM1368_init,
                                        .receive_result_fn = BM1368_proccess_work,
                                        .set_max_baud_fn = BM1368_set_max_baud,
                                        .set_difficulty_mask_fn = BM1368_set_job_difficulty_mask,
                                        .send_work_fn = BM1368_send_work,
                                        .set_version_mask = BM1368_set_version_mask};
        GLOBAL_STATE->asic_job_frequency_ms = 500; //ms
        GLOBAL_STATE->ASIC_difficulty = BM1368_ASIC_DIFFICULTY;

        GLOBAL_STATE->ASIC_functions = ASIC_functions;
    } else if (strcmp(GLOBAL_STATE->asic_model_str, "BM1397") == 0) {
        ESP_LOGI(TAG, "ASIC: %dx BM1397 (%" PRIu64 " cores)", GLOBAL_STATE->asic_count, BM1397_SMALL_CORE_COUNT);
        GLOBAL_STATE->asic_model = ASIC_BM1397;
        AsicFunctions ASIC_functions = {.init_fn = BM1397_init,
                                        .receive_result_fn = BM1397_proccess_work,
                                        .set_max_baud_fn = BM1397_set_max_baud,
                                        .set_difficulty_mask_fn = BM1397_set_job_difficulty_mask,
                                        .send_work_fn = BM1397_send_work,
                                        .set_version_mask = BM1397_set_version_mask};
        GLOBAL_STATE->asic_job_frequency_ms = (NONCE_SPACE / (double) (GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value * BM1397_SMALL_CORE_COUNT * 1000)) / (double) GLOBAL_STATE->asic_count;
        GLOBAL_STATE->ASIC_difficulty = BM1397_ASIC_DIFFICULTY;

        GLOBAL_STATE->ASIC_functions = ASIC_functions;
    } else {
        ESP_LOGE(TAG, "Invalid ASIC model");
        AsicFunctions ASIC_functions = {.init_fn = NULL,
                                        .receive_result_fn = NULL,
                                        .set_max_baud_fn = NULL,
                                        .set_difficulty_mask_fn = NULL,
                                        .send_work_fn = NULL};
        GLOBAL_STATE->ASIC_functions = ASIC_functions;
        return ESP_FAIL;
    }

    return ESP_OK;
}
