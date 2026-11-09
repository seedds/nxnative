// Discord Rich Presence over Discord's local IPC (issue #1845): the desktop client listens on a Unix socket, discord-ipc-<0..9> in the user's temporary folder, and takes
// frames of a little-endian header (opcode, payload length, both uint32) and a JSON payload. A client opens with a handshake naming its application ID, receives a READY
// dispatch, then sends SET_ACTIVITY. The client clears the activity by itself when the socket closes, so there is nothing to undo on exit.
// Plain POSIX and no host headers, so the protocol is tested without a game or the Discord client (tests/t_discord.c runs a fake server). hle_discord_mac.c is its only user.
#pragma once
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum { DRPC_HANDSHAKE = 0, DRPC_FRAME = 1, DRPC_CLOSE = 2, DRPC_PING = 3, DRPC_PONG = 4 };
enum { DRPC_MAX_PAYLOAD = 64 * 1024 };   // READY and command replies are a few hundred bytes; a larger frame is read past and dropped
enum { DRPC_TEXT_MAX = 128 };            // details / state: Discord takes 2 to 128 characters; bytes are cut at a UTF-8 boundary, so the count holds for characters too

// JSON string body of s (no quotes) into out, at most max_src bytes of s, cut back to a UTF-8 character boundary. Returns the length written, or -1 when out is too small
static inline int drpc_json_escape(char *out, size_t cap, const char *s, size_t max_src) {
    size_t n = strlen(s), o = 0;
    if (n > max_src) { n = max_src; while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }   // never end inside a multi-byte character
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i]; char esc[8]; size_t l;
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; l = 2; }
        else if (c < 0x20) l = (size_t)snprintf(esc, sizeof esc, "\\u%04x", c);
        else { esc[0] = (char)c; l = 1; }
        if (o + l >= cap) return -1;
        memcpy(out + o, esc, l); o += l;
    }
    if (o >= cap) return -1;
    out[o] = 0; return (int)o;
}

// A frame (header + payload) into out. Returns its size, or 0 when out is too small
static inline size_t drpc_frame(uint8_t *out, size_t cap, uint32_t op, const char *json, size_t len) {
    if (len > DRPC_MAX_PAYLOAD || cap < 8 + len) return 0;
    uint32_t h[2] = {op, (uint32_t)len};   // little-endian on every host this runs on (arm64 Macs; x86-64 and arm64 test machines)
    memcpy(out, h, 8); memcpy(out + 8, json, len); return 8 + len;
}

