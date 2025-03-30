#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

// #include "protocol_examples_common.h"
#include "main.h"

#include "asic_result_task.h"
#include "asic_task.h"
#include "create_jobs_task.h"
#include "esp_netif.h"
#include "system.h"
#include "http_server.h"
#include "nvs_config.h"
#include "serial.h"
#include "stratum_task.h"
#include "i2c_bitaxe.h"
#include "adc.h"
#include "nvs_device.h"
#include "self_test.h"

// Developer Notes:
// This static global variable defines the initial state of the Bitaxe system, stored in RAM and accessible across tasks.
// Key fields include:
// - extranonce_str: Pointer to the Stratum-provided extranonce (initially NULL).
// - extranonce_2_len: Length of the client-generated extranonce2 (initially 0).
// - abandon_work: Flag to abandon current mining work (initially 0).
// - version_mask: Version rolling mask for ASIC jobs (initially 0).
// - ASIC_initalized: Flag indicating ASIC initialization status (initially false).
// It serves as the central state repository, coordinating data between tasks like Stratum communication, ASIC control,
// and system management, initialized here and populated during runtime.
static GlobalState GLOBAL_STATE = {
    .extranonce_str = NULL, 
    .extranonce_2_len = 0, 
    .abandon_work = 0, 
    .version_mask = 0,
    .ASIC_initalized = false
};

// Developer Notes:
// This static constant defines the logging tag for the main application, used with ESP_LOG macros to identify log
// messages related to system startup, Wi-Fi connectivity, and task initialization. It provides a consistent identifier
// for debugging the Bitaxe’s core operations, from hardware setup to mining task execution.
static const char * TAG = "bitaxe";

// Developer Notes:
// This is the main entry point for the Bitaxe firmware, executed on ESP32 boot. It initializes hardware (I2C, ADC, NVS),
// loads configuration from NVS into GLOBAL_STATE, and sets up Wi-Fi connectivity. It optionally runs a self-test if
// triggered by a boot button or NVS flag. After successful Wi-Fi connection, it initializes peripherals, starts the
// power management task, launches the HTTP server, and—if an ASIC is configured—starts mining tasks (Stratum, job
// creation, ASIC control, and result processing). It handles Wi-Fi failures by entering a wait loop, freeing resources,
// and logging key events. This function orchestrates the entire system startup, transitioning from boot to active mining.
void app_main(void)
{
    ESP_LOGI(TAG, "Welcome to the bitaxe - hack the planet!");

    ESP_ERROR_CHECK(i2c_bitaxe_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    vTaskDelay(100 / portTICK_PERIOD_MS);

    ADC_init();

    if (NVSDevice_init() != ESP_OK){
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    if (NVSDevice_parse_config(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse NVS config");
        return;
    }

    bool pressed = gpio_get_level(CONFIG_GPIO_BUTTON_BOOT) == 0;
    if (should_test(&GLOBAL_STATE) || pressed) {
        self_test((void *) &GLOBAL_STATE);
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);

    char * wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, WIFI_SSID);
    char * wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS, WIFI_PASS);
    char * hostname  = nvs_config_get_string(NVS_CONFIG_HOSTNAME, HOSTNAME);

    strncpy(GLOBAL_STATE.SYSTEM_MODULE.ssid, wifi_ssid, sizeof(GLOBAL_STATE.SYSTEM_MODULE.ssid));
    GLOBAL_STATE.SYSTEM_MODULE.ssid[sizeof(GLOBAL_STATE.SYSTEM_MODULE.ssid)-1] = 0;

    wifi_init(wifi_ssid, wifi_pass, hostname, GLOBAL_STATE.SYSTEM_MODULE.ip_addr_str);

    generate_ssid(GLOBAL_STATE.SYSTEM_MODULE.ap_ssid);

    SYSTEM_init_peripherals(&GLOBAL_STATE);

    xTaskCreate(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10, NULL);

    start_rest_server((void *) &GLOBAL_STATE);
    EventBits_t result_bits = wifi_connect();

    if (result_bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID: %s", wifi_ssid);
        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "Connected!", 20);
    } else if (result_bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", wifi_ssid);

        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "Failed to connect", 20);
        ESP_LOGI(TAG, "Finished, waiting for user input.");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        strncpy(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, "unexpected error", 20);
        ESP_LOGI(TAG, "Finished, waiting for user input.");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    free(wifi_ssid);
    free(wifi_pass);
    free(hostname);

    GLOBAL_STATE.new_stratum_version_rolling_msg = false;

    if (GLOBAL_STATE.ASIC_functions.init_fn != NULL) {
        wifi_softap_off();

        queue_init(&GLOBAL_STATE.stratum_queue);
        queue_init(&GLOBAL_STATE.ASIC_jobs_queue);

        SERIAL_init();
        (*GLOBAL_STATE.ASIC_functions.init_fn)(GLOBAL_STATE.POWER_MANAGEMENT_MODULE.frequency_value, GLOBAL_STATE.asic_count);
        SERIAL_set_baud((*GLOBAL_STATE.ASIC_functions.set_max_baud_fn)());
        SERIAL_clear_buffer();

        GLOBAL_STATE.ASIC_initalized = true;

        xTaskCreate(stratum_task, "stratum admin", 8192, (void *) &GLOBAL_STATE, 5, NULL);
        xTaskCreate(create_jobs_task, "stratum miner", 8192, (void *) &GLOBAL_STATE, 10, NULL);
        xTaskCreate(ASIC_task, "asic", 8192, (void *) &GLOBAL_STATE, 10, NULL);
        xTaskCreate(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL);
    }
}

// Developer Notes:
// This function updates the Wi-Fi status string in the SYSTEM_MODULE of GLOBAL_STATE based on the provided wifi_status_t
// enum value, retry count, and reason code. It handles three cases:
// - WIFI_CONNECTING: Sets status to "Connecting...".
// - WIFI_CONNECTED: Sets status to "Connected!".
// - WIFI_RETRYING: Sets status based on reason (e.g., "No AP found", "Password error") with retry count.
// It logs a warning for unknown statuses. This function is called by the Wi-Fi connection logic to provide real-time
// feedback on connection attempts, displayed via the UI or logs, enhancing user awareness of network state during mining.
void MINER_set_wifi_status(wifi_status_t status, int retry_count, int reason)
{
    switch(status) {
        case WIFI_CONNECTING:
            snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Connecting...");
            return;
        case WIFI_CONNECTED:
            snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Connected!");
            return;
        case WIFI_RETRYING:
            switch(reason) {
                case 201:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "No AP found (%d)", retry_count);
                    return;
                case 15:
                case 205:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Password error (%d)", retry_count);
                    return;
                default:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Error %d (%d)", reason, retry_count);
                    return;
            }
    }
    ESP_LOGW(TAG, "Unknown status: %d", status);
}

// Developer Notes:
// This function sets the ap_enabled flag in the SYSTEM_MODULE of GLOBAL_STATE based on whether the Wi-Fi SoftAP (access
// point) mode is enabled. It’s called by the Wi-Fi management logic to reflect the current state of the AP, which might
// be toggled for configuration purposes (e.g., captive portal setup). This simple flag update ensures the system tracks
// whether it’s operating as an AP, affecting network behavior and UI feedback during mining operations or initial setup.
void MINER_set_ap_status(bool enabled) {
    GLOBAL_STATE.SYSTEM_MODULE.ap_enabled = enabled;
}
