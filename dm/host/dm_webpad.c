// dm_webpad.c: Clean-room embedded HTTP + WebSocket server for Wi-Fi browser controllers with PIN pairing.
// MIT License.
#include "dm_webpad.h"
#include "dm_input.h"
#include "hle.h"
#include "webpad_html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <time.h>
#include <dns_sd.h>
#include <CommonCrypto/CommonDigest.h>

#define MAX_WEB_CLIENTS 16
#define MAX_IP_LOCKOUTS 64
#define MAX_PIN_FAILURES 5
#define MAX_GLOBAL_FAILURES 20
#define WEBPAD_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WEBPAD_MAX_HEADER_SIZE 2048
#define WEBPAD_MAX_PAYLOAD_SIZE 256

typedef struct {
    struct in_addr ip;
    int fail_count;
    bool locked;
} ip_lockout_t;

typedef struct {
    int fd;
    struct in_addr ip;
    uint16_t port;
    int player_slot; // 0..DM_PLAYERS-1 or -1 if unassigned
    uint64_t last_seen_ms;
    uint64_t last_ack_ms;
    bool authenticated;
    bool active;
    pthread_t thread;
} webpad_client_t;

static int g_webpad_enabled = -1;
static int g_server_fd = -1;
static pthread_t g_server_thread;
static pthread_mutex_t g_webpad_lock = PTHREAD_MUTEX_INITIALIZER;
static webpad_client_t g_clients[MAX_WEB_CLIENTS];
static ip_lockout_t g_lockouts[MAX_IP_LOCKOUTS];
static int g_lockout_count = 0;
static int g_global_failures = 0;
static bool g_pairing_stopped = false;
static DNSServiceRef g_dns_service = NULL;
static char g_pairing_pin[8] = "";
static dm_webpad_pin_prompt_cb g_pin_prompt_cb = NULL;

static bool is_ip_locked(struct in_addr ip) {
    pthread_mutex_lock(&g_webpad_lock);
    if (g_pairing_stopped) {
        pthread_mutex_unlock(&g_webpad_lock);
        return true;
    }
    for (int i = 0; i < g_lockout_count; i++) {
        if (g_lockouts[i].ip.s_addr == ip.s_addr && g_lockouts[i].locked) {
            pthread_mutex_unlock(&g_webpad_lock);
            return true;
        }
    }
    pthread_mutex_unlock(&g_webpad_lock);
    return false;
}

static bool record_failed_attempt(struct in_addr ip, const char *ip_str) {
    pthread_mutex_lock(&g_webpad_lock);
    g_global_failures++;
    if (g_global_failures >= MAX_GLOBAL_FAILURES) {
        g_pairing_stopped = true;
        pthread_mutex_unlock(&g_webpad_lock);
        LOG("[webpad] global PIN failure limit reached (%d), pairing stopped", MAX_GLOBAL_FAILURES);
        return true;
    }

    int idx = -1;
    for (int i = 0; i < g_lockout_count; i++) {
        if (g_lockouts[i].ip.s_addr == ip.s_addr) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (g_lockout_count < MAX_IP_LOCKOUTS) {
            idx = g_lockout_count++;
            g_lockouts[idx].ip = ip;
            g_lockouts[idx].fail_count = 0;
            g_lockouts[idx].locked = false;
        } else {
            // Table full: evict first unlocked entry
            for (int i = 0; i < MAX_IP_LOCKOUTS; i++) {
                if (!g_lockouts[i].locked) {
                    idx = i;
                    g_lockouts[idx].ip = ip;
                    g_lockouts[idx].fail_count = 0;
                    g_lockouts[idx].locked = false;
                    break;
                }
            }
            if (idx < 0) idx = 0; // fallback
        }
    }

    g_lockouts[idx].fail_count++;
    if (g_lockouts[idx].fail_count >= MAX_PIN_FAILURES) {
        g_lockouts[idx].locked = true;
        pthread_mutex_unlock(&g_webpad_lock);
        LOG("[webpad] client %s exceeded maximum PIN attempts, locked out", ip_str);
        return true;
    }
    pthread_mutex_unlock(&g_webpad_lock);
    return false;
}

void dm_webpad_set_pin_prompt_cb(dm_webpad_pin_prompt_cb cb) {
    g_pin_prompt_cb = cb;
}