// The handshake: {"v":1,"client_id":"<application ID>"}. The ID is digits only (Discord's snowflake); anything else is refused here, before it reaches the socket
static inline int drpc_valid_app_id(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (n < 15 || n > 20) return 0;
    for (size_t i = 0; i < n; i++) if (id[i] < '0' || id[i] > '9') return 0;
    return 1;
}
static inline int drpc_handshake_json(char *out, size_t cap, const char *app_id) {
    if (!drpc_valid_app_id(app_id)) return -1;
    int n = snprintf(out, cap, "{\"v\":1,\"client_id\":\"%s\"}", app_id);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

// SET_ACTIVITY for this process: details = the game's name, state = an optional second line (NULL or shorter than 2 bytes: left out, as Discord would refuse it),
// start = Unix seconds the elapsed time counts from (0: no timestamp). Returns the JSON's length, or -1
static inline int drpc_activity_json(char *out, size_t cap, int pid, const char *details, const char *state, int64_t start, unsigned nonce) {
    char d[DRPC_TEXT_MAX * 6 + 1], s[DRPC_TEXT_MAX * 6 + 1];   // every byte may become \u00XX
    if (!details || drpc_json_escape(d, sizeof d, details, DRPC_TEXT_MAX) < 2) return -1;
    int has_state = state && drpc_json_escape(s, sizeof s, state, DRPC_TEXT_MAX) >= 2;
    char ts[64] = "";
    if (start > 0) snprintf(ts, sizeof ts, ",\"timestamps\":{\"start\":%lld}", (long long)start);
    int n = snprintf(out, cap, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"type\":0,\"details\":\"%s\"%s%s%s%s}},\"nonce\":\"%u\"}",
                     pid, d, has_state ? ",\"state\":\"" : "", has_state ? s : "", has_state ? "\"" : "", ts, nonce);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

// Socket I/O. Every call is bounded by timeout_ms, so a stalled client never holds the caller. SIGPIPE: a write to a socket the client closed would kill the process,
// so the socket is made not to raise it (SO_NOSIGPIPE on Apple platforms, MSG_NOSIGNAL elsewhere)
#ifdef MSG_NOSIGNAL
#define DRPC_SEND_FLAGS MSG_NOSIGNAL
#else
#define DRPC_SEND_FLAGS 0
#endif
static inline int drpc_wait(int fd, short ev, int timeout_ms) {
    struct pollfd p = {fd, ev, 0};
    int r; do r = poll(&p, 1, timeout_ms); while (r < 0 && errno == EINTR);
    return r > 0 && (p.revents & (ev | POLLHUP | POLLERR)) ? 1 : r == 0 ? 0 : -1;
}
static inline int drpc_write_all(int fd, const uint8_t *b, size_t n, int timeout_ms) {
    while (n) {
        if (drpc_wait(fd, POLLOUT, timeout_ms) != 1) return -1;
        ssize_t w = send(fd, b, n, DRPC_SEND_FLAGS);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}
static inline int drpc_read_all(int fd, uint8_t *b, size_t n, int timeout_ms) {
    while (n) {
        if (drpc_wait(fd, POLLIN, timeout_ms) != 1) return -1;
        ssize_t r = recv(fd, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;   // 0: the client closed the socket
        b += r; n -= (size_t)r;
    }
    return 0;
}
static inline int drpc_send(int fd, uint32_t op, const char *json, size_t len, int timeout_ms) {
    static _Thread_local uint8_t buf[8 + 4096];
    size_t n = drpc_frame(buf, sizeof buf, op, json, len);
    return n ? drpc_write_all(fd, buf, n, timeout_ms) : -1;
}
// One frame from the client into payload (NUL-terminated, cap bytes with the NUL). A larger payload is read past and returned empty. Returns the opcode, or -1 when the
// socket closed, failed or stayed silent for timeout_ms (0: poll once, -1: wait forever)
static inline int drpc_recv(int fd, char *payload, size_t cap, int timeout_ms) {
    uint32_t h[2];
    if (drpc_read_all(fd, (uint8_t *)h, 8, timeout_ms)) return -1;
    if (h[1] > DRPC_MAX_PAYLOAD) return -1;   // not Discord's framing: give up on this socket
    if (h[1] < cap) { if (drpc_read_all(fd, (uint8_t *)payload, h[1], timeout_ms)) return -1; payload[h[1]] = 0; return (int)h[0]; }
    for (uint32_t left = h[1]; left;) {
        char skip[512]; uint32_t k = left < sizeof skip ? left : (uint32_t)sizeof skip;
        if (drpc_read_all(fd, (uint8_t *)skip, k, timeout_ms)) return -1;
        left -= k;
    }
    if (cap) payload[0] = 0;
    return (int)h[0];
}

// The folders the client may have put its socket in, in the order Discord's own SDK searches them. dirs gets up to max entries (pointers into the environment or the
// static buffers); darwin_tmp is the per-user temporary folder from confstr(_CS_DARWIN_USER_TEMP_DIR) on a Mac, which $TMPDIR is when nothing changed it, or NULL
static inline int drpc_dirs(const char **dirs, int max, const char *darwin_tmp) {
    const char *env[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    int n = 0;
    for (size_t i = 0; i < sizeof env / sizeof *env && n < max; i++) { const char *v = getenv(env[i]); if (v && *v) dirs[n++] = v; }
    if (darwin_tmp && *darwin_tmp && n < max) dirs[n++] = darwin_tmp;
    if (n < max) dirs[n++] = "/tmp";
    return n;
}
// Opens discord-ipc-<0..9> in the first folder that has one listening, sends the handshake and waits for READY. Returns the socket, or -1 with why in err:
// 0 no client listening, 1 a client refused the handshake (wrong application ID; its message in reason), 2 a protocol or I/O failure
static inline int drpc_connect(const char *const *dirs, int ndirs, const char *app_id, int timeout_ms, int *err, char *reason, size_t rcap) {
    char hs[128]; int hl = drpc_handshake_json(hs, sizeof hs, app_id);
    *err = 2; if (rcap) reason[0] = 0;
    if (hl < 0) return -1;
    *err = 0;
    for (int d = 0; d < ndirs; d++) for (int i = 0; i < 10 && dirs[d] && *dirs[d]; i++) {
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        const char *sep = dirs[d][strlen(dirs[d]) - 1] == '/' ? "" : "/";
        if ((size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s%sdiscord-ipc-%d", dirs[d], sep, i) >= sizeof a.sun_path) break;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
        if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); continue; }
        char rep[2048]; int op = -1;
        if (!drpc_send(fd, DRPC_HANDSHAKE, hs, (size_t)hl, timeout_ms)) op = drpc_recv(fd, rep, sizeof rep, timeout_ms);
        if (op == DRPC_FRAME && strstr(rep, "\"READY\"")) { *err = 0; return fd; }
        close(fd);
        if (op == DRPC_CLOSE) { *err = 1; if (rcap) snprintf(reason, rcap, "%s", rep); return -1; }   // the client answered: another socket would answer the same
        *err = 2;
    }
    return -1;
}
// SET_ACTIVITY and its reply. Returns 0 when Discord took it, 1 when it answered with an error (its reply in reason), -1 when the socket failed
static inline int drpc_set_activity(int fd, int pid, const char *details, const char *state, int64_t start, unsigned nonce, int timeout_ms, char *reason, size_t rcap) {
    char js[2048]; int jl = drpc_activity_json(js, sizeof js, pid, details, state, start, nonce);
    if (rcap) reason[0] = 0;
    if (jl < 0 || drpc_send(fd, DRPC_FRAME, js, (size_t)jl, timeout_ms)) return -1;
    char want[32]; snprintf(want, sizeof want, "\"nonce\":\"%u\"", nonce);
    for (;;) {   // the reply carries our nonce; a PING may come first
        char rep[2048]; int op = drpc_recv(fd, rep, sizeof rep, timeout_ms);
        if (op == DRPC_PING) { if (drpc_send(fd, DRPC_PONG, rep, strlen(rep), timeout_ms)) return -1; continue; }
        if (op != DRPC_FRAME) return -1;
        if (!strstr(rep, want)) continue;
        if (strstr(rep, "\"evt\":\"ERROR\"")) { if (rcap) snprintf(reason, rcap, "%s", rep); return 1; }
        return 0;
    }
}
// Keeps a connection alive until the client closes it: answers PINGs, ignores everything else. Returns when the socket closes or fails, or after timeout_ms of silence (0)
static inline int drpc_pump(int fd, int timeout_ms) {
    char rep[2048];
    for (;;) {
        int w = drpc_wait(fd, POLLIN, timeout_ms);
        if (w == 0) return 0;
        int op = w < 0 ? -1 : drpc_recv(fd, rep, sizeof rep, 2000);
        if (op < 0 || op == DRPC_CLOSE) return -1;
        if (op == DRPC_PING && drpc_send(fd, DRPC_PONG, rep, strlen(rep), 2000)) return -1;
    }
}
