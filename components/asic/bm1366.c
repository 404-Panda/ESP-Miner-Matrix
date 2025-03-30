#include "bm1366.h"
#include "crc.h"
#include "global_state.h"
#include "serial.h"
#include "utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPIO_ASIC_RESET CONFIG_GPIO_ASIC_RESET
#define TYPE_JOB 0x20
#define TYPE_CMD 0x40
#define GROUP_SINGLE 0x00
#define GROUP_ALL 0x10
#define CMD_JOB 0x01
#define CMD_SETADDRESS 0x00
#define CMD_WRITE 0x01
#define CMD_READ 0x02
#define CMD_INACTIVE 0x03
#define RESPONSE_CMD 0x00
#define RESPONSE_JOB 0x80
#define SLEEP_TIME 20
#define FREQ_MULT 25.0
#define CLOCK_ORDER_CONTROL_0 0x80
#define CLOCK_ORDER_CONTROL_1 0x84
#define ORDERED_CLOCK_ENABLE 0x20
#define CORE_REGISTER_CONTROL 0x3C
#define PLL3_PARAMETER 0x68
#define FAST_UART_CONFIGURATION 0x28
#define TICKET_MASK 0x14
#define MISC_CONTROL 0x18
#define BM1366_TIMEOUT_MS 10000
#define BM1366_TIMEOUT_THRESHOLD 2

typedef struct __attribute__((__packed__))
{
    uint8_t preamble[2];
    uint32_t nonce;
    uint8_t midstate_num;
    uint8_t job_id;
    uint16_t version;
    uint8_t crc;
} asic_result;

static float current_frequency = 56.25;
static const char * TAG = "bm1366Module";
static uint8_t asic_response_buffer[SERIAL_BUF_SIZE];
static task_result result;

// Developer Notes:
// This function serves as the fundamental communication interface between the microcontroller and the BM1366 ASIC.
// It constructs and transmits packets over the serial interface, supporting two distinct types: JOB_PACKET for sending
// mining jobs and CMD_PACKET for configuration commands. The packet structure includes a fixed preamble (0x55 0xAA)
// for synchronization, a header byte defining the packet type and target, a length byte, the data payload, and a CRC
// checksum (16-bit for jobs, 5-bit for commands) to ensure data integrity. The function dynamically allocates memory
// to accommodate varying payload sizes, making it versatile for different operations. The debug flag enables detailed
// logging of transmitted packets, which is invaluable for diagnosing communication issues during development or
// deployment. This is a low-level building block used by higher-level functions for all ASIC interactions.
static void _send_BM1366(uint8_t header, uint8_t * data, uint8_t data_len, bool debug)
{
    packet_type_t packet_type = (header & TYPE_JOB) ? JOB_PACKET : CMD_PACKET;
    uint8_t total_length = (packet_type == JOB_PACKET) ? (data_len + 6) : (data_len + 5);
    unsigned char * buf = malloc(total_length);
    buf[0] = 0x55;
    buf[1] = 0xAA;
    buf[2] = header;
    buf[3] = (packet_type == JOB_PACKET) ? (data_len + 4) : (data_len + 3);
    memcpy(buf + 4, data, data_len);
    if (packet_type == JOB_PACKET) {
        uint16_t crc16_total = crc16_false(buf + 2, data_len + 2);
        buf[4 + data_len] = (crc16_total >> 8) & 0xFF;
        buf[5 + data_len] = crc16_total & 0xFF;
    } else {
        buf[4 + data_len] = crc5(buf + 2, data_len + 2);
    }
    SERIAL_send(buf, total_length, debug);
    free(buf);
}

// Developer Notes:
// This utility function provides a straightforward way to send pre-constructed packets when the caller has already
// formatted the entire message, including preamble and CRC. Unlike _send_BM1366, it doesn’t build the packet structure,
// making it suitable for initialization sequences or fixed commands where the exact byte sequence is predefined.
// It allocates a temporary buffer, copies the input data, and sends it via the serial interface with debugging enabled
// by default (BM1366_SERIALTX_DEBUG). This approach reduces overhead for simple transmissions and is used extensively
// in the initialization process where precise control over packet contents is required. Memory is managed dynamically
// and freed after transmission to prevent leaks.
static void _send_simple(uint8_t * data, uint8_t total_length)
{
    unsigned char * buf = malloc(total_length);
    memcpy(buf, data, total_length);
    SERIAL_send(buf, total_length, BM1366_SERIALTX_DEBUG);
    free(buf);
}

