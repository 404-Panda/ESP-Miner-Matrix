#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

#include "system.h"
#include "i2c_bitaxe.h"
#include "EMC2101.h"
#include "INA260.h"
#include "adc.h"
#include "connect.h"
#include "nvs_config.h"
#include "display.h"
#include "input.h"
#include "screen.h"
#include "vcore.h"

static const char * TAG = "SystemModule";

// Developer Notes:
// This static function prototype declares _suffix_string, which converts a uint64_t value into a human-readable string
// with an appropriate suffix (e.g., k, M, G) for units like hashrate or difficulty. It’s defined later in the file and
// used internally by system functions to format metrics for display or logging. The declaration here ensures it’s
// recognized by earlier functions like SYSTEM_init_system.
static void _suffix_string(uint64_t, char *, size_t, int);

// Developer Notes:
// These are local function prototypes for static functions defined later in the file. They ensure proper function
// recognition within the module:
// - ensure_overheat_mode_config: Ensures the overheat mode setting exists in NVS (non-volatile storage).
// - _check_for_best_diff: Updates the best difficulty metrics when a new nonce is found.
// - _suffix_string (repeated): Already declared above, formats large numbers with suffixes.
static esp_err_t ensure_overheat_mode_config();
static void _check_for_best_diff(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id);
static void _suffix_string(uint64_t val, char * buf, size_t bufsiz, int sigdigits);

// Developer Notes:
// This global static pointer holds the network interface handle for the Wi-Fi station (STA) mode. It’s initialized in
// SYSTEM_init_peripherals and used to manage network connectivity, reflecting the system’s Wi-Fi state. It’s a key
// component for tracking network status in a mining device that relies on internet access to communicate with a pool.
static esp_netif_t * netif;

// Developer Notes:
// This function initializes the system module within the global state structure, setting up initial values for mining
// metrics, configuration, and status tracking. It resets counters (e.g., shares_accepted, shares_rejected), loads
// persistent settings like best_nonce_diff and pool details from NVS (with defaults from config), and formats difficulty
// strings using _suffix_string. It also sets up timing (start_time) and network-related fields (pool_url, port). This
// is the first step in preparing the system for mining operations, ensuring all state variables are ready before
// hardware initialization or task creation. It’s typically called once at system startup.
void SYSTEM_init_system(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->duration_start = 0;
    module->historical_hashrate_rolling_index = 0;
    module->historical_hashrate_init = 0;
    module->current_hashrate = 0;
    module->screen_page = 0;
    module->shares_accepted = 0;
    module->shares_rejected = 0;
    module->best_nonce_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF, 0);
    module->best_session_nonce_diff = 0;
    module->start_time = esp_timer_get_time();
    module->lastClockSync = 0;
    module->FOUND_BLOCK = false;
    
    module->pool_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
    module->fallback_pool_url = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);

    module->pool_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT);
    module->fallback_pool_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT);

    module->is_using_fallback = false;

    module->overheat_mode = nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
    ESP_LOGI(TAG, "Initial overheat_mode value: %d", module->overheat_mode);

    _suffix_string(module->best_nonce_diff, module->best_diff_string, DIFF_STRING_SIZE, 0);
    _suffix_string(module->best_session_nonce_diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);

    memset(module->ssid, 0, sizeof(module->ssid));
    memset(module->wifi_status, 0, 20);
}

