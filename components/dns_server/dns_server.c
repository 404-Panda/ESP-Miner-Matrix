/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <sys/param.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"

#include "dns_server.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#define DNS_PORT (53)
#define DNS_MAX_LEN (256)

#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define ANS_TTL_SEC (300)

static const char * TAG = "example_dns_redirect_server";

// Developer Notes:
// This structure defines the header format of a DNS packet as per RFC 1035. It’s a compact, packed representation
// (using __attribute__((__packed__))) to ensure no padding bytes interfere with the binary layout. The fields include:
// - id: A 16-bit identifier for matching requests and responses.
// - flags: A 16-bit field with various flags (e.g., QR for query/response, opcode).
// - qd_count: Number of questions in the packet.
// - an_count, ns_count, ar_count: Counts of answers, name server records, and additional records, respectively.
// This structure is used to parse incoming DNS queries and construct responses, forming the foundation of the DNS
// packet handling logic in this server implementation.
typedef struct __attribute__((__packed__))
{
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

// Developer Notes:
// This structure represents a DNS question section, containing the type (e.g., A for IPv4 address) and class (typically
// IN for Internet) of the query. It follows the name field in a DNS packet and is not packed because alignment isn’t
// a concern here (16-bit fields are naturally aligned). It’s used to interpret the type of DNS request (e.g., A record)
// and ensure the server responds appropriately, supporting the parsing logic in parse_dns_request.
typedef struct
{
    uint16_t type;
    uint16_t class;
} dns_question_t;

// Developer Notes:
// This packed structure defines a DNS answer section, used to construct responses to queries. Key fields include:
// - ptr_offset: A pointer to the original question name (compressed using DNS offset notation, e.g., 0xC00C).
// - type, class: Matching the question’s type and class (e.g., A and IN).
// - ttl: Time-to-live in seconds for the response (set to ANS_TTL_SEC, 300s).
// - addr_len: Length of the address data (4 bytes for IPv4).
// - ip_addr: The IPv4 address to return.
// It’s packed to match the exact binary format expected by DNS clients, enabling the server to craft valid responses
// redirecting queries to a configured IP (e.g., a softAP address).
typedef struct __attribute__((__packed__))
{
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

// Developer Notes:
// This structure represents the DNS server’s runtime handle, managing its state and configuration. It includes:
// - started: A boolean flag indicating whether the server is active.
// - task: A handle to the FreeRTOS task running the server.
// - num_of_entries: Number of DNS redirection rules.
// - entry: A flexible array of dns_entry_pair_t structs (name-IP or name-interface mappings).
// The flexible array allows dynamic allocation based on the number of rules, making it memory-efficient. This handle
// is passed between start/stop functions and the task, centralizing control over the server’s lifecycle and behavior.
struct dns_server_handle
{
    bool started;
    TaskHandle_t task;
    int num_of_entries;
    dns_entry_pair_t entry[];
};

// Developer Notes:
// This static function parses a DNS name from its compressed, length-prefixed format (e.g., 3www6google3com0) into a
// human-readable, dot-separated string (e.g., "www.google.com"). It iterates through the raw_name, where each label
// starts with a length byte followed by that many characters, appending a dot after each label until a zero-length
// byte terminates the name. The result is written to parsed_name, with bounds checking against parsed_name_max_len
// to prevent buffer overflows. The function returns a pointer to the next part of the packet (after the name), enabling
// further parsing (e.g., the question section). It’s a critical utility for interpreting DNS queries, supporting the
// server’s ability to match names against configured rules.
static char * parse_dns_name(char * raw_name, char * parsed_name, size_t parsed_name_max_len)
{
    char * label = raw_name;
    char * name_itr = parsed_name;
    int name_len = 0;

    do {
        int sub_name_len = *label;
        name_len += (sub_name_len + 1);
        if (name_len > parsed_name_max_len) {
            return NULL;
        }

        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += (sub_name_len + 1);
        label += sub_name_len + 1;
    } while (*label != 0);

    parsed_name[name_len - 1] = '\0';
    return label + 1;
}

// Developer Notes:
// This static function processes a DNS request packet (req) and constructs a response (dns_reply) redirecting type A
// (IPv4) queries based on the server’s rules. It validates the request length, copies it to the reply buffer, and adjusts
// the header to mark it as a response (QR_FLAG) with answers matching the question count. It then iterates through each
// question, parsing the name and checking its type (QD_TYPE_A). For A queries, it searches the handle’s entry array for
// a matching name or wildcard ("*"), retrieving the corresponding IP—either from a network interface (via if_key) or a
// static IP. The answer is formatted with a pointer to the question name, the IP, and a TTL (300s), appended to the reply.
// The function returns the reply length or -1 on error (e.g., overflow, parsing failure). It’s the core logic for DNS
// redirection, enabling captive portal functionality by pointing clients to a specific IP (e.g., a softAP).
static int parse_dns_request(char * req, size_t req_len, char * dns_reply, size_t dns_reply_max_len, dns_server_handle_t h)
{
    ESP_LOGD(TAG, "TEST");
    if (req_len > dns_reply_max_len) {
        return -1;
    }

    memset(dns_reply, 0, dns_reply_max_len);
    memcpy(dns_reply, req, req_len);

    dns_header_t * header = (dns_header_t *) dns_reply;
    ESP_LOGD(TAG, "DNS query with header id: 0x%X, flags: 0x%X, qd_count: %d", ntohs(header->id), ntohs(header->flags),
             ntohs(header->qd_count));

    if ((header->flags & OPCODE_MASK) != 0) {
        return 0;
    }

    header->flags |= QR_FLAG;

    uint16_t qd_count = ntohs(header->qd_count);
    header->an_count = htons(qd_count);

    int reply_len = qd_count * sizeof(dns_answer_t) + req_len;
    if (reply_len > dns_reply_max_len) {
        return -1;
    }

    char * cur_ans_ptr = dns_reply + req_len;
    char * cur_qd_ptr = dns_reply + sizeof(dns_header_t);
    char name[128];

    for (int qd_i = 0; qd_i < qd_count; qd_i++) {
        char * name_end_ptr = parse_dns_name(cur_qd_ptr, name, sizeof(name));
        if (name_end_ptr == NULL) {
            ESP_LOGE(TAG, "Failed to parse DNS question: %s", cur_qd_ptr);
            return -1;
        }

        dns_question_t * question = (dns_question_t *) (name_end_ptr);
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->class);

        ESP_LOGD(TAG, "Received type: %d | Class: %d | Question for: %s", qd_type, qd_class, name);

        if (qd_type == QD_TYPE_A) {
            esp_ip4_addr_t ip = {.addr = IPADDR_ANY};
            for (int i = 0; i < h->num_of_entries; ++i) {
                if (strcmp(h->entry[i].name, "*") == 0 || strcmp(h->entry[i].name, name) == 0) {
                    if (h->entry[i].if_key) {
                        esp_netif_ip_info_t ip_info;
                        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey(h->entry[i].if_key), &ip_info);
                        ip.addr = ip_info.ip.addr;
                        ESP_LOGD(TAG, "TEST %s", h->entry[i].if_key);
                        break;
                    } else if (h->entry->ip.addr != IPADDR_ANY) {
                        ip.addr = h->entry[i].ip.addr;
                        break;
                    }
                }
            }
            uint8_t octet1 = (ip.addr >> 24) & 0xFF;
            uint8_t octet2 = (ip.addr >> 16) & 0xFF;
            uint8_t octet3 = (ip.addr >> 8) & 0xFF;
            uint8_t octet4 = ip.addr & 0xFF;

            printf("IP address: %u.%u.%u.%u\n", octet1, octet2, octet3, octet4);

            if (ip.addr == IPADDR_ANY) {
                continue;
            }
            dns_answer_t * answer = (dns_answer_t *) cur_ans_ptr;

            answer->ptr_offset = htons(0xC000 | (cur_qd_ptr - dns_reply));
            answer->type = htons(qd_type);
            answer->class = htons(qd_class);
            answer->ttl = htonl(ANS_TTL_SEC);

            ESP_LOGD(TAG, "Answer with PTR offset: 0x%" PRIX16 " and IP 0x%" PRIX32, ntohs(answer->ptr_offset), ip.addr);

            answer->addr_len = htons(sizeof(ip.addr));
            answer->ip_addr = ip.addr;
        }
    }
    return reply_len;
}

