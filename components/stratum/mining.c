#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "mining.h"
#include "utils.h"
#include "mbedtls/sha256.h"

// Developer Notes:
// This function frees the memory allocated for a bm_job structure, which represents a mining job sent to the ASIC.
// The bm_job structure contains dynamically allocated fields (jobid and extranonce2) that must be individually freed
// before releasing the structure itself. This is essential for preventing memory leaks, as bm_jobs are created and
// destroyed frequently during mining operations when new work arrives from the Stratum pool or old jobs are completed.
// The function assumes the job pointer and its fields were properly allocated (e.g., by construct_bm_job or similar)
// and is typically called when a job is no longer needed, such as after submission or when replacing it with new work
// in the GlobalState tracking system.
void free_bm_job(bm_job *job)
{
    free(job->jobid);
    free(job->extranonce2);
    free(job);
}

// Developer Notes:
// This function constructs a Coinbase transaction string by concatenating four hexadecimal components: coinbase_1
// (prefix from the pool), extranonce (server-provided nonce), extranonce_2 (client-generated nonce), and coinbase_2
// (suffix from the pool). It calculates the total length, allocates a buffer, and builds the string using strcpy and
// strcat, ensuring a null-terminated result. The Coinbase transaction is a critical part of Bitcoin mining, as it’s
// the input to the Merkle root calculation and includes the miner’s reward address and extra nonce space for uniqueness.
// The function returns a dynamically allocated string that must be freed by the caller, typically after it’s used to
// compute the Merkle root. It assumes all input strings are valid hex and properly null-terminated, making it a simple
// but foundational step in preparing mining data.
char *construct_coinbase_tx(const char *coinbase_1, const char *coinbase_2,
                            const char *extranonce, const char *extranonce_2)
{
    int coinbase_tx_len = strlen(coinbase_1) + strlen(coinbase_2) + strlen(extranonce) + strlen(extranonce_2) + 1;

    char *coinbase_tx = malloc(coinbase_tx_len);
    strcpy(coinbase_tx, coinbase_1);
    strcat(coinbase_tx, extranonce);
    strcat(coinbase_tx, extranonce_2);
    strcat(coinbase_tx, coinbase_2);
    coinbase_tx[coinbase_tx_len - 1] = '\0';

    return coinbase_tx;
}

// Developer Notes:
// This function calculates the Merkle root hash for a mining job by combining the Coinbase transaction with a series
// of Merkle branches provided by the pool. It first converts the hex-encoded coinbase_tx to binary, computes its double
// SHA-256 hash (a Bitcoin standard), and uses this as the initial root. It then iteratively concatenates each Merkle
// branch (32-byte binary hash) with the current root, double-hashing the 64-byte result to produce a new root, repeating
// for all branches. The final 32-byte binary hash is converted to a 64-character hex string (plus null terminator) and
// returned. This Merkle root is a key component of the Bitcoin block header, linking the Coinbase transaction to the
// rest of the block’s transactions. The function manages memory dynamically and assumes valid inputs, making it a core
// piece of the mining process that connects pool data to ASIC-ready headers.
char *calculate_merkle_root_hash(const char *coinbase_tx, const uint8_t merkle_branches[][32], const int num_merkle_branches)
{
    size_t coinbase_tx_bin_len = strlen(coinbase_tx) / 2;
    uint8_t *coinbase_tx_bin = malloc(coinbase_tx_bin_len);
    hex2bin(coinbase_tx, coinbase_tx_bin, coinbase_tx_bin_len);

    uint8_t both_merkles[64];
    uint8_t *new_root = double_sha256_bin(coinbase_tx_bin, coinbase_tx_bin_len);
    free(coinbase_tx_bin);
    memcpy(both_merkles, new_root, 32);
    free(new_root);
    for (int i = 0; i < num_merkle_branches; i++)
    {
        memcpy(both_merkles + 32, merkle_branches[i], 32);
        uint8_t *new_root = double_sha256_bin(both_merkles, 64);
        memcpy(both_merkles, new_root, 32);
        free(new_root);
    }

    char *merkle_root_hash = malloc(65);
    bin2hex(both_merkles, 32, merkle_root_hash, 65);
    return merkle_root_hash;
}

// Developer Notes:
// This function constructs a bm_job structure (ASIC-ready mining job) from a mining_notify structure (Stratum pool data)
// and a precomputed Merkle root, optionally applying a version mask for rolling. It copies basic fields (version, target,
// ntime, difficulty) directly, sets the starting nonce to 0, and converts hex strings (merkle_root, prev_block_hash) to
// binary in both little-endian (for hashing) and big-endian (for ASIC packet) formats. It generates a midstate hash—a
// partial SHA-256 of the first 64 bytes of the block header (version, prev_block_hash, part of merkle_root)—to optimize
// ASIC computation. If a version_mask is provided, it creates up to four midstates by incrementing the version within
// the mask, enhancing nonce space exploration. The resulting bm_job is tailored for the BM1366 ASIC, balancing pool data
// with hardware-specific requirements, and is returned by value (caller must manage dynamic fields separately).
bm_job construct_bm_job(mining_notify *params, const char *merkle_root, const uint32_t version_mask)
{
    bm_job new_job;

    new_job.version = params->version;
    new_job.starting_nonce = 0;
    new_job.target = params->target;
    new_job.ntime = params->ntime;
    new_job.pool_diff = params->difficulty;

    hex2bin(merkle_root, new_job.merkle_root, 32);

    swap_endian_words(merkle_root, new_job.merkle_root_be);
    reverse_bytes(new_job.merkle_root_be, 32);

    swap_endian_words(params->prev_block_hash, new_job.prev_block_hash);

    hex2bin(params->prev_block_hash, new_job.prev_block_hash_be, 32);
    reverse_bytes(new_job.prev_block_hash_be, 32);

    uint8_t midstate_data[64];
    memcpy(midstate_data, &new_job.version, 4);
    memcpy(midstate_data + 4, new_job.prev_block_hash, 32);
    memcpy(midstate_data + 36, new_job.merkle_root, 28);

    midstate_sha256_bin(midstate_data, 64, new_job.midstate);
    reverse_bytes(new_job.midstate, 32);

    if (version_mask != 0)
    {
        uint32_t rolled_version = increment_bitmask(new_job.version, version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, 64, new_job.midstate1);
        reverse_bytes(new_job.midstate1, 32);

        rolled_version = increment_bitmask(rolled_version, version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, 64, new_job.midstate2);
        reverse_bytes(new_job.midstate2, 32);

        rolled_version = increment_bitmask(rolled_version, version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, 64, new_job.midstate3);
        reverse_bytes(new_job.midstate3, 32);
        new_job.num_midstates = 4;
    }
    else
    {
        new_job.num_midstates = 1;
    }

    return new_job;
}

