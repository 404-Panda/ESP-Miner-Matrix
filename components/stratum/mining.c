#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include "mining.h"
#include "utils.h"
#include "mbedtls/sha256.h"

// ================================================================================================
// CONSTANTS AND GLOBAL TRACKING VARIABLES
// ================================================================================================

// Bitcoin's difficulty-1 target value - used to calculate share difficulty
// This represents the maximum target value (easiest difficulty)
static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;

// Global variables to track the best share found during mining session
static double best_diff = 0.0;           // Highest difficulty share found
static uint32_t best_nonce = 0;          // Nonce that produced the best share
static uint32_t best_version = 0;        // Block version used for best share
static uint32_t best_extranonce2 = 0;    // Extra nonce 2 value for best share

// ================================================================================================
// LOGGING AND PERFORMANCE TRACKING FUNCTIONS
// ================================================================================================

/**
 * Display current best share statistics to console
 * Used for monitoring mining performance and progress
 */
void log_best_share() {
    printf("[BEST] Diff=%.2f Nonce=0x%08X Version=0x%08X ExtraNonce2=0x%08X\n",
           best_diff, best_nonce, best_version, best_extranonce2);
}

/**
 * Log detailed information about a valid share found
 * Includes difficulty, nonce, and full midstate hex dump for debugging
 * 
 * @param diff - Calculated difficulty of the share
 * @param nonce - Nonce value that produced this share
 * @param midstate - SHA256 midstate used (32 bytes)
 * @param tag - Category tag for the log entry (e.g., "DIFF", "POOL")
 */
void log_share(double diff, uint32_t nonce, const uint8_t *midstate, const char *tag) {
    printf("[%s] Valid Share Found: Diff=%.2f, Nonce=0x%08X\nMidstate: ", tag, diff, nonce);
    // Print midstate as hex string for debugging/verification
    for (int i = 0; i < 32; ++i) printf("%02x", midstate[i]);
    printf("\n");
}

// ================================================================================================
// MEMORY MANAGEMENT
// ================================================================================================

/**
 * Properly deallocate memory used by a mining job structure
 * Prevents memory leaks by freeing all dynamically allocated components
 * 
 * @param job - Pointer to the bm_job structure to be freed
 */
void free_bm_job(bm_job *job) {
    free(job->jobid);       // Free job ID string
    free(job->extranonce2); // Free extra nonce 2 string
    free(job);              // Free the job structure itself
}

// ================================================================================================
// COINBASE TRANSACTION CONSTRUCTION
// ================================================================================================

/**
 * Construct complete coinbase transaction by concatenating all components
 * The coinbase transaction is the first transaction in a block that pays the miner
 * 
 * Format: coinbase_1 + extranonce + extranonce_2 + coinbase_2
 * 
 * @param coinbase_1 - First part of coinbase transaction (from pool)
 * @param coinbase_2 - Second part of coinbase transaction (from pool)
 * @param extranonce - Pool's extra nonce for uniqueness
 * @param extranonce_2 - Miner's extra nonce for local uniqueness
 * @return Dynamically allocated complete coinbase transaction string
 */
char *construct_coinbase_tx(const char *coinbase_1, const char *coinbase_2,
                            const char *extranonce, const char *extranonce_2) {
    // Calculate total length needed for concatenated string
    int coinbase_tx_len = strlen(coinbase_1) + strlen(coinbase_2) + strlen(extranonce) + strlen(extranonce_2) + 1;
    
    // Allocate memory and build the complete coinbase transaction
    char *coinbase_tx = malloc(coinbase_tx_len);
    strcpy(coinbase_tx, coinbase_1);        // Start with first part
    strcat(coinbase_tx, extranonce);        // Add pool's extra nonce
    strcat(coinbase_tx, extranonce_2);      // Add miner's extra nonce
    strcat(coinbase_tx, coinbase_2);        // End with second part
    coinbase_tx[coinbase_tx_len - 1] = '\0'; // Ensure null termination
    
    return coinbase_tx;
}

/**
 * Calculate the merkle root hash from coinbase transaction and merkle branches
 * The merkle root represents all transactions in the block as a single hash
 * 
 * Process:
 * 1. Hash the coinbase transaction (double SHA256)
 * 2. Combine with each merkle branch using double SHA256
 * 3. Result is the merkle root that goes into the block header
 * 
 * @param coinbase_tx - Complete coinbase transaction (hex string)
 * @param merkle_branches - Array of merkle branch hashes (32 bytes each)
 * @param num_merkle_branches - Number of merkle branches to process
 * @return Dynamically allocated merkle root hash (hex string)
 */
