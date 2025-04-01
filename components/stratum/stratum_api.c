/******************************************************************************
 *  *
 * References:
 *  1. Stratum Protocol - [link](https://reference.cash/mining/stratum-protocol)
 *****************************************************************************/

#include "stratum_api.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

#define BUFFER_SIZE 1024
static const char * TAG = "stratum_api";

static char * json_rpc_buffer = NULL;
static size_t json_rpc_buffer_size = 0;

// A message ID that must be unique per request that expects a response.
// For requests not expecting a response (called notifications), this is null.
static int send_uid = 1;

// Developer Notes:
// This static function logs Stratum protocol messages sent to the mining pool. It’s a utility for debugging
// communication by printing the transmitted JSON-RPC message to the system log. To ensure clean output, it
// temporarily removes the trailing newline from the message before logging (since ESP_LOGI adds its own),
// then restores it afterward to preserve the original string for transmission. This function is called by all
// Stratum request functions (e.g., STRATUM_V1_subscribe, STRATUM_V1_submit_share) to provide visibility into
// outgoing traffic, aiding in troubleshooting connection issues or protocol compliance during development.
static void debug_stratum_tx(const char * msg)
{
    char * newline = strchr(msg, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    ESP_LOGI(TAG, "tx: %s", msg);
    if (newline != NULL) {
        *newline = '\n';
    }
}

// Developer Notes:
// This internal helper function parses the result of a "mining.subscribe" response from the Stratum server.
// It takes a JSON string (result_json_str) and extracts two critical pieces of data: the extranonce (a server-
// provided nonce prefix) and extranonce2_len (the length of the client-generated nonce suffix). Using the cJSON
// library, it validates the JSON structure, expecting an array with at least three elements: subscriptions,
// extranonce, and extranonce2_len. If parsing fails or the expected fields are missing, it logs an error and
// returns -1. On success, it allocates memory for the extranonce string, copies the value, sets the length, and
// returns 0. This function supports the subscription process by providing data needed for subsequent share
// submissions, ensuring the miner aligns with the pool’s nonce requirements.
int _parse_stratum_subscribe_result_message(const char * result_json_str, char ** extranonce, int * extranonce2_len)
{
    cJSON * root = cJSON_Parse(result_json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Unable to parse %s", result_json_str);
        return -1;
    }
    cJSON * result = cJSON_GetObjectItem(root, "result");
    if (result == NULL) {
        ESP_LOGE(TAG, "Unable to parse subscribe result %s", result_json_str);
        return -1;
    }

    cJSON * extranonce2_len_json = cJSON_GetArrayItem(result, 2);
    if (extranonce2_len_json == NULL) {
        ESP_LOGE(TAG, "Unable to parse extranonce2_len: %s", result->valuestring);
        return -1;
    }
    *extranonce2_len = extranonce2_len_json->valueint;

    cJSON * extranonce_json = cJSON_GetArrayItem(result, 1);
    if (extranonce_json == NULL) {
        ESP_LOGE(TAG, "Unable parse extranonce: %s", result->valuestring);
        return -1;
    }
    *extranonce = malloc(strlen(extranonce_json->valuestring) + 1);
    strcpy(*extranonce, extranonce_json->valuestring);

    cJSON_Delete(root);

    return 0;
}

// Developer Notes:
// This function resets the unique message ID counter (send_uid) to 1. In the Stratum protocol, each request that
// expects a response must have a unique ID, which the server echoes back in its reply. Resetting this counter is
// useful when reconnecting to a pool or restarting the Stratum session to avoid potential ID conflicts or overflow
// after prolonged operation. The function logs the reset action for tracking purposes. It’s a simple but essential
// utility for managing the protocol’s state, ensuring reliable request-response pairing throughout the mining session.
void STRATUM_V1_reset_uid()
{
    ESP_LOGI(TAG, "Resetting stratum uid");
    send_uid = 1;
}

// Developer Notes:
// This function initializes the global JSON-RPC buffer used for receiving Stratum messages from the server. It
// allocates a fixed-size buffer (BUFFER_SIZE, 1024 bytes) and sets the json_rpc_buffer_size to match. If memory
// allocation fails, it prints an error and exits the program, indicating a critical failure since the buffer is
// necessary for all Stratum communication. The buffer is zeroed out to ensure no garbage data affects parsing.
// This function is called implicitly by STRATUM_V1_receive_jsonrpc_line if the buffer isn’t already initialized,
// providing a lazy initialization mechanism to prepare the system for receiving JSON-RPC messages over the socket.
void STRATUM_V1_initialize_buffer()
{
    json_rpc_buffer = malloc(BUFFER_SIZE);
    json_rpc_buffer_size = BUFFER_SIZE;
    if (json_rpc_buffer == NULL) {
        printf("Error: Failed to allocate memory for buffer\n");
        exit(1);
    }
    memset(json_rpc_buffer, 0, BUFFER_SIZE);
}

// Developer Notes:
// This function cleans up the global JSON-RPC buffer by freeing its allocated memory and resetting the pointer to
// NULL (though it doesn’t reset json_rpc_buffer_size here, which could be a minor oversight). It’s intended to be
// called when shutting down the Stratum client or during error recovery to release resources. Since the buffer is
// used across multiple receive operations, proper cleanup prevents memory leaks, especially in long-running mining
// applications. This is a straightforward memory management utility, complementing the initialization function.
void cleanup_stratum_buffer()
{
    free(json_rpc_buffer);
}

// Developer Notes:
// This static function dynamically resizes the JSON-RPC buffer when new data exceeds its current capacity. It calculates
// the new required size by adding the length of incoming data (len) to the existing buffer content, rounding up to the
// next multiple of BUFFER_SIZE (1024 bytes) for efficiency. If the current size is sufficient, it returns without action.
// Otherwise, it uses realloc to expand the buffer, initializing new space with zeros. If reallocation fails, it logs an
// error, waits briefly, and restarts the ESP system—a drastic but effective recovery mechanism for a critical failure.
// This function ensures the buffer can handle variable-length Stratum messages, supporting robust communication with
// the mining pool.
static void realloc_json_buffer(size_t len)
{
    size_t old, new;
    old = strlen(json_rpc_buffer);
    new = old + len + 1;

    if (new < json_rpc_buffer_size) {
        return;
    }

    new = new + (BUFFER_SIZE - (new % BUFFER_SIZE));
    void * new_sockbuf = realloc(json_rpc_buffer, new);

    if (new_sockbuf == NULL) {
        fprintf(stderr, "Error: realloc failed in recalloc_sock()\n");
        ESP_LOGI(TAG, "Restarting System because of ERROR: realloc failed in recalloc_sock");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    json_rpc_buffer = new_sockbuf;
    memset(json_rpc_buffer + old, 0, new - old);
    json_rpc_buffer_size = new;
}

// Developer Notes:
// This function reads a complete JSON-RPC line from the Stratum server over a socket (sockfd) and returns it as a
// dynamically allocated string. It uses a global buffer (json_rpc_buffer) to accumulate data, initializing it if needed.
// The function repeatedly calls recv to fetch data in chunks (up to BUFFER_SIZE - 1 bytes) until a newline (\n) is detected,
// indicating the end of a JSON-RPC message. Each chunk is appended to the buffer, resizing it as necessary via
// realloc_json_buffer. Once a full line is received, it extracts the first line using strtok, duplicates it for the
// return value, and shifts any remaining buffer content forward. If a socket error occurs (e.g., connection closed), it
// logs the error, frees the buffer, and returns NULL. This function is the core of Stratum message reception, enabling
// the miner to process server notifications and responses.
char * STRATUM_V1_receive_jsonrpc_line(int sockfd)
{
    if (json_rpc_buffer == NULL) {
        STRATUM_V1_initialize_buffer();
    }
    char *line, *tok = NULL;
    char recv_buffer[BUFFER_SIZE];
    int nbytes;
    size_t buflen = 0;

    if (!strstr(json_rpc_buffer, "\n")) {
        do {
            memset(recv_buffer, 0, BUFFER_SIZE);
            nbytes = recv(sockfd, recv_buffer, BUFFER_SIZE - 1, 0);
            if (nbytes == -1) {
                ESP_LOGI(TAG, "Error: recv (errno %d: %s)", errno, strerror(errno));
                if (json_rpc_buffer) {
                    free(json_rpc_buffer);
                    json_rpc_buffer=0;
                }
                return 0;
            }

            realloc_json_buffer(nbytes);
            strncat(json_rpc_buffer, recv_buffer, nbytes);
        } while (!strstr(json_rpc_buffer, "\n"));
    }
    buflen = strlen(json_rpc_buffer);
    tok = strtok(json_rpc_buffer, "\n");
    line = strdup(tok);
    int len = strlen(line);
    if (buflen > len + 1)
        memmove(json_rpc_buffer, json_rpc_buffer + len + 1, buflen - len + 1);
    else
        strcpy(json_rpc_buffer, "");
    return line;
}

// Developer Notes:
// This function parses a Stratum JSON-RPC message into a StratumApiV1Message structure, handling both requests from the
// server (e.g., mining.notify) and responses to client requests (e.g., mining.subscribe result). It uses cJSON to parse
// the JSON string, extracting the message ID (defaulting to -1 if absent) and determining the message type based on the
// "method" field (for server requests) or "result"/"error" fields (for responses). Supported methods include MINING_NOTIFY
// (new work), MINING_SET_DIFFICULTY (difficulty update), and others, with unhandled methods logged for debugging. For
// responses, it checks success/failure via the "result" and "error" fields, handling special cases like subscription
// results (extranonce, extranonce2_len) and version mask configuration. For MINING_NOTIFY, it allocates and populates a
// mining_notify structure with job details (e.g., job_id, merkle branches). This is the central parsing logic for
// interpreting Stratum communication, driving the miner’s workflow.
void STRATUM_V1_parse(StratumApiV1Message * message, const char * stratum_json)
{
    cJSON * json = cJSON_Parse(stratum_json);

    cJSON * id_json = cJSON_GetObjectItem(json, "id");
    int64_t parsed_id = -1;
    if (id_json != NULL && cJSON_IsNumber(id_json)) {
        parsed_id = id_json->valueint;
    }
    message->message_id = parsed_id;

    cJSON * method_json = cJSON_GetObjectItem(json, "method");
    stratum_method result = STRATUM_UNKNOWN;

    if (method_json != NULL && cJSON_IsString(method_json)) {
        if (strcmp("mining.notify", method_json->valuestring) == 0) {
            result = MINING_NOTIFY;
        } else if (strcmp("mining.set_difficulty", method_json->valuestring) == 0) {
            result = MINING_SET_DIFFICULTY;
        } else if (strcmp("mining.set_version_mask", method_json->valuestring) == 0) {
            result = MINING_SET_VERSION_MASK;
        } else if (strcmp("client.reconnect", method_json->valuestring) == 0) {
            result = CLIENT_RECONNECT;
        } else {
            ESP_LOGI(TAG, "unhandled method in stratum message: %s", stratum_json);
        }
    } else {
        cJSON * result_json = cJSON_GetObjectItem(json, "result");
        cJSON * error_json = cJSON_GetObjectItem(json, "error");
        cJSON * reject_reason_json = cJSON_GetObjectItem(json, "reject-reason");

        if (result_json == NULL) {
            message->response_success = false;
        } else if (!cJSON_IsNull(error_json)) {
            if (parsed_id < 5) {
                result = STRATUM_RESULT_SETUP;
            } else {
                result = STRATUM_RESULT;
            }
            if (cJSON_IsArray(error_json)) {
                int len = cJSON_GetArraySize(error_json);
                if (len >= 2) {
                    cJSON * error_msg = cJSON_GetArrayItem(error_json, 1);
                    if (cJSON_IsString(error_msg)) {
                        message->error_str = strdup(cJSON_GetStringValue(error_msg));
                    }
                }
            }
            message->response_success = false;
        } else if (cJSON_IsBool(result_json)) {
            if (parsed_id < 5) {
                result = STRATUM_RESULT_SETUP;
            } else {
                result = STRATUM_RESULT;
            }
            if (cJSON_IsTrue(result_json)) {
                message->response_success = true;
            } else {
                message->response_success = false;
                if (cJSON_IsString(reject_reason_json)) {
                    message->error_str = strdup(cJSON_GetStringValue(reject_reason_json));
                }                
            }
        } else if (parsed_id == STRATUM_ID_SUBSCRIBE) {
            result = STRATUM_RESULT_SUBSCRIBE;

            cJSON * extranonce2_len_json = cJSON_GetArrayItem(result_json, 2);
            if (extranonce2_len_json == NULL) {
                ESP_LOGE(TAG, "Unable to parse extranonce2_len: %s", result_json->valuestring);
                message->response_success = false;
                goto done;
            }
            message->extranonce_2_len = extranonce2_len_json->valueint;

            cJSON * extranonce_json = cJSON_GetArrayItem(result_json, 1);
            if (extranonce_json == NULL) {
                ESP_LOGE(TAG, "Unable parse extranonce: %s", result_json->valuestring);
                message->response_success = false;
                goto done;
            }
            message->extranonce_str = malloc(strlen(extranonce_json->valuestring) + 1);
            strcpy(message->extranonce_str, extranonce_json->valuestring);
            message->response_success = true;

            ESP_LOGI(TAG, "extranonce_str: %s", message->extranonce_str);
            ESP_LOGI(TAG, "extranonce_2_len: %d", message->extranonce_2_len);
        } else if (parsed_id == STRATUM_ID_CONFIGURE) {
            cJSON * mask = cJSON_GetObjectItem(result_json, "version-rolling.mask");
            if (mask != NULL) {
                result = STRATUM_RESULT_VERSION_MASK;
                message->version_mask = strtoul(mask->valuestring, NULL, 16);
                ESP_LOGI(TAG, "Set version mask: %08lx", message->version_mask);
            } else {
                ESP_LOGI(TAG, "error setting version mask: %s", stratum_json);
            }
        } else {
            ESP_LOGI(TAG, "unhandled result in stratum message: %s", stratum_json);
        }
    }

    message->method = result;

    if (message->method == MINING_NOTIFY) {
        mining_notify * new_work = malloc(sizeof(mining_notify));
        cJSON * params = cJSON_GetObjectItem(json, "params");
        new_work->job_id = strdup(cJSON_GetArrayItem(params, 0)->valuestring);
        new_work->prev_block_hash = strdup(cJSON_GetArrayItem(params, 1)->valuestring);
        new_work->coinbase_1 = strdup(cJSON_GetArrayItem(params, 2)->valuestring);
        new_work->coinbase_2 = strdup(cJSON_GetArrayItem(params, 3)->valuestring);

        cJSON * merkle_branch = cJSON_GetArrayItem(params, 4);
        new_work->n_merkle_branches = cJSON_GetArraySize(merkle_branch);
        if (new_work->n_merkle_branches > MAX_MERKLE_BRANCHES) {
            printf("Too many Merkle branches.\n");
            abort();
        }
        new_work->merkle_branches = malloc(HASH_SIZE * new_work->n_merkle_branches);
        for (size_t i = 0; i < new_work->n_merkle_branches; i++) {
            hex2bin(cJSON_GetArrayItem(merkle_branch, i)->valuestring, new_work->merkle_branches + HASH_SIZE * i, HASH_SIZE);
        }

        new_work->version = strtoul(cJSON_GetArrayItem(params, 5)->valuestring, NULL, 16);
        new_work->target = strtoul(cJSON_GetArrayItem(params, 6)->valuestring, NULL, 16);
        new_work->ntime = strtoul(cJSON_GetArrayItem(params, 7)->valuestring, NULL, 16);

        message->mining_notification = new_work;

        int paramsLength = cJSON_GetArraySize(params);
        int value = cJSON_IsTrue(cJSON_GetArrayItem(params, paramsLength - 1));
        message->should_abandon_work = value;

        // Log the parsed mining.notify details for the frontend
        ESP_LOGI(TAG, "Mining Notify - Job ID: %s, PrevBlockHash: %s, Coinbase1: %s, Coinbase2: %s, Version: %08lx, Target: %08lx, Ntime: %08lx",
                 new_work->job_id, new_work->prev_block_hash, new_work->coinbase_1, new_work->coinbase_2,
                 new_work->version, new_work->target, new_work->ntime);
        // Log merkle branches separately if needed
        if (new_work->n_merkle_branches > 0) {
            char merkle_str[1024] = {0};
            for (size_t i = 0; i < new_work->n_merkle_branches; i++) {
                char branch_hex[65]; // 64 chars for 32 bytes + null terminator
                bin2hex(new_work->merkle_branches + HASH_SIZE * i, HASH_SIZE, branch_hex, 65); // Fixed: Added hexlen argument
                strcat(merkle_str, branch_hex);
                if (i < new_work->n_merkle_branches - 1) strcat(merkle_str, ",");
            }
            ESP_LOGI(TAG, "Merkle Branches: [%s]", merkle_str);
        }
    } else if (message->method == MINING_SET_DIFFICULTY) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        uint32_t difficulty = cJSON_GetArrayItem(params, 0)->valueint;

        message->new_difficulty = difficulty;
    } else if (message->method == MINING_SET_VERSION_MASK) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        uint32_t version_mask = strtoul(cJSON_GetArrayItem(params, 0)->valuestring, NULL, 16);
        message->version_mask = version_mask;
    }
done:
    cJSON_Delete(json);
}