// Developer Notes:
// This function broadcasts a command to all ASIC chips in the chain, instructing them to enter an inactive state.
// It’s a critical part of the initialization or reset process, ensuring all chips are synchronized and ready for
// subsequent configuration. The command uses the TYPE_CMD and GROUP_ALL flags to target every chip, combined with
// CMD_INACTIVE to specify the action. The payload (read_address) is a minimal 2-byte array set to zeros, as the
// inactive command doesn’t require additional data. Debug logging is enabled to monitor this operation, which helps
// verify that the entire chain responds correctly. This function is typically called before assigning addresses or
// applying specific configurations to ensure a clean starting point.
static void _send_chain_inactive(void)
{
    unsigned char read_address[2] = {0x00, 0x00};
    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_INACTIVE), read_address, 2, BM1366_SERIALTX_DEBUG);
}

// Developer Notes:
// This function assigns a unique address to a single ASIC chip within the chain, enabling targeted communication
// with individual chips over the shared serial bus. It uses the TYPE_CMD and GROUP_SINGLE flags to address a specific
// chip, with CMD_SETADDRESS indicating the intent to set its address. The chipAddr parameter is the desired address,
// placed in the first byte of a 2-byte payload, with the second byte zeroed out as padding. This addressing scheme is
// essential for managing multiple chips in a daisy-chain configuration, allowing the system to differentiate between
// them for commands or job assignments. Debug output is enabled to confirm successful address assignment, which is
// a key step in the initialization sequence.
static void _set_chip_address(uint8_t chipAddr)
{
    unsigned char read_address[2] = {chipAddr, 0x00};
    _send_BM1366((TYPE_CMD | GROUP_SINGLE | CMD_SETADDRESS), read_address, 2, BM1366_SERIALTX_DEBUG);
}

// Developer Notes:
// This function configures the version rolling mask for all ASIC chips, a feature critical for supporting the stratum
// mining protocol. Version rolling allows miners to generate multiple block header variations from a single job,
// increasing efficiency by exploring more nonce space. The function takes a 32-bit version_mask, shifts it right by
// 13 bits to extract the relevant portion, and splits it into two bytes (version_byte0 and version_byte1). These
// bytes are then embedded in a 6-byte command payload, prefixed with register address 0xA4 and control bytes (0x90, 0x00).
// The command is broadcast to all chips (GROUP_ALL) using TYPE_CMD and CMD_WRITE, ensuring uniform configuration.
// This setup is typically applied during initialization to enable stratum compatibility across the chain.
void BM1366_set_version_mask(uint32_t version_mask) 
{
    int versions_to_roll = version_mask >> 13;
    uint8_t version_byte0 = (versions_to_roll >> 8);
    uint8_t version_byte1 = (versions_to_roll & 0xFF); 
    uint8_t version_cmd[] = {0x00, 0xA4, 0x90, 0x00, version_byte0, version_byte1};
    _send_BM1366(TYPE_CMD | GROUP_ALL | CMD_WRITE, version_cmd, 6, BM1366_SERIALTX_DEBUG);
}

