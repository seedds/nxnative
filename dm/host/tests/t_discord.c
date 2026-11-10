// Discord IPC (discord_rpc.h, used by hle_discord_mac.c): payloads, framing and a whole session against a fake client on a Unix socket, with no Discord installed.
#include "discord_rpc.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>

#define APP "1234567890123456789"
static char dir[256];

// A fake Discord client listening on <dir>/discord-ipc-<slot>: mode 0 accepts the handshake and the activity (with PINGs in between), 1 refuses the handshake, 2 refuses the activity
struct srv { int slot, mode, ok; pthread_t th; int lfd; char activity[2048]; };
static void srv_send(int fd, uint32_t op, const char *js) { assert(!drpc_send(fd, op, js, strlen(js), 2000)); }
static void *srv_main(void *p) {
    struct srv *s = p; char buf[4096];
    int fd = accept(s->lfd, NULL, NULL); assert(fd >= 0);
    assert(drpc_recv(fd, buf, sizeof buf, 2000) == DRPC_HANDSHAKE);
    assert(!strcmp(buf, "{\"v\":1,\"client_id\":\"" APP "\"}"));
    if (s->mode == 1) { srv_send(fd, DRPC_CLOSE, "{\"code\":4000,\"message\":\"Invalid Client ID\"}"); close(fd); s->ok = 1; return NULL; }
    srv_send(fd, DRPC_FRAME, "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1},\"evt\":\"READY\",\"nonce\":null}");
    assert(drpc_recv(fd, s->activity, sizeof s->activity, 2000) == DRPC_FRAME);
    srv_send(fd, DRPC_PING, "{\"p\":1}");   // the client must answer a PING that comes before the reply
    assert(drpc_recv(fd, buf, sizeof buf, 2000) == DRPC_PONG && !strcmp(buf, "{\"p\":1}"));
    srv_send(fd, DRPC_FRAME, "{\"cmd\":\"SET_ACTIVITY\",\"evt\":null,\"nonce\":\"99\"}");   // another request's reply: skipped
    if (s->mode == 2) srv_send(fd, DRPC_FRAME, "{\"cmd\":\"SET_ACTIVITY\",\"data\":{\"code\":4000,\"message\":\"bad\"},\"evt\":\"ERROR\",\"nonce\":\"7\"}");
    else srv_send(fd, DRPC_FRAME, "{\"cmd\":\"SET_ACTIVITY\",\"data\":{},\"evt\":null,\"nonce\":\"7\"}");
    if (s->mode == 0) {   // the connection is kept: a later PING is answered by drpc_pump, then the client goes away
        char big[DRPC_MAX_PAYLOAD - 1]; memset(big, 'x', sizeof big); big[sizeof big - 1] = 0;
        srv_send(fd, DRPC_FRAME, "{\"evt\":\"ACTIVITY_JOIN\"}");
        { uint32_t h[2] = {DRPC_FRAME, (uint32_t)strlen(big)}; assert(!drpc_write_all(fd, (uint8_t *)h, 8, 2000)); assert(!drpc_write_all(fd, (uint8_t *)big, strlen(big), 2000)); }   // larger than the reader's buffer: skipped whole
        srv_send(fd, DRPC_PING, "{\"p\":2}");
        assert(drpc_recv(fd, buf, sizeof buf, 2000) == DRPC_PONG && !strcmp(buf, "{\"p\":2}"));
    }
    close(fd); s->ok = 1; return NULL;
}
static void srv_start(struct srv *s, int slot, int mode) {
    memset(s, 0, sizeof *s); s->slot = slot; s->mode = mode;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    assert((size_t)snprintf(a.sun_path, sizeof a.sun_path, "%s/discord-ipc-%d", dir, slot) < sizeof a.sun_path); unlink(a.sun_path);
    s->lfd = socket(AF_UNIX, SOCK_STREAM, 0); assert(s->lfd >= 0);
    assert(!bind(s->lfd, (struct sockaddr *)&a, sizeof a) && !listen(s->lfd, 1));
    assert(!pthread_create(&s->th, NULL, srv_main, s));
}
static void srv_end(struct srv *s) {
    pthread_join(s->th, NULL); close(s->lfd); assert(s->ok);
    char p[300]; snprintf(p, sizeof p, "%s/discord-ipc-%d", dir, s->slot); unlink(p);
}