// Developer Notes:
// This function frees the memory allocated for a mining_notify structure, which is created by STRATUM_V1_parse when
// processing a MINING_NOTIFY message. It releases all dynamically allocated fields (job_id, prev_block_hash, coinbase_1,
// coinbase_2, and merkle_branches) before freeing the structure itself. This is crucial for preventing memory leaks,
// especially since mining notifications arrive frequently during operation, each requiring a new allocation. The
// function assumes the structure was properly initialized by STRATUM_V1_parse and is called when the mining job is
// completed or discarded, ensuring efficient resource management in the miner’s memory-constrained environment.
void STRATUM_V1_free_mining_notify(mining_notify * params)
{
    free(params->job_id);
    free(params->prev_block_hash);
    free(params->coinbase_1);
    free(params->coinbase_2);
    free(params->merkle_branches);
    free(params);
}

// Developer Notes:
// This function sends a "mining.subscribe" request to the Stratum server over the specified socket, initiating the
// miner’s subscription to receive mining jobs. It constructs a JSON-RPC message with a unique ID (send_uid++), the
// method "mining.subscribe", and a params array containing a user-agent string in the format "bitaxe/model/version".
// The model and version (from the ESP app description) identify the miner to the pool, aiding in compatibility and
// debugging. The message is logged via debug_stratum_tx and written to the socket, returning the number of bytes sent
// or an error code. This is a critical first step in the Stratum handshake, establishing the miner’s session and
// obtaining extranonce data for share construction.
int STRATUM_V1_subscribe(int socket, char * model)
{
    char subscribe_msg[BUFFER_SIZE];
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = app_desc->version;    
    sprintf(subscribe_msg, "{\"id\": %d, \"method\": \"mining.subscribe\", \"params\": [\"bitaxe/%s/%s\"]}\n", send_uid++, model, version);
    debug_stratum_tx(subscribe_msg);

    return write(socket, subscribe_msg, strlen(subscribe_msg));
}