// Developer Notes:
// This function programs the hash frequency for all ASIC chips by configuring their PLL (Phase-Locked Loop) parameters.
// The BM1366 uses a 25 MHz reference clock, and the target frequency is achieved by adjusting the feedback divider
// (fb_divider), reference divider (ref_divider), and two post-dividers (post_divider1, post_divider2). The function
// iterates through possible divider combinations within hardware constraints (e.g., fb_divider between 144 and 235)
// to find the set that minimizes the difference between the target_freq and the achievable frequency. If no valid
// combination is found, it defaults to 200 MHz with predefined settings. The calculated values are packed into a
// 6-byte command buffer targeting register 0x08 (PLL0_PARAMETER) and broadcast to all chips (GROUP_ALL). Logging
// displays both the requested and actual frequencies, providing visibility into the tuning process. This is a key
// function for controlling the hash rate and power consumption of the ASIC chain.
void BM1366_send_hash_frequency(float target_freq)
{
    unsigned char freqbuf[9] = {0x00, 0x08, 0x40, 0xA0, 0x02, 0x41};
    float newf = 200.0;
    uint8_t fb_divider = 0;
    uint8_t post_divider1 = 0, post_divider2 = 0;
    uint8_t ref_divider = 0;
    float min_difference = 10;

    for (uint8_t refdiv_loop = 2; refdiv_loop > 0 && fb_divider == 0; refdiv_loop--) {
        for (uint8_t postdiv1_loop = 7; postdiv1_loop > 0 && fb_divider == 0; postdiv1_loop--) {
            for (uint8_t postdiv2_loop = 1; postdiv2_loop < postdiv1_loop && fb_divider == 0; postdiv2_loop++) {
                int temp_fb_divider = round(((float) (postdiv1_loop * postdiv2_loop * target_freq * refdiv_loop) / 25.0));
                if (temp_fb_divider >= 144 && temp_fb_divider <= 235) {
                    float temp_freq = 25.0 * (float) temp_fb_divider / (float) (refdiv_loop * postdiv2_loop * postdiv1_loop);
                    float freq_diff = fabs(target_freq - temp_freq);
                    if (freq_diff < min_difference) {
                        fb_divider = temp_fb_divider;
                        post_divider1 = postdiv1_loop;
                        post_divider2 = postdiv2_loop;
                        ref_divider = refdiv_loop;
                        min_difference = freq_diff;
                        break;
                    }
                }
            }
        }
    }

    if (fb_divider == 0) {
        puts("Finding dividers failed, using default value (200Mhz)");
    } else {
        newf = 25.0 * (float) (fb_divider) / (float) (ref_divider * post_divider1 * post_divider2);
        freqbuf[3] = fb_divider;
        freqbuf[4] = ref_divider;
        freqbuf[5] = (((post_divider1 - 1) & 0xf) << 4) + ((post_divider2 - 1) & 0xf);
        if (fb_divider * 25 / (float) ref_divider >= 2400) {
            freqbuf[2] = 0x50;
        }
    }

    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_WRITE), freqbuf, 6, BM1366_SERIALTX_DEBUG);
    ESP_LOGI(TAG, "Setting Frequency to %.2fMHz (%.2f)", target_freq, newf);
}

