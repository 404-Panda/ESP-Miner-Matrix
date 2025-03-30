#include "nvs_config.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

// Developer Notes:
// This constant defines the namespace used for all NVS operations in this module, set to "main". A namespace groups
// related key-value pairs in NVS, preventing naming conflicts with other modules or applications. It’s used consistently
// across all functions to access configuration data like Wi-Fi credentials, ASIC settings, or device parameters, ensuring
// organized storage in the flash memory of the ESP device.
#define NVS_CONFIG_NAMESPACE "main"

// Developer Notes:
// This static constant defines the logging tag for the NVS configuration module, used with ESP_LOG macros to identify
// log messages related to reading and writing NVS data. It aids in debugging configuration operations, especially when
// errors occur during NVS access or updates, providing visibility into the system’s persistent storage behavior.
static const char * TAG = "nvs_config";

// Developer Notes:
// This function retrieves a string value from NVS for a given key, returning a dynamically allocated copy of the value
// or a duplicate of the default_value if retrieval fails. It opens the "main" namespace in read-only mode, determines the
// required size with nvs_get_str, allocates memory, and fetches the string. If any step fails (e.g., NVS not initialized,
// key not found), it falls back to strdup(default_value). The returned string must be freed by the caller. This function
// is essential for loading configuration data like Wi-Fi SSIDs or pool URLs, ensuring robust error handling with defaults.
char * nvs_config_get_string(const char * key, const char * default_value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return strdup(default_value);
    }

    size_t size = 0;
    err = nvs_get_str(handle, key, NULL, &size);

    if (err != ESP_OK) {
        nvs_close(handle);
        return strdup(default_value);
    }

    char * out = malloc(size);
    err = nvs_get_str(handle, key, out, &size);

    if (err != ESP_OK) {
        free(out);
        nvs_close(handle);
        return strdup(default_value);
    }

    nvs_close(handle);
    return out;
}

// Developer Notes:
// This function writes a string value to NVS under the specified key in the "main" namespace. It opens NVS in read-write
// mode, sets the value using nvs_set_str, and logs a warning if either operation fails (e.g., due to insufficient space
// or NVS not initialized). It does not return a status, silently failing on errors after logging. This function is used
// to persistently store configuration changes, such as updated Wi-Fi credentials or device settings, ensuring they
// survive reboots. The lack of a return value assumes the caller can tolerate silent failures, relying on logs for diagnostics.
void nvs_config_set_string(const char * key, const char * value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open nvs");
        return;
    }

    err = nvs_set_str(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not write nvs key: %s, value: %s", key, value);
    }

    nvs_close(handle);
}

// Developer Notes:
// This function retrieves a 16-bit unsigned integer (uint16_t) from NVS for a given key, returning the stored value or
// the default_value if retrieval fails. It opens the "main" namespace in read-only mode, fetches the value with nvs_get_u16,
// and closes the handle. If NVS access or key retrieval fails (e.g., key not set), it returns the default. This function
// is used for settings like ASIC frequency or port numbers, providing a simple, fail-safe way to access numeric configuration
// data with a fallback mechanism.
uint16_t nvs_config_get_u16(const char * key, const uint16_t default_value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return default_value;
    }

    uint16_t out;
    err = nvs_get_u16(handle, key, &out);
    nvs_close(handle);

    if (err != ESP_OK) {
        return default_value;
    }
    return out;
}

// Developer Notes:
// This function writes a 16-bit unsigned integer (uint16_t) to NVS under the specified key in the "main" namespace. It
// opens NVS in read-write mode, sets the value using nvs_set_u16, and logs a warning if either operation fails (e.g., due
// to write errors or NVS not initialized). It does not return a status, silently failing after logging. This function is
// used to store small numeric settings like overheat mode or voltage values, ensuring persistence across reboots with
// basic error reporting via logs.
void nvs_config_set_u16(const char * key, const uint16_t value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open nvs");
        return;
    }

    err = nvs_set_u16(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not write nvs key: %s, value: %u", key, value);
    }

    nvs_close(handle);
}

// Developer Notes:
// This function retrieves a 64-bit unsigned integer (uint64_t) from NVS for a given key, returning the stored value or
// the default_value if retrieval fails. It opens the "main" namespace in read-only mode, fetches the value with nvs_get_u64,
// and closes the handle. If NVS access or key retrieval fails (e.g., key not found), it returns the default. This function
// is used for large numeric settings like best nonce difficulty, offering a reliable way to access critical configuration
// data with a fallback to a default value.
uint64_t nvs_config_get_u64(const char * key, const uint64_t default_value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return default_value;
    }

    uint64_t out;
    err = nvs_get_u64(handle, key, &out);

    if (err != ESP_OK) {
        nvs_close(handle);
        return default_value;
    }

    nvs_close(handle);
    return out;
}

// Developer Notes:
// This function writes a 64-bit unsigned integer (uint64_t) to NVS under the specified key in the "main" namespace. It
// opens NVS in read-write mode, sets the value using nvs_set_u64, and logs a warning if either operation fails (e.g., due
// to insufficient space or NVS errors). It does not return a status, silently failing after logging. This function is
// used to store large numeric values like best difficulty metrics, ensuring they persist across reboots with basic error
// notification via logs for debugging purposes.
void nvs_config_set_u64(const char * key, const uint64_t value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open nvs");
        return;
    }

    err = nvs_set_u64(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not write nvs key: %s, value: %llu", key, value);
    }
    nvs_close(handle);
}