void dm_webpad_trigger_pin_prompt(const char *client_ip) {
    if (g_pin_prompt_cb) {
        g_pin_prompt_cb(dm_webpad_pin(), client_ip);
    }
}

void dm_webpad_check_pending_auth(void) {
    pthread_mutex_lock(&g_webpad_lock);
    bool any_pending = false;
    for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
        if (g_clients[i].active && !g_clients[i].authenticated) {
            any_pending = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_webpad_lock);

    if (!any_pending && g_pin_prompt_cb) {
        g_pin_prompt_cb(NULL, NULL);
    }
}

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int dm_webpad_enabled(void) {
    if (g_webpad_enabled >= 0) return g_webpad_enabled;
    const char *e1 = getenv("DM_BROWSER_PADS");
    const char *e2 = getenv("NX_BROWSER_PADS");
    if ((e1 && atoi(e1)) || (e2 && atoi(e2))) {
        g_webpad_enabled = 1;
    } else {
        g_webpad_enabled = 0;
    }
    return g_webpad_enabled;
}

void dm_webpad_enable(int on) {
    g_webpad_enabled = on ? 1 : 0;
}

const char *dm_webpad_pin(void) {
    if (!g_pairing_pin[0]) {
        dm_webpad_set_pin(NULL);
    }
    return g_pairing_pin;
}

void dm_webpad_set_pin(const char *pin) {
    pthread_mutex_lock(&g_webpad_lock);
    if (pin && strlen(pin) == 4) {
        snprintf(g_pairing_pin, sizeof(g_pairing_pin), "%s", pin);
    } else {
        const char *env_pin = getenv("DM_WEBPAD_PIN");
        if (env_pin && strlen(env_pin) == 4) {
            snprintf(g_pairing_pin, sizeof(g_pairing_pin), "%s", env_pin);
        } else {
            // Generate random 4-digit PIN
            uint32_t val = (uint32_t)(arc4random_uniform(9000) + 1000);
            snprintf(g_pairing_pin, sizeof(g_pairing_pin), "%04u", val);
        }
    }
    pthread_mutex_unlock(&g_webpad_lock);
}

uint16_t dm_webpad_port(void) {
    return DM_WEBPAD_DEFAULT_PORT;
}

int dm_webpad_get_ip(char *buf, size_t cap) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return -1;
    int found = -1;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, (socklen_t)cap)) {
            found = 0;
            // Prefer standard Wi-Fi adapter (en0)
            if (strcmp(ifa->ifa_name, "en0") == 0) break;
        }
    }
    freeifaddrs(ifaddr);
    return found;
}

static void base64_encode(const unsigned char *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t oct_a = in[i++];
        uint32_t oct_b = rem > 1 ? in[i++] : 0;
        uint32_t oct_c = rem > 2 ? in[i++] : 0;
        uint32_t triple = (oct_a << 16) | (oct_b << 8) | oct_c;
        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = rem > 1 ? tbl[(triple >> 6) & 0x3F] : '=';
        out[j++] = rem > 2 ? tbl[triple & 0x3F] : '=';
    }
    out[j] = '\0';
}

static void ws_accept_key(const char *key, char *out) {
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", key, WEBPAD_GUID);
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1((const unsigned char *)combined, (CC_LONG)strlen(combined), digest);
    base64_encode(digest, sizeof(digest), out);
}

static int ws_send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t len) {
    if (len > WEBPAD_MAX_PAYLOAD_SIZE) return -1;
    uint8_t header[10];
    size_t hlen = 0;
    header[hlen++] = 0x80 | (opcode & 0x0F);
    if (len < 126) {
        header[hlen++] = (uint8_t)len;
    } else {
        header[hlen++] = 126;
        header[hlen++] = (uint8_t)((len >> 8) & 0xFF);
        header[hlen++] = (uint8_t)(len & 0xFF);
    }
    if (write(fd, header, hlen) != (ssize_t)hlen) return -1;
    if (len > 0 && write(fd, payload, len) != (ssize_t)len) return -1;
    return 0;
}

static int assign_player_slot(struct in_addr ip) {
    for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].ip.s_addr == ip.s_addr && g_clients[i].player_slot >= 0) {
            return g_clients[i].player_slot;
        }
    }
    bool taken[DM_PLAYERS] = { false };
    for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].player_slot >= 0 && g_clients[i].player_slot < DM_PLAYERS) {
            taken[g_clients[i].player_slot] = true;
        }
    }
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (!taken[p]) return p;
    }
    return -1;
}