// Developer Notes:
// This function implements a controlled frequency adjustment process to transition the ASIC from its current
// frequency to a target frequency safely. Abrupt frequency changes can destabilize the PLL or overstress the
// hardware, so this ramps the frequency in 6.25 MHz steps with 100ms delays between adjustments. It first aligns
// the current frequency to a step boundary (using ceil or floor based on direction), then iteratively applies
// smaller steps as needed until reaching the target. Each step calls BM1366_send_hash_frequency to update the PLL,
// and the delay ensures stability. This gradual approach is particularly important during initialization or when
// adapting to new mining conditions, balancing performance with reliability. The final call ensures the exact
// target frequency is set.
static void do_frequency_ramp_up(float target_frequency) {
    float step = 6.25;
    float current = current_frequency;
    float target = target_frequency;
    float direction = (target > current) ? step : -step;

    if (fmod(current, step) != 0) {
        float next_dividable;
        if (direction > 0) {
            next_dividable = ceil(current / step) * step;
        } else {
            next_dividable = floor(current / step) * step;
        }
        current = next_dividable;
        BM1366_send_hash_frequency(current);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    while ((direction > 0 && current < target) || (direction < 0 && current > target)) {
        float next_step = fmin(fabs(direction), fabs(target - current));
        current += direction > 0 ? next_step : -next_step;
        BM1366_send_hash_frequency(current);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    BM1366_send_hash_frequency(target);
    return;
}

// Developer Notes:
// This function handles the detailed initialization of the BM1366 ASIC chain, invoked by BM1366_init. It’s a complex
// sequence that sets up version rolling, detects the number of chips, assigns addresses, configures registers, and
// ramps up the operating frequency. It begins by applying the version mask three times for redundancy, then sends a
// probe command (init3) and counts responses to determine the actual number of chips, logging the result against
// the expected count. A series of predefined commands (init4, init5, etc.) configure registers like 0xA8 (misc control)
// and 0x18 (misc control), followed by deactivating the chain and assigning evenly spaced addresses across the 256-
// address space. Per-chip configurations (e.g., registers 0xA8, 0x18, 0x3C) are applied individually, and the frequency
// is ramped up to the target. Finally, it sets the hash counting range (register 0x10) and applies a final version mask.
// The function returns the detected chip count, providing feedback on the chain’s status.
static uint8_t _send_init(uint64_t frequency, uint16_t asic_count)
{
    for (int i = 0; i < 3; i++) {
        BM1366_set_version_mask(STRATUM_DEFAULT_VERSION_MASK);
    }
    unsigned char init3[7] = {0x55, 0xAA, 0x52, 0x05, 0x00, 0x00, 0x0A};
    _send_simple(init3, 7);

    int chip_counter = 0;
    while (true) {
        if(SERIAL_rx(asic_response_buffer, 11, 1000) > 0) {
            chip_counter++;
        } else {
            break;
        }
    }
    ESP_LOGI(TAG, "%i chip(s) detected on the chain, expected %i", chip_counter, asic_count);

    unsigned char init4[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0xA8, 0x00, 0x07, 0x00, 0x00, 0x03};
    _send_simple(init4, 11);
    unsigned char init5[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x18, 0xFF, 0x0F, 0xC1, 0x00, 0x00};
    _send_simple(init5, 11);
    _send_chain_inactive();

    uint8_t address_interval = (uint8_t) (256 / chip_counter);
    for (uint8_t i = 0; i < chip_counter; i++) {
        _set_chip_address(i * address_interval);
    }

    unsigned char init135[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x3C, 0x80, 0x00, 0x85, 0x40, 0x0C};
    _send_simple(init135, 11);
    unsigned char init136[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x3C, 0x80, 0x00, 0x80, 0x20, 0x19};
    _send_simple(init136, 11);
    BM1366_set_job_difficulty_mask(BM1366_ASIC_DIFFICULTY);

    unsigned char init138[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x54, 0x00, 0x00, 0x00, 0x03, 0x1D};
    _send_simple(init138, 11);
    unsigned char init139[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x58, 0x02, 0x11, 0x11, 0x11, 0x06};
    _send_simple(init139, 11);
    unsigned char init171[11] = {0x55, 0xAA, 0x41, 0x09, 0x00, 0x2C, 0x00, 0x7C, 0x00, 0x03, 0x03};
    _send_simple(init171, 11);

    for (uint8_t i = 0; i < chip_counter; i++) {
        unsigned char set_a8_register[6] = {i * address_interval, 0xA8, 0x00, 0x07, 0x01, 0xF0};
        _send_BM1366((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), set_a8_register, 6, BM1366_SERIALTX_DEBUG);
        unsigned char set_18_register[6] = {i * address_interval, 0x18, 0xF0, 0x00, 0xC1, 0x00};
        _send_BM1366((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), set_18_register, 6, BM1366_SERIALTX_DEBUG);
        unsigned char set_3c_register_first[6] = {i * address_interval, 0x3C, 0x80, 0x00, 0x85, 0x40};
        _send_BM1366((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), set_3c_register_first, 6, BM1366_SERIALTX_DEBUG);
        unsigned char set_3c_register_second[6] = {i * address_interval, 0x3C, 0x80, 0x00, 0x80, 0x20};
        _send_BM1366((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), set_3c_register_second, 6, BM1366_SERIALTX_DEBUG);
        unsigned char set_3c_register_third[6] = {i * address_interval, 0x3C, 0x80, 0x00, 0x82, 0xAA};
        _send_BM1366((TYPE_CMD | GROUP_SINGLE | CMD_WRITE), set_3c_register_third, 6, BM1366_SERIALTX_DEBUG);
    }

    do_frequency_ramp_up((float)frequency);
    unsigned char set_10_hash_counting[6] = {0x00, 0x10, 0x00, 0x00, 0x15, 0x1C};
    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_WRITE), set_10_hash_counting, 6, BM1366_SERIALTX_DEBUG);

    unsigned char init795[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0xA4, 0x90, 0x00, 0xFF, 0xFF, 0x1C};
    _send_simple(init795, 11);

    return chip_counter;
}

