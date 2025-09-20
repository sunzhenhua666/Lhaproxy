/*
 * MySQL Handshake Logic for HAProxy
 */

#include <haproxy/mysql.h>
#include <haproxy/stream.h>
#include <haproxy/channel.h>
#include <haproxy/connection.h>
#include <haproxy/log.h>
#include <string.h>


static struct mysql_ssl_request_node *ssl_request_list_head = NULL;

/* Global storage for MySQL greeting packets */
struct mysql_greeting_storage {
    char *data;
    size_t length;
    time_t timestamp;
};

static struct mysql_greeting_storage stored_greeting = {NULL, 0, 0};

/*
 * Parse MySQL Server Greeting packet and extract key information
 */
static int parse_mysql_greeting(const char *data, uint32_t packet_length, struct mysql_server_greeting *greeting)
{
    size_t offset;
    size_t version_len;
    size_t max_len;
    size_t part2_len;
    size_t remaining;
    
    if (!data || !greeting || packet_length < 1) {
        return -1;
    }
    
    memset(greeting, 0, sizeof(*greeting));
    
    offset = 0;
    
    // Protocol version (1 byte)
    greeting->protocol_version = data[offset++];
    
    // Server version string (null-terminated)
    version_len = 0;
    max_len = packet_length - offset;
    
    for (size_t i = 0; i < max_len && i < sizeof(greeting->server_version) - 1; i++) {
        if (data[offset + i] == '\0') {
            version_len = i;
            break;
        }
        greeting->server_version[i] = data[offset + i];
    }
    
    if (version_len == 0) {
        return -1; // Invalid server version
    }
    
    offset += version_len + 1; // +1 for null terminator
    
    // Connection ID (4 bytes)
    if (offset + 4 > packet_length) return -1;
    greeting->connection_id = *(uint32_t*)(data + offset);
    offset += 4;
    
    // Auth plugin data part 1 (8 bytes)
    if (offset + 8 > packet_length) return -1;
    memcpy(greeting->auth_plugin_data_part1, data + offset, 8);
    offset += 8;
    
    // Filler (1 byte, should be 0x00)
    if (offset + 1 > packet_length) return -1;
    greeting->filler = data[offset++];
    
    // Capability flags lower 2 bytes
    if (offset + 2 > packet_length) return -1;
    greeting->capability_flags_low = *(uint16_t*)(data + offset);
    offset += 2;
    
    // Character set (1 byte)
    if (offset + 1 > packet_length) return -1;
    greeting->character_set = data[offset++];
    
    // Status flags (2 bytes)
    if (offset + 2 > packet_length) return -1;
    greeting->status_flags = *(uint16_t*)(data + offset);
    offset += 2;
    
    // Capability flags upper 2 bytes
    if (offset + 2 > packet_length) return -1;
    greeting->capability_flags_high = *(uint16_t*)(data + offset);
    offset += 2;
    
    // Auth plugin data length (1 byte)
    if (offset + 1 > packet_length) return -1;
    greeting->auth_plugin_data_len = data[offset++];
    
    // Reserved (10 bytes)
    if (offset + 10 > packet_length) return -1;
    memcpy(greeting->reserved, data + offset, 10);
    offset += 10;
    
    // Auth plugin data part 2 (up to 12 bytes, but depends on auth_plugin_data_len)
    part2_len = greeting->auth_plugin_data_len > 8 ? greeting->auth_plugin_data_len - 8 : 0;
    if (part2_len > 12) part2_len = 12;
    
    if (offset + part2_len <= packet_length) {
        memcpy(greeting->auth_plugin_data_part2, data + offset, part2_len);
        offset += part2_len;
    }
    
    // Auth plugin name (null-terminated string)
    if (offset < packet_length) {
        remaining = packet_length - offset;
        
        for (size_t i = 0; i < remaining && i < sizeof(greeting->auth_plugin_name) - 1; i++) {
            if (data[offset + i] == '\0') {
                break;
            }
            greeting->auth_plugin_name[i] = data[offset + i];
        }
    }
    
    return 0; // Success
}

/*
 * Store MySQL greeting packet data for later retrieval
 */
void mysql_store_greeting_data(const char *data, size_t length)
{
    if (stored_greeting.data) {
        free(stored_greeting.data);
    }
    
    stored_greeting.data = malloc(length);
    if (stored_greeting.data) {
        memcpy(stored_greeting.data, data, length);
        stored_greeting.length = length;
        stored_greeting.timestamp = time(NULL);
        ha_notice("MySQL: Stored greeting data (%zu bytes)\n", length);
    } else {
        ha_alert("MySQL: Failed to allocate memory for greeting data\n");
        stored_greeting.length = 0;
        stored_greeting.timestamp = 0;
    }
}

/*
 * Retrieve stored MySQL greeting packet data
 */
