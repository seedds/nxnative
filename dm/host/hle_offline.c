// Networking and peripheral system services (out of scope: online play, local wireless, friends, telemetry, SSL) as seen through newer SDKs: all made "unavailable", the game takes its offline branch.
// Only entry points that go to a service are hooked; library functions that only read and write the game's own buffers (prepo Add / Array / Struct, err ErrorResultVariant, friends getters, socket byte-order conversion) keep running in the SDK.
// Return convention as in hle_friends.c: Result returns RES_FAIL ("saved / submitted" kinds return RES_OK so the game does not retry); bool returns false; events get a bound SystemEvent that never fires.
// UPDATE 2026-10-31: LDN (local wireless) now implements local network discovery for same-WiFi multiplayer
#include "hle.h"
#include <string.h>
#include <stdatomic.h>

static _Alignas(16) uint8_t g_ev_ldn[0x100];

// LDN state machine for local wireless
enum {
    LDN_STATE_NONE = 0,
    LDN_STATE_INITIALIZED = 1,
    LDN_STATE_ACCESS_POINT = 2,
    LDN_STATE_ACCESS_POINT_CREATED = 3,
    LDN_STATE_STATION = 4,
    LDN_STATE_STATION_CONNECTED = 5,
};

static _Atomic int g_ldn_state = LDN_STATE_NONE;
static _Atomic int g_ldn_initialized = 0;

// Network constants
#define LDN_PORT 11451
#define LDN_SSID_MAX 32
#define LDN_ADVERTISE_DATA_MAX 384
#define LDN_USERNAME_MAX 32
#define LDN_NODE_COUNT_MAX 8
#define LDN_PASSPHRASE_MAX 64

// Structure definitions matching Switch SDK layout (from eden ldn_types.h)
typedef struct {
    uint8_t length;
    char raw[LDN_SSID_MAX + 1];
} __attribute__((packed)) LdnSsid;

typedef struct {
    uint8_t raw[4];  // Network byte order
} __attribute__((packed)) LdnIpv4Address;

typedef struct {
    uint8_t raw[6];
} __attribute__((packed)) LdnMacAddress;

typedef struct {
    uint64_t high;
    uint64_t low;
} __attribute__((packed)) LdnSessionId;

typedef struct {
    uint64_t local_communication_id;
    uint8_t _reserved1[2];
    uint16_t scene_id;
    uint8_t _reserved2[4];
} __attribute__((packed)) LdnIntentId;

typedef struct {
    LdnIntentId intent_id;
    LdnSessionId session_id;
} __attribute__((packed)) LdnNetworkId;

typedef struct {
    LdnMacAddress bssid;
    LdnSsid ssid;
    int16_t channel;
    int8_t link_level;
    uint8_t network_type;
    uint8_t _reserved[4];
} __attribute__((packed)) LdnCommonNetworkInfo;

typedef struct {
    LdnIpv4Address ipv4_address;
    LdnMacAddress mac_address;
    int8_t node_id;
    uint8_t is_connected;
    uint8_t user_name[LDN_USERNAME_MAX + 1];
    uint8_t _reserved1;
    int16_t local_communication_version;
    uint8_t _reserved2[16];
} __attribute__((packed)) LdnNodeInfo;

typedef struct {
    uint8_t security_parameter[16];
    uint16_t security_mode;
    uint8_t station_accept_policy;
    uint8_t has_action_frame;
    uint8_t _pad[2];
    uint8_t node_count_max;
    uint8_t node_count;
    LdnNodeInfo nodes[LDN_NODE_COUNT_MAX];
    uint8_t _reserved1[2];
    uint16_t advertise_data_size;
    uint8_t advertise_data[LDN_ADVERTISE_DATA_MAX];
    uint8_t _reserved2[140];
    uint64_t random_authentication_id;
} __attribute__((packed)) LdnNetworkInfo;

typedef struct {
    LdnNetworkId network_id;
    LdnCommonNetworkInfo common;
    LdnNetworkInfo ldn;
} __attribute__((packed)) LdnFullNetworkInfo;

typedef struct {
    uint16_t security_mode;
    uint16_t passphrase_size;
    uint8_t passphrase[LDN_PASSPHRASE_MAX];
} __attribute__((packed)) LdnSecurityConfig;

typedef struct {
    uint8_t user_name[LDN_USERNAME_MAX + 1];
    uint8_t _reserved[15];
} __attribute__((packed)) LdnUserConfig;

typedef struct {
    LdnIntentId intent_id;
    int16_t channel;
    uint8_t node_count_max;
    uint8_t _reserved1;
    uint16_t local_communication_version;
    uint8_t _reserved2[10];
} __attribute__((packed)) LdnNetworkConfig;

// Verify structure sizes match Switch SDK
_Static_assert(sizeof(LdnSsid) == 0x22, "LdnSsid size mismatch");
_Static_assert(sizeof(LdnNodeInfo) == 0x40, "LdnNodeInfo size mismatch");
_Static_assert(sizeof(LdnNetworkInfo) == 0x430, "LdnNetworkInfo size mismatch");
_Static_assert(sizeof(LdnFullNetworkInfo) == 0x480, "LdnFullNetworkInfo size mismatch");
_Static_assert(sizeof(LdnNetworkConfig) == 0x20, "LdnNetworkConfig size mismatch");

// UDP packet types
enum {
    LDN_PACKET_SCAN = 0,
    LDN_PACKET_SCAN_RESP = 1,
    LDN_PACKET_CONNECT = 2,
    LDN_PACKET_DISCONNECT = 3,
    LDN_PACKET_DESTROY = 4,
    LDN_PACKET_SYNC = 5,
};

// Network state
static int g_ldn_socket = -1;
static LdnFullNetworkInfo g_network_info;
static LdnFullNetworkInfo g_scan_results[32];
static _Atomic int g_scan_result_count = 0;

// Phase 3: Background listener thread
#include <pthread.h>
static pthread_t g_listener_thread;
static _Atomic int g_listener_running = 0;