// Developer Notes:
// This function performs a hardware reset of the BM1366 ASIC using a designated GPIO pin (GPIO_ASIC_RESET). It drives
// the pin low for 100ms, then high for another 100ms, adhering to the chip’s reset timing specification. This reset
// clears internal states, stops ongoing operations, and prepares the ASIC for a fresh initialization. It’s a critical
// first step in the boot process, ensuring the hardware is in a predictable state before serial communication begins.
// The use of FreeRTOS’s vTaskDelay ensures precise timing within a multitasking environment, avoiding conflicts with
// other system tasks. This function is simple but foundational for reliable ASIC operation.
static void _reset(void)
{
    gpio_set_level(GPIO_ASIC_RESET, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(GPIO_ASIC_RESET, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

// Developer Notes:
// This is the primary entry point for initializing the BM1366 ASIC chain. It sets up the reset GPIO pin, clears the
// response buffer to prevent stale data, performs a hardware reset via _reset, and then calls _send_init to execute
// the full initialization sequence. The function takes two parameters: the target operating frequency (frequency) and
// the expected number of ASIC chips (asic_count). It returns the actual number of detected chips, allowing the caller
// to verify the chain’s integrity. Logging at the start provides a clear marker in the system logs for debugging.
// This function orchestrates the transition from a powered-off state to a fully configured mining-ready state.
uint8_t BM1366_init(uint64_t frequency, uint16_t asic_count)
{
    ESP_LOGI(TAG, "Initializing BM1366");
    memset(asic_response_buffer, 0, SERIAL_BUF_SIZE);
    esp_rom_gpio_pad_select_gpio(GPIO_ASIC_RESET);
    gpio_set_direction(GPIO_ASIC_RESET, GPIO_MODE_OUTPUT);
    _reset();
    return _send_init(frequency, asic_count);
}

// Developer Notes:
// This function sets the serial baud rate of all ASIC chips to a default value of approximately 115,749 bps, calculated
// using the formula 25M/((denominator+1)*8), where the denominator is 26 (binary 11010). It writes to the MISC_CONTROL
// register (0x18) with a predefined 6-byte payload, adjusting bits 9-13 to set the baud divider. The command is broadcast
// to all chips (GROUP_ALL) using TYPE_CMD and CMD_WRITE. This baud rate is a conservative default, balancing reliability
// and speed for initial communication. The function returns the calculated baud rate for use by the caller in configuring
// the microcontroller’s UART. It’s typically called after initialization to establish a stable communication baseline.
int BM1366_set_default_baud(void)
{
    unsigned char baudrate[9] = {0x00, MISC_CONTROL, 0x00, 0x00, 0b01111010, 0b00110001};
    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_WRITE), baudrate, 6, BM1366_SERIALTX_DEBUG);
    return 115749;
}

// Developer Notes:
// This function configures all ASIC chips to operate at a maximum baud rate of 1,000,000 bps, significantly increasing
// communication speed for high-performance operation. It sends a predefined 11-byte command to register 0x28
// (FAST_UART_CONFIGURATION) using _send_simple, bypassing the dynamic packet construction of _send_BM1366. The exact
// settings achieve a higher baud rate by reducing the divider in the UART configuration, though the precise calculation
// isn’t shown here (it’s hardcoded). Logging confirms the change, and the function returns the new baud rate for the
// caller to adjust the microcontroller’s UART accordingly. This is typically used after initial setup to optimize
// data throughput during mining.
int BM1366_set_max_baud(void)
{
    ESP_LOGI(TAG, "Setting max baud of 1000000");
    unsigned char reg28[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x28, 0x11, 0x30, 0x02, 0x00, 0x03};
    _send_simple(reg28, 11);
    return 1000000;
}

// Developer Notes:
// This function sets the job difficulty mask for all ASIC chips, controlling the range of nonces they process before
// reporting results. The mask must be a power of 2 (e.g., 256, 512) to ensure continuous nonce coverage, and the
// function adjusts the input difficulty to the largest power of 2 minus 1 for optimal sampling. It converts this value
// into a 4-byte mask, reversing the bit order for each byte to match the ASIC’s register format, and writes it to the
// TICKET_MASK register (0x14). The command is broadcast to all chips (GROUP_ALL) using TYPE_CMD and CMD_WRITE. Logging
// reports the applied difficulty, aiding in tuning the mining process. This configuration affects how frequently the
// ASIC reports solutions, balancing hash rate sampling with communication overhead.
void BM1366_set_job_difficulty_mask(int difficulty)
{
    unsigned char job_difficulty_mask[9] = {0x00, TICKET_MASK, 0b00000000, 0b00000000, 0b00000000, 0b11111111};
    difficulty = _largest_power_of_two(difficulty) - 1;

    for (int i = 0; i < 4; i++) {
        char value = (difficulty >> (8 * i)) & 0xFF;
        job_difficulty_mask[5 - i] = _reverse_bits(value);
    }

    ESP_LOGI(TAG, "Setting job ASIC mask to %d", difficulty);
    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_WRITE), job_difficulty_mask, 6, BM1366_SERIALTX_DEBUG);
}