// Developer Notes:
// This function generates a hex-encoded extranonce_2 string of a specified length (in bytes) from a 32-bit integer value.
// It allocates a buffer for the hex string (length * 2 + 1 for null terminator), initializes it with zeros, and converts
// the binary extranonce_2 value to hex, right-aligning it in the buffer. If the requested length exceeds 4 bytes (the size
// of the input integer), it pads the left side with zeros, ensuring the string meets the pool’s extranonce2_len requirement.
// The extranonce_2 is a client-generated nonce suffix appended to the Coinbase transaction, providing additional nonce
// space for mining. The returned string must be freed by the caller and is used in share submission to uniquely identify
// the miner’s work.
char *extranonce_2_generate(uint32_t extranonce_2, uint32_t length)
{
    char *extranonce_2_str = malloc(length * 2 + 1);
    memset(extranonce_2_str, '0', length * 2);
    extranonce_2_str[length * 2] = '\0';
    bin2hex((uint8_t *)&extranonce_2, sizeof(extranonce_2), extranonce_2_str, length * 2 + 1);
    if (length > 4)
    {
        extranonce_2_str[8] = '0';
    }
    return extranonce_2_str;
}

// Developer Notes:
// Regarding `truediffone`: This static constant defines the Bitcoin difficulty 1 target as a double-precision floating-point
// number (approximately 2.696e70). It represents the value 0x00000000FFFF0000... (32 bytes, with the first 4 bytes as 0x0000FFFF
// and the rest zeros) when interpreted as a 256-bit little-endian integer. In Bitcoin mining, difficulty is calculated as
// truediffone divided by the hash value of a block header (also as a 256-bit integer). This constant is used in test_nonce_value
// to compute the difficulty of a mined share, providing a standardized reference for validating solutions against the pool’s
// target. Its large magnitude reflects the vast nonce space and ensures precision in difficulty calculations.
// 
// This function tests a nonce value against a bm_job to calculate its difficulty, mimicking cgminer’s share validation
// logic. It constructs an 80-byte Bitcoin block header from the job’s fields (rolled_version, prev_block_hash, merkle_root,
// ntime, target, nonce), double-hashes it using SHA-256, and computes the difficulty as truediffone divided by the hash
// value (converted to a double via le256todouble). The result indicates the share’s difficulty—higher values mean a better
// solution, with 0 implying an invalid hash (though not explicitly checked here). This function is used to verify ASIC
// results before submission, ensuring they meet the pool’s target, and could be optimized with midstate hashing (as noted
// in the TODO) to match ASIC efficiency. It’s a critical validation step in the mining workflow.
static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;

double test_nonce_value(const bm_job *job, const uint32_t nonce, const uint32_t rolled_version)
{
    double d64, s64, ds;
    unsigned char header[80];

    memcpy(header, &rolled_version, 4);
    memcpy(header + 4, job->prev_block_hash, 32);
    memcpy(header + 36, job->merkle_root, 32);
    memcpy(header + 68, &job->ntime, 4);
    memcpy(header + 72, &job->target, 4);
    memcpy(header + 76, &nonce, 4);

    unsigned char hash_buffer[32];
    unsigned char hash_result[32];

    mbedtls_sha256(header, 80, hash_buffer, 0);
    mbedtls_sha256(hash_buffer, 32, hash_result, 0);

    d64 = truediffone;
    s64 = le256todouble(hash_result);
    ds = d64 / s64;

    return ds;
}

// Developer Notes:
// This function increments a 32-bit value (value) within a specified bitmask (mask), used for version rolling in Bitcoin
// mining. Version rolling allows the miner to modify specific bits of the block header’s version field, effectively
// expanding the nonce space without recomputing the full header hash. It adds the least significant masked bit’s value
// to the masked portion of value, detects overflow beyond the mask, and recursively propagates carries to higher bits
// if needed. If the mask is zero, it returns the original value unchanged. This utility is key for generating multiple
// midstates in construct_bm_job, enabling the ASIC to explore a larger solution space efficiently, and is a clever
// optimization borrowed from Bitcoin’s Stratum protocol extensions.
uint32_t increment_bitmask(const uint32_t value, const uint32_t mask)
{
    if (mask == 0)
        return value;

    uint32_t carry = (value & mask) + (mask & -mask);
    uint32_t overflow = carry & ~mask;
    uint32_t new_value = (value & ~mask) | (carry & mask);

    if (overflow > 0)
    {
        uint32_t carry_mask = (overflow << 1);
        new_value = increment_bitmask(new_value, carry_mask);
    }

    return new_value;
}
