// LDN service ABI and the ldn_mitm LAN wire format. Original implementation;
// layout references: switchbrew/libnx ldn.h and the ldn_mitm protocol headers.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t len; char text[33]; } dm_ldn_ssid;
typedef struct { int64_t local_id; uint8_t reserved[2]; uint16_t scene; uint8_t reserved2[4]; } dm_ldn_intent;
typedef struct { dm_ldn_intent intent; uint8_t session[16]; } dm_ldn_id;
typedef struct { uint32_t ip; uint8_t mac[6]; int8_t id; uint8_t connected; char name[33]; uint8_t platform; int16_t version; uint8_t reserved[16]; } dm_ldn_node;
typedef struct { uint8_t mac[6]; dm_ldn_ssid ssid; int16_t channel; int8_t link; uint8_t type; uint8_t reserved[4]; } dm_ldn_common;
typedef struct {
    dm_ldn_id id; dm_ldn_common common; uint8_t security[16];
    uint16_t security_mode; uint8_t accept_policy, version; uint8_t reserved[2];
    int8_t max_nodes; uint8_t node_count; dm_ldn_node nodes[8]; uint8_t reserved2[2];
    uint16_t advertise_size; uint8_t advertise[384], reserved3[140]; uint64_t reserved4;
} dm_ldn_network;
typedef struct { dm_ldn_intent intent; int16_t channel; int8_t max_nodes; uint8_t reserved; int16_t version; uint8_t reserved2[10]; } dm_ldn_network_config;
typedef struct { uint16_t mode, size; uint8_t passphrase[64]; } dm_ldn_security_config;
typedef struct { uint8_t server_random[16], session[16]; } dm_ldn_security_parameter;
typedef struct { char name[33]; uint8_t reserved[15]; } dm_ldn_user;
typedef struct { dm_ldn_id id; uint32_t type; uint8_t mac[6]; dm_ldn_ssid ssid; uint8_t reserved[16]; uint32_t flags; } dm_ldn_filter;
typedef struct { uint32_t ip; uint8_t mac[6], reserved[2]; } dm_ldn_address;
typedef struct { uint8_t change, reserved[7]; } dm_ldn_update;
_Static_assert(sizeof(dm_ldn_node) == 0x40, "LDN NodeInfo ABI");
_Static_assert(sizeof(dm_ldn_network) == 0x480, "LDN NetworkInfo ABI");
_Static_assert(offsetof(dm_ldn_network, nodes) == 0x68, "LDN node array offset");
_Static_assert(offsetof(dm_ldn_network, advertise) == 0x26c, "LDN advertise offset");
_Static_assert(sizeof(dm_ldn_network_config) == 0x20 && sizeof(dm_ldn_filter) == 0x60, "LDN config ABI");

enum { DM_LDN_PACKET_MAX = 2048, DM_LDN_SCAN = 0, DM_LDN_SCAN_RESPONSE = 1, DM_LDN_CONNECT = 2, DM_LDN_SYNC = 3 };
// Wire integers are little endian. A zero byte followed by N expands to N+1
// zeros. Nonzero bytes are literals. Unknown types and non-exact sizes fail.
int dm_ldn_packet_encode(uint8_t *out, size_t capacity, unsigned type, const void *payload, size_t size, int compress);
int dm_ldn_packet_size(const uint8_t header[12]);
int dm_ldn_packet_decode(const uint8_t *packet, size_t size, unsigned *type, void *payload, size_t capacity);
int dm_ldn_network_valid(const dm_ldn_network *network);
int dm_ldn_filter_match(const dm_ldn_network *network, const dm_ldn_filter *filter, int channel);

typedef struct dm_ldn_context dm_ldn_context;
// Addresses use native in_addr byte order; guest NodeInfo/Ipv4Address values
// use the SDK's host numeric order. A zero bind address listens on all adapters.
// A supplied locally administered MAC identifies a virtual adapter; zero derives
// the conventional ldn_mitm identity from its IPv4 address.
typedef struct { uint32_t address, mask, discovery, bind_address; uint16_t port, discovery_port; unsigned scan_ms, connect_ms; uint8_t mac[6]; } dm_ldn_transport;
typedef void (*dm_ldn_notify)(void *opaque);
dm_ldn_context *dm_ldn_start(const dm_ldn_transport *transport, dm_ldn_notify notify, void *opaque);
void dm_ldn_stop(dm_ldn_context *context);
uint32_t dm_ldn_open(dm_ldn_context *context, int access_point);
uint32_t dm_ldn_close(dm_ldn_context *context, int access_point);
int dm_ldn_state(dm_ldn_context *context);
int dm_ldn_reason(dm_ldn_context *context);
uint32_t dm_ldn_scan(dm_ldn_context *context, dm_ldn_network *out, int *count, int maximum, const dm_ldn_filter *filter, int channel);
uint32_t dm_ldn_create(dm_ldn_context *context, const dm_ldn_network_config *network, const dm_ldn_security_config *security, const dm_ldn_user *user, const dm_ldn_security_parameter *parameter, int address_count, const dm_ldn_address *addresses);
uint32_t dm_ldn_connect(dm_ldn_context *context, const dm_ldn_network *network, const dm_ldn_security_config *security, const dm_ldn_user *user, int version, int option);
uint32_t dm_ldn_disconnect(dm_ldn_context *context, int host);
uint32_t dm_ldn_get_network(dm_ldn_context *context, dm_ldn_network *network, dm_ldn_update *updates, int count);
uint32_t dm_ldn_advertise(dm_ldn_context *context, const void *data, size_t size);
uint32_t dm_ldn_policy(dm_ldn_context *context, int policy);
uint32_t dm_ldn_filter_add(dm_ldn_context *context, const uint8_t mac[6]);
uint32_t dm_ldn_filter_clear(dm_ldn_context *context);
uint32_t dm_ldn_reject(dm_ldn_context *context, uint32_t ip);