// Connected clients tracking
#define MAX_CONNECTED_CLIENTS 7
typedef struct {
    char ip[16];
    LdnMacAddress mac;
    int8_t node_id;
    uint64_t last_seen_ns;
} ConnectedClient;

static ConnectedClient g_connected_clients[MAX_CONNECTED_CLIENTS];
static _Atomic int g_connected_client_count = 0;

// Socket and network utilities
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <errno.h>

static int ldn_socket_init(void) {
    if (g_ldn_socket >= 0) return 1;

    g_ldn_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_ldn_socket < 0) return 0;

    // Enable broadcast
    int broadcast = 1;
    setsockopt(g_ldn_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Reuse address
    int reuse = 1;
    setsockopt(g_ldn_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind to LDN port
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LDN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_ldn_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_ldn_socket);
        g_ldn_socket = -1;
        return 0;
    }

    // Non-blocking mode
    int flags = fcntl(g_ldn_socket, F_GETFL, 0);
    fcntl(g_ldn_socket, F_SETFL, flags | O_NONBLOCK);

    return 1;
}

static void ldn_socket_cleanup(void) {
    if (g_ldn_socket >= 0) {
        close(g_ldn_socket);
        g_ldn_socket = -1;
    }
}

static int ldn_get_local_ip(uint32_t *ip_out, uint32_t *mask_out) {
    struct ifaddrs *ifaddr, *ifa;

    if (getifaddrs(&ifaddr) == -1) return 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;

        // Look for IPv4 on WiFi (en0 iOS, en1 some Macs)
        if (ifa->ifa_addr->sa_family == AF_INET &&
            (strncmp(ifa->ifa_name, "en0", 3) == 0 || strncmp(ifa->ifa_name, "en1", 3) == 0)) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            *ip_out = ntohl(sa->sin_addr.s_addr);

            if (ifa->ifa_netmask) {
                struct sockaddr_in *sm = (struct sockaddr_in *)ifa->ifa_netmask;
                *mask_out = ntohl(sm->sin_addr.s_addr);
            } else {
                *mask_out = 0xFFFFFF00; // Default /24
            }

            freeifaddrs(ifaddr);
            return 1;
        }
    }

    freeifaddrs(ifaddr);
    return 0;
}

static void ldn_ip_to_bytes(uint32_t ip, uint8_t *out) {
    out[0] = (ip >> 24) & 0xFF;
    out[1] = (ip >> 16) & 0xFF;
    out[2] = (ip >> 8) & 0xFF;
    out[3] = ip & 0xFF;
}

static void ldn_generate_mac(uint32_t ip, LdnMacAddress *mac) {
    mac->raw[0] = 0x02; // Locally administered
    mac->raw[1] = (ip >> 24) & 0xFF;
    mac->raw[2] = (ip >> 16) & 0xFF;
    mac->raw[3] = (ip >> 8) & 0xFF;
    mac->raw[4] = ip & 0xFF;
    mac->raw[5] = 0x01;
}

static void ldn_send_packet(uint8_t type, const void *data, size_t data_len, const char *dest_ip) {
    if (g_ldn_socket < 0) return;

    // Simple packet: type byte + data
    uint8_t packet[2048];
    if (data_len + 1 > sizeof(packet)) return;

    packet[0] = type;
    if (data && data_len > 0) {
        memcpy(packet + 1, data, data_len);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LDN_PORT);
    inet_pton(AF_INET, dest_ip, &addr.sin_addr);

    sendto(g_ldn_socket, packet, data_len + 1, 0, (struct sockaddr *)&addr, sizeof(addr));
}

static void ldn_send_broadcast(uint8_t type, const void *data, size_t data_len) {
    ldn_send_packet(type, data, data_len, "255.255.255.255");
}

static int ldn_receive_packet(uint8_t *type_out, void *data_out, size_t *data_len_out, size_t max_len) {
    if (g_ldn_socket < 0) return 0;

    uint8_t packet[2048];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    ssize_t len = recvfrom(g_ldn_socket, packet, sizeof(packet), 0, (struct sockaddr *)&from, &fromlen);
    if (len <= 0) return 0;

    *type_out = packet[0];
    size_t payload_len = (size_t)(len - 1);

    if (payload_len > 0 && data_out && payload_len <= max_len) {
        memcpy(data_out, packet + 1, payload_len);
        if (data_len_out) *data_len_out = payload_len;
    }

    return 1;
}

static int ldn_receive_packet_with_sender(uint8_t *type_out, void *data_out, size_t *data_len_out, size_t max_len, char *sender_ip) {
    if (g_ldn_socket < 0) return 0;

    uint8_t packet[2048];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    ssize_t len = recvfrom(g_ldn_socket, packet, sizeof(packet), 0, (struct sockaddr *)&from, &fromlen);
    if (len <= 0) return 0;

    if (sender_ip) {
        inet_ntop(AF_INET, &from.sin_addr, sender_ip, INET_ADDRSTRLEN);
    }

    *type_out = packet[0];
    size_t payload_len = (size_t)(len - 1);

    if (payload_len > 0 && data_out && payload_len <= max_len) {
        memcpy(data_out, packet + 1, payload_len);
        if (data_len_out) *data_len_out = payload_len;
    }

    return 1;
}

// Find connected client by IP
static ConnectedClient* ldn_find_client_by_ip(const char *ip) {
    int count = atomic_load(&g_connected_client_count);
    for (int i = 0; i < count; i++) {
        if (strcmp(g_connected_clients[i].ip, ip) == 0) {
            return &g_connected_clients[i];
        }
    }
    return NULL;
}