// Developer Notes:
// This function sends a "mining.suggest_difficulty" request to the Stratum server, proposing a preferred difficulty
// level for mining jobs. It constructs a JSON-RPC message with a unique ID (send_uid++), the method "mining.suggest_difficulty",
// and the difficulty value as a parameter. The message is logged and sent over the socket, returning the number of bytes
// written or an error code. This optional request allows the miner to influence the difficulty of work it receives,
// optimizing for its hash rate and reducing unnecessary network load from submitting low-difficulty shares. It’s typically
// called after subscription but before authentication, though pools may ignore it if they enforce their own difficulty.
int STRATUM_V1_suggest_difficulty(int socket, uint32_t difficulty)
{
    char difficulty_msg[BUFFER_SIZE];
    sprintf(difficulty_msg, "{\"id\": %d, \"method\": \"mining.suggest_difficulty\", \"params\": [%ld]}\n", send_uid++, difficulty);
    debug_stratum_tx(difficulty_msg);

    return write(socket, difficulty_msg, strlen(difficulty_msg));
}

// Developer Notes:
// This function sends a "mining.authorize" request to the Stratum server, authenticating the miner with the pool using
// a username and password. It constructs a JSON-RPC message with a unique ID (send_uid++), the method "mining.authorize",
// and parameters containing the username and password (typically a worker ID and optional password). The message is logged
// and transmitted over the socket, returning the bytes sent or an error code. Successful authorization is required before
// the pool will send mining jobs, making this a key step in the connection process after subscription. The function assumes
// the socket is already connected and relies on the pool to validate credentials, logging aiding in debugging auth failures.
int STRATUM_V1_authenticate(int socket, const char * username, const char * pass)
{
    char authorize_msg[BUFFER_SIZE];
    sprintf(authorize_msg, "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n", send_uid++, username,
            pass);
    debug_stratum_tx(authorize_msg);

    return write(socket, authorize_msg, strlen(authorize_msg));
}