// Developer Notes:
// This function runs as a FreeRTOS task, implementing the DNS server’s main loop. It creates a UDP socket bound to port
// 53 (DNS_PORT) on all interfaces (INADDR_ANY), listening for incoming DNS queries. Upon receiving a packet, it logs the
// source IP, parses the request using parse_dns_request, and sends a response redirecting type A queries to a configured
// IP (e.g., a softAP’s address). The task continues running while handle->started is true, handling errors (e.g., socket
// creation, binding, or sending failures) by logging them and either retrying or shutting down. If stopped, it cleans up
// the socket and deletes itself. This function is the heart of the DNS server, providing continuous redirection for
// captive portal or network control scenarios, leveraging UDP for lightweight, stateless communication.
void dns_server_task(void * pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family;
    int ip_protocol;
    dns_server_handle_t handle = pvParameters;

    while (handle->started) {

        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(DNS_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

        int err = bind(sock, (struct sockaddr *) &dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket bound, port %d", DNS_PORT);

        while (handle->started) {
            ESP_LOGI(TAG, "Waiting for data");
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *) &source_addr, &socklen);

            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                close(sock);
                break;
            }
            else {
                if (source_addr.sin6_family == PF_INET) {
                    inet_ntoa_r(((struct sockaddr_in *) &source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
                } else if (source_addr.sin6_family == PF_INET6) {
                    inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
                }

                rx_buffer[len] = 0;

                char reply[DNS_MAX_LEN];
                int reply_len = parse_dns_request(rx_buffer, len, reply, DNS_MAX_LEN, handle);

                ESP_LOGI(TAG, "Received %d bytes from %s | DNS reply with len: %d", len, addr_str, reply_len);
                if (reply_len <= 0) {
                    ESP_LOGE(TAG, "Failed to prepare a DNS reply");
                } else {
                    int err = sendto(sock, reply, reply_len, 0, (struct sockaddr *) &source_addr, sizeof(source_addr));
                    if (err < 0) {
                        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                        break;
                    }
                }
            }
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

// Developer Notes:
// This function initializes and starts the DNS server, returning a handle for control. It allocates memory for the
// dns_server_handle_t structure, including space for the configured number of dns_entry_pair_t rules (from config).
// If allocation fails, it logs an error and returns NULL using ESP_RETURN_ON_FALSE. It sets the started flag to true,
// copies the rules from the config, and launches the dns_server_task with a 4KB stack and priority 5, passing the handle
// as a parameter. The handle is used to manage the server’s lifecycle and rules, making this function the entry point
// for enabling DNS redirection (e.g., in an ESP-based softAP setup). It’s designed for simplicity and integration with
// Espressif’s networking stack.
dns_server_handle_t start_dns_server(dns_server_config_t * config)
{
    dns_server_handle_t handle = calloc(1, sizeof(struct dns_server_handle) + config->num_of_entries * sizeof(dns_entry_pair_t));
    ESP_RETURN_ON_FALSE(handle, NULL, TAG, "Failed to allocate dns server handle");

    handle->started = true;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item, config->num_of_entries * sizeof(dns_entry_pair_t));

    xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task);
    return handle;
}

// Developer Notes:
// This function stops the DNS server by cleaning up its resources. It checks if the handle is valid, sets the started
// flag to false (signaling the task to exit), deletes the FreeRTOS task, and frees the handle’s memory. The task’s
// self-deletion (vTaskDelete(NULL)) ensures it exits cleanly, but this function provides an external stop mechanism.
// It’s a straightforward shutdown routine, ensuring no memory leaks or dangling tasks remain after the server is no
// longer needed, typically called when disabling a captive portal or reconfiguring the network. It relies on the
// handle’s state to coordinate with the running task.
void stop_dns_server(dns_server_handle_t handle)
{
    if (handle) {
        handle->started = false;
        vTaskDelete(handle->task);
        free(handle);
    }
}
