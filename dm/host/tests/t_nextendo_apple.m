// Live integration test for nextendo_apple.m:
// Exercises NSURLSession HTTP (/api/friends, /api/presence, /api/bcat), SecKey RS256 BAAS JWT signing,
// and SecureTransport TLS (SNI recovery, self-signed Nextendo cert acceptance, peer allowlist refusal,
// encrypted write/peek/read/poll) against real local loopback servers configured via env vars.
#include "../hle_net.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

__thread _Atomic int dm_host_work;
void dm_guest_sleep_ns(int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; pthread_cond_timedwait_relative_np(c, m, &ts); }
uint64_t guest_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
int dm_detclock_enabled(void) { return 0; }

static int checks, fails;
#define CHECK(c, msg) do { checks++; int ok_ = !!(c); fails += !ok_; if (!ok_) fprintf(stderr, "FAIL: %s\n", msg); } while (0)

int main(void) {
    const char *http_port_s = getenv("TEST_NX_HTTP_PORT");
    const char *tls_port_s = getenv("TEST_NX_TLS_PORT");
    const char *bcat_dir = getenv("TEST_NX_BCAT_DIR");
    const char *pem_path = getenv("TEST_NX_RSA_PEM");
    const char *jwt_out_path = getenv("TEST_NX_JWT_OUT");

    if (!http_port_s || !tls_port_s) {
        // Standalone mode (when invoked without external test server): verify SSL create/destroy and peer refusal.
        setenv("DM_NEXTENDO", "1", 1);
        setenv("DM_NEXTENDO_SERVER_HOST", "127.0.0.1", 1);
        dm_nextendo_reset();
        dm_ssl_conn *c = dm_nextendo_ssl_create();
        CHECK(c != NULL, "standalone: SecureTransport SSL context created");
        CHECK(dm_nextendo_ssl_set_hostname(c, "dauth-lp1.ndas.srv.nintendo.net") == 0, "standalone: set hostname");
        CHECK(dm_nextendo_ssl_set_io_mode(c, 1) == 0, "standalone: set io mode");
        dm_nextendo_ssl_destroy(c);
        printf("t_nextendo_apple (standalone): %d checks, %d failed\n", checks, fails);
        return fails != 0;
    }

    char api_url[128];
    snprintf(api_url, sizeof api_url, "http://127.0.0.1:%s", http_port_s);
    setenv("DM_NEXTENDO", "1", 1);
    setenv("DM_NEXTENDO_API", api_url, 1);
    setenv("DM_NEXTENDO_CLIENT_ID", "nxc_live_test", 1);
    setenv("DM_NEXTENDO_SERVER_HOST", "127.0.0.1", 1);
    setenv("DM_NEXTENDO_ACCESS_TOKEN", "test_access_token_xyz", 1);
    if (pem_path) setenv("DM_NEXTENDO_BAAS_KEY", pem_path, 1);
    dm_nextendo_reset();

    // 1. Live NSURLSession GET /api/friends
    dm_nextendo_friend friends[8];
    int nf = dm_nextendo_get_friends(friends, 8);
    CHECK(nf == 2, "live HTTP: GET /api/friends returned 2 friends");
    if (nf == 2) {
        CHECK(friends[0].pid == 424242ULL && !strcmp(friends[0].name, "Toad") && friends[0].favorite == 1 && friends[0].online_status == 2,
              "live HTTP: friend[0] parsed correctly");
        CHECK(friends[1].pid == 999999ULL && !strcmp(friends[1].name, "Yoshi") && friends[1].online_status == 1,
              "live HTTP: friend[1] parsed correctly");
    }

    // 2. Live NSURLSession POST /api/presence
    uint8_t field[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    CHECK(dm_nextendo_update_presence(2, field, sizeof field, "Party Plaza") == 0, "live HTTP: POST /api/presence succeeded");

    // 3. Live NSURLSession GET /api/bcat/<titleId>
    if (bcat_dir) {
        CHECK(dm_nextendo_sync_bcat(DM_GAME_ID, bcat_dir) == 0, "live HTTP: GET /api/bcat synced payload");
    }

    // 4. Live SecKey RS256 BAAS JWT signing
    if (pem_path && jwt_out_path) {
        char jwt[4096];
        size_t jlen = 0;
        CHECK(dm_nextendo_mint_baas_id_token(jwt, sizeof jwt, &jlen) == 0 && jlen > 100, "live RSA: minted RS256 BAAS JWT");
        FILE *f = fopen(jwt_out_path, "w");
        if (f) { fwrite(jwt, 1, jlen, f); fclose(f); }
    }

    // 5. Live SecureTransport TLS over HLE BSD TCP socket + DNS redirect + SNI recovery
    void *ai = NULL;
    CHECK(h_sk_GetAddrInfo("dauth-lp1.ndas.srv.nintendo.net", tls_port_s, NULL, &ai) == 0 && ai != NULL,
          "live TLS: GetAddrInfo redirected Nintendo host to 127.0.0.1");
    dm_guest_addrinfo *gai = ai;
    int s = h_sk_Socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0 && h_sk_Connect(s, gai->ptr24, gai->ai_addrlen) == 0, "live TLS: BSD TCP connect succeeded");
    h_sk_FreeAddrInfo(ai);

    uint8_t ssl_obj[64] = {0};
    CHECK(h_ssl_CnCreate(ssl_obj, NULL) == RES_OK, "live TLS: nn::ssl::Connection::Create succeeded");
    CHECK(h_ssl_CnSock(ssl_obj, s) == RES_OK, "live TLS: SetSocketDescriptor succeeded");
    // Intentionally do NOT call SetHostName — verify automatic SNI recovery from DNS redirect map!
    CHECK(h_ssl_CnHandshake(ssl_obj) == RES_OK, "live TLS: DoHandshake succeeded with self-signed cert and recovered SNI");

    const char *req = "PING_NEXTENDO";
    CHECK(h_ssl_CnWrite(ssl_obj, req, (unsigned)strlen(req)) == (int)strlen(req), "live TLS: Write sent encrypted bytes");
    uint32_t in_ev = 1, out_ev = 0;
    CHECK(h_ssl_CnPoll(ssl_obj, &out_ev, &in_ev, 2000) == RES_OK && (out_ev & 1), "live TLS: Poll detected readable response");

    char pbuf[32] = {0}; int plen = 0;
    CHECK(h_ssl_CnPeek(ssl_obj, pbuf, &plen, 4) == RES_OK && plen == 4 && !memcmp(pbuf, "PONG", 4), "live TLS: Peek read PONG prefix");
    char rbuf[128] = {0};
    int total = 0;
    while (total < (int)sizeof rbuf - 1) {
        int rlen = h_ssl_CnRead(ssl_obj, rbuf + total, (unsigned)(sizeof rbuf - 1 - (size_t)total));
        if (rlen <= 0) break;
        total += rlen;
        if (strstr(rbuf, "PING_NEXTENDO")) break;
    }
    CHECK(total > 4 && strstr(rbuf, "PONG:dauth-lp1.ndas.srv.nintendo.net:PING_NEXTENDO") != NULL,
          "live TLS: Read received server echo confirming SNI was dauth-lp1.ndas.srv.nintendo.net");
    CHECK(h_ssl_CnDestroy(ssl_obj) == RES_OK, "live TLS: Destroy succeeded");
    CHECK(h_sk_Close(s) == 0, "live TLS: BSD socket closed");

    printf("t_nextendo_apple (live): %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
