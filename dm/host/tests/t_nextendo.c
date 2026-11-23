// Nextendo Network client & online/offline HLE routing unit tests.
// Tests PKCE, nx2. token parsing, config persistence, local BAAS id_token JWT minting,
// DNS/NSD redirection + SNI lookup, real host TCP/UDP sockets + mixed poll, and online vs offline HLE routing.
#include "../hle_net.c"
#include "../hle_account.c"
#include "../hle_friends.c"
#include "../hle_misc.c"
#include <assert.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

__thread _Atomic int dm_host_work;

void dm_guest_sleep_ns(int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; pthread_cond_timedwait_relative_np(c, m, &ts); }
uint64_t guest_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
int dm_detclock_enabled(void) { return 0; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { (void)g; }
void dm_thread_native_wait(int entering) { (void)entering; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
void dm_backtrace(uint64_t fp) { (void)fp; }
int front_user_select(const front_user *u, int n, int c, int s) { (void)u; (void)n; (void)c; (void)s; return FRONT_USER_CANCEL; }
int front_text_input(const char *t, const char *s, const char *g, const char *i, int m, char *o, int c) { (void)t; (void)s; (void)g; (void)i; (void)m; (void)o; (void)c; return -1; }
void guest_clock_freeze_for(int r, int on) { (void)r; (void)on; }
uint32_t dm_native_image_blank(uint32_t w, uint32_t h) { (void)w; (void)h; return 0; }
void dm_native_image_blank_free(uint32_t handle) { (void)handle; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *guest_sym(const char *n) { (void)n; return NULL; }

static int checks, fails;
#define CHECK(c, msg) do { checks++; int ok_ = !!(c); fails += !ok_; if (!ok_) fprintf(stderr, "FAIL: %s\n", msg); } while (0)

static void make_nx2_token(uint64_t pid, const char *user, int64_t exp, char *out, size_t cap) {
    char raw[256], b64p[384], b64s[64];
    int n = snprintf(raw, sizeof raw, "%llu.%s.%lld", (unsigned long long)pid, user, (long long)exp);
    assert(n > 0 && (size_t)n < sizeof raw);
    assert(dm_nextendo_base64url_encode((const uint8_t *)raw, (size_t)n, b64p, sizeof b64p) > 0);
    uint8_t dummy_sig[32];
    memset(dummy_sig, 0xAB, sizeof dummy_sig);
    assert(dm_nextendo_base64url_encode(dummy_sig, sizeof dummy_sig, b64s, sizeof b64s) > 0);
    snprintf(out, cap, "nx2.%s.%s", b64p, b64s);
}

static void test_pkce_and_oauth(void) {
    dm_nextendo_pkce a, b;
    CHECK(dm_nextendo_pkce_generate(&a) == 0, "pkce generate a");
    CHECK(dm_nextendo_pkce_generate(&b) == 0, "pkce generate b");
    CHECK(strlen(a.verifier) == 43, "pkce verifier is 43 chars (32 bytes base64url)");
    CHECK(strlen(a.challenge) == 43, "pkce challenge is 43 chars (SHA-256 base64url)");
    CHECK(strlen(a.state) == 22, "pkce state is 22 chars (16 bytes base64url)");
    CHECK(strcmp(a.verifier, b.verifier) != 0, "pkce verifiers are random");

    // Verify S256 challenge = base64url(SHA256(verifier)).
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    char expected_chal[64];
    CC_SHA256(a.verifier, (CC_LONG)strlen(a.verifier), digest);
    dm_nextendo_base64url_encode(digest, sizeof digest, expected_chal, sizeof expected_chal);
    CHECK(strcmp(a.challenge, expected_chal) == 0, "pkce S256 challenge matches SHA-256(verifier)");

    setenv("DM_NEXTENDO_API", "https://example.nextendo.test/", 1);
    setenv("DM_NEXTENDO_CLIENT_ID", "nxc_test_client", 1);
    dm_nextendo_reset();

    char url[1024];
    CHECK(dm_nextendo_build_authorize_url(&a, "http://127.0.0.1:47823/callback", url, sizeof url) == 0, "authorize URL built");
    CHECK(strncmp(url, "https://example.nextendo.test/oauth/authorize?", 46) == 0, "authorize URL strips trailing slash");
    CHECK(strstr(url, "client_id=nxc_test_client") != NULL, "authorize URL contains client_id");
    CHECK(strstr(url, "code_challenge_method=S256") != NULL, "authorize URL uses S256");
    CHECK(strstr(url, a.challenge) != NULL, "authorize URL contains challenge");
    CHECK(strstr(url, a.state) != NULL, "authorize URL contains state");

    char q[256], code[128];
    snprintf(q, sizeof q, "code=auth_code_123%%2B45&state=%s", a.state);
    CHECK(dm_nextendo_parse_callback_query(q, a.state, code, sizeof code) == 0, "callback parses valid code+state");
    CHECK(strcmp(code, "auth_code_123+45") == 0, "callback URL-decodes code");
    CHECK(dm_nextendo_parse_callback_query(q, "wrong_state", code, sizeof code) != 0, "callback rejects state mismatch");
    snprintf(q, sizeof q, "error=access_denied&state=%s", a.state);
    CHECK(dm_nextendo_parse_callback_query(q, a.state, code, sizeof code) != 0, "callback rejects OAuth error");
}

static void test_nx2_token_and_baas_jwt(void) {
    char tok[512];
    make_nx2_token(123456789ULL, "player.with.dots", 1900000000LL, tok, sizeof tok);

    uint64_t pid = 0;
    char user[64] = "";
    int64_t exp = 0;
    CHECK(dm_nextendo_parse_nx2_token(tok, &pid, user, sizeof user, &exp) == 0, "valid nx2 token parses");
    CHECK(pid == 123456789ULL, "nx2 token extracts pid");
    CHECK(strcmp(user, "player.with.dots") == 0, "nx2 token preserves dots in username");
    CHECK(exp == 1900000000LL, "nx2 token extracts expiry");

    CHECK(dm_nextendo_parse_nx2_token("nx1.abc.def", &pid, user, sizeof user, &exp) != 0, "rejects wrong prefix");
    CHECK(dm_nextendo_parse_nx2_token("nx2.abc", &pid, user, sizeof user, &exp) != 0, "rejects missing sig");
    CHECK(dm_nextendo_parse_nx2_token("nx2.abc.def.ghi", &pid, user, sizeof user, &exp) != 0, "rejects extra dots");

    // Persistence (0600) and reload.
    char tmpl[] = "/tmp/t_nextendo_XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != NULL);
    char cfg_path[512];
    snprintf(cfg_path, sizeof cfg_path, "%s/nextendo.conf", dir);
    setenv("DM_NEXTENDO_CONFIG", cfg_path, 1);
    unsetenv("DM_NEXTENDO_TOKEN");
    dm_nextendo_reset();

    CHECK(dm_nextendo_set_tokens(tok, "acc_123", "ref_456") == 0, "set_tokens accepts valid nx2 token");
    CHECK(dm_nextendo_save_config() == 0, "save_config writes file");
    struct stat st;
    CHECK(stat(cfg_path, &st) == 0 && (st.st_mode & 0777) == 0600, "config file permissions are 0600");

    dm_nextendo_reset();
    CHECK(dm_nextendo_is_linked() == 1, "reloaded config stays linked");
    CHECK(dm_nextendo_get_pid() == 123456789ULL, "reloaded pid matches");
    dm_nextendo_get_username(user, sizeof user);
    CHECK(strcmp(user, "player.with.dots") == 0, "reloaded username matches");

    // Mint local BAAS id_token JWT and verify claims.
    char jwt1[4096], jwt2[4096];
    size_t len1 = 0, len2 = 0;
    CHECK(dm_nextendo_mint_baas_id_token(jwt1, sizeof jwt1, &len1) == 0 && len1 > 100, "mints BAAS id_token JWT");
    CHECK(dm_nextendo_mint_baas_id_token(jwt2, sizeof jwt2, &len2) == 0 && len1 == len2 && !strcmp(jwt1, jwt2), "caches BAAS id_token JWT");

    char *d1 = strchr(jwt1, '.');
    char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    CHECK(d1 && d2 && strchr(d2 + 1, '.') == NULL, "JWT has 3 dot-separated parts");
    char pay_b64[3072];
    size_t pay_len = (size_t)(d2 - (d1 + 1));
    memcpy(pay_b64, d1 + 1, pay_len);
    pay_b64[pay_len] = '\0';
    uint8_t pay_raw[2048];
    int dec = dm_nextendo_base64url_decode(pay_b64, pay_raw, sizeof pay_raw - 1);
    CHECK(dec > 0, "JWT payload decodes");
    pay_raw[dec] = '\0';
    CHECK(strstr((const char *)pay_raw, "\"hm\":true") != NULL, "JWT payload sets hm:true");
    CHECK(strstr((const char *)pay_raw, tok) != NULL, "JWT payload embeds nx2. token in nnex claim");
    CHECK(strstr((const char *)pay_raw, DM_GAME_ID) != NULL, "JWT payload embeds game title ID");

    unlink(cfg_path);
    rmdir(dir);
    unsetenv("DM_NEXTENDO_CONFIG");
}

static void test_dns_and_sni(void) {
    setenv("DM_NEXTENDO_SERVER_HOST", "127.0.0.1", 1);
    setenv("DM_NEXTENDO_NAT_HOST", "127.0.0.2", 1);
    setenv("DM_NEXTENDO_NAT_DOMAINS", "g2b309e01-lp1.s.n.srv.nintendo.net", 1);
    dm_nextendo_reset();

    char fqdn[256];
    CHECK(dm_nextendo_nsd_resolve("g2b309e01-%.s.n.srv.nintendo.net", fqdn, sizeof fqdn) == 0 &&
          !strcmp(fqdn, "g2b309e01-lp1.s.n.srv.nintendo.net"), "nsd_resolve substitutes % with lp1");
    CHECK(dm_nextendo_nsd_resolve("accounts.nintendo.com", fqdn, sizeof fqdn) == 0 &&
          !strcmp(fqdn, "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"), "nsd_resolve maps accounts.nintendo.com to BAAS");

    uint32_t ip = 0;
    char sni[256] = "";
    CHECK(dm_nextendo_resolve_host("dauth-lp1.ndas.srv.nintendo.net", &ip, sni, sizeof sni) == 0 && ip == 0x7f000001u,
          "Nintendo host redirects to server_host");
    CHECK(dm_nextendo_lookup_sni(0x7f000001u, sni, sizeof sni) == 0 && !strcmp(sni, "dauth-lp1.ndas.srv.nintendo.net"),
          "SNI reverse map recovers original Nintendo hostname");

    CHECK(dm_nextendo_resolve_host("nncs2-lp1.n.n.srv.nintendo.net", &ip, sni, sizeof sni) == 0 && ip == 0x7f000002u,
          "nncs2 NAT-check host redirects to nat_host");
    CHECK(dm_nextendo_resolve_host("g2b309e01-lp1.s.n.srv.nintendo.net", &ip, sni, sizeof sni) == 0 && ip == 0x7f000002u,
          "custom DM_NEXTENDO_NAT_DOMAINS redirects to nat_host");

    CHECK(dm_nextendo_resolve_host("example.com", &ip, sni, sizeof sni) != 0, "unlisted third-party domain is blocked by default");
    CHECK(dm_nextendo_resolve_host("10.0.0.5", &ip, sni, sizeof sni) == 0 && ip == 0x0a000005u, "literal IPv4 passes through");
    CHECK(dm_nextendo_lookup_sni(0x0a000005u, sni, sizeof sni) != 0, "literal IPv4 does not pollute SNI map");

    CHECK(dm_nextendo_is_allowed_tls_peer(0x7f000001u) == 1, "TLS allowlist permits server_host");
    CHECK(dm_nextendo_is_allowed_tls_peer(0x7f000002u) == 1, "TLS allowlist permits nat_host");
    CHECK(dm_nextendo_is_allowed_tls_peer(0x08080808u) == 0, "TLS allowlist blocks unknown peer");

    CHECK(dm_nextendo_host_prefers_h2("prod.npln.srv.nintendo.net") == 1, "NPLN host prefers h2");
    CHECK(dm_nextendo_host_prefers_h2("g2b309e01-lp1.s.n.srv.nintendo.net") == 0, "NEX host uses http/1.1");
}

static void test_poll_infinite(void);
static void test_grpc_socket_patterns(void);

static void test_online_vs_offline_hle(void) {
    // 1. Offline mode (default): every online service stays unavailable.
    unsetenv("DM_NEXTENDO");
    dm_nextendo_reset();
    CHECK(h_nifm_Available() == 0, "offline: nifm::IsNetworkAvailable is 0");
    uint32_t ip = 0;
    CHECK(h_nifm_GetIp(&ip) == RES_FAIL, "offline: nifm::GetCurrentPrimaryIpAddress fails");
    void *ai = NULL;
    CHECK(h_sk_GetAddrInfo("dauth-lp1.ndas.srv.nintendo.net", "443", NULL, &ai) == 4 && ai == NULL, "offline: GetAddrInfo returns EAI_FAIL");
    CHECK(h_sk_Socket(AF_INET, SOCK_STREAM, 0) == -1, "offline: TCP socket fails");
    uint8_t avail = 1;
    UserHandle uh = {{DM_UID0, DM_UID1, 1}};
    CHECK(h_acc_NsaAvail(&avail, &uh) == RES_OK && avail == 0, "offline: NSA unavailable");
    uint64_t nsa_id = 0;
    CHECK(h_acc_GetNsaId(&nsa_id, &uh) == RES_FAIL, "offline: GetNetworkServiceAccountId fails");
    int fcnt = -1;
    CHECK(h_fr_GetFriendList(&fcnt, NULL, 0, 10, NULL) == RES_FAIL, "offline: GetFriendList fails");
    char fqdn[256] = {0};
    CHECK(h_nsd_Resolve(fqdn, "accounts.nintendo.com") == RES_FAIL, "offline: nsd::Resolve fails");
    CHECK(h_pctl_IsFree() == 0, "offline: pctl::IsFreeCommunicationAvailable is 0");

    // 2. Online mode (DM_NEXTENDO=1) with linked token.
    char tok[512];
    make_nx2_token(987654321ULL, "MarioHost", 1900000000LL, tok, sizeof tok);
    setenv("DM_NEXTENDO", "1", 1);
    setenv("DM_NEXTENDO_TOKEN", tok, 1);
    setenv("DM_NEXTENDO_SERVER_HOST", "127.0.0.1", 1);
    dm_nextendo_reset();

    CHECK(h_nifm_Available() == 1, "online: nifm::IsNetworkAvailable is 1");
    CHECK(h_nifm_GetIp(&ip) == RES_OK && ip != 0, "online: nifm::GetCurrentPrimaryIpAddress succeeds");
    CHECK(h_nsd_Resolve(fqdn, "g123-%.s.n.srv.nintendo.net") == RES_OK && !strcmp(fqdn, "g123-lp1.s.n.srv.nintendo.net"),
          "online: nsd::Resolve succeeds");
    CHECK(h_pctl_IsFree() == 1 && h_pctl_CheckFree(0) == 1 && h_pctl_TryBegin(0) == 1, "online: pctl free communication allowed");

    // DNS GetAddrInfo -> FreeAddrInfo.
    CHECK(h_sk_GetAddrInfo("dauth-lp1.ndas.srv.nintendo.net", "443", NULL, &ai) == 0 && ai != NULL, "online: GetAddrInfo resolves Nintendo host");
    dm_guest_addrinfo *gai = ai;
    CHECK(gai->ai_family == AF_INET && gai->ai_addrlen == 16, "online: addrinfo family/len");
    const uint8_t *ab = gai->ptr24;
    CHECK(ab[2] == 0x01 && ab[3] == 0xbb && ab[4] == 127 && ab[7] == 1, "online: addrinfo has 127.0.0.1:443");
    h_sk_FreeAddrInfo(ai);

    // Account online HLE.
    CHECK(h_acc_NsaAvail(&avail, &uh) == RES_OK && avail == 1, "online: NSA available");
    CHECK(h_acc_GetNsaId(&nsa_id, &uh) == RES_OK && nsa_id == 987654321ULL, "online: GetNetworkServiceAccountId returns PID");
    uint8_t async_ctx[32] = {0};
    CHECK(h_acc_EnsureTokAsync(async_ctx, &uh) == RES_OK, "online: EnsureNetworkServiceAccountIdTokenCacheAsync succeeds");
    CHECK(h_acc_AcGetRes(async_ctx) == RES_OK, "online: AsyncContext::GetResult succeeds");
    char jwt_buf[4096] = {0};
    uint64_t jwt_len = 0;
    CHECK(h_acc_LoadTok(&jwt_len, jwt_buf, sizeof jwt_buf, &uh) == RES_OK && jwt_len > 100 && strchr(jwt_buf, '.') != NULL,
          "online: LoadNetworkServiceAccountIdTokenCache returns BAAS JWT");
    uint8_t na_info[0x68] = {0};
    CHECK(h_acc_LoadNaInfo(na_info, &uh, NULL, 0) == RES_OK && !memcmp(na_info, "2000-01-01", 10) && !memcmp(na_info + 0x20, "US", 2),
          "online: LoadCachedNintendoAccountInfo populates birthday and country");

    // Friends online HLE (filtering + 0x200-byte layout).
    dm_nextendo_friend flist[2];
    memset(flist, 0, sizeof flist);
    flist[0].pid = 111; strlcpy(flist[0].name, "Luigi", sizeof flist[0].name); flist[0].favorite = 1; flist[0].online_status = 2; flist[0].app_id = DM_GAME_PROGRAM_ID;
    flist[1].pid = 222; strlcpy(flist[1].name, "Bowser", sizeof flist[1].name); flist[1].favorite = 0; flist[1].online_status = 0; flist[1].app_id = 0;
    dm_nextendo_set_friends_cache(flist, 2);

    uint8_t fbuf[2 * 0x200];
    uint8_t filt[16] = {0};   // no filter
    CHECK(h_fr_GetFriendList(&fcnt, fbuf, 0, 2, filt) == RES_OK && fcnt == 2, "online: GetFriendList returns 2 friends");
    uint64_t f0_pid = 0; memcpy(&f0_pid, fbuf, 8);
    CHECK(f0_pid == 111 && !strcmp((const char *)(fbuf + 8), "Luigi") && fbuf[0x1f0] == 1, "online: Friend[0] struct layout matches");

    uint32_t online_only = 2; memcpy(filt, &online_only, 4);
    CHECK(h_fr_GetFriendList(&fcnt, fbuf, 0, 2, filt) == RES_OK && fcnt == 1, "online: FriendFilter filters to online-play friend");
    CHECK(h_fr_OpenSession(&uh) == RES_OK && h_fr_CloseSession(&uh) == RES_OK, "online: online play session open/close succeed");

    // Real host TCP loopback + mixed poll (host socket + guest eventfd + closed fd POLLNVAL).
    int srv = dm_sk_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(srv >= 0, "online: TCP server socket created");
    uint8_t bind_addr[16] = {16, AF_INET, 0, 0, 127, 0, 0, 1};
    CHECK(dm_sk_bind(srv, bind_addr, 16) == 0 && dm_sk_listen(srv, 4) == 0, "online: TCP bind+listen succeeds");
    uint8_t bound[16] = {0}; unsigned blen = sizeof bound;
    CHECK(dm_sk_name(srv, bound, &blen, 0) == 0 && (bound[2] || bound[3]), "online: TCP getsockname returns bound port");

    int cli = dm_sk_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cli >= 0 && dm_sk_connect(cli, bound, 16) == 0, "online: TCP client connect succeeds");
    int acc = dm_sk_accept(srv, NULL, NULL);
    CHECK(acc >= 0, "online: TCP accept succeeds");

    int efd = h_sk_EventFd(0, 6);
    CHECK(efd >= 0, "online: eventfd created");
    CHECK(dm_sk_sendto(cli, "hello", 5, 0, NULL, 0) == 5, "online: TCP send succeeds");
    CHECK(h_sk_EventFdWrite(efd, 1) == 0, "online: eventfd write succeeds");

    sk_pollfd pfds[3] = {
        {.fd = acc, .events = SK_POLLIN, .revents = 0},
        {.fd = efd, .events = SK_POLLIN, .revents = 0},
        {.fd = 60,  .events = SK_POLLIN, .revents = 0},   // unused fd -> POLLNVAL
    };
    int pr = dm_sk_poll(pfds, 3, 200);
    CHECK(pr == 3 && (pfds[0].revents & SK_POLLIN) && (pfds[1].revents & SK_POLLIN) && (pfds[2].revents & SK_POLLNVAL),
          "online: mixed poll reports host TCP POLLIN, eventfd POLLIN, and closed fd POLLNVAL together");

    char rbuf[16] = {0};
    CHECK(dm_sk_recvfrom(acc, rbuf, sizeof rbuf, 0, NULL, NULL) == 5 && !memcmp(rbuf, "hello", 5), "online: TCP recv reads payload");
    CHECK(dm_sk_close(acc) == 0 && dm_sk_close(cli) == 0 && dm_sk_close(srv) == 0 && dm_sk_close(efd) == 0, "online: sockets closed");

    test_poll_infinite();
    test_grpc_socket_patterns();

    // UDP port parking: bind -> close -> rebind same port succeeds.
    int u1 = dm_sk_socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u1 >= 0 && dm_sk_bind(u1, bind_addr, 16) == 0, "online: UDP bind succeeds");
    blen = sizeof bound;
    CHECK(dm_sk_name(u1, bound, &blen, 0) == 0, "online: UDP getsockname succeeds");
    CHECK(dm_sk_close(u1) == 0, "online: UDP close parks bound socket");
    int u2 = dm_sk_socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u2 >= 0 && dm_sk_bind(u2, bound, 16) == 0, "online: UDP rebind reclaims parked socket");
    CHECK(dm_sk_close(u2) == 0, "online: UDP second close succeeds");

    unsetenv("DM_NEXTENDO");
    unsetenv("DM_NEXTENDO_TOKEN");
    unsetenv("DM_NEXTENDO_SERVER_HOST");
    unsetenv("DM_NEXTENDO_NAT_HOST");
    unsetenv("DM_NEXTENDO_NAT_DOMAINS");
    unsetenv("DM_NEXTENDO_API");
    unsetenv("DM_NEXTENDO_CLIENT_ID");
}