// Developer Notes:
// This function submits a completed mining share to the Stratum server, reporting a solution found by the miner. It
// constructs a "mining.submit" JSON-RPC message with a unique ID (send_uid++), the username, jobid (from the mining job),
// extranonce_2 (client-generated nonce suffix), ntime, nonce, and version (all hex-encoded). These parameters represent
// the block header fields that produced a hash meeting the target difficulty. The message is logged and sent over the
// socket, returning the bytes written or an error code. This is the core function for submitting work to the pool,
// invoked whenever the ASIC finds a valid nonce, and its success determines whether the miner earns credit for its work.
// The function assumes all inputs are valid and properly formatted from prior processing.
int STRATUM_V1_submit_share(int socket, const char * username, const char * jobid, const char * extranonce_2, const uint32_t ntime,
                             const uint32_t nonce, const uint32_t version)
{
    char submit_msg[BUFFER_SIZE];
    sprintf(submit_msg,
            "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\", \"%08lx\"]}\n",
            send_uid++, username, jobid, extranonce_2, ntime, nonce, version);
    debug_stratum_tx(submit_msg);

    return write(socket, submit_msg, strlen(submit_msg));
}

// Developer Notes:
// This function sends a "mining.configure" request to the Stratum server to enable version rolling, a feature allowing
// the miner to modify the block header’s version field within a specified mask. It constructs a JSON-RPC message with a
// unique ID (send_uid++), the method "mining.configure", and parameters requesting "version-rolling" with a full mask
// ("ffffffff"). The message is logged and sent over the socket, returning the bytes written or an error code. The server’s
// response (parsed elsewhere) provides the actual version mask, which the miner uses to generate additional solutions.
// This optional request, typically sent early in the session, enhances mining efficiency by expanding the nonce space,
// and the hardcoded mask requests maximum flexibility, though the pool may restrict it.
int STRATUM_V1_configure_version_rolling(int socket, uint32_t * version_mask)
{
    char configure_msg[BUFFER_SIZE * 2];
    sprintf(configure_msg,
            "{\"id\": %d, \"method\": \"mining.configure\", \"params\": [[\"version-rolling\"], {\"version-rolling.mask\": "
            "\"ffffffff\"}]}\n",
            send_uid++);
    debug_stratum_tx(configure_msg);

    return write(socket, configure_msg, strlen(configure_msg));
}