// Add connected client
static int ldn_add_client(const char *ip, const LdnNodeInfo *node_info) {
    int count = atomic_load(&g_connected_client_count);
    if (count >= MAX_CONNECTED_CLIENTS) return -1;

    // Find first free node ID (1-7, 0 is host)
    int8_t node_id = -1;
    for (int8_t i = 1; i <= MAX_CONNECTED_CLIENTS; i++) {
        int found = 0;
        for (int j = 0; j < count; j++) {
            if (g_connected_clients[j].node_id == i) {
                found = 1;
                break;
            }
        }
        if (!found) {
            node_id = i;
            break;
        }
    }
    if (node_id < 0) return -1;

    ConnectedClient *client = &g_connected_clients[count];
    strncpy(client->ip, ip, 15);
    client->ip[15] = 0;
    memcpy(&client->mac, &node_info->mac_address, sizeof(LdnMacAddress));
    client->node_id = node_id;
    client->last_seen_ns = now_ns();

    atomic_fetch_add(&g_connected_client_count, 1);
    return node_id;
}

// Remove connected client
static void ldn_remove_client(const char *ip) {
    int count = atomic_load(&g_connected_client_count);
    for (int i = 0; i < count; i++) {
        if (strcmp(g_connected_clients[i].ip, ip) == 0) {
            // Shift array down
            for (int j = i; j < count - 1; j++) {
                g_connected_clients[j] = g_connected_clients[j + 1];
            }
            atomic_fetch_sub(&g_connected_client_count, 1);
            break;
        }
    }
}

// Background listener thread for auto-responding to packets
static void* ldn_listener_thread(void* arg) {
    (void)arg;

    while (atomic_load(&g_listener_running)) {
        uint8_t type;
        union {
            LdnFullNetworkInfo network_info;
            LdnNodeInfo node_info;
            uint8_t raw[2048];
        } data;
        size_t len;
        char sender_ip[INET_ADDRSTRLEN];

        if (ldn_receive_packet_with_sender(&type, &data, &len, sizeof(data), sender_ip)) {
            int state = atomic_load(&g_ldn_state);

            switch(type) {
                case LDN_PACKET_SCAN:
                    // Auto-respond to scan if we're hosting
                    if (state == LDN_STATE_ACCESS_POINT_CREATED) {
                        ldn_send_packet(LDN_PACKET_SCAN_RESP, &g_network_info,
                                       sizeof(g_network_info), sender_ip);
                    }
                    break;

                case LDN_PACKET_CONNECT:
                    // Handle client connection
                    if (state == LDN_STATE_ACCESS_POINT_CREATED && len >= sizeof(LdnNodeInfo)) {
                        int8_t node_id = ldn_add_client(sender_ip, &data.node_info);
                        if (node_id >= 0) {
                            // Update network info with new node
                            LdnNodeInfo *node = &g_network_info.ldn.nodes[node_id];
                            memcpy(node, &data.node_info, sizeof(LdnNodeInfo));
                            node->node_id = node_id;
                            node->is_connected = 1;
                            g_network_info.ldn.node_count++;

                            // Send updated network info to all clients
                            int count = atomic_load(&g_connected_client_count);
                            for (int i = 0; i < count; i++) {
                                ldn_send_packet(LDN_PACKET_SYNC, &g_network_info,
                                              sizeof(g_network_info), g_connected_clients[i].ip);
                            }
                        }
                    }
                    break;

                case LDN_PACKET_DISCONNECT:
                    // Handle client disconnection
                    if (state == LDN_STATE_ACCESS_POINT_CREATED && len >= sizeof(LdnNodeInfo)) {
                        ConnectedClient *client = ldn_find_client_by_ip(sender_ip);
                        if (client) {
                            int8_t node_id = client->node_id;
                            ldn_remove_client(sender_ip);

                            // Clear node in network info
                            g_network_info.ldn.nodes[node_id].is_connected = 0;
                            g_network_info.ldn.node_count--;

                            // Notify remaining clients
                            int count = atomic_load(&g_connected_client_count);
                            for (int i = 0; i < count; i++) {
                                ldn_send_packet(LDN_PACKET_SYNC, &g_network_info,
                                              sizeof(g_network_info), g_connected_clients[i].ip);
                            }
                        }
                    }
                    break;

                case LDN_PACKET_SYNC:
                    // Client receives network sync from host
                    if ((state == LDN_STATE_STATION || state == LDN_STATE_STATION_CONNECTED)
                        && len >= sizeof(LdnFullNetworkInfo)) {
                        memcpy(&g_network_info, &data.network_info, sizeof(LdnFullNetworkInfo));
                        if (state == LDN_STATE_STATION) {
                            atomic_store(&g_ldn_state, LDN_STATE_STATION_CONNECTED);
                        }
                    }
                    break;

                case LDN_PACKET_DESTROY:
                    // Host destroyed network
                    if (state == LDN_STATE_STATION_CONNECTED) {
                        atomic_store(&g_ldn_state, LDN_STATE_STATION);
                    }
                    break;
            }
        }

        usleep(10000); // 10ms poll interval
    }

    return NULL;
}

// Start background listener
static int ldn_start_listener(void) {
    if (atomic_load(&g_listener_running)) return 1;

    atomic_store(&g_listener_running, 1);
    if (pthread_create(&g_listener_thread, NULL, ldn_listener_thread, NULL) != 0) {
        atomic_store(&g_listener_running, 0);
        return 0;
    }

    return 1;
}

// Stop background listener
static void ldn_stop_listener(void) {
    if (atomic_load(&g_listener_running)) {
        atomic_store(&g_listener_running, 0);
        pthread_join(g_listener_thread, NULL);
    }
}