int main(void) {
    char out[4096];
    // Escaping: quotes, backslashes and control characters; UTF-8 passes through, and a cut never ends inside a character
    assert(drpc_json_escape(out, sizeof out, "a\"b\\c\n\x01", 100) == 19 && !strcmp(out, "a\\\"b\\\\c\\u000a\\u0001"));
    assert(drpc_json_escape(out, sizeof out, "Pok\xC3\xA9mon", 100) == 8 && !strcmp(out, "Pok\xC3\xA9mon"));
    assert(drpc_json_escape(out, sizeof out, "Pok\xC3\xA9mon", 4) == 3 && !strcmp(out, "Pok"));   // 4 bytes would split the two-byte e-acute
    assert(drpc_json_escape(out, sizeof out, "\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xAA", 7) == 6);     // three katakana (Mario) cut to 7 bytes keep two whole characters
    assert(drpc_json_escape(out, 4, "abcd", 100) == -1);
    // Application IDs: 15 to 20 digits
    assert(drpc_valid_app_id(APP) && !drpc_valid_app_id("") && !drpc_valid_app_id(NULL) && !drpc_valid_app_id("12345678901234x") && !drpc_valid_app_id("123"));
    assert(drpc_handshake_json(out, sizeof out, "1\"}") == -1);
    // The activity: details always, state only from 2 characters, the start time only when given
    assert(drpc_activity_json(out, sizeof out, 42, "Super Mario Odyssey", "Version 1.4.1", 1700000000, 7) > 0);
    assert(!strcmp(out, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"type\":0,\"details\":\"Super Mario Odyssey\",\"state\":\"Version 1.4.1\","
                        "\"timestamps\":{\"start\":1700000000}}},\"nonce\":\"7\"}"));
    assert(drpc_activity_json(out, sizeof out, 42, "Hades", "", 0, 8) > 0 && !strstr(out, "state") && !strstr(out, "timestamps"));
    assert(drpc_activity_json(out, sizeof out, 42, "Hades", "v", 0, 8) > 0 && !strstr(out, "state"));
    assert(drpc_activity_json(out, sizeof out, 42, "H", NULL, 0, 8) == -1);   // Discord refuses details shorter than 2
    { char lng[400]; memset(lng, 'z', sizeof lng); lng[399] = 0;
      assert(drpc_activity_json(out, sizeof out, 1, lng, NULL, 0, 1) > 0); char *d = strstr(out, "\"details\":\"") + 11; assert(strspn(d, "z") == DRPC_TEXT_MAX); }
    // Framing
    { uint8_t f[64]; assert(drpc_frame(f, sizeof f, DRPC_FRAME, "{}", 2) == 10); uint32_t h[2]; memcpy(h, f, 8); assert(h[0] == 1 && h[1] == 2 && !memcmp(f + 8, "{}", 2));
      assert(drpc_frame(f, 9, DRPC_FRAME, "{}", 2) == 0); }
    // Search folders: the environment's in Discord's order, the per-user folder, /tmp last
    { setenv("XDG_RUNTIME_DIR", "/x", 1); setenv("TMPDIR", "/y/", 1); unsetenv("TMP"); unsetenv("TEMP");
      const char *d[8]; int n = drpc_dirs(d, 8, "/z");
      assert(n == 4 && !strcmp(d[0], "/x") && !strcmp(d[1], "/y/") && !strcmp(d[2], "/z") && !strcmp(d[3], "/tmp"));
      assert(drpc_dirs(d, 2, "/z") == 2); }

    snprintf(dir, sizeof dir, "/tmp/t_discord.XXXXXX");   // /tmp, not the build folder: a socket path must fit sun_path (104 bytes on the Mac)
    assert(mkdtemp(dir));
    const char *dirs[] = {"/nonexistent-folder", dir};
    int err; char why[256];
    // Nobody listening: err 0, so the caller waits for Discord to start
    assert(drpc_connect(dirs, 2, APP, 500, &err, why, sizeof why) == -1 && err == 0);
    // A whole session, on the second socket of the second folder (the first folder and slot 0 have none)
    { struct srv s; srv_start(&s, 1, 0);
      int fd = drpc_connect(dirs, 2, APP, 2000, &err, why, sizeof why); assert(fd >= 0 && err == 0);
      assert(drpc_set_activity(fd, 42, "Super Mario Odyssey", "Version 1.4.1", 1700000000, 7, 2000, why, sizeof why) == 0);
      assert(strstr(s.activity, "\"details\":\"Super Mario Odyssey\"") && strstr(s.activity, "\"nonce\":\"7\""));
      assert(drpc_pump(fd, 2000) == -1);   // answers the PING, then sees the client close
      close(fd); srv_end(&s); }
    // A refused application ID: err 1 and the client's reason
    { struct srv s; srv_start(&s, 0, 1);
      assert(drpc_connect(dirs, 2, APP, 2000, &err, why, sizeof why) == -1 && err == 1 && strstr(why, "Invalid Client ID"));
      srv_end(&s); }
    // A refused activity: 1 and the reply
    { struct srv s; srv_start(&s, 0, 2);
      int fd = drpc_connect(dirs, 2, APP, 2000, &err, why, sizeof why); assert(fd >= 0);
      assert(drpc_set_activity(fd, 42, "Hades", NULL, 0, 7, 2000, why, sizeof why) == 1 && strstr(why, "\"ERROR\""));
      close(fd); srv_end(&s); }
    // Silence: drpc_pump returns 0 after its timeout and the socket stays usable
    { int sv[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
      assert(drpc_pump(sv[0], 50) == 0); close(sv[1]); assert(drpc_pump(sv[0], 1000) == -1); close(sv[0]); }
    rmdir(dir);
    puts("t_discord: ok");
    return 0;
}