static void update_pad_players_mask(void) {
    uint32_t mask = 0;
    for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].player_slot >= 0 && g_clients[i].player_slot < DM_PLAYERS) {
            mask |= (1u << g_clients[i].player_slot);
        }
    }
    dm_pad_players(mask);
}

static void parse_and_apply_pad(int slot, const char *payload) {
    if (slot < 0 || slot >= DM_PLAYERS) return;
    unsigned long raw_btn = 0;
    float lx = 0.0f, ly = 0.0f;
    if (sscanf(payload, "%lu,%f,%f", &raw_btn, &lx, &ly) < 3) return;

    if (lx < -1.0f) lx = -1.0f; else if (lx > 1.0f) lx = 1.0f;
    if (ly < -1.0f) ly = -1.0f; else if (ly > 1.0f) ly = 1.0f;

    uint64_t buttons = (uint64_t)raw_btn & 0xFFFF;
    int32_t stick_lx = (int32_t)(lx * DM_STICK_MAX);
    int32_t stick_ly = (int32_t)(ly * DM_STICK_MAX);

    dm_pad_set_player(slot, buttons, stick_lx, stick_ly, 0, 0);
}

static void *webpad_client_worker(void *arg) {
    webpad_client_t *c = (webpad_client_t *)arg;
    int fd = c->fd;
    bool is_ws = false;

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &c->ip, ip_str, sizeof(ip_str));

    if (is_ip_locked(c->ip)) {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\nLocked out";
        (void)write(fd, forbidden, strlen(forbidden));
        goto cleanup;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    uint8_t buf[WEBPAD_MAX_HEADER_SIZE];
    size_t buf_len = 0;
    char client_pin[16] = "";

    // Read initial HTTP request (bounded to WEBPAD_MAX_HEADER_SIZE)
    while (buf_len < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
        if (n > 0) {
            buf_len += (size_t)n;
            buf[buf_len] = '\0';
            if (strstr((char *)buf, "\r\n\r\n")) break;
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            goto cleanup;
        }
        usleep(5000);
    }

    char *req = (char *)buf;
    if (strncmp(req, "GET /", 5) != 0) goto cleanup;

    char *path_end = strchr(req + 4, ' ');
    if (!path_end) goto cleanup;
    *path_end = '\0';
    char *path = req + 4;
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
        char *pin_param = strstr(query + 1, "pin=");
        if (pin_param) {
            snprintf(client_pin, sizeof(client_pin), "%.4s", pin_param + 4);
        }
    }

    if (strcmp(path, "/ws") == 0) {
        // WebSocket upgrade
        char *key_header = strcasestr(path_end + 1, "Sec-WebSocket-Key:");
        if (!key_header) goto cleanup;
        key_header += 18;
        while (*key_header == ' ') key_header++;
        char *key_end = strstr(key_header, "\r\n");
        if (!key_end) goto cleanup;
        *key_end = '\0';

        char accept_str[64];
        ws_accept_key(key_header, accept_str);

        char response[512];
        int rlen = snprintf(response, sizeof(response),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept_str);
        if (write(fd, response, (size_t)rlen) != rlen) goto cleanup;

        is_ws = true;

        // Verify PIN from query string
        if (client_pin[0]) {
            if (strlen(client_pin) == 4 && strlen(g_pairing_pin) == 4 && strcmp(client_pin, g_pairing_pin) == 0) {
                c->authenticated = true;
                pthread_mutex_lock(&g_webpad_lock);
                c->player_slot = assign_player_slot(c->ip);
                update_pad_players_mask();
                pthread_mutex_unlock(&g_webpad_lock);
                LOG("[webpad] client from %s authenticated via URL PIN (assigned player %d)", ip_str, c->player_slot + 1);
            } else {
                LOG("[webpad] client from %s PIN mismatch", ip_str);
                if (record_failed_attempt(c->ip, ip_str)) {
                    goto cleanup;
                }
                const char *auth_req = "AUTH_FAILED";
                ws_send_frame(fd, 0x1, (const uint8_t *)auth_req, strlen(auth_req));
                dm_webpad_trigger_pin_prompt(ip_str);
            }
        } else {
            // Require PIN entry
            const char *auth_req = "AUTH_REQUIRED";
            ws_send_frame(fd, 0x1, (const uint8_t *)auth_req, strlen(auth_req));
            dm_webpad_trigger_pin_prompt(ip_str);
        }
    } else {
        // Serve HTML
        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n\r\n", webpad_html_len);
        (void)write(fd, header, (size_t)hlen);
        (void)write(fd, webpad_html, webpad_html_len);
        goto cleanup;
    }

    // WebSocket frame loop
    buf_len = 0;
    c->last_seen_ms = get_time_ms();
    c->last_ack_ms = 0;

    while (c->active) {
        uint64_t now = get_time_ms();

        // 5 seconds timeout if not authenticated, 4 seconds silence if authenticated
        uint64_t timeout_limit = c->authenticated ? 4000 : 5000;
        if (now - c->last_seen_ms > timeout_limit) {
            LOG("[webpad] client %s %s timed out", ip_str, c->authenticated ? "connection" : "auth wait");
            break;
        }

        // Send periodic player ping if authenticated
        if (c->authenticated && now - c->last_ack_ms > 500) {
            c->last_ack_ms = now;
            char msg[32];
            int mlen = snprintf(msg, sizeof(msg), "player %d", c->player_slot + 1);
            if (ws_send_frame(fd, 0x1, (const uint8_t *)msg, (size_t)mlen) < 0) {
                break;
            }
        }

        ssize_t n = read(fd, buf + buf_len, sizeof(buf) - buf_len);
        if (n > 0) {
            buf_len += (size_t)n;
            while (buf_len >= 2) {
                uint8_t b0 = buf[0];
                uint8_t b1 = buf[1];
                uint8_t opcode = b0 & 0x0F;
                bool masked = (b1 & 0x80) != 0;
                size_t payload_len = b1 & 0x7F;
                size_t header_len = 2;

                if (payload_len == 127) {
                    LOG("[webpad] client %s sent 64-bit length frame, disconnecting", ip_str);
                    goto cleanup;
                } else if (payload_len == 126) {
                    if (buf_len < 4) break;
                    payload_len = ((size_t)buf[2] << 8) | buf[3];
                    header_len = 4;
                }

                // Protect against oversized WebSocket frames
                if (payload_len > WEBPAD_MAX_PAYLOAD_SIZE) {
                    LOG("[webpad] client %s sent oversized payload (%zu bytes), disconnecting", ip_str, payload_len);
                    goto cleanup;
                }

                size_t mask_offset = header_len;
                if (masked) header_len += 4;

                if (buf_len < header_len + payload_len) break; // Incomplete frame

                uint8_t *payload_ptr = buf + header_len;
                if (masked) {
                    uint8_t *mask = buf + mask_offset;
                    for (size_t k = 0; k < payload_len; k++) {
                        payload_ptr[k] ^= mask[k % 4];
                    }
                }

                if (opcode == 0x8) {
                    goto cleanup; // Close
                } else if (opcode == 0x9) {
                    ws_send_frame(fd, 0xA, payload_ptr, payload_len); // Ping -> Pong
                } else if (opcode == 0x1) {
                    payload_ptr[payload_len] = '\0';
                    char *text = (char *)payload_ptr;

                    if (strncmp(text, "PIN:", 4) == 0) {
                        char *attempt = text + 4;
                        if (strlen(attempt) == 4 && strlen(g_pairing_pin) == 4 && strcmp(attempt, g_pairing_pin) == 0) {
                            c->authenticated = true;
                            pthread_mutex_lock(&g_webpad_lock);
                            c->player_slot = assign_player_slot(c->ip);
                            update_pad_players_mask();
                            pthread_mutex_unlock(&g_webpad_lock);
                            LOG("[webpad] client from %s authenticated via PIN (player %d)", ip_str, c->player_slot + 1);
                            c->last_seen_ms = now;
                            char msg[32];
                            int mlen = snprintf(msg, sizeof(msg), "player %d", c->player_slot + 1);
                            ws_send_frame(fd, 0x1, (const uint8_t *)msg, (size_t)mlen);
                            dm_webpad_check_pending_auth();
                        } else {
                            LOG("[webpad] client from %s PIN mismatch", ip_str);
                            if (record_failed_attempt(c->ip, ip_str)) {
                                goto cleanup;
                            }
                            const char *fail = "AUTH_FAILED";
                            ws_send_frame(fd, 0x1, (const uint8_t *)fail, strlen(fail));
                            dm_webpad_trigger_pin_prompt(ip_str);
                        }
                    } else if (c->authenticated) {
                        c->last_seen_ms = now;
                        parse_and_apply_pad(c->player_slot, text);
                    }
                }

                size_t total_frame = header_len + payload_len;
                memmove(buf, buf + total_frame, buf_len - total_frame);
                buf_len -= total_frame;
            }
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }

        usleep(5000);
    }