// ---------- prepo: telemetry reports. Saving / submitting all report success; nothing is sent ----------
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "telemetry report save (user); nothing is sent, reports success", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "telemetry report save; nothing is sent, reports success", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10InitializeEv", "telemetry init: the original connects to the prepo service (through sm), the host has no service process; no-op (Engage calls it explicitly)", Result, h_prepo_Init, void) { return RES_OK; }
// Add the session ID / connection status to a report: on hardware these first connect to the prepo service for the session (Super Mario 3D World + Bowser's Fury calls them entering the save select); the host adds nothing and reports success
HLE_FORCE("_ZN2nn5prepo12AddSessionIdEPNS0_10PlayReportEPKc", "telemetry session ID: no service connection, nothing added, reports success", Result, h_prepo_AddSess, void *rep, const char *key) { (void)rep; (void)key; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", "telemetry internet connection status: no service connection, nothing added, reports success", Result, h_prepo_AddNet, void *rep, const char *a, const char *b, const char *c) { (void)rep; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "telemetry immediate upload; nothing is sent, reports success", Result, h_prepo_Tx, void) { return RES_OK; }

// ---------- ldn: local wireless - Local network multiplayer (Phase 1: state machine) ----------

HLE_FORCE("_ZN2nn3ldn10InitializeEv", "local wireless: initialization succeeds, enables local network discovery", Result, h_ldn_Init, void) {
    if (atomic_load(&g_ldn_initialized)) return RES_OK;

    // Initialize socket for network discovery
    if (!ldn_socket_init()) {
        // Socket init failed but still succeed - will be offline
    }

    // Clear state
    memset(&g_network_info, 0, sizeof(g_network_info));
    atomic_store(&g_scan_result_count, 0);
    atomic_store(&g_connected_client_count, 0);

    atomic_store(&g_ldn_state, LDN_STATE_INITIALIZED);
    atomic_store(&g_ldn_initialized, 1);
    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn8FinalizeEv", "local wireless: finalize", void, h_ldn_Fini, void) {
    ldn_stop_listener();
    ldn_socket_cleanup();
    atomic_store(&g_ldn_state, LDN_STATE_NONE);
    atomic_store(&g_ldn_initialized, 0);
    atomic_store(&g_connected_client_count, 0);
}

HLE_FORCE("_ZN2nn3ldn8GetStateEv", "local wireless: returns current state", int, h_ldn_State, void) {
    return atomic_load(&g_ldn_state);
}

HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "local wireless state event", void, h_ldn_Attach, void *ev) {
    hle_bind_system_event(ev, 0);
    (void)g_ev_ldn;
}

HLE_FORCE("_ZN2nn3ldn11OpenStationEv", "local wireless: open station (client mode)", Result, h_ldn_OpenSta, void) {
    int expected = LDN_STATE_INITIALIZED;
    if (atomic_compare_exchange_strong(&g_ldn_state, &expected, LDN_STATE_STATION)) {
        return RES_OK;
    }
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn12CloseStationEv", "local wireless: close station", Result, h_ldn_CloseSta, void) {
    int state = atomic_load(&g_ldn_state);
    if (state == LDN_STATE_STATION || state == LDN_STATE_STATION_CONNECTED) {
        atomic_store(&g_ldn_state, LDN_STATE_INITIALIZED);
        return RES_OK;
    }
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", "local wireless: open access point (host mode)", Result, h_ldn_OpenAp, void) {
    int expected = LDN_STATE_INITIALIZED;
    if (atomic_compare_exchange_strong(&g_ldn_state, &expected, LDN_STATE_ACCESS_POINT)) {
        return RES_OK;
    }
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", "local wireless: close access point", Result, h_ldn_CloseAp, void) {
    int state = atomic_load(&g_ldn_state);
    if (state == LDN_STATE_ACCESS_POINT || state == LDN_STATE_ACCESS_POINT_CREATED) {
        atomic_store(&g_ldn_state, LDN_STATE_INITIALIZED);
        return RES_OK;
    }
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn10DisconnectEv", "local wireless: disconnect from network", Result, h_ldn_Disc, void) {
    int state = atomic_load(&g_ldn_state);
    if (state == LDN_STATE_STATION_CONNECTED) {
        // Build our node info to send disconnect
        LdnNodeInfo our_node;
        memset(&our_node, 0, sizeof(our_node));

        uint32_t local_ip = 0, mask = 0;
        if (ldn_get_local_ip(&local_ip, &mask)) {
            ldn_ip_to_bytes(local_ip, our_node.ipv4_address.raw);
            ldn_generate_mac(local_ip, &our_node.mac_address);
        }

        // Extract host IP from network info
        const uint8_t *host_ip_bytes = g_network_info.ldn.nodes[0].ipv4_address.raw;
        char host_ip[16];
        snprintf(host_ip, sizeof(host_ip), "%d.%d.%d.%d",
                 host_ip_bytes[0], host_ip_bytes[1], host_ip_bytes[2], host_ip_bytes[3]);

        // Send disconnect to host
        ldn_send_packet(LDN_PACKET_DISCONNECT, &our_node, sizeof(our_node), host_ip);

        // Stop listener
        ldn_stop_listener();

        atomic_store(&g_ldn_state, LDN_STATE_STATION);
        return RES_OK;
    }
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", "local wireless: destroy network", Result, h_ldn_Destroy, void) {
    int state = atomic_load(&g_ldn_state);
    if (state == LDN_STATE_ACCESS_POINT_CREATED) {
        // Notify all connected clients
        int count = atomic_load(&g_connected_client_count);
        for (int i = 0; i < count; i++) {
            ldn_send_packet(LDN_PACKET_DESTROY, NULL, 0, g_connected_clients[i].ip);
        }

        // Stop listener
        ldn_stop_listener();

        // Clear clients
        atomic_store(&g_connected_client_count, 0);

        atomic_store(&g_ldn_state, LDN_STATE_ACCESS_POINT);
        return RES_OK;
    }
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless: scan for networks via UDP broadcast", Result, h_ldn_Scan,
          void *out, int *n, int max, const void *f, int ch) {
    (void)f; (void)ch;

    if (!atomic_load(&g_ldn_initialized)) {
        if (n) *n = 0;
        return RES_FAIL;
    }

    // Clear previous scan results
    atomic_store(&g_scan_result_count, 0);

    // Send broadcast scan request
    ldn_send_broadcast(LDN_PACKET_SCAN, NULL, 0);

    // Wait 1 second for responses
    for (int i = 0; i < 100; i++) {
        usleep(10000); // 10ms x 100 = 1 second

        // Process incoming packets
        uint8_t type;
        LdnFullNetworkInfo info;
        size_t len;

        while (ldn_receive_packet(&type, &info, &len, sizeof(info))) {
            if (type == LDN_PACKET_SCAN_RESP && len == sizeof(LdnFullNetworkInfo)) {
                int count = atomic_load(&g_scan_result_count);
                if (count < 32) {
                    memcpy(&g_scan_results[count], &info, sizeof(info));
                    atomic_fetch_add(&g_scan_result_count, 1);
                }
            }
        }
    }

    // Copy results to output
    int count = atomic_load(&g_scan_result_count);
    int to_copy = (count < max) ? count : max;

    if (out && to_copy > 0) {
        memcpy(out, g_scan_results, to_copy * sizeof(LdnFullNetworkInfo));
    }

    if (n) *n = to_copy;

    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless: scan private", Result, h_ldn_ScanP,
          void *out, int *n, int max, const void *f, int ch) {
    return h_ldn_Scan(out, n, max, f, ch);
}