char *calculate_merkle_root_hash(const char *coinbase_tx, const uint8_t merkle_branches[][32], const int num_merkle_branches) {
    // Convert coinbase transaction from hex string to binary
    size_t coinbase_tx_bin_len = strlen(coinbase_tx) / 2;
    uint8_t *coinbase_tx_bin = malloc(coinbase_tx_bin_len);
    hex2bin(coinbase_tx, coinbase_tx_bin, coinbase_tx_bin_len);

    // Start merkle tree calculation with coinbase transaction hash
    uint8_t both_merkles[64];  // Buffer for combining two 32-byte hashes
    uint8_t *new_root = double_sha256_bin(coinbase_tx_bin, coinbase_tx_bin_len);
    free(coinbase_tx_bin);
    memcpy(both_merkles, new_root, 32);  // Copy initial hash to working buffer
    free(new_root);

    // Iteratively combine with each merkle branch
    for (int i = 0; i < num_merkle_branches; i++) {
        memcpy(both_merkles + 32, merkle_branches[i], 32);  // Add next branch
        uint8_t *new_root = double_sha256_bin(both_merkles, 64);  // Hash combined data
        memcpy(both_merkles, new_root, 32);  // Update working hash
        free(new_root);
    }

    // Convert final merkle root to hex string for return
    char *merkle_root_hash = malloc(65);  // 32 bytes * 2 + null terminator
    bin2hex(both_merkles, 32, merkle_root_hash, 65);
    return merkle_root_hash;
}

// ================================================================================================
// MINING JOB CONSTRUCTION AND OPTIMIZATION
// ================================================================================================

/**
 * Convert mining notification parameters into optimized mining job structure
 * This is the core function that prepares data for efficient mining operations
 * 
 * Key optimizations:
 * - Pre-computes SHA256 midstates to avoid redundant hashing
 * - Handles endianness conversions for different data formats
 * - Supports version rolling for increased mining efficiency
 * 
 * @param params - Mining notification from pool (contains basic block data)
 * @param merkle_root - Calculated merkle root hash (hex string)
 * @param version_mask - Bitmask for version rolling (0 = disabled)
 * @param difficulty - Pool difficulty setting
 * @return Fully constructed and optimized mining job structure
 */
bm_job construct_bm_job(mining_notify *params, const char *merkle_root, const uint32_t version_mask, const uint32_t difficulty) {
    bm_job new_job;
    
    // Copy basic parameters from mining notification
    new_job.version = params->version;
    new_job.target = params->target;
    new_job.ntime = params->ntime;
    new_job.starting_nonce = 0;
    new_job.pool_diff = difficulty;

    // Convert merkle root to binary and handle endianness
    hex2bin(merkle_root, new_job.merkle_root, 32);
    swap_endian_words(merkle_root, new_job.merkle_root_be);
    reverse_bytes(new_job.merkle_root_be, 32);

    // Convert previous block hash and handle endianness  
    swap_endian_words(params->prev_block_hash, new_job.prev_block_hash);
    hex2bin(params->prev_block_hash, new_job.prev_block_hash_be, 32);
    reverse_bytes(new_job.prev_block_hash_be, 32);

    // *** MIDSTATE OPTIMIZATION ***
    // Pre-compute SHA256 midstate for the first 64 bytes of block header
    // This avoids repeating the same hash operations for every nonce test
    uint8_t midstate_data[64];
    memcpy(midstate_data, &new_job.version, 4);           // Bytes 0-3: Version
    memcpy(midstate_data + 4, new_job.prev_block_hash, 32); // Bytes 4-35: Previous block hash
    memcpy(midstate_data + 36, new_job.merkle_root, 28);   // Bytes 36-63: First 28 bytes of merkle root

    // Compute and store the midstate (partial SHA256 state)
    midstate_sha256_bin(midstate_data, 64, new_job.midstate);
    reverse_bytes(new_job.midstate, 32);  // Reverse for hardware compatibility

    // *** VERSION ROLLING SUPPORT ***
    // Generate additional midstates for different version values
    // This allows mining multiple version variations in parallel
    if (version_mask != 0) {
        // Generate midstate for version + 1
        uint32_t rolled_version = increment_bitmask(new_job.version, version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, 64, new_job.midstate1);
        reverse_bytes(new_job.midstate1, 32);

        // Generate midstate for version + 2
        rolled_version = increment_bitmask(rolled_version, version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, 64, new_job.midstate2);
        reverse_bytes(new_job.midstate2, 32);

        // Generate midstate for version + 3
        rolled_version = increment_bitmask(rolled_version, version_mask);
        memcpy(midstate_data, &rolled_version, 4);
        midstate_sha256_bin(midstate_data, 64, new_job.midstate3);
        reverse_bytes(new_job.midstate3, 32);

        new_job.num_midstates = 4;  // We have 4 midstates available
    } else {
        new_job.num_midstates = 1;  // Only base midstate available
    }

    return new_job;
}

// ================================================================================================
// UTILITY FUNCTIONS
// ================================================================================================

/**
 * Generate extranonce2 string with proper formatting and padding
 * Extranonce2 provides local uniqueness for each mining attempt
 * 
 * @param extranonce_2 - Integer value to convert
 * @param length - Required byte length of output
 * @return Dynamically allocated hex string with zero padding
 */
