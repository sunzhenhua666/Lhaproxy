#ifndef _HAPROXY_MYSQL_H
#define _HAPROXY_MYSQL_H

#include <haproxy/stream.h>

/* Structure to hold MySQL handshake parameters */
struct mysql_conn_params {
    unsigned char scramble[20];
    unsigned int server_caps;
};

/* MySQL Handshake States */
enum {
	SC_ST_MYSQL_HS_WAIT_GREETING = 0,
	SC_ST_MYSQL_HS_SEND_AUTH,
	SC_ST_MYSQL_HS_WAIT_OK,
};

/* Function declarations */
int mysql_process_handshake(struct stream *s);

////////////////////////////////////////////////////////
/* MySQL Protocol Constants */
#define MYSQL_PROTOCOL_VERSION 10
#define MYSQL_SERVER_VERSION "8.0.35-HAProxy-MySQL-SSL"
#define MYSQL_DEFAULT_CHARSET 33  /* utf8_general_ci */
#define MYSQL_CLIENT_SSL_FLAG 0x0800
#define MYSQL_SSL_REQUEST_SIZE 32

/* MySQL Server Greeting Packet Structure */
struct mysql_server_greeting {
	uint8_t protocol_version;
	char server_version[256];
	uint32_t connection_id;
	uint8_t auth_plugin_data_part1[8];
	uint8_t filler;
	uint16_t capability_flags_low;
	uint8_t character_set;
	uint16_t status_flags;
	uint16_t capability_flags_high;
	uint8_t auth_plugin_data_len;
	uint8_t reserved[10];
	uint8_t auth_plugin_data_part2[12];
	char auth_plugin_name[21];
} __attribute__((packed));

/* MySQL SSL Request Packet Structure */
struct mysql_ssl_request {
	uint32_t capability_flags;
	uint32_t max_packet_size;
	uint8_t character_set;
	uint8_t reserved[23];
} __attribute__((packed));

/*
 * Data structure for caching MySQL SSL Request packets, indexed by server address.
 */
struct mysql_ssl_request_node {
    char *server_addr;
    struct mysql_ssl_request ssl_request;
    struct mysql_ssl_request_node *next;
};

int add_or_update_ssl_request(const char *server_addr, const struct mysql_ssl_request *req);

/* Functions for storing and retrieving MySQL greeting data */
void mysql_store_greeting_data(const char *data, size_t length);
char* mysql_get_greeting_data(size_t *length);

#endif /* _HAPROXY_MYSQL_H */