static uint8_t id = 0;

// Developer Notes:
// This function sends a mining job to a single ASIC chip, integrating it into the system’s job management framework.
// It takes a GlobalState pointer for tracking active jobs and a bm_job structure containing the job details (nonce,
// target, merkle root, etc.). The function assigns a unique job_id (incrementing by 8, wrapping at 128), constructs a
// BM1366_job structure, and copies the necessary fields from the input job. It manages the GlobalState by freeing any
// existing job with the same ID, storing the new job, and marking it as valid under a mutex lock for thread safety.
// The job is then sent as a JOB_PACKET to a single chip (GROUP_SINGLE) with CMD_WRITE, using debug logging if enabled.
// This function is the bridge between the mining software and the ASIC hardware, ensuring jobs are dispatched and tracked
// correctly.
void BM1366_send_work(void * pvParameters, bm_job * next_bm_job)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    BM1366_job job;
    id = (id + 8) % 128;
    job.job_id = id;
    job.num_midstates = 0x01;
    memcpy(&job.starting_nonce, &next_bm_job->starting_nonce, 4);
    memcpy(&job.nbits, &next_bm_job->target, 4);
    memcpy(&job.ntime, &next_bm_job->ntime, 4);
    memcpy(job.merkle_root, next_bm_job->merkle_root_be, 32);
    memcpy(job.prev_block_hash, next_bm_job->prev_block_hash_be, 32);
    memcpy(&job.version, &next_bm_job->version, 4);

    if (GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job.job_id] != NULL) {
        free_bm_job(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job.job_id]);
    }
    GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job.job_id] = next_bm_job;

    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    GLOBAL_STATE->valid_jobs[job.job_id] = 1;
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

    #if BM1366_DEBUG_JOBS
    ESP_LOGI(TAG, "Send Job: %02X", job.job_id);
    #endif

    _send_BM1366((TYPE_JOB | GROUP_SINGLE | CMD_WRITE), (uint8_t *)&job, sizeof(BM1366_job), BM1366_DEBUG_WORK);
}