char *extranonce_2_generate(uint32_t extranonce_2, uint32_t length) {
    // Allocate string buffer (2 hex chars per byte + null terminator)
    char *extranonce_2_str = malloc(length * 2 + 1);
    
    // Initialize with zeros for proper padding
    memset(extranonce_2_str, '0', length * 2);
    extranonce_2_str[length * 2] = '\0';
    
    // Convert binary value to hex string (overwrites trailing zeros)
    bin2hex((uint8_t *)&extranonce_2, length, extranonce_2_str, length * 2 + 1);
    
    return extranonce_2_str;
}

// ================================================================================================
// CORE MINING ALGORITHM - NONCE TESTING AND VALIDATION
// ================================================================================================

/**
 * Test a specific nonce value and calculate its difficulty
 * This is the core mining function that validates potential solutions
 * 
 * Process:
 * 1. Construct complete 80-byte block header with the nonce
 * 2. Perform double SHA256 hash (Bitcoin's proof-of-work algorithm)
 * 3. Convert hash result to difficulty value
 * 4. Log and track valid shares (difficulty > 1.0)
 * 
 * @param job - Mining job containing all necessary data
 * @param nonce - 32-bit nonce value to test
 * @param rolled_version - Version value (may be rolled for version rolling)
 * @return Calculated difficulty (>1.0 indicates valid share, higher = better)
 */
double test_nonce_value(const bm_job *job, const uint32_t nonce, const uint32_t rolled_version) {
    double d64, s64, ds;
    unsigned char header[80];  // Bitcoin block header is exactly 80 bytes

    // *** CONSTRUCT BLOCK HEADER ***
    // Bitcoin block header format (80 bytes total):
    memcpy(header, &rolled_version, 4);           // Bytes 0-3: Block version
    memcpy(header + 4, job->prev_block_hash, 32); // Bytes 4-35: Previous block hash
    memcpy(header + 36, job->merkle_root, 32);    // Bytes 36-67: Merkle root hash
    memcpy(header + 68, &job->ntime, 4);          // Bytes 68-71: Timestamp
    memcpy(header + 72, &job->target, 4);         // Bytes 72-75: Difficulty target
    memcpy(header + 76, &nonce, 4);               // Bytes 76-79: Nonce (what we're testing)

    // *** BITCOIN PROOF-OF-WORK CALCULATION ***
    // Bitcoin uses double SHA256 for proof-of-work
    unsigned char hash_buffer[32];   // Buffer for first hash
    unsigned char hash_result[32];   // Buffer for final hash
    
    mbedtls_sha256(header, 80, hash_buffer, 0);      // First SHA256
    mbedtls_sha256(hash_buffer, 32, hash_result, 0); // Second SHA256

    // *** DIFFICULTY CALCULATION ***
    // Difficulty = truediffone / hash_value
    // Higher difficulty means smaller hash value (more leading zeros)
    d64 = truediffone;                    // Maximum target (difficulty 1)
    s64 = le256todouble(hash_result);     // Convert hash to double (little-endian)
    ds = d64 / s64;                       // Calculate actual difficulty

    // *** SHARE VALIDATION AND TRACKING ***
    // Any difficulty > 1.0 is considered a valid share
    if (ds > 1.0) {
        log_share(ds, nonce, job->midstate, "DIFF");  // Log the valid share
        
        // Update best share tracking if this is better than previous best
        if (ds > best_diff) {
            best_diff = ds;
            best_nonce = nonce;
            best_version = rolled_version;
            // Note: best_extranonce2 would need to be passed in to track properly
        }
    }
    
    return ds;  // Return calculated difficulty
}

// ================================================================================================
// VERSION ROLLING UTILITY
// ================================================================================================

/**
 * Safely increment bits within a specified bitmask for version rolling
 * Version rolling allows testing multiple block versions simultaneously
 * 
 * This function increments only the bits specified in the mask while preserving
 * other bits, and handles carry propagation correctly across bit boundaries.
 * 
 * Example: If mask = 0x1FFF0000 (bits 16-28), this function will increment
 * through all possible values in that range while leaving other bits unchanged.
 * 
 * @param value - Current value to increment
 * @param mask - Bitmask specifying which bits can be modified
 * @return New value with incremented bits within the mask
 */
uint32_t increment_bitmask(const uint32_t value, const uint32_t mask) {
    // If no mask specified, return original value unchanged
    if (mask == 0)
        return value;

    // Increment the least significant bit set in the mask
    uint32_t carry = (value & mask) + (mask & -mask);
    
    // Check for overflow outside the mask boundaries
    uint32_t overflow = carry & ~mask;
    
    // Combine unchanged bits with new incremented bits
    uint32_t new_value = (value & ~mask) | (carry & mask);

    // Handle carry propagation to higher bits if needed
    if (overflow > 0) {
        uint32_t carry_mask = (overflow << 1);  // Shift overflow to next bit position
        new_value = increment_bitmask(new_value, carry_mask);  // Recursively handle carry
    }

    return new_value;
}