cleanup:
    close(fd);
    pthread_mutex_lock(&g_webpad_lock);
    int slot = c->player_slot;
    bool was_auth = c->authenticated;
    c->active = false;
    c->authenticated = false;
    c->fd = -1;
    c->player_slot = -1;
    if (slot >= 0) {
        dm_pad_set_player(slot, 0, 0, 0, 0, 0);
        update_pad_players_mask();
    }
    pthread_mutex_unlock(&g_webpad_lock);

    if (!was_auth) {
        dm_webpad_check_pending_auth();
    }

    if (is_ws && was_auth) {
        LOG("[webpad] client from %s disconnected (player %d freed)", ip_str, slot + 1);
    }
    return NULL;
}

static void *webpad_server_worker(void *arg) {
    (void)arg;
    LOG("[webpad] listening for Wi-Fi browser controllers on TCP port %d", DM_WEBPAD_DEFAULT_PORT);

    // Register clean Bonjour service (_nxpad._tcp)
    DNSServiceRegister(&g_dns_service, 0, 0, "NXPad", "_nxpad._tcp", NULL, NULL, htons(DM_WEBPAD_DEFAULT_PORT), 0, NULL, NULL, NULL);

    while (g_server_fd >= 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (g_server_fd < 0) break;
            usleep(10000);
            continue;
        }

        pthread_mutex_lock(&g_webpad_lock);
        int free_idx = -1;
        for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
            if (!g_clients[i].active) {
                free_idx = i;
                break;
            }
        }

        if (free_idx >= 0) {
            g_clients[free_idx].fd = client_fd;
            g_clients[free_idx].ip = client_addr.sin_addr;
            g_clients[free_idx].port = ntohs(client_addr.sin_port);
            g_clients[free_idx].player_slot = -1;
            g_clients[free_idx].last_seen_ms = get_time_ms();
            g_clients[free_idx].last_ack_ms = 0;
            g_clients[free_idx].authenticated = false;
            g_clients[free_idx].active = true;
            pthread_create(&g_clients[free_idx].thread, NULL, webpad_client_worker, &g_clients[free_idx]);
            pthread_detach(g_clients[free_idx].thread);
        } else {
            close(client_fd);
        }
        pthread_mutex_unlock(&g_webpad_lock);
    }

    if (g_dns_service) {
        DNSServiceRefDeallocate(g_dns_service);
        g_dns_service = NULL;
    }
    return NULL;
}