char* mysql_get_greeting_data(size_t *length)
{
    if (stored_greeting.data && stored_greeting.length > 0) {
        *length = stored_greeting.length;
        return stored_greeting.data;
    }
    
    *length = 0;
    return NULL;
}

/*
 * Finds an SSL request packet in the list by server address.
 */
struct mysql_ssl_request_node* find_ssl_request(const char *server_addr)
{
    struct mysql_ssl_request_node *curr = ssl_request_list_head;
    while (curr) {
        if (strcmp(curr->server_addr, server_addr) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

/*
 * Adds a new SSL request to the list, or updates it if the server address already exists.
 */
int add_or_update_ssl_request(const char *server_addr, const struct mysql_ssl_request *req)
{
    struct mysql_ssl_request temp_req = *req;
    struct mysql_ssl_request_node *node;

    // Remove the SSL flag before caching, as requested.
    temp_req.capability_flags &= ~MYSQL_CLIENT_SSL_FLAG;

    node = find_ssl_request(server_addr);
    if (node) {
        // Found, update existing node
        node->ssl_request = temp_req;
        ha_notice("MySQL SSL: Updated SSL request for server %s\n", server_addr);
        return 0;
    }

    // Not found, create a new node
    node = (struct mysql_ssl_request_node *)malloc(sizeof(struct mysql_ssl_request_node));
    if (!node) {
        ha_alert("MySQL SSL: Failed to allocate memory for SSL request node\n");
        return -1;
    }

    // Use strdup to create a copy of the address for safe memory management.
    node->server_addr = strdup(server_addr);
    if (!node->server_addr) {
        ha_alert("MySQL SSL: Failed to duplicate server address for SSL request node\n");
        free(node);
        return -1;
    }
    
    node->ssl_request = temp_req;
    node->next = ssl_request_list_head;
    ssl_request_list_head = node;

    ha_notice("MySQL SSL: Added new SSL request for server %s\n", server_addr);
    return 0;
}

/*
 * Deletes an SSL request packet from the list by server address.
 */
int delete_ssl_request(const char *server_addr)
{
    struct mysql_ssl_request_node *curr = ssl_request_list_head;
    struct mysql_ssl_request_node *prev = NULL;

    while (curr) {
        if (strcmp(curr->server_addr, server_addr) == 0) {
            if (prev) {
                prev->next = curr->next;
            } else {
                ssl_request_list_head = curr->next;
            }
            free(curr->server_addr);
            free(curr);
            ha_notice("MySQL SSL: Deleted SSL request for server %s\n", server_addr);
            return 1; // Found and deleted
        }
        prev = curr;
        curr = curr->next;
    }
    return 0; // Not found
}

/* 
 * Process the MySQL handshake based on the current stream state.
 * This function acts as a state machine.
 */
int mysql_process_handshake(struct stream *s)
{
    struct stconn *sc = s->scb;
    struct channel *res = &s->res;  // response channel (从服务器来的数据)
    struct mysql_ssl_request_node *ssl_request_node;
    size_t available_data;
    char *data_ptr;
    uint32_t packet_length;
    uint8_t sequence_id;
    uint32_t total_packet_size;
    struct mysql_server_greeting greeting;
    char scramble_hex[41];
    size_t part2_len;
    uint32_t full_capability;
    char complete_scramble[21]; // 20字节scramble + null terminator
    
    ha_notice("MySQL Backend: Starting handshake process for server %s\n", 
             s->srv_conn->mysql_tcp_lookup_addr ? s->srv_conn->mysql_tcp_lookup_addr : "unknown");

    switch (sc->mysql_handshake_step)
    {
    case 0:
        // 初始化握手状态 - 准备接收greeting包
        sc->mysql_handshake_step = 1;
        sc->greeting_size = 0;
        sc->greeting_bytes_to_receive = 0;
        sc->mysql_handshake_retries = 0;
        ha_notice("MySQL Backend: Step 0->1 - Initializing to receive greeting\n");
        // fall through
        
    case 1:
        // Step 1: 接收并解析 MySQL greeting 包
        available_data = ci_data(res);
        sc->mysql_handshake_retries++;
        
        // 防止无限循环
        if (sc->mysql_handshake_retries > 10) {
            ha_alert("MySQL Backend: Too many retries (%d), aborting handshake\n", sc->mysql_handshake_retries);
            return -1;
        }
        
        ha_notice("MySQL Backend: Step 1 - Available data: %zu bytes (retry: %d)\n", available_data, sc->mysql_handshake_retries);
        
        if (available_data == 0) {
            // 在backend正式连接中，需要等待MySQL服务器发送真正的greeting包
            // 这个greeting包与SSL预握手阶段的是不同的连接，有不同的connection_id和scramble
            ha_notice("MySQL Backend: Waiting for MySQL server greeting on backend connection\n");
            return 0; // 继续等待真正的greeting包
        }
        
        data_ptr = ci_head(res);
        
        // 检查包头是否完整
        if (available_data < 4) {
            ha_notice("MySQL Backend: Waiting for complete packet header (%zu/4 bytes)\n", available_data);
            return 0;
        }
        
        // 解析MySQL包头
        packet_length = data_ptr[0] | (data_ptr[1] << 8) | (data_ptr[2] << 16);
        sequence_id = data_ptr[3];
        total_packet_size = packet_length + 4;
        
        ha_notice("MySQL Backend: Packet header - Length: %u, Sequence: %d, Total: %u\n", 
                 packet_length, sequence_id, total_packet_size);
        
        // 检查完整包是否到达
        if (available_data < total_packet_size) {
            ha_notice("MySQL Backend: Waiting for complete packet (%zu/%u bytes)\n", 
                     available_data, total_packet_size);
            sc->greeting_bytes_to_receive = total_packet_size;
            return 0;
        }
        
        // 4. 解析greeting包，提取scramble
        if (parse_mysql_greeting((char*)&data_ptr[4], packet_length, &greeting) == 0) {
            ha_notice("MySQL Backend: Successfully parsed greeting packet:\n");
            ha_notice("  Protocol Version: %d\n", greeting.protocol_version);
            ha_notice("  Server Version: %s\n", greeting.server_version);
            ha_notice("  Connection ID: %u\n", greeting.connection_id);
            
            // 5. 提取并缓存完整的scramble
            memset(complete_scramble, 0, sizeof(complete_scramble));
            memcpy(complete_scramble, greeting.auth_plugin_data_part1, 8);
            
            part2_len = greeting.auth_plugin_data_len > 8 ? greeting.auth_plugin_data_len - 8 : 0;
            if (part2_len > 12) part2_len = 12;
            if (part2_len > 0) {
                memcpy(complete_scramble + 8, greeting.auth_plugin_data_part2, part2_len);
            }
            
            // 打印scramble用于调试
            memset(scramble_hex, 0, sizeof(scramble_hex));
            for (int i = 0; i < 20 && complete_scramble[i]; i++) {
                sprintf(scramble_hex + i*2, "%02x", (unsigned char)complete_scramble[i]);
            }
            ha_notice("  Extracted Scramble: %s\n", scramble_hex);
            
            // 转到下一步：查找SSL request并发送认证
            sc->mysql_handshake_step = 2;
            return 0; // 继续处理
            
        } else {
            ha_alert("MySQL Backend: Failed to parse greeting packet\n");
            return -1;
        }
        
        data_ptr = ci_head(res);
        
        // 检查是否有足够的数据读取包头 (至少4字节: 3字节长度 + 1字节序列号)
        if (available_data < 4) {
            ha_notice("MySQL: Insufficient data for packet header (%zu bytes), waiting for more\n", available_data);
            return 0; // 等待更多数据
        }
        
        // 解析 MySQL 包头
        packet_length = data_ptr[0] | (data_ptr[1] << 8) | (data_ptr[2] << 16);
        sequence_id = data_ptr[3];
        
        // 计算完整包的大小 (包头4字节 + 包体)
        total_packet_size = packet_length + 4;
        
        if (available_data < total_packet_size) {
            // 数据不完整，等待更多数据
            ha_notice("MySQL: Incomplete packet: have %zu bytes, need %u bytes\n", 
                     available_data, total_packet_size);
            sc->greeting_bytes_to_receive = total_packet_size;
            return 0;
        }
        
        // 成功接收到完整的 greeting 包
        sc->greeting_size = total_packet_size;
        sc->greeting_bytes_to_receive = total_packet_size;
        
        // 解析 greeting 包内容
        if (parse_mysql_greeting((char*)&data_ptr[4], packet_length, &greeting) == 0) {
            // 成功解析 greeting 包
            ha_notice("MySQL Greeting Received:\n");
            ha_notice("  Protocol Version: %d\n", greeting.protocol_version);
            ha_notice("  Server Version: %s\n", greeting.server_version);
            ha_notice("  Connection ID: %u\n", greeting.connection_id);
            ha_notice("  Packet Length: %u bytes\n", packet_length);
            ha_notice("  Sequence ID: %d\n", sequence_id);
            ha_notice("  Character Set: %d\n", greeting.character_set);
            ha_notice("  Status Flags: 0x%04x\n", greeting.status_flags);
            ha_notice("  Capability Flags: 0x%04x%04x\n", 
                     greeting.capability_flags_high, greeting.capability_flags_low);
            ha_notice("  Auth Plugin Data Len: %d\n", greeting.auth_plugin_data_len);
            ha_notice("  Auth Plugin Name: %s\n", greeting.auth_plugin_name);
            
            // 打印完整的 scramble (salt)
            memset(scramble_hex, 0, sizeof(scramble_hex));
            for (int i = 0; i < 8; i++) {
                sprintf(scramble_hex + i*2, "%02x", greeting.auth_plugin_data_part1[i]);
            }
            part2_len = greeting.auth_plugin_data_len > 8 ? greeting.auth_plugin_data_len - 8 : 0;
            if (part2_len > 12) part2_len = 12;
            for (size_t i = 0; i < part2_len; i++) {
                sprintf(scramble_hex + 16 + i*2, "%02x", greeting.auth_plugin_data_part2[i]);
            }
            ha_notice("  Complete Scramble (Salt): %s\n", scramble_hex);
            
            // 检查是否支持 SSL
            full_capability = (greeting.capability_flags_high << 16) | greeting.capability_flags_low;
            if (full_capability & MYSQL_CLIENT_SSL_FLAG) {
                ha_notice("  Server supports SSL connection\n");
            } else {
                ha_notice("  Server does not support SSL connection\n");
            }
            
        } else {
            ha_alert("MySQL: Failed to parse greeting packet\n");
        }
        
        // Greeting 包解析完成，现在可以完成握手
        ha_notice("MySQL: Greeting packet parsed successfully, completing handshake\n");
        
        // 重置握手状态
        sc->mysql_handshake_step = 0;
        sc->greeting_bytes_to_receive = 0;
        sc->greeting_size = 0;
        
        // 握手完成，允许转换到 EST 状态
        return 1;
        
    case 2:
        // Step 2: 查找缓存的SSL request包并发送认证
        ha_notice("MySQL Backend: Step 2 - Looking up cached SSL request\n");
        
        if (!s->srv_conn || !s->srv_conn->mysql_tcp_lookup_addr) {
            ha_alert("MySQL Backend: No mysql_tcp_lookup_addr available\n");
            return -1;
        }
        
        // 6. 根据mysql-tcp中的地址查找缓存的SSL request包
        ssl_request_node = find_ssl_request(s->srv_conn->mysql_tcp_lookup_addr);
        
        if (!ssl_request_node) {
            ha_alert("MySQL Backend: No cached SSL request found for server %s\n", 
                    s->srv_conn->mysql_tcp_lookup_addr);
            return -1;
        }
        
        ha_notice("MySQL Backend: Found cached SSL request for server %s\n", 
                 s->srv_conn->mysql_tcp_lookup_addr);
        
        // 7. 这里应该根据scramble修改SSL request包内容并发送给服务器
        // 现在先输出调试信息并完成握手
        ha_notice("MySQL Backend: SSL request capability_flags: 0x%08x\n", 
                 ssl_request_node->ssl_request.capability_flags);
        ha_notice("MySQL Backend: SSL request max_packet_size: %u\n", 
                 ssl_request_node->ssl_request.max_packet_size);
        ha_notice("MySQL Backend: SSL request character_set: %d\n", 
                 ssl_request_node->ssl_request.character_set);
        
        // TODO: 实现实际的认证包构造和发送
        // 这里需要：
        // 1. 构造认证响应包（使用scramble）
        // 2. 通过channel发送给服务器
        // 3. 等待服务器响应
        
        ha_notice("MySQL Backend: Authentication packet sending not yet implemented\n");
        
        // 8. 完成握手，进入EST状态
        sc->mysql_handshake_step = 0;
        sc->greeting_bytes_to_receive = 0;
        sc->greeting_size = 0;
        sc->mysql_handshake_retries = 0;
        
        ha_notice("MySQL Backend: Handshake completed, transitioning to EST state\n");
        return 1;
        break;

    default:
        ha_alert("MySQL: Unknown handshake step: %d\n", sc->mysql_handshake_step);
        return -1; // 未知状态，错误
    }

    return 0; // 默认继续处理
}


/*
 * Scramble password for MySQL 4.1+ native authentication.
 * scramble_result = SHA1(password) XOR SHA1(scramble + SHA1(SHA1(password)))
 */
static void scramble_password(unsigned char *to, const unsigned char *scramble, const char *password)
{
    // struct sha1_ctx ctx;
    // unsigned char hash_stage1[20];
    // unsigned char hash_stage2[20];
    // int i;

    // // Stage 1: SHA1(password)
    // sha1_init(&ctx);
    // sha1_update(&ctx, password, strlen(password));
    // sha1_final(&ctx, hash_stage1);

    // // Stage 2: SHA1(scramble + SHA1(hash_stage1))
    // sha1_init(&ctx);
    // sha1_update(&ctx, scramble, 20);
    // sha1_update(&ctx, hash_stage1, 20);
    // sha1_final(&ctx, hash_stage2);

    // // Result: hash_stage1 XOR hash_stage2
    // for (i = 0; i < 20; i++) {
    //     to[i] = hash_stage1[i] ^ hash_stage2[i];
    // }
}