// Developer Notes:
// This function initializes the peripheral hardware components of the mining system based on the device model stored
// in GLOBAL_STATE. It sets up the core voltage regulator (VCORE), fan controller (EMC2101), power monitor (INA260),
// display, input buttons, and screen task, using model-specific configurations. It loads settings like ASIC voltage
// and fan polarity from NVS, applies them, and logs success or failure. A 500ms delay ensures hardware stability before
// proceeding. It also initializes the Wi-Fi network interface (netif) for STA mode. This function is called after
// SYSTEM_init_system to bring the physical components online, preparing the device for mining and user interaction.
void SYSTEM_init_peripherals(GlobalState * GLOBAL_STATE) {
    VCORE_init(GLOBAL_STATE);
    VCORE_set_voltage(nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE, CONFIG_ASIC_VOLTAGE) / 1000.0, GLOBAL_STATE);

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            EMC2101_init(nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 1));
            break;
        case DEVICE_GAMMA:
            EMC2101_init(nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 1));
            EMC2101_set_ideality_factor(EMC2101_IDEALITY_1_0319);
            EMC2101_set_beta_compensation(EMC2101_BETA_11);
            break;
        default:
    }

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            if (GLOBAL_STATE->board_version < 402) {
                INA260_init();
            }
            break;
        case DEVICE_GAMMA:
        default:
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);

    esp_err_t ret = ensure_overheat_mode_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to ensure overheat_mode config");
    }

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_MAX:
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            if (display_init(GLOBAL_STATE) != ESP_OK || !GLOBAL_STATE->SYSTEM_MODULE.is_screen_active) {
                ESP_LOGW(TAG, "OLED init failed!");
            } else {
                ESP_LOGI(TAG, "OLED init success!");
            }
            break;
        default:
    }

    if (input_init(screen_next, toggle_wifi_softap) != ESP_OK) {
        ESP_LOGW(TAG, "Input init failed!");
    }

    if (screen_start(GLOBAL_STATE) != ESP_OK) {
        ESP_LOGW(TAG, "Screen init failed");
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

// Developer Notes:
// This function increments the shares_accepted counter in the system module when a mining share is accepted by the pool.
// It’s a simple notification handler called by the mining task (e.g., after STRATUM_V1_submit_share receives a successful
// response). Tracking accepted shares is crucial for monitoring mining performance and calculating effective hashrate,
// providing feedback on the system’s contribution to the pool. It’s lightweight and thread-safe, assuming GLOBAL_STATE
// is properly managed across tasks.
void SYSTEM_notify_accepted_share(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_accepted++;
}

// Developer Notes:
// This function increments the shares_rejected counter in the system module when a mining share is rejected by the pool.
// Similar to SYSTEM_notify_accepted_share, it’s called by the mining task to track unsuccessful submissions (e.g., stale
// shares or invalid nonces). This metric helps diagnose issues like network latency or ASIC misconfiguration, offering
// insight into system reliability. It’s a straightforward counter update, designed for efficiency in a real-time system.
void SYSTEM_notify_rejected_share(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->shares_rejected++;
}

// Developer Notes:
// This function records the start time of mining activity by setting duration_start to the current system time (via
// esp_timer_get_time). It’s called when the mining task begins processing jobs, marking the beginning of a hashrate
// calculation period. This timestamp is used later in SYSTEM_notify_found_nonce to compute the time-to-find for nonces,
// enabling accurate hashrate estimation. It’s a simple but essential function for performance tracking in the mining loop.
void SYSTEM_notify_mining_started(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->duration_start = esp_timer_get_time();
}

// Developer Notes:
// This function synchronizes the system clock with the ntime value from a mining job, ensuring the device’s time aligns
// with the Bitcoin network. It checks if an hour has passed since the last sync (lastClockSync + 3600s) to avoid frequent
// updates, then sets the system time using settimeofday with the provided ntime (Unix timestamp). This is important for
// validating mining jobs and submitting shares with correct timestamps. It logs the sync event for monitoring and updates
// lastClockSync, making it a key utility for time-sensitive mining operations.
void SYSTEM_notify_new_ntime(GlobalState * GLOBAL_STATE, uint32_t ntime)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if (module->lastClockSync + (60 * 60) > ntime) {
        return;
    }
    ESP_LOGI(TAG, "Syncing clock");
    module->lastClockSync = ntime;
    struct timeval tv;
    tv.tv_sec = ntime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

// Developer Notes:
// This function updates hashrate and difficulty metrics when a nonce is found by the ASIC. It records the difficulty
// (ASIC_difficulty) and timestamp in a rolling history buffer (historical_hashrate and historical_hashrate_time_stamps),
// advancing the index circularly. It calculates the rolling hashrate as (sum of difficulties * 2^32) / time elapsed,
// smoothing it with a weighted average once the buffer is full (HISTORY_LENGTH). The hashrate reflects the device’s
// mining performance in hashes per second. It then calls _check_for_best_diff to update best difficulty records. This
// function is central to performance monitoring, providing real-time feedback on mining efficiency and success.
void SYSTEM_notify_found_nonce(GlobalState * GLOBAL_STATE, double found_diff, uint8_t job_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    module->historical_hashrate[module->historical_hashrate_rolling_index] = GLOBAL_STATE->ASIC_difficulty;
    module->historical_hashrate_time_stamps[module->historical_hashrate_rolling_index] = esp_timer_get_time();

    module->historical_hashrate_rolling_index = (module->historical_hashrate_rolling_index + 1) % HISTORY_LENGTH;

    if (module->historical_hashrate_init < HISTORY_LENGTH) {
        module->historical_hashrate_init++;
    } else {
        module->duration_start =
            module->historical_hashrate_time_stamps[(module->historical_hashrate_rolling_index + 1) % HISTORY_LENGTH];
    }
    double sum = 0;
    for (int i = 0; i < module->historical_hashrate_init; i++) {
        sum += module->historical_hashrate[i];
    }

    double duration = (double) (esp_timer_get_time() - module->duration_start) / 1000000;

    double rolling_rate = (sum * 4294967296) / (duration * 1000000000);
    if (module->historical_hashrate_init < HISTORY_LENGTH) {
        module->current_hashrate = rolling_rate;
    } else {
        module->current_hashrate = ((module->current_hashrate * 9) + rolling_rate) / 10;
    }

    _check_for_best_diff(GLOBAL_STATE, found_diff, job_id);
}

// Developer Notes:
// This static function calculates the Bitcoin network difficulty from an nBits value (a compact representation of the
// target hash). It extracts the mantissa (23 bits) and exponent (8 bits) from nBits, computes the target as mantissa * 256^(exponent-3),
// and derives difficulty as (2^208 * 65535) / target. This reflects the network’s mining difficulty, where a lower target
// means higher difficulty. It’s used in _check_for_best_diff to compare a found nonce’s difficulty against the network’s,
// determining if a block was found. This is a key calculation for validating mining success beyond pool shares.
static double _calculate_network_difficulty(uint32_t nBits)
{
    uint32_t mantissa = nBits & 0x007fffff;
    uint8_t exponent = (nBits >> 24) & 0xff;

    double target = (double) mantissa * pow(256, (exponent - 3));

    double difficulty = (pow(2, 208) * 65535) / target;

    return difficulty;
}

// Developer Notes:
// This static function updates the best difficulty metrics (session and all-time) when a new nonce is found. It compares
// the found difficulty (diff) against the session best (best_session_nonce_diff), updating it and its string representation
// if higher. If it exceeds the all-time best (best_nonce_diff), it updates that too, persists it to NVS, and checks if it
// surpasses the network difficulty (via _calculate_network_difficulty) to flag a block find (FOUND_BLOCK). The function
// uses _suffix_string to format difficulty strings for display. It’s a critical evaluation step, tracking mining achievements
// and detecting block discoveries.
static void _check_for_best_diff(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;

    if ((uint64_t) diff > module->best_session_nonce_diff) {
        module->best_session_nonce_diff = (uint64_t) diff;
        _suffix_string((uint64_t) diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);
    }

    if ((uint64_t) diff <= module->best_nonce_diff) {
        return;
    }
    module->best_nonce_diff = (uint64_t) diff;

    nvs_config_set_u64(NVS_CONFIG_BEST_DIFF, module->best_nonce_diff);

    _suffix_string((uint64_t) diff, module->best_diff_string, DIFF_STRING_SIZE, 0);

    double network_diff = _calculate_network_difficulty(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->target);
    if (diff > network_diff) {
        module->FOUND_BLOCK = true;
        ESP_LOGI(TAG, "FOUND BLOCK!!!!!!!!!!!!!!!!!!!!!! %f > %f", diff, network_diff);
    }
    ESP_LOGI(TAG, "Network diff: %f", network_diff);
}

// Developer Notes:
// This static function converts a uint64_t value (e.g., difficulty or hashrate) into a truncated string with an appropriate
// suffix (k, M, G, T, P, E) based on its magnitude. It divides the value by powers of 1000, selects a suffix, and formats
// the result as either a decimal (e.g., "1.23G") or integer (e.g., "123k") depending on the scale. The sigdigits parameter
// controls precision; if 0, it uses a compact format (3 significant digits). The output is written to a provided buffer with
// bounds checking. This utility is used throughout the system module to present large numbers in a readable form for logs
// or display, enhancing user feedback.
static void _suffix_string(uint64_t val, char * buf, size_t bufsiz, int sigdigits)
{
    const double dkilo = 1000.0;
    const uint64_t kilo = 1000ull;
    const uint64_t mega = 1000000ull;
    const uint64_t giga = 1000000000ull;
    const uint64_t tera = 1000000000000ull;
    const uint64_t peta = 1000000000000000ull;
    const uint64_t exa = 1000000000000000000ull;
    char suffix[2] = "";
    bool decimal = true;
    double dval;

    if (val >= exa) {
        val /= peta;
        dval = (double) val / dkilo;
        strcpy(suffix, "E");
    } else if (val >= peta) {
        val /= tera;
        dval = (double) val / dkilo;
        strcpy(suffix, "P");
    } else if (val >= tera) {
        val /= giga;
        dval = (double) val / dkilo;
        strcpy(suffix, "T");
    } else if (val >= giga) {
        val /= mega;
        dval = (double) val / dkilo;
        strcpy(suffix, "G");
    } else if (val >= mega) {
        val /= kilo;
        dval = (double) val / dkilo;
        strcpy(suffix, "M");
    } else if (val >= kilo) {
        dval = (double) val / dkilo;
        strcpy(suffix, "k");
    } else {
        dval = val;
        decimal = false;
    }

    if (!sigdigits) {
        if (decimal)
            snprintf(buf, bufsiz, "%.3g%s", dval, suffix);
        else
            snprintf(buf, bufsiz, "%d%s", (unsigned int) dval, suffix);
    } else {
        int ndigits = sigdigits - 1 - (dval > 0.0 ? floor(log10(dval)) : 0);
        snprintf(buf, bufsiz, "%*.*f%s", sigdigits + 1, ndigits, dval, suffix);
    }
}

// Developer Notes:
// This static function ensures the overheat_mode configuration exists in NVS, setting it to 0 if absent. It attempts to
// read the value; if it gets UINT16_MAX (indicating no value or read failure), it writes the default (0) and logs the
// action. Otherwise, it logs the existing value. This ensures the system has a valid overheat mode setting (e.g., for
// thermal management) at startup, preventing undefined behavior in temperature-sensitive mining hardware. It always
// returns ESP_OK, assuming NVS operations succeed after setting the default.
static esp_err_t ensure_overheat_mode_config() {
    uint16_t overheat_mode = nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, UINT16_MAX);

    if (overheat_mode == UINT16_MAX) {
        nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
        ESP_LOGI(TAG, "Default value for overheat_mode set to 0");
    } else {
        ESP_LOGI(TAG, "Existing overheat_mode value: %d", overheat_mode);
    }

    return ESP_OK;
}