HLE_FORCE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "local wireless: create network and start background listener", Result, h_ldn_Create,
          const void *net_cfg, const void *sec_cfg, const void *user_cfg) {
    (void)sec_cfg;

    int state = atomic_load(&g_ldn_state);
    if (state != LDN_STATE_ACCESS_POINT) return RES_FAIL;

    const LdnNetworkConfig *cfg = (const LdnNetworkConfig *)net_cfg;
    const LdnUserConfig *user = (const LdnUserConfig *)user_cfg;

    // Build network info
    memset(&g_network_info, 0, sizeof(g_network_info));

    // Generate session ID (random)
    g_network_info.network_id.session_id.high = (uint64_t)now_ns();
    g_network_info.network_id.session_id.low = (uint64_t)now_ns() ^ 0x123456789ABCDEF0ULL;

    if (cfg) {
        g_network_info.network_id.intent_id = cfg->intent_id;
        g_network_info.common.channel = cfg->channel ? cfg->channel : 6;
        g_network_info.ldn.node_count_max = cfg->node_count_max;
    } else {
        g_network_info.common.channel = 6;
        g_network_info.ldn.node_count_max = 8;
    }

    // Set common info
    g_network_info.common.link_level = 2; // Good
    g_network_info.common.network_type = 2; // Ldn

    uint32_t local_ip, mask;
    if (ldn_get_local_ip(&local_ip, &mask)) {
        ldn_ip_to_bytes(local_ip, g_network_info.ldn.nodes[0].ipv4_address.raw);
        ldn_generate_mac(local_ip, &g_network_info.common.bssid);
        ldn_generate_mac(local_ip, &g_network_info.ldn.nodes[0].mac_address);
    }

    // Set SSID
    const char *ssid_str = "nxnative-ldn";
    g_network_info.common.ssid.length = (uint8_t)strlen(ssid_str);
    strncpy(g_network_info.common.ssid.raw, ssid_str, LDN_SSID_MAX);

    // Set node 0 (host)
    g_network_info.ldn.nodes[0].node_id = 0;
    g_network_info.ldn.nodes[0].is_connected = 1;
    g_network_info.ldn.node_count = 1;

    if (user) {
        memcpy(g_network_info.ldn.nodes[0].user_name, user->user_name, LDN_USERNAME_MAX);
    } else {
        strcpy((char *)g_network_info.ldn.nodes[0].user_name, "Host");
    }

    // Clear connected clients
    atomic_store(&g_connected_client_count, 0);

    // Start background listener thread for auto-responding
    if (!ldn_start_listener()) {
        // Failed to start listener, but continue (will just not auto-respond)
    }

    atomic_store(&g_ldn_state, LDN_STATE_ACCESS_POINT_CREATED);
    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "local wireless: create network private",
          Result, h_ldn_CreateP, const void *a, const void *b, const void *c, const void *d, int n, const void *e) {
    (void)c; (void)d; (void)n; (void)e;
    return h_ldn_Create(a, b, d);
}

HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless: connect (Phase 1: state transition only)", Result, h_ldn_Connect,
          const void *a, const void *b, const void *c, int v, int o) {
    (void)a; (void)b; (void)c; (void)v; (void)o;
    int state = atomic_load(&g_ldn_state);
    if (state == LDN_STATE_STATION) {
        atomic_store(&g_ldn_state, LDN_STATE_STATION_CONNECTED);
        return RES_OK;
    }
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless: connect private",
          Result, h_ldn_ConnectP, const void *a, const void *b, const void *c, const void *d, int v, int o) {
    (void)c;
    return h_ldn_Connect(a, b, d, v, o);
}

HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "local wireless: get current network info", Result, h_ldn_NetInfo, void *o) {
    int state = atomic_load(&g_ldn_state);
    if (state != LDN_STATE_ACCESS_POINT_CREATED && state != LDN_STATE_STATION_CONNECTED) {
        return RES_FAIL;
    }

    if (o) {
        memcpy(o, &g_network_info, sizeof(LdnFullNetworkInfo));
    }

    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "local wireless: get local WiFi IPv4 address", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) {
    int state = atomic_load(&g_ldn_state);
    if (state != LDN_STATE_ACCESS_POINT_CREATED && state != LDN_STATE_STATION_CONNECTED) {
        if (a) *a = 0;
        if (m) *m = 0;
        return RES_FAIL;
    }

    uint32_t ip, mask;
    if (ldn_get_local_ip(&ip, &mask)) {
        if (a) *a = ip;
        if (m) *m = mask;
        return RES_OK;
    }

    if (a) *a = 0;
    if (m) *m = 0;
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "local wireless: get security parameter", Result, h_ldn_SecPrm, void *o) {
    (void)o;
    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "local wireless: set advertise data", Result, h_ldn_Adv, const void *d, uint64_t n) {
    (void)d; (void)n;
    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "local wireless: disconnect reason", int, h_ldn_DiscReason, void) {
    return 0;
}

