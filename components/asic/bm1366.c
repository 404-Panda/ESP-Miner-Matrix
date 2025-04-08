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
#include <inttypes.h>

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

#define PUNIT_SIZE (0x4000 * 10000ULL)
#define SUBRANGE_SIZE (10 * PUNIT_SIZE) // 400M nonce range per job
typedef struct {
    uint64_t start_nonce;
    uint64_t end_nonce;
    uint8_t is_patoshi;
} PatoshiRange;

static const PatoshiRange PATOSHI_RANGES[] = {
    {0ULL, 163840000ULL, 1}, {163840000ULL, 327680000ULL, 1}, {327680000ULL, 491520000ULL, 1},
    {491520000ULL, 655360000ULL, 1}, {655360000ULL, 819200000ULL, 1}, {819200000ULL, 983040000ULL, 1},
    {983040000ULL, 1146880000ULL, 1}, {1146880000ULL, 1310720000ULL, 1}, {1310720000ULL, 1474560000ULL, 1},
    {1474560000ULL, 1638400000ULL, 1},
    {1638400000ULL, 1802240000ULL, 0}, {1802240000ULL, 1966080000ULL, 0}, {1966080000ULL, 2129920000ULL, 0},
    {2129920000ULL, 2293760000ULL, 0}, {2293760000ULL, 2457600000ULL, 0}, {2457600000ULL, 2621440000ULL, 0},
    {2621440000ULL, 2785280000ULL, 0}, {2785280000ULL, 2949120000ULL, 0}, {2949120000ULL, 3112960000ULL, 0},
    {3112960000ULL, 3276800000ULL, 1}, {3276800000ULL, 3440640000ULL, 1}, {3440640000ULL, 3604480000ULL, 1},
    {3604480000ULL, 3768320000ULL, 1}, {3768320000ULL, 3932160000ULL, 1}, {3932160000ULL, 4096000000ULL, 1},
    {4096000000ULL, 4259840000ULL, 1}, {4259840000ULL, 4423680000ULL, 1}, {4423680000ULL, 4587520000ULL, 1},
    {4587520000ULL, 4751360000ULL, 1}, {4751360000ULL, 4294967295ULL, 0}
};
#define NUM_PATOSHI_RANGES (sizeof(PATOSHI_RANGES) / sizeof(PatoshiRange))

typedef struct {
    uint8_t core_id;
    uint32_t best_nonce;
    uint32_t best_range_index;
    uint64_t nonce_count;
} CorePatoshiStats;

static CorePatoshiStats core_stats[112];
typedef struct __attribute__((__packed__)) {
    uint8_t preamble[2];
    uint32_t nonce;
    uint8_t midstate_num;
    uint8_t job_id;
    uint16_t version;
    uint8_t crc;
} asic_result;

static float current_frequency = 56.25;
static const char *TAG = "bm1366Module";
static uint8_t asic_response_buffer[SERIAL_BUF_SIZE];
static task_result result;

static void _send_BM1366(uint8_t header, uint8_t *data, uint8_t data_len, bool debug) {
    packet_type_t packet_type = (header & TYPE_JOB) ? JOB_PACKET : CMD_PACKET;
    uint8_t total_length = (packet_type == JOB_PACKET) ? (data_len + 6) : (data_len + 5);
    unsigned char *buf = malloc(total_length);
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

static void _send_simple(uint8_t *data, uint8_t total_length) {
    unsigned char *buf = malloc(total_length);
    memcpy(buf, data, total_length);
    SERIAL_send(buf, total_length, BM1366_SERIALTX_DEBUG);
    free(buf);
}

static void _send_chain_inactive(void) {
    unsigned char read_address[2] = {0x00, 0x00};
    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_INACTIVE), read_address, 2, BM1366_SERIALTX_DEBUG);
}

static void _set_chip_address(uint8_t chipAddr) {
    unsigned char read_address[2] = {chipAddr, 0x00};
    _send_BM1366((TYPE_CMD | GROUP_SINGLE | CMD_SETADDRESS), read_address, 2, BM1366_SERIALTX_DEBUG);
}