static void *delayed_send_thread(void *arg) {
    int fd = *(int *)arg;
    usleep(160000);
    const char b = 'X';
    (void)dm_sk_sendto(fd, &b, 1, 0, NULL, 0);
    return NULL;
}

static void test_poll_infinite(void) {
    int ls = dm_sk_socket(AF_INET, SOCK_STREAM, 0);
    uint8_t lb[16] = {16, AF_INET, 0, 0, 127, 0, 0, 1};
    CHECK(ls >= 0 && dm_sk_bind(ls, lb, 16) == 0 && dm_sk_listen(ls, 2) == 0, "poll(-1): listen");
    uint8_t bn[16] = {0}; unsigned bnl = 16;
    CHECK(dm_sk_name(ls, bn, &bnl, 0) == 0, "poll(-1): getsockname");
    int cs = dm_sk_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cs >= 0 && dm_sk_connect(cs, bn, 16) == 0, "poll(-1): connect");
    int as = dm_sk_accept(ls, NULL, NULL);
    CHECK(as >= 0, "poll(-1): accept");
    sk_pollfd pfd = {.fd = cs, .events = SK_POLLIN, .revents = 0};
    CHECK(dm_sk_poll(&pfd, 1, 40) == 0, "poll(40ms): returns 0 when quiet");
    uint64_t t0 = guest_ns();
    pthread_t th;
    pthread_create(&th, NULL, delayed_send_thread, &as);
    int pr = dm_sk_poll(&pfd, 1, -1);
    uint64_t elapsed_ms = (guest_ns() - t0) / 1000000ULL;
    pthread_join(th, NULL);
    CHECK(pr == 1 && (pfd.revents & SK_POLLIN) && elapsed_ms >= 140, "poll(-1): blocks past 100ms and wakes on data");
    dm_sk_close(as); dm_sk_close(cs); dm_sk_close(ls);
}