HLE_FORCE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", "local wireless: add accept filter", Result, h_ldn_Filter, uint64_t mac) {
    (void)mac;
    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "local wireless: set accept policy", Result, h_ldn_Policy, int p) {
    (void)p;
    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "local wireless: reject station", Result, h_ldn_Reject, uint32_t a) {
    (void)a;
    return RES_OK;
}

HLE_FORCE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", "local wireless: get network config", Result, h_ldn_NetCfg, void *o) {
    (void)o;
    return RES_OK;
}

// ---------- socket: offline. Initialization succeeds (the network library's global init depends on it), every later operation fails with errno = ENETDOWN ----------
enum { SK_ENETDOWN = 50, SK_EAI_FAIL = 4 };
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) { (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK; }
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "pairs with Initialize (cut)", Result, h_sk_Fini, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "offline: ENETDOWN", int, h_sk_Errno, void) { return SK_ENETDOWN; }
HLE_FORCE("_ZN2nn6socket6SocketEiii", "offline (cut): -1", int, h_sk_Socket3i, int a, int b, int c) { (void)a; (void)b; (void)c; return -1; }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", "offline (cut)", int, h_sk_Shutdown, int s, int h) { (void)s; (void)h; return -1; }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", "offline (cut)", int, h_sk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return -1; }
int dm_sk_poll(void *fds, uint64_t n, int ms);   // hle_net.c: nothing to wait on → the timeout passes, 0; else -1
int dm_sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv);
HLE_FORCE("_ZN2nn6socket4PollEP6pollfdmi", "offline: waits out the timeout when there is nothing to wait on, else fails", int, h_sk_Poll, void *p, uint64_t n, int t) { return dm_sk_poll(p, n, t); }
// SDK 4's Select with plain fd_set / timeval (Dark Souls Remastered)
HLE_FORCE("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", "offline: waits out the timeout when there is nothing to wait on, else fails", int, h_sk_Select2, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "offline (cut)", long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "offline (cut)", long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "offline (cut)", long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_Gai2, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAi2, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "resolver error text", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
// SDK 11's overloads with plain sockaddr / int arguments (Big Brain Academy: Brain vs. Brain, through Pia)
HLE_FORCE("_ZN2nn6socket7ConnectEiPK8sockaddrj", "offline (cut)", int, h_sk_Connect2, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket6AcceptEiP8sockaddrPj", "offline (cut)", int, h_sk_Accept2, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", "offline (cut)", int, h_sk_GetSockName2, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket10GetSockOptEiiiPvPj", "offline (cut)", int, h_sk_GetSockOpt2, int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket10SetSockOptEiiiPKvj", "offline (cut)", int, h_sk_SetSockOpt2, int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket5FcntlEiiz", "offline (cut)", int, h_sk_Fcntl2, int s, int c) { (void)s; (void)c; return -1; }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmi", "offline (cut)", long, h_sk_Recv2, int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return -1; }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmi", "offline (cut)", long, h_sk_Send2, int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return -1; }

// ---------- irsensor: the Joy-Con IR camera (Big Brain Academy: Brain vs. Brain's input layer makes one per controller). The host has none: every camera is unconnected,
// processors cannot run and report no states. The default-config getters and MakeRect only fill the game's structs and keep running in the SDK ----------
enum { IRS_UNCONNECTED = 2 };   // IrCameraStatus: 0 available, 1 unsupported, 2 unconnected (the game takes 0 as "available", main+0x497fd4)
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraHandleERKj", "IR camera handle of a controller (player number in the low byte)", uint32_t, h_irs_Handle, const uint32_t *npad) { return *npad & 0xff; }
HLE_FORCE("_ZN2nn8irsensor10InitializeERKNS0_14IrCameraHandleE", "IR camera (cut): nothing to initialize", void, h_irs_Init, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor8FinalizeERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Fini, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraStatusERKNS0_14IrCameraHandleE", "IR camera (cut): unconnected", int, h_irs_Status, const void *h) { (void)h; return IRS_UNCONNECTED; }
HLE_FORCE("_ZN2nn8irsensor18StopImageProcessorERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Stop, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor18RunMomentProcessorERKNS0_14IrCameraHandleERKNS0_21MomentProcessorConfigE", "IR camera (cut)", Result, h_irs_RunMoment, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor22RunClusteringProcessorERKNS0_14IrCameraHandleERKNS0_25ClusteringProcessorConfigE", "IR camera (cut)", Result, h_irs_RunCluster, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor15RunHandAnalysisERKNS0_14IrCameraHandleERKNS0_18HandAnalysisConfigE", "IR camera (cut)", Result, h_irs_RunHand, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_28ImageTransferProcessorConfigEPvm", "IR camera (cut)", Result, h_irs_RunImage,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_30ImageTransferProcessorExConfigEPvm", "IR camera (cut)", Result, h_irs_RunImageEx,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor24GetMomentProcessorStatesEPNS0_20MomentProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Moment,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor28GetClusteringProcessorStatesEPNS0_24ClusteringProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Cluster,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25GetHandAnalysisImageStateEPNS0_22HandAnalysisImageStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandImage,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetHandAnalysisSilhouetteStateEPNS0_27HandAnalysisSilhouetteStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandSil,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetImageTransferProcessorStateEPNS0_27ImageTransferProcessorStateEPvmRKNS0_14IrCameraHandleE", "IR camera (cut)", Result, h_irs_ImageState,
          void *s, void *b, size_t n, const void *h) { (void)s; (void)b; (void)n; (void)h; return RES_FAIL; }

// ---------- ssl: offline, creating a context fails ----------
HLE_FORCE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "SSL (cut)", Result, h_ssl_ImpCli, void *self, uint64_t *id, const char *a, const char *b, unsigned x, unsigned y) { (void)self; (void)a; (void)b; (void)x; (void)y; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "SSL (cut)", Result, h_ssl_ImpSrv, void *self, uint64_t *id, const char *a, unsigned n, int f) { (void)self; (void)a; (void)n; (void)f; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", "SSL (cut)", Result, h_ssl_ImpCrl, void *self, uint64_t *id, const char *a, unsigned n) { (void)self; (void)a; (void)n; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", "SSL (cut)", Result, h_ssl_Certs,
          void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }

// ---------- nifm / nsd: offline ----------
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "offline: the local network mode switch is ignored", void, h_nifm_LocalMode, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "offline: the network request is not submitted (returns at once, IsNetworkAvailable is always false)", void, h_nifm_SubmitWait, void) {}
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "offline: no IP configuration", Result, h_nifm_IpCfg, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
// SDK 10's functions of the same name take socket::InAddr / in_addr, exactly the other way round from the generation above and in hle_net.c (Super Mario 3D World + Bowser's Fury)
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", "offline: no IP configuration (SDK 10 overload)", Result, h_nifm_IpCfg10, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", "offline: no IP (SDK 10 overload)", Result, h_nifm_GetIp10, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
// EnvironmentIdentifier = char[8]: retail environment "lp1"
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "server environment name: retail lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }

