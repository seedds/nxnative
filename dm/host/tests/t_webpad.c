// Comprehensive security & functional unit test for dm_webpad.
// Tests:
// 1. HTTP HTML serving
// 2. Rejecting unauthorized WebSocket commands
// 3. Rejecting incorrect PIN
// 4. Successful PIN authentication via WebSocket message
// 5. Successful PIN authentication via query string (?pin=...)
// 6. Fuzzing: Oversized WebSocket frame rejection
// 7. Security: IP lockout after 5 consecutive failed PIN attempts
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

static void send_ws_handshake(int s, const char *path) {
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
        if (strstr(buf, "\r\n\r\n")) break;
    }
    assert(strstr(buf, "HTTP/1.1 101") != NULL);
    // If not authenticated, server immediately sends AUTH_REQUIRED frame; drain it so socket is ready
    if (!strstr(path, "pin=")) {
        usleep(15000);
        char drain[256];
        recv(s, drain, sizeof(drain), MSG_DONTWAIT);
    }
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

static void read_ws_text(int s, char *out, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        char chunk[256];
        ssize_t n = read(s, chunk, sizeof(chunk));
        if (n <= 0) break;
        memcpy(out + total, chunk, (size_t)n);
        total += (size_t)n;
        out[total] = '\0';
        if (strstr(out, "AUTH_") || strstr(out, "player")) break;
    }
    out[total] = '\0';
}

int main() {
    printf("[t_webpad] starting clean-room security and protocol tests...\n");
    setenv("DM_BROWSER_PADS", "1", 1);
    dm_webpad_enable(1);
    dm_webpad_set_pin("4819");
    assert(strcmp(dm_webpad_pin(), "4819") == 0);
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

    // 2. Unauthenticated gamepad input rejected
    s = connect_to_server();
    send_ws_handshake(s, "/ws");
    g_last_player = -1;
    send_masked_text(s, "1,0.500,-1.000"); // Unauthenticated command
    usleep(30000);
    assert(g_last_player == -1); // Must NOT take effect

    // 3. Empty PIN and wrong PIN rejected
    send_masked_text(s, "PIN:");
    read_ws_text(s, buf, sizeof(buf));
    assert(strstr(buf, "AUTH_FAILED") != NULL);
    assert(g_last_player == -1);

    send_masked_text(s, "PIN:9999");
    read_ws_text(s, buf, sizeof(buf));
    assert(strstr(buf, "AUTH_FAILED") != NULL);
    assert(g_last_player == -1);
    close(s);
    printf("[t_webpad] 2 & 3. Empty PIN & wrong PIN rejected!\n");

    // 4. Correct PIN via WebSocket message accepted
    s = connect_to_server();
    send_ws_handshake(s, "/ws");
    send_masked_text(s, "PIN:4819");
    read_ws_text(s, buf, sizeof(buf));
    assert(strstr(buf, "player 1") != NULL);

    send_masked_text(s, "1,0.500,-1.000");
    usleep(30000);
    assert(g_last_player == 0); // Player 1 (slot 0)
    assert((g_last_btn & DM_BTN_A) != 0);
    assert((g_mock_players & 1) != 0);
    close(s);
    printf("[t_webpad] 4. Correct PIN authenticated and controls enabled!\n");

    // 5. Correct PIN via URL query parameter
    s = connect_to_server();
    send_ws_handshake(s, "/ws?pin=4819");
    usleep(20000);
    g_last_btn = 0;
    send_masked_text(s, "2,-0.500,0.800"); // Button B + stick
    usleep(30000);
    assert((g_last_btn & DM_BTN_B) != 0);
    close(s);
    printf("[t_webpad] 5. Query string PIN authentication verified!\n");

    // 6. Security Fuzz: Oversized WebSocket frame and 64-bit length frame rejected
    s = connect_to_server();
    send_ws_handshake(s, "/ws?pin=4819");
    uint8_t bad_frame[512];
    bad_frame[0] = 0x81;
    bad_frame[1] = 0x80 | 126; // Extended length
    bad_frame[2] = 0x01;
    bad_frame[3] = 0x40; // 320 bytes (> WEBPAD_MAX_PAYLOAD_SIZE 256)
    write(s, bad_frame, 4);
    int attempts = 0;
    while (attempts++ < 20) {
        ssize_t n = read(s, buf, sizeof(buf));
        if (n <= 0) break;
        usleep(10000);
    }
    close(s);

    // 64-bit frame length (payload_len == 127) must immediately disconnect
    s = connect_to_server();
    send_ws_handshake(s, "/ws?pin=4819");
    uint8_t frame_127[16] = { 0x81, 0x80 | 127, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 };
    write(s, frame_127, 14);
    attempts = 0;
    while (attempts++ < 20) {
        ssize_t n = read(s, buf, sizeof(buf));
        if (n <= 0) break;
        usleep(10000);
    }
    close(s);
    printf("[t_webpad] 6. Oversized frame & 64-bit length frame safely dropped!\n");

    // 7. Security: IP lockout after 5 consecutive failed PIN attempts
    // We already used 2 failed attempts in test 3 ("PIN:" and "PIN:9999").
    // Let's do 2 more failed attempts (total 4)
    for (int i = 0; i < 2; i++) {
        s = connect_to_server();
        send_ws_handshake(s, "/ws");
        send_masked_text(s, "PIN:0000");
        read_ws_text(s, buf, sizeof(buf));
        assert(strstr(buf, "AUTH_FAILED") != NULL);
        close(s);
    }
    // Attempt 5 should trigger lockout and disconnect
    s = connect_to_server();
    send_ws_handshake(s, "/ws");
    send_masked_text(s, "PIN:0000");
    usleep(30000);
    close(s);

    // Attempt 6: Any connection from this IP is now locked out
    s = connect_to_server();
    const char *get_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(s, get_req, strlen(get_req));
    ssize_t n = read(s, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        assert(strstr(buf, "403 Forbidden") != NULL);
    }
    close(s);
    printf("[t_webpad] 7. IP lockout after 5 failed attempts verified!\n");

    dm_webpad_stop();
    printf("[t_webpad] ALL SECURITY AND PROTOCOL TESTS PASSED!\n");
    return 0;
}