static void *delayed_efd_write_thread(void *arg) {
    int efd = *(int *)arg;
    usleep(60000);
    (void)efd_write(efd, 1);
    return NULL;
}

static void test_grpc_socket_patterns(void) {
    // 1. Multi-iov sendmsg / recvmsg (iov=2, matching gRPC HTTP/2 DATA+HEADERS writes)
    int ls = dm_sk_socket(AF_INET, SOCK_STREAM, 0);
    uint8_t lb[16] = {16, AF_INET, 0, 0, 127, 0, 0, 1};
    CHECK(ls >= 0 && dm_sk_bind(ls, lb, 16) == 0 && dm_sk_listen(ls, 2) == 0, "grpc: listen");
    uint8_t bn[16] = {0}; unsigned bnl = 16;
    CHECK(dm_sk_name(ls, bn, &bnl, 0) == 0, "grpc: getsockname");

    // Non-blocking connect + poll(POLLOUT) + getsockopt(SO_ERROR)
    int cs = dm_sk_socket(AF_INET, SOCK_STREAM | 0x20000000, 0);
    CHECK(cs >= 0, "grpc: SOCK_NONBLOCK socket");
    int crc = dm_sk_connect(cs, bn, 16);
    CHECK(crc == 0 || dm_sk_errno() == 115, "grpc: non-blocking connect returns 0 or Linux EINPROGRESS(115)");
    int as = dm_sk_accept(ls, NULL, NULL);
    CHECK(as >= 0, "grpc: accept");
    sk_pollfd pwr = {.fd = cs, .events = SK_POLLOUT, .revents = 0};
    CHECK(dm_sk_poll(&pwr, 1, 500) == 1 && (pwr.revents & SK_POLLOUT), "grpc: poll(POLLOUT) ready");
    int so_err = -1; unsigned so_len = sizeof so_err;
    CHECK(dm_sk_getopt(cs, 0xffff, 0x1007, &so_err, &so_len) == 0 && so_err == 0, "grpc: SO_ERROR == 0");

    // Multi-iov sendmsg (iov=2) -> multi-iov recvmsg (iov=2)
    char p1[] = "PRI * HTTP/2.0\r\n";
    char p2[] = "\r\nSM\r\n\r\n";
    struct iovec tx_iov[2] = {{p1, sizeof p1 - 1}, {p2, sizeof p2 - 1}};
    struct msghdr txm = {0};
    txm.msg_iov = tx_iov; txm.msg_iovlen = 2;
    long sent = dm_sk_sendmsg(cs, &txm, 0);
    CHECK(sent == (long)(sizeof p1 - 1 + sizeof p2 - 1), "grpc: sendmsg(iov=2) sends full 24B preface");

    char r1[16] = {0}, r2[16] = {0};
    struct iovec rx_iov[2] = {{r1, 16}, {r2, 8}};
    struct msghdr rxm = {0};
    rxm.msg_iov = rx_iov; rxm.msg_iovlen = 2;
    long got = dm_sk_recvmsg(as, &rxm, 0);
    CHECK(got == 24 && !memcmp(r1, p1, 16) && !memcmp(r2, p2, 8), "grpc: recvmsg(iov=2) scatters 24B cleanly");

    // 2. Combined EventFd + SK_HOST TCP in poll(-1): EventFdWrite from worker thread wakes poller
    int efd = efd_create(0, 0);
    CHECK(efd >= 0, "grpc: EventFd created");
    sk_pollfd both[2] = {
        {.fd = efd, .events = SK_POLLIN, .revents = 0},
        {.fd = cs,  .events = SK_POLLIN, .revents = 0},
    };
    pthread_t th;
    pthread_create(&th, NULL, delayed_efd_write_thread, &efd);
    int pr = dm_sk_poll(both, 2, -1);
    pthread_join(th, NULL);
    CHECK(pr == 1 && (both[0].revents & SK_POLLIN) && both[1].revents == 0, "grpc: EventFd wakes poll(-1) alongside quiet TCP fd");
    uint64_t ev = 0;
    CHECK(efd_read(efd, &ev) == 0 && ev == 1, "grpc: EventFd read drains counter");

    // 3. Non-blocking UDP recvfrom returns Linux EAGAIN (11), not Darwin 35; u32 SO_RCVTIMEO works
    int ud = dm_sk_socket(AF_INET, SOCK_DGRAM | 0x20000000, IPPROTO_UDP);
    CHECK(ud >= 0 && dm_sk_bind(ud, lb, 16) == 0, "grpc: non-blocking UDP bind");
    uint32_t rto_ms = 250, rto_out = 0; unsigned rto_sz = 4;
    CHECK(dm_sk_setopt(ud, 0xffff, 0x1006, &rto_ms, 4) == 0 && dm_sk_getopt(ud, 0xffff, 0x1006, &rto_out, &rto_sz) == 0 && rto_out == 250,
          "grpc: u32 ms SO_RCVTIMEO round-trip");
    char ub[16];
    CHECK(dm_sk_recvfrom(ud, ub, sizeof ub, SK_MSG_DONTWAIT, NULL, NULL) == -1 && dm_sk_errno() == 11,
          "grpc: non-blocking UDP recvfrom returns Linux EAGAIN(11)");

    // 4. Dual GetAddrInfo ABI layouts (posix=0 FreeBSD @32 vs posix=1 Linux @24)
    void *ai0 = NULL, *ai1 = NULL;
    CHECK(dm_sk_getaddrinfo_ex("dauth-lp1.ndas.srv.nintendo.net", "443", NULL, &ai0, 0) == 0 && ai0 != NULL, "abi: GetAddrInfo posix=0");
    CHECK(dm_sk_getaddrinfo_ex("nncs1-%.n.n.srv.nintendo.net", "", NULL, &ai1, 1) == 0 && ai1 != NULL, "abi: GetAddrInfo posix=1");
    dm_guest_addrinfo *g0 = ai0, *g1 = ai1;
    CHECK(((const uint8_t *)g0->ptr32)[1] == AF_INET && ((const uint8_t *)g0->ptr32)[4] == 127, "abi: posix=0 puts sockaddr at offset 32");
    CHECK(((const uint8_t *)g1->ptr24)[1] == AF_INET && ((const uint8_t *)g1->ptr24)[4] == 127, "abi: posix=1 puts sockaddr at offset 24");
    dm_sk_freeaddrinfo(ai0);
    dm_sk_freeaddrinfo(ai1);

    // 5. Per-PID unique Uid maps to slot 0 + PlayHistory key & AddPlayHistory succeed online
    UserHandle uh = {{0}};
    CHECK(h_acc_OpenPre(&uh) == RES_OK, "online: OpenPreselectedUser succeeds");
    CHECK(uh.v[0] == (DM_UID0 ^ UINT64_C(0x3ade68b1)) && uh.v[1] == DM_UID1, "online: Uid derived from Nextendo PID");
    Uid u_check = {{uh.v[0], uh.v[1]}};
    CHECK(dm_account_user(&u_check) == 0, "online: derived Uid still maps to save slot 0");
    h_acc_CloseUser(&uh);
    uint8_t phkey[64] = {0};
    CHECK(h_fr_GetPHKey(phkey, &u_check, 0) == RES_OK && memcmp(phkey, &(uint64_t){UINT64_C(0x3ade68b1)}, 8) == 0,
          "online: GetPlayHistoryRegistrationKey synthesizes PID-backed key");
    CHECK(h_fr_AddPlayHistory(phkey, NULL, NULL) == RES_OK, "online: AddPlayHistory succeeds");

    dm_sk_close(ud); dm_sk_close(efd); dm_sk_close(as); dm_sk_close(cs); dm_sk_close(ls);
}

int main(void) {
    test_pkce_and_oauth();
    test_nx2_token_and_baas_jwt();
    test_dns_and_sni();
    test_online_vs_offline_hle();
    printf("t_nextendo: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