// ---------- friends: newer SDK overloads ----------
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "friends library allocator; no session is created, ignored", void, h_fr_SetAlloc, void *a, void *f) { (void)a; (void)f; }
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "friend list is empty (cut)", Result, h_fr_List2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "friend list unavailable (cut)", Result, h_fr_ListAvail, uint8_t *out, const void *uid) { (void)uid; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "friend list (cut): the request fails at once", Result, h_fr_ListEnsure, void *ctx, const void *uid) { (void)ctx; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12AsyncContext7HasDoneEPb", "async context: always done", Result, h_fr_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "play history is not registered (cut)",
          Result, h_fr_PlayHist2, const void *uid, const void *key, const void *a, const void *b) { (void)uid; (void)key; (void)a; (void)b; return RES_FAIL; }

// ---------- ngc: newer SDK overloads (with bool). Always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", "profanity filter init; the host has no word list", Result, h_ngc_Init3, void *self, void *work, uint64_t sz, unsigned check) { (void)self; (void)work; (void)sz; (void)check; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", "profanity check: no profanity in anything", Result, h_ngc_Check16, void *self, uint32_t *out, unsigned mask, const void **words, uint64_t n) {
    (void)self; (void)mask; (void)words; for (uint64_t i = 0; out && i < n; i++) out[i] = 0; return RES_OK;
}