void dm_webpad_start(void) {
    if (!dm_webpad_enabled()) return;
    if (g_server_fd >= 0) return;

    // Ensure pairing PIN is generated immediately upon start
    dm_webpad_pin();

    memset(g_clients, 0, sizeof(g_clients));
    for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].player_slot = -1;
    }

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        LOG("[webpad] failed to create socket: %s", strerror(errno));
        return;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(g_server_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DM_WEBPAD_DEFAULT_PORT);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG("[webpad] cannot bind port %d: %s", DM_WEBPAD_DEFAULT_PORT, strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    if (listen(g_server_fd, 8) < 0) {
        LOG("[webpad] cannot listen on socket: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    char ip_str[64] = "127.0.0.1";
    dm_webpad_get_ip(ip_str, sizeof(ip_str));
    LOG("[webpad] Wi-Fi controller active: open http://%s:%d", ip_str, DM_WEBPAD_DEFAULT_PORT);

    pthread_create(&g_server_thread, NULL, webpad_server_worker, NULL);
}

void dm_webpad_stop(void) {
    if (g_server_fd < 0) return;
    int s = g_server_fd;
    g_server_fd = -1;
    close(s);

    pthread_mutex_lock(&g_webpad_lock);
    for (int i = 0; i < MAX_WEB_CLIENTS; i++) {
        if (g_clients[i].active) {
            g_clients[i].active = false;
            if (g_clients[i].fd >= 0) {
                close(g_clients[i].fd);
                g_clients[i].fd = -1;
            }
        }
    }
    pthread_mutex_unlock(&g_webpad_lock);

    pthread_join(g_server_thread, NULL);
}