// Developer Notes:
// This function receives completed work results from the ASIC via the serial interface, returning them as an asic_result
// structure. It attempts to read 11 bytes (the expected response size) with a timeout (BM1366_TIMEOUT_MS), handling three
// possible outcomes: UART errors (negative return), timeouts (zero return), or successful reads. Errors and timeouts are
// logged, with a counter tracking consecutive timeouts to detect unresponsive ASICs (though it resets without action here).
// A successful response is validated by checking its length (11 bytes) and preamble (0xAA 0x55); invalid responses are
// logged with a hex dump, and the serial buffer is cleared to prevent data corruption. The function returns a pointer to
// the response buffer cast as an asic_result, or NULL on failure, making it the primary input for result processing.
asic_result * BM1366_receive_work(void)
{
    int received = SERIAL_rx(asic_response_buffer, 11, BM1366_TIMEOUT_MS);

    bool uart_err = received < 0;
    bool uart_timeout = received == 0;
    uint8_t asic_timeout_counter = 0;

    if (uart_err) {
        ESP_LOGI(TAG, "UART Error in serial RX");
        return NULL;
    } else if (uart_timeout) {
        if (asic_timeout_counter >= BM1366_TIMEOUT_THRESHOLD) {
            ESP_LOGE(TAG, "ASIC not sending data");
            asic_timeout_counter = 0;
        }
        asic_timeout_counter++;
        return NULL;
    }

    if (received != 11 || asic_response_buffer[0] != 0xAA || asic_response_buffer[1] != 0x55) {
        ESP_LOGI(TAG, "Serial RX invalid %i", received);
        ESP_LOG_BUFFER_HEX(TAG, asic_response_buffer, received);
        SERIAL_clear_buffer();
        return NULL;
    }

    return (asic_result *) asic_response_buffer;
}

// Developer Notes:
// This utility function reverses the byte order of a 16-bit unsigned integer, swapping the high and low bytes. It’s used
// in processing ASIC responses where data may be returned in a different endianness than expected by the microcontroller.
// The operation is a simple bit shift: the high byte (num >> 8) is moved to the low position, and the low byte (num << 8)
// is moved to the high position, combined with a bitwise OR. This is a helper for BM1366_proccess_work to correctly
// interpret version fields in the asic_result structure, ensuring accurate reconstruction of mining results.
static uint16_t reverse_uint16(uint16_t num)
{
    return (num >> 8) | (num << 8);
}

// Developer Notes:
// This utility function reverses the byte order of a 32-bit unsigned integer, rearranging all four bytes from big-endian
// to little-endian (or vice versa). It’s used to normalize nonce values returned by the ASIC, which may differ in endianness
// from the system’s expectation. The function shifts and masks each byte to its new position: byte 3 to 0, byte 2 to 1,
// byte 1 to 2, and byte 0 to 3, combining them with bitwise OR operations. This is essential in BM1366_proccess_work for
// extracting core IDs and nonces from the asic_result, ensuring accurate interpretation of mining solutions.
static uint32_t reverse_uint32(uint32_t val)
{
    return ((val >> 24) & 0xff) | ((val << 8) & 0xff0000) | ((val >> 8) & 0xff00) | ((val << 24) & 0xff000000);
}

// Developer Notes:
// This function processes work results received from the ASIC, validating them against the system’s job tracking and
// preparing them for submission to the mining pool. It calls BM1366_receive_work to get a result, then extracts key fields:
// job_id (upper 5 bits), core_id (7 bits from the reversed nonce), small_core_id (lower 3 bits of job_id), and version_bits
// (reversed 16-bit version shifted left by 13). It logs these for debugging, then checks the job_id against the GlobalState’s
// valid_jobs array to ensure it’s a current job. If valid, it combines the version_bits with the original job’s version to
// reconstruct the rolled version, storing the job_id, nonce, and rolled_version in a task_result structure. The function
// returns a pointer to this result or NULL if the result is invalid or missing, making it the final step in the mining
// workflow from ASIC to software.
task_result * BM1366_proccess_work(void * pvParameters)
{
    asic_result * asic_result = BM1366_receive_work();

    if (asic_result == NULL) {
        return NULL;
    }

    uint8_t job_id = asic_result->job_id & 0xf8;
    uint8_t core_id = (uint8_t)((reverse_uint32(asic_result->nonce) >> 25) & 0x7f);
    uint8_t small_core_id = asic_result->job_id & 0x07;
    uint32_t version_bits = (reverse_uint16(asic_result->version) << 13);
    ESP_LOGI(TAG, "Job ID: %02X, Core: %d/%d, Ver: %08" PRIX32, job_id, core_id, small_core_id, version_bits);

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    if (GLOBAL_STATE->valid_jobs[job_id] == 0) {
        ESP_LOGE(TAG, "Invalid job found, 0x%02X", job_id);
        return NULL;
    }

    uint32_t rolled_version = GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->version | version_bits;

    result.job_id = job_id;
    result.nonce = asic_result->nonce;
    result.rolled_version = rolled_version;

    return &result;
}