// ---------- err: error display applet. Only logged, no UI is shown ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "error display applet; the host only logs it", void, h_err_ShowCode, uint64_t code) { DM_APPLET("error"); LOG("game asks to show an error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "error display applet; the host only logs it", void, h_err_ShowVar, const void *v) { DM_APPLET("error"); (void)v; LOG("game asks to show an error (ErrorResultVariant)"); }
HLE_FORCE("_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", "add-on content version mismatch error applet; the host only logs it", void, h_err_AocVer, void) { DM_APPLET("error"); LOG("game asks to show an error: add-on content version mismatch"); }
HLE_FORCE("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv", "game version mismatch error applet; the host only logs it", void, h_err_AppVer, void) { DM_APPLET("error"); LOG("game asks to show an error: game version mismatch"); }
// Pokémon Legends: Z-A (SDK 19): the application's own error screen (an error code number and two messages in the argument) goes through the error applet as well
HLE_FORCE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", "application error applet; the host only logs it", void, h_err_ShowApp, const void *arg) { DM_APPLET("error"); (void)arg; LOG("game asks to show an application error (ApplicationErrorArg)"); }
// Pokémon Sword (SDK 7.3.2): the error applet's network service form, after a failed online call
HLE_FORCE("_ZN2nn3err23ShowNetworkServiceErrorENS0_9ErrorCodeE", "network service error applet; the host only logs it", void, h_err_ShowNet, uint64_t code) { DM_APPLET("error"); LOG("game asks to show a network service error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }

// ---------- pdm: the system's play log ----------
// Pokémon Sword asks at boot how often this user has launched two other games (main+0xc88074, one ID each call, 0x18 bytes {application ID, play time, launch count} per entry; the result is
// not read, a launch count of at least 1 is): the host keeps no play log, so every one was never played. The SDK's own call opens the pdm service (null read at sdk+0x336ccc)
HLE_FORCE("_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsERKNS_7account3UidEPKNS_13ApplicationIdEi", "play statistics of other games: never played (the host keeps no play log)",
          Result, h_pdm_QueryStats, uint64_t *out, const void *uid, const uint64_t *ids, int n) {
    (void)uid; for (int i = 0; out && ids && i < n; i++) { out[3 * i] = ids[i]; out[3 * i + 1] = 0; out[3 * i + 2] = 0; }
    return RES_OK;
}

// ---------- web: web page applet (online lobby). Unavailable ----------
HLE_FORCE("_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", "online lobby web applet (cut)", Result, h_web_Lobby, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowLobbyPage (cut)"); return RES_FAIL; }
// Tears of the Kingdom imports the general web page applet too (the game constructs the return value itself, so it stays as constructed)
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "web page applet (cut)", Result, h_web_Page, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowWebPage (cut)"); return RES_FAIL; }
// Octopath Traveler 0 imports the offline HTML page applet too (pages shipped in the game's HTML document NCA, which the host does not extract)
HLE_FORCE("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", "offline HTML page applet (cut)", Result, h_web_Offline, void *rv, const void *arg) {
    (void)rv; (void)arg; LOG("web::ShowOfflineHtmlPage (cut)"); return RES_FAIL;
}

// ---------- C interface (nnsocket* / nnssl*): offline as well. The arguments are all in registers and the host does not look at them ----------
HLE_FORCE("nnsocketAccept", "offline (cut)", int, h_c_sk_Accept, void) { return -1; }
HLE_FORCE("nnsocketListen", "offline (cut)", int, h_c_sk_Listen, void) { return -1; }
HLE_FORCE("nnsocketRecvMsg", "offline (cut)", int, h_c_sk_RecvMsg, void) { return -1; }
HLE_FORCE("nnsocketSendMsg", "offline (cut)", int, h_c_sk_SendMsg, void) { return -1; }
HLE_FORCE("nnsocketShutdown", "offline (cut)", int, h_c_sk_Shutdown, void) { return -1; }
HLE_FORCE("nnsocketGetAddrInfo", "name resolution (cut): EAI_FAIL", int, h_c_sk_Gai, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketFreeAddrInfo", "pairs with GetAddrInfo (cut)", void, h_c_sk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("nnsocketGAIStrError", "resolver error text", const char *, h_c_sk_GaiStr, int e) { (void)e; return "network unavailable"; }

// ---------- bcat: news / data delivered from the server (Super Mario Odyssey). Initialize talks to the bcat service (an SDK abort without it); mounting the delivery cache fails,
// which the game treats as "nothing delivered" (main+0x6c9258: a failure skips the read unless the caller asked for an error dialog, which only logs here). Syncing needs the network, which nifm reports unavailable ----------
HLE_FORCE("_ZN2nn4bcat10InitializeEv", "data delivery init (connects to the bcat service); the host has none, no-op", void, h_bcat_Init, void) {}
HLE_FORCE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", "mount delivery cache: no delivery data, fails", Result, h_bcat_Mount, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", "unmount delivery cache", void, h_bcat_Unmount, void) {}

// ---------- album: the capture album (Monster Hunter Rise saves its own photo-mode pictures there). Initialize connects to the capture service (capsrv, an abort in sm::Initialize at the start of
// nnMain, main+0x6bc5478); the host has no album, so saving fails and showing it does nothing. GetImageWidth / GetImageHeight only compute and keep running in the SDK ----------
HLE_FORCE("_ZN2nn5album10InitializeEv", "capture album init (capsrv); the host has no album, no-op", void, h_album_Init, void) {}
HLE_FORCE("_ZN2nn5album8FinalizeEv", "capture album", void, h_album_Fini, void) {}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save,
          const void *p, size_t n, int size, int orient, int report) { (void)p; (void)n; (void)size; (void)orient; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
// The SDK 7 overload, without an orientation (DRAGON QUEST XI S)
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save7,
          const void *p, size_t n, int size, int report) { (void)p; (void)n; (void)size; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_Share,
          const void *p, size_t n, int size, int orient, int report, const void *uid, const char *text) { (void)p; (void)n; (void)size; (void)orient; (void)report; (void)uid; (void)text; return RES_FAIL; }
// The SDK 20 overload that hands back a share ID, and the URL of the last post (Animal Crossing: New Horizons 3.0.3)
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_ShareId,
          void *id, const void *p, size_t n, int size, int report, const void *uid, const char *text) { (void)id; (void)p; (void)n; (void)size; (void)report; (void)uid; (void)text; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE", "URL of the last shared post: nothing was shared, fails", Result, h_album_ShareUrl, char *url, size_t n, const void *id) {
    (void)id; if (url && n) url[0] = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album14ShowAlbumFilesEv", "album applet: not shown", Result, h_album_Show, void) { return RES_FAIL; }
// Pokémon Legends: Z-A (SDK 19): a screenshot of the current frame, taken by the capture service
HLE_FORCE("_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE", "screenshot of the current frame (capsrv): no album, fails", Result, h_album_SaveCur, int report) { (void)report; LOG("album::SaveCurrentScreenshot (no album)"); return RES_FAIL; }

// ---------- Phase 3: Connect/Disconnect handling ----------

HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_10UserConfigEi", "local wireless: connect to network (simple)", Result, h_ldn_ConnectSimple,
          const void *net_info, const void *user_cfg, int local_ver) {
    (void)local_ver;
    
    int state = atomic_load(&g_ldn_state);
    if (state != LDN_STATE_STATION) return RES_FAIL;
    
    const LdnFullNetworkInfo *network = (const LdnFullNetworkInfo *)net_info;
    const LdnUserConfig *user = (const LdnUserConfig *)user_cfg;
    
    // Build our node info to send to host
    LdnNodeInfo our_node;
    memset(&our_node, 0, sizeof(our_node));

    uint32_t local_ip = 0, mask = 0;
    if (ldn_get_local_ip(&local_ip, &mask)) {
        ldn_ip_to_bytes(local_ip, our_node.ipv4_address.raw);
        ldn_generate_mac(local_ip, &our_node.mac_address);
    }
    
    if (user) {
        memcpy(our_node.user_name, user->user_name, LDN_USERNAME_MAX);
    } else {
        strcpy((char *)our_node.user_name, "Client");
    }
    
    our_node.node_id = -1; // Host will assign
    our_node.is_connected = 0;
    our_node.local_communication_version = (int16_t)local_ver;
    
    // Extract host IP from network info
    const uint8_t *host_ip_bytes = network->ldn.nodes[0].ipv4_address.raw;
    char host_ip[16];
    snprintf(host_ip, sizeof(host_ip), "%d.%d.%d.%d", 
             host_ip_bytes[0], host_ip_bytes[1], host_ip_bytes[2], host_ip_bytes[3]);
    
    // Send connect request to host
    ldn_send_packet(LDN_PACKET_CONNECT, &our_node, sizeof(our_node), host_ip);
    
    // Start listener to receive SYNC packets
    ldn_start_listener();
    
    // Wait for sync (background thread will update state)
    for (int i = 0; i < 100; i++) {
        if (atomic_load(&g_ldn_state) == LDN_STATE_STATION_CONNECTED) {
            return RES_OK;
        }
        usleep(10000); // 10ms
    }
    
    // Timeout
    return RES_FAIL;
}
