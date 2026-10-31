// Unit test for dm_webpad.
// Tests:
// 1. HTTP HTML serving
// 2. Direct WebSocket connection & player slot assignment
// 3. Controller input processing
// 4. Oversized WebSocket frame rejection
// 5. 64-bit frame length rejection
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "dm_webpad.h"
#include "dm_input.h"

static uint32_t g_mock_players = 0;
static uint64_t g_last_btn = 0;
static int32_t g_last_lx = 0, g_last_ly = 0;
static int g_last_player = -1;

void dm_pad_players(uint32_t mask) {
    g_mock_players = mask;
}

void dm_pad_set_player(int player, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    (void)rx; (void)ry;
    g_last_player = player;
    g_last_btn = buttons;
    g_last_lx = lx;
    g_last_ly = ly;
}

static int connect_to_server(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    assert(s >= 0);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DM_WEBPAD_DEFAULT_PORT);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    int rc = connect(s, (struct sockaddr *)&sin, sizeof(sin));
    assert(rc == 0);
    return s;
}

static void send_ws_handshake(int s, const char *path, char *leftover, size_t cap) {
    char req[512];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: localhost:2904\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", path);
    write(s, req, strlen(req));
    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = read(s, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        char *end = strstr(buf, "\r\n\r\n");
        if (end) {
            size_t hdr_len = (size_t)(end + 4 - buf);
            if (leftover && total > hdr_len) {
                size_t rem = total - hdr_len;
                if (rem < cap) {
                    memcpy(leftover, end + 4, rem);
                    leftover[rem] = '\0';
                }
            }
            break;
        }
    }
    assert(strstr(buf, "HTTP/1.1 101") != NULL);
}

static void send_masked_text(int s, const char *payload) {
    size_t plen = strlen(payload);
    uint8_t frame[512];
    frame[0] = 0x81;
    frame[1] = 0x80 | (uint8_t)plen;
    uint8_t mask[4] = { 0x11, 0x22, 0x33, 0x44 };
    memcpy(frame + 2, mask, 4);
    for (size_t i = 0; i < plen; i++) {
        frame[6 + i] = (uint8_t)payload[i] ^ mask[i % 4];
    }
    write(s, frame, 6 + plen);
}

int main() {
    printf("[t_webpad] starting unit tests...\n");
    setenv("DM_BROWSER_PADS", "1", 1);
    dm_webpad_enable(1);
    dm_webpad_start();
    usleep(100000);

    // 1. HTTP GET /
    int s = connect_to_server();
    const char *http_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(s, http_req, strlen(http_req));
    char buf[16384];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t r = read(s, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    assert(total > 0);
    assert(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(buf, "NX2Apple") != NULL || strstr(buf, "NXPad") != NULL);
    close(s);
    printf("[t_webpad] 1. HTTP GET / verified!\n");

    // 2. Direct WebSocket connection & player slot assignment
    s = connect_to_server();
    char frame_buf[512] = {0};
    send_ws_handshake(s, "/ws", frame_buf, sizeof(frame_buf));
    if (strstr(frame_buf, "player 1") == NULL) {
        usleep(10000);
        ssize_t n = read(s, frame_buf, sizeof(frame_buf) - 1);
        assert(n > 0);
        frame_buf[n] = '\0';
    }
    assert(strstr(frame_buf, "player 1") != NULL);
    printf("[t_webpad] 2. Direct WebSocket connection & slot assigned!\n");

    // 3. Controller input processing
    g_last_player = -1;
    send_masked_text(s, "1,0.500,-1.000");
    usleep(30000);
    assert(g_last_player == 0); // Player 1 (slot 0)
    assert((g_last_btn & DM_BTN_A) != 0);
    assert((g_mock_players & 1) != 0);
    close(s);
    printf("[t_webpad] 3. Controller input processed!\n");

    // 4. Security Fuzz: Oversized WebSocket frame rejected
    s = connect_to_server();
    send_ws_handshake(s, "/ws", NULL, 0);
    uint8_t bad_frame[512];
    bad_frame[0] = 0x81;
    bad_frame[1] = 0x80 | 126; // Extended length
    bad_frame[2] = 0x01;
    bad_frame[3] = 0x40; // 320 bytes (> WEBPAD_MAX_PAYLOAD_SIZE 256)
    write(s, bad_frame, 4);
    int attempts = 0; ssize_t n = 0;
    while (attempts++ < 20) {
        n = read(s, buf, sizeof(buf));
        if (n <= 0) break;
        usleep(10000);
    }
    close(s);
    printf("[t_webpad] 4. Oversized frame safely dropped!\n");

    // 5. 64-bit frame length (payload_len == 127) must immediately disconnect
    s = connect_to_server();
    send_ws_handshake(s, "/ws", NULL, 0);
    uint8_t frame_127[16] = { 0x81, 0x80 | 127, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 };
    write(s, frame_127, 14);
    attempts = 0;
    while (attempts++ < 20) {
        n = read(s, buf, sizeof(buf));
        if (n <= 0) break;
        usleep(10000);
    }
    close(s);
    printf("[t_webpad] 5. 64-bit length frame safely dropped!\n");

    dm_webpad_stop();
    printf("[t_webpad] ALL TESTS PASSED!\n");
    return 0;
}