void BM1366_set_version_mask(uint32_t version_mask) {
    int versions_to_roll = version_mask >> 13;
    uint8_t version_byte0 = (versions_to_roll >> 8);
    uint8_t version_byte1 = (versions_to_roll & 0xFF);
    uint8_t version_cmd[] = {0x00, 0xA4, 0x90, 0x00, version_byte0, version_byte1};
    _send_BM1366(TYPE_CMD | GROUP_ALL | CMD_WRITE, version_cmd, 6, BM1366_SERIALTX_DEBUG);
}

void BM1366_send_hash_frequency(float target_freq) {
    unsigned char freqbuf[9] = {0x00, 0x08, 0x40, 0xA0, 0x02, 0x41};
    float newf = 200.0;
    uint8_t fb_divider = 0;
    uint8_t post_divider1 = 0, post_divider2 = 0;
    uint8_t ref_divider = 0;
    float min_difference = 10;

    for (uint8_t refdiv_loop = 2; refdiv_loop > 0 && fb_divider == 0; refdiv_loop--) {
        for (uint8_t postdiv1_loop = 7; postdiv1_loop > 0 && fb_divider == 0; postdiv1_loop--) {
            for (uint8_t postdiv2_loop = 1; postdiv2_loop < postdiv1_loop && fb_divider == 0; postdiv2_loop++) {
                int temp_fb_divider = round(((float)(postdiv1_loop * postdiv2_loop * target_freq * refdiv_loop) / 25.0));
                if (temp_fb_divider >= 144 && temp_fb_divider <= 235) {
                    float temp_freq = 25.0 * (float)temp_fb_divider / (float)(refdiv_loop * postdiv2_loop * postdiv1_loop);
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
        newf = 25.0 * (float)(fb_divider) / (float)(ref_divider * post_divider1 * post_divider2);
        freqbuf[3] = fb_divider;
        freqbuf[4] = ref_divider;
        freqbuf[5] = (((post_divider1 - 1) & 0xf) << 4) + ((post_divider2 - 1) & 0xf);
        if (fb_divider * 25 / (float)ref_divider >= 2400) {
            freqbuf[2] = 0x50;
        }
    }

    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_WRITE), freqbuf, 6, BM1366_SERIALTX_DEBUG);
    ESP_LOGI(TAG, "Setting Frequency to %.2fMHz (%.2f)", target_freq, newf);
    current_frequency = newf;
}

static void do_frequency_ramp_up(float target_frequency) {
    float step = 6.25;
    float current = current_frequency;
    float target = target_frequency;
    float direction = (target > current) ? step : -step;

    if (fmod(current, step) != 0) {
        float next_dividable = (direction > 0) ? ceil(current / step) * step : floor(current / step) * step;
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
}

static uint8_t _send_init(uint64_t frequency, uint16_t asic_count) {
    memset(core_stats, 0, sizeof(core_stats));
    
    for (int i = 0; i < 3; i++) {
        BM1366_set_version_mask(STRATUM_DEFAULT_VERSION_MASK);
    }
    unsigned char init3[7] = {0x55, 0xAA, 0x52, 0x05, 0x00, 0x00, 0x0A};
    _send_simple(init3, 7);

    int chip_counter = 0;
    while (true) {
        if (SERIAL_rx(asic_response_buffer, 11, 1000) > 0) {
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

    uint8_t address_interval = (uint8_t)(256 / chip_counter);
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

static void _reset(void) {
    gpio_set_level(GPIO_ASIC_RESET, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(GPIO_ASIC_RESET, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

uint8_t BM1366_init(uint64_t frequency, uint16_t asic_count) {
    ESP_LOGI(TAG, "Initializing BM1366");
    memset(asic_response_buffer, 0, SERIAL_BUF_SIZE);
    esp_rom_gpio_pad_select_gpio(GPIO_ASIC_RESET);
    gpio_set_direction(GPIO_ASIC_RESET, GPIO_MODE_OUTPUT);
    _reset();
    return _send_init(frequency, asic_count);
}

int BM1366_set_default_baud(void) {
    unsigned char baudrate[9] = {0x00, MISC_CONTROL, 0x00, 0x00, 0b01111010, 0b00110001};
    _send_BM1366((TYPE_CMD | GROUP_ALL | CMD_WRITE), baudrate, 6, BM1366_SERIALTX_DEBUG);
    return 115749;
}

int BM1366_set_max_baud(void) {
    ESP_LOGI(TAG, "Setting max baud of 1000000");
    unsigned char reg28[11] = {0x55, 0xAA, 0x51, 0x09, 0x00, 0x28, 0x11, 0x30, 0x02, 0x00, 0x03};
    _send_simple(reg28, 11);
    return 1000000;
}

void BM1366_set_job_difficulty_mask(int difficulty) {
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

void BM1366_send_work(void *pvParameters, bm_job *next_bm_job) {
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    BM1366_job job;
    id = (id + 8) % 128;
    job.job_id = id;
    job.num_midstates = 0x01;

    // Randomize starting_nonce across the full 32-bit nonce space, aligned to SUBRANGE_SIZE
    uint32_t max_starts = (4294967295ULL / SUBRANGE_SIZE) + 1; // Number of 400M chunks
    uint32_t random_start_index = rand() % max_starts;
    uint32_t optimal_start = random_start_index * SUBRANGE_SIZE;
    memcpy(&job.starting_nonce, &optimal_start, 4);

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
    ESP_LOGI(TAG, "Send Job: %02X (Patoshi range: %" PRIu32 "-%" PRIu32 ")", job.job_id, optimal_start, optimal_start + SUBRANGE_SIZE);
#endif

    _send_BM1366((TYPE_JOB | GROUP_SINGLE | CMD_WRITE), (uint8_t *)&job, sizeof(BM1366_job), BM1366_DEBUG_WORK);
}

asic_result *BM1366_receive_work(void) {
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

    return (asic_result *)asic_response_buffer;
}

static uint16_t reverse_uint16(uint16_t num) {
    return (num >> 8) | (num << 8);
}

static uint32_t reverse_uint32(uint32_t val) {
    return ((val >> 24) & 0xff) | ((val << 8) & 0xff0000) | ((val >> 8) & 0xff00) | ((val << 24) & 0xff000000);
}

task_result *BM1366_proccess_work(void *pvParameters) {
    asic_result *asic_result = BM1366_receive_work();

    if (asic_result == NULL) {
        return NULL;
    }

    uint8_t job_id = asic_result->job_id & 0xf8;
    uint8_t core_id = (uint8_t)((reverse_uint32(asic_result->nonce) >> 25) & 0x7f);
    uint8_t small_core_id = asic_result->job_id & 0x07;
    uint32_t version_bits = (reverse_uint16(asic_result->version) << 13);

    uint32_t nonce = reverse_uint32(asic_result->nonce);
    core_stats[core_id].nonce_count++;
    for (int i = 0; i < NUM_PATOSHI_RANGES; i++) {
        if ((uint64_t)nonce >= PATOSHI_RANGES[i].start_nonce && (uint64_t)nonce < PATOSHI_RANGES[i].end_nonce) {
            // Log all hits, not just Patoshi ones
            ESP_LOGI(TAG, "Range hit: Core %d, Nonce %" PRIu32 ", Range %d [%" PRIu64 "-%" PRIu64 "], Patoshi: %d",
                     core_id, nonce, i, PATOSHI_RANGES[i].start_nonce, PATOSHI_RANGES[i].end_nonce, PATOSHI_RANGES[i].is_patoshi);
            if (PATOSHI_RANGES[i].is_patoshi) {
                core_stats[core_id].best_nonce = nonce;
                core_stats[core_id].best_range_index = i;
                ESP_LOGI(TAG, "Patoshi hit: Core %d, Nonce %" PRIu32 ", Range [%" PRIu64 "-%" PRIu64 "]",
                         core_id, nonce, PATOSHI_RANGES[i].start_nonce, PATOSHI_RANGES[i].end_nonce);
            }
            break;
        }
    }

    ESP_LOGI(TAG, "Job ID: %02X, Core: %d/%d, Ver: %08" PRIX32, job_id, core_id, small_core_id, version_bits);

    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

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
