// Nextendo Network client core: configuration, OAuth 2.0 + PKCE, nx2. token parsing,
// local BAAS id_token JWT minting, DNS/hostname redirection, SNI tracking, and friends/presence cache.
#include "nextendo_client.h"
#include "dm.h"
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_nx_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_nx_loaded;
static dm_nextendo_config g_nx_cfg;
static char g_nx_cfg_path[1024];

// Session-stable random device identifiers for local BAAS id_token JWTs.
static int g_nx_ids_ready;
static char g_nx_sub[33], g_nx_di[33], g_nx_bsdid[33];
static char g_nx_jwt_cache[4096];
static int64_t g_nx_jwt_exp;

// DNS resolution cache (10-minute TTL) and IP -> SNI reverse map.
enum { NX_DNS_TTL = 600, NX_SNI_MAX = 32 };
static struct {
    char host[256];
    uint32_t ip;
    int64_t ts;
} g_nx_dns_server, g_nx_dns_nat;
static struct {
    uint32_t ip;
    char host[256];
} g_nx_sni[NX_SNI_MAX];
static unsigned g_nx_sni_next;

// Cached friend list (10-second TTL).
static dm_nextendo_friend g_nx_friends[DM_NEXTENDO_MAX_FRIENDS];
static int g_nx_friends_count;
static int64_t g_nx_friends_ts;

static const char k_default_baas[] = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";

int dm_nextendo_enabled(void) {
    const char *m = getenv("DM_ONLINE_MODE");
    if (m && strcasecmp(m, "nextendo") == 0) return 1;
    const char *e = getenv("DM_NEXTENDO");
    return e && atoi(e) != 0;
}

void dm_nextendo_reset(void) {
    pthread_mutex_lock(&g_nx_mu);
    g_nx_loaded = 0;
    memset(&g_nx_cfg, 0, sizeof g_nx_cfg);
    g_nx_cfg_path[0] = 0;
    g_nx_jwt_cache[0] = 0;
    g_nx_jwt_exp = 0;
    memset(&g_nx_dns_server, 0, sizeof g_nx_dns_server);
    memset(&g_nx_dns_nat, 0, sizeof g_nx_dns_nat);
    memset(g_nx_sni, 0, sizeof g_nx_sni);
    g_nx_sni_next = 0;
    g_nx_friends_count = 0;
    g_nx_friends_ts = 0;
    pthread_mutex_unlock(&g_nx_mu);
}

// ---------- Base64url (RFC 4648 §5, unpadded) ----------
int dm_nextendo_base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (!out || (!in && in_len)) return -1;
    size_t needed = (in_len / 3) * 4 + (in_len % 3 ? (in_len % 3) + 1 : 0);
    if (needed + 1 > out_cap) return -1;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i + 2];
        out[j++] = k_b64[(v >> 18) & 63];
        out[j++] = k_b64[(v >> 12) & 63];
        if (i + 1 < in_len) out[j++] = k_b64[(v >> 6) & 63];
        if (i + 2 < in_len) out[j++] = k_b64[v & 63];
    }
    out[j] = '\0';
    return (int)j;
}

static int b64url_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

int dm_nextendo_base64url_decode(const char *in, uint8_t *out, size_t out_cap) {
    if (!in || !out) return -1;
    size_t len = strlen(in);
    while (len > 0 && in[len - 1] == '=') len--;
    if ((len & 3) == 1) return -1;
    size_t out_len = (len / 4) * 3 + (len % 4 ? (len % 4) - 1 : 0);
    if (out_len > out_cap) return -1;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = b64url_val((unsigned char)in[i]);
        int b = b64url_val((unsigned char)in[i + 1]);
        int c = (i + 2 < len) ? b64url_val((unsigned char)in[i + 2]) : 0;
        int d = (i + 3 < len) ? b64url_val((unsigned char)in[i + 3]) : 0;
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        out[j++] = (uint8_t)(v >> 16);
        if (i + 2 < len) out[j++] = (uint8_t)(v >> 8);
        if (i + 3 < len) out[j++] = (uint8_t)v;
    }
    return (int)j;
}

// ---------- OAuth 2.0 + PKCE ----------
int dm_nextendo_pkce_generate(dm_nextendo_pkce *out) {
    if (!out) return -1;
    uint8_t raw_v[32], raw_s[16], digest[CC_SHA256_DIGEST_LENGTH];
    arc4random_buf(raw_v, sizeof raw_v);
    arc4random_buf(raw_s, sizeof raw_s);
    if (dm_nextendo_base64url_encode(raw_v, sizeof raw_v, out->verifier, sizeof out->verifier) < 0) return -1;
    CC_SHA256(out->verifier, (CC_LONG)strlen(out->verifier), digest);
    if (dm_nextendo_base64url_encode(digest, sizeof digest, out->challenge, sizeof out->challenge) < 0) return -1;
    if (dm_nextendo_base64url_encode(raw_s, sizeof raw_s, out->state, sizeof out->state) < 0) return -1;
    return 0;
}

static int url_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (j + 2 > cap) return -1;
            out[j++] = (char)*p;
        } else {
            if (j + 4 > cap) return -1;
            out[j++] = '%';
            out[j++] = hex[*p >> 4];
            out[j++] = hex[*p & 15];
        }
    }
    out[j] = '\0';
    return (int)j;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *in, size_t in_len, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 1 < cap; i++) {
        if (in[i] == '%' && i + 2 < in_len) {
            int hi = hex_val(in[i + 1]), lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[j++] = in[i] == '+' ? ' ' : in[i];
    }
    if (cap) out[j] = '\0';
}

// ---------- Configuration & Token Persistence ----------
static void resolve_config_path_locked(char *out, size_t cap) {
    const char *env = getenv("DM_NEXTENDO_CONFIG");
    if (env && *env) {
        strlcpy(out, env, cap);
        return;
    }
    const char *save = getenv("DM_SAVE");
    if (save && *save) {
        char tmp[1024];
        strlcpy(tmp, save, sizeof tmp);
        size_t n = strlen(tmp);
        while (n > 1 && tmp[n - 1] == '/') tmp[--n] = '\0';
        char *slash = strrchr(tmp, '/');
        if (slash && slash != tmp) {
            *slash = '\0';
            snprintf(out, cap, "%s/nextendo.conf", tmp);
            return;
        }
    }
    const char *home = getenv("HOME");
    if (home && *home) snprintf(out, cap, "%s/Library/Application Support/nxnative/nextendo.conf", home);
    else out[0] = '\0';
}

static void apply_kv_locked(const char *k, const char *v) {
    if (!strcmp(k, "api_url")) strlcpy(g_nx_cfg.api_url, v, sizeof g_nx_cfg.api_url);
    else if (!strcmp(k, "client_id")) strlcpy(g_nx_cfg.client_id, v, sizeof g_nx_cfg.client_id);
    else if (!strcmp(k, "server_host")) strlcpy(g_nx_cfg.server_host, v, sizeof g_nx_cfg.server_host);
    else if (!strcmp(k, "nat_host")) strlcpy(g_nx_cfg.nat_host, v, sizeof g_nx_cfg.nat_host);
    else if (!strcmp(k, "nat_domains")) strlcpy(g_nx_cfg.nat_domains, v, sizeof g_nx_cfg.nat_domains);
    else if (!strcmp(k, "baas_host")) strlcpy(g_nx_cfg.baas_host, v, sizeof g_nx_cfg.baas_host);
    else if (!strcmp(k, "baas_key_path")) strlcpy(g_nx_cfg.baas_key_path, v, sizeof g_nx_cfg.baas_key_path);
    else if (!strcmp(k, "nex_token")) strlcpy(g_nx_cfg.nex_token, v, sizeof g_nx_cfg.nex_token);
    else if (!strcmp(k, "access_token")) strlcpy(g_nx_cfg.access_token, v, sizeof g_nx_cfg.access_token);
    else if (!strcmp(k, "refresh_token")) strlcpy(g_nx_cfg.refresh_token, v, sizeof g_nx_cfg.refresh_token);
    else if (!strcmp(k, "username")) strlcpy(g_nx_cfg.username, v, sizeof g_nx_cfg.username);
    else if (!strcmp(k, "country")) strlcpy(g_nx_cfg.country, v, sizeof g_nx_cfg.country);
    else if (!strcmp(k, "pid")) g_nx_cfg.pid = strtoull(v, NULL, 0);
    else if (!strcmp(k, "allow_passthrough")) g_nx_cfg.allow_passthrough = atoi(v) != 0;
}

static void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') s[--n] = '\0';
}

static void load_config_locked(void) {
    char path[1024];
    resolve_config_path_locked(path, sizeof path);
    if (g_nx_loaded && !strcmp(path, g_nx_cfg_path)) return;
    memset(&g_nx_cfg, 0, sizeof g_nx_cfg);
    strlcpy(g_nx_cfg.baas_host, k_default_baas, sizeof g_nx_cfg.baas_host);
    strlcpy(g_nx_cfg.country, "US", sizeof g_nx_cfg.country);
    strlcpy(g_nx_cfg_path, path, sizeof g_nx_cfg_path);
    g_nx_loaded = 1;

    if (path[0]) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (!*p || *p == '#') continue;
                char *eq = strchr(p, '=');
                if (!eq) continue;
                *eq = '\0';
                char *val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                char *kend = eq;
                while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = '\0';
                apply_kv_locked(p, val);
            }
            fclose(f);
        }
    }

    // Environment variables always override config file entries.
    const char *e;
    if ((e = getenv("DM_NEXTENDO_API")) && *e) strlcpy(g_nx_cfg.api_url, e, sizeof g_nx_cfg.api_url);
    if ((e = getenv("DM_NEXTENDO_CLIENT_ID")) && *e) strlcpy(g_nx_cfg.client_id, e, sizeof g_nx_cfg.client_id);
    if ((e = getenv("DM_NEXTENDO_SERVER_HOST")) && *e) strlcpy(g_nx_cfg.server_host, e, sizeof g_nx_cfg.server_host);
    if ((e = getenv("DM_NEXTENDO_NAT_HOST")) && *e) strlcpy(g_nx_cfg.nat_host, e, sizeof g_nx_cfg.nat_host);
    if ((e = getenv("DM_NEXTENDO_NAT_DOMAINS")) && *e) strlcpy(g_nx_cfg.nat_domains, e, sizeof g_nx_cfg.nat_domains);
    if ((e = getenv("DM_NEXTENDO_BAAS_HOST")) && *e) strlcpy(g_nx_cfg.baas_host, e, sizeof g_nx_cfg.baas_host);
    if ((e = getenv("DM_NEXTENDO_BAAS_KEY")) && *e) strlcpy(g_nx_cfg.baas_key_path, e, sizeof g_nx_cfg.baas_key_path);
    if ((e = getenv("DM_NEXTENDO_TOKEN")) && *e) strlcpy(g_nx_cfg.nex_token, e, sizeof g_nx_cfg.nex_token);
    if ((e = getenv("DM_NEXTENDO_ACCESS_TOKEN")) && *e) strlcpy(g_nx_cfg.access_token, e, sizeof g_nx_cfg.access_token);
    if ((e = getenv("DM_NEXTENDO_REFRESH_TOKEN")) && *e) strlcpy(g_nx_cfg.refresh_token, e, sizeof g_nx_cfg.refresh_token);
    if (!g_nx_cfg.baas_host[0]) strlcpy(g_nx_cfg.baas_host, k_default_baas, sizeof g_nx_cfg.baas_host);
    // Derive nex.<api_host> and nat.<api_host> automatically when API URL is a non-loopback domain.
    if (g_nx_cfg.api_url[0] && (!g_nx_cfg.server_host[0] || !g_nx_cfg.nat_host[0])) {
        char api_host[128] = {0};
        const char *p = strstr(g_nx_cfg.api_url, "://");
        p = p ? p + 3 : g_nx_cfg.api_url;
        size_t i = 0;
        while (p[i] && p[i] != '/' && p[i] != ':' && i + 1 < sizeof api_host) { api_host[i] = p[i]; i++; }
        api_host[i] = '\0';
        int is_local = (!api_host[0] || strcmp(api_host, "localhost") == 0 || strcmp(api_host, "127.0.0.1") == 0);
        if (!g_nx_cfg.server_host[0] && api_host[0]) {
            if (is_local) strlcpy(g_nx_cfg.server_host, api_host, sizeof g_nx_cfg.server_host);
            else snprintf(g_nx_cfg.server_host, sizeof g_nx_cfg.server_host, "nex.%s", api_host);
        }
        if (!g_nx_cfg.nat_host[0] && api_host[0] && !is_local) {
            snprintf(g_nx_cfg.nat_host, sizeof g_nx_cfg.nat_host, "nat.%s", api_host);
        }
    }
    if ((e = getenv("DM_NEXTENDO_ALLOW_PASSTHROUGH")) && *e) g_nx_cfg.allow_passthrough = atoi(e) != 0;

    strip_trailing_slash(g_nx_cfg.api_url);

    if (g_nx_cfg.nex_token[0]) {
        uint64_t pid = 0;
        char user[64] = "";
        int64_t exp = 0;
        if (dm_nextendo_parse_nx2_token(g_nx_cfg.nex_token, &pid, user, sizeof user, &exp) == 0) {
            g_nx_cfg.pid = pid;
            if (user[0]) strlcpy(g_nx_cfg.username, user, sizeof g_nx_cfg.username);
            g_nx_cfg.token_expiry = exp;
        }
    }
}

void dm_nextendo_get_config(dm_nextendo_config *out) {
    if (!out) return;
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    *out = g_nx_cfg;
    pthread_mutex_unlock(&g_nx_mu);
}

static void mkdir_parents(const char *path) {
    char tmp[1024];
    strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
}

int dm_nextendo_save_config(void) {
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    if (!g_nx_cfg_path[0]) {
        pthread_mutex_unlock(&g_nx_mu);
        return -1;
    }
    mkdir_parents(g_nx_cfg_path);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", g_nx_cfg_path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        pthread_mutex_unlock(&g_nx_mu);
        return -1;
    }
    fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        pthread_mutex_unlock(&g_nx_mu);
        return -1;
    }
    if (g_nx_cfg.api_url[0]) fprintf(f, "api_url=%s\n", g_nx_cfg.api_url);
    if (g_nx_cfg.client_id[0]) fprintf(f, "client_id=%s\n", g_nx_cfg.client_id);
    if (g_nx_cfg.server_host[0]) fprintf(f, "server_host=%s\n", g_nx_cfg.server_host);
    if (g_nx_cfg.nat_host[0]) fprintf(f, "nat_host=%s\n", g_nx_cfg.nat_host);
    if (g_nx_cfg.nat_domains[0]) fprintf(f, "nat_domains=%s\n", g_nx_cfg.nat_domains);
    if (g_nx_cfg.baas_host[0]) fprintf(f, "baas_host=%s\n", g_nx_cfg.baas_host);
    if (g_nx_cfg.baas_key_path[0]) fprintf(f, "baas_key_path=%s\n", g_nx_cfg.baas_key_path);
    if (g_nx_cfg.nex_token[0]) fprintf(f, "nex_token=%s\n", g_nx_cfg.nex_token);
    if (g_nx_cfg.access_token[0]) fprintf(f, "access_token=%s\n", g_nx_cfg.access_token);
    if (g_nx_cfg.refresh_token[0]) fprintf(f, "refresh_token=%s\n", g_nx_cfg.refresh_token);
    if (g_nx_cfg.username[0]) fprintf(f, "username=%s\n", g_nx_cfg.username);
    if (g_nx_cfg.country[0]) fprintf(f, "country=%s\n", g_nx_cfg.country);
    if (g_nx_cfg.pid) fprintf(f, "pid=%llu\n", (unsigned long long)g_nx_cfg.pid);
    if (g_nx_cfg.allow_passthrough) fprintf(f, "allow_passthrough=1\n");
    int err = ferror(f) || fflush(f) || fsync(fileno(f));
    if (fclose(f) || err || rename(tmp, g_nx_cfg_path)) {
        unlink(tmp);
        pthread_mutex_unlock(&g_nx_mu);
        return -1;
    }
    pthread_mutex_unlock(&g_nx_mu);
    return 0;
}

int dm_nextendo_build_authorize_url(const dm_nextendo_pkce *pkce, const char *redirect_uri, char *out, size_t cap) {
    if (!pkce || !redirect_uri || !out || !cap) return -1;
    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    if (!cfg.api_url[0] || !cfg.client_id[0]) return -1;
    char enc_cid[384], enc_redir[512], enc_state[192], enc_chal[192];
    if (url_encode(cfg.client_id, enc_cid, sizeof enc_cid) < 0 ||
        url_encode(redirect_uri, enc_redir, sizeof enc_redir) < 0 ||
        url_encode(pkce->state, enc_state, sizeof enc_state) < 0 ||
        url_encode(pkce->challenge, enc_chal, sizeof enc_chal) < 0)
        return -1;
    int n = snprintf(out, cap,
                     "%s/oauth/authorize?response_type=code&client_id=%s&redirect_uri=%s"
                     "&scope=identity+friends+game.matchmaking+presence&state=%s"
                     "&code_challenge=%s&code_challenge_method=S256",
                     cfg.api_url, enc_cid, enc_redir, enc_state, enc_chal);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int const_time_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

int dm_nextendo_parse_callback_query(const char *query, const char *expected_state, char *code_out, size_t code_cap) {
    if (!query || !expected_state || !code_out || !code_cap) return -1;
    if (*query == '?') query++;
    char state[128] = "", code[512] = "";
    int has_error = 0;
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg);
        if (eq) {
            size_t klen = (size_t)(eq - p);
            const char *v = eq + 1;
            size_t vlen = seg - klen - 1;
            if (klen == 4 && !memcmp(p, "code", 4)) url_decode(v, vlen, code, sizeof code);
            else if (klen == 5 && !memcmp(p, "state", 5)) url_decode(v, vlen, state, sizeof state);
            else if (klen == 5 && !memcmp(p, "error", 5)) has_error = 1;
        }
        p = amp ? amp + 1 : p + seg;
    }
    if (has_error || !code[0] || !const_time_eq(state, expected_state)) return -1;
    if (strlen(code) + 1 > code_cap) return -1;
    strlcpy(code_out, code, code_cap);
    return 0;
}

// ---------- nx2. Token Parsing ----------
// Format: nx2.<base64url(pid.username.unix_expiry)>.<base64url(hmac_sha256)>
int dm_nextendo_parse_nx2_token(const char *token, uint64_t *pid_out, char *user_out, size_t user_cap, int64_t *exp_out) {
    if (!token || strncmp(token, "nx2.", 4) != 0) return -1;
    const char *p1 = token + 4;
    const char *dot = strchr(p1, '.');
    if (!dot || dot == p1) return -1;
    const char *sig = dot + 1;
    if (!*sig || strchr(sig, '.')) return -1;   // must have exactly 3 dot-separated parts

    char payload_b64[384];
    size_t plen = (size_t)(dot - p1);
    if (plen >= sizeof payload_b64) return -1;
    memcpy(payload_b64, p1, plen);
    payload_b64[plen] = '\0';

    uint8_t decoded[300];
    int dlen = dm_nextendo_base64url_decode(payload_b64, decoded, sizeof decoded - 1);
    if (dlen <= 0) return -1;
    decoded[dlen] = '\0';

    // Split on first '.' and last '.' (usernames may contain dots).
    char *first = strchr((char *)decoded, '.');
    char *last = strrchr((char *)decoded, '.');
    if (!first || !last || first == last || first == (char *)decoded || !last[1]) return -1;

    *first = '\0';
    *last = '\0';
    char *end = NULL;
    uint64_t pid = strtoull((const char *)decoded, &end, 10);
    if (!end || *end != '\0' || pid == 0) return -1;
    int64_t exp = strtoll(last + 1, &end, 10);
    if (!end || *end != '\0' || exp <= 0) return -1;

    if (pid_out) *pid_out = pid;
    if (user_out && user_cap) strlcpy(user_out, first + 1, user_cap);
    if (exp_out) *exp_out = exp;
    return 0;
}

int dm_nextendo_set_tokens(const char *nex_token, const char *access_token, const char *refresh_token) {
    uint64_t pid = 0;
    char user[64] = "";
    int64_t exp = 0;
    if (nex_token && *nex_token && dm_nextendo_parse_nx2_token(nex_token, &pid, user, sizeof user, &exp) != 0)
        return -1;
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    if (nex_token && *nex_token) {
        strlcpy(g_nx_cfg.nex_token, nex_token, sizeof g_nx_cfg.nex_token);
        g_nx_cfg.pid = pid;
        if (user[0]) strlcpy(g_nx_cfg.username, user, sizeof g_nx_cfg.username);
        g_nx_cfg.token_expiry = exp;
        g_nx_jwt_cache[0] = '\0';
        g_nx_jwt_exp = 0;
    }
    if (access_token && *access_token) strlcpy(g_nx_cfg.access_token, access_token, sizeof g_nx_cfg.access_token);
    if (refresh_token && *refresh_token) strlcpy(g_nx_cfg.refresh_token, refresh_token, sizeof g_nx_cfg.refresh_token);
    pthread_mutex_unlock(&g_nx_mu);
    return 0;
}

int dm_nextendo_is_linked(void) {
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    int linked = g_nx_cfg.pid != 0 && g_nx_cfg.nex_token[0] != '\0';
    pthread_mutex_unlock(&g_nx_mu);
    return linked;
}

uint64_t dm_nextendo_get_pid(void) {
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    uint64_t pid = g_nx_cfg.pid;
    pthread_mutex_unlock(&g_nx_mu);
    return pid;
}

void dm_nextendo_get_username(char *out, size_t cap) {
    if (!out || !cap) return;
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    strlcpy(out, g_nx_cfg.username[0] ? g_nx_cfg.username : "Player", cap);
    pthread_mutex_unlock(&g_nx_mu);
}

void dm_nextendo_get_country(char *out, size_t cap) {
    if (!out || !cap) return;
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    strlcpy(out, g_nx_cfg.country[0] ? g_nx_cfg.country : "US", cap);
    pthread_mutex_unlock(&g_nx_mu);
}

int dm_nextendo_ensure_login(void) {
    if (dm_nextendo_is_linked()) return 0;
    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    if (!cfg.api_url[0] || !cfg.client_id[0]) {
        LOG("nextendo: not linked and DM_NEXTENDO_API / DM_NEXTENDO_CLIENT_ID not configured");
        return -1;
    }
    char nex[512] = "", acc[512] = "", ref[512] = "";
    if (dm_nextendo_platform_sign_in(&cfg, nex, sizeof nex, acc, sizeof acc, ref, sizeof ref) != 0) return -1;
    if (dm_nextendo_set_tokens(nex, acc, ref) != 0) return -1;
    (void)dm_nextendo_save_config();
    return 0;
}

// ---------- Local BAAS id_token JWT Minting ----------
static void rand_hex(char *out, size_t nbytes) {
    static const char h[] = "0123456789abcdef";
    uint8_t buf[32];
    if (nbytes > sizeof buf) nbytes = sizeof buf;
    arc4random_buf(buf, nbytes);
    for (size_t i = 0; i < nbytes; i++) {
        out[2 * i] = h[buf[i] >> 4];
        out[2 * i + 1] = h[buf[i] & 15];
    }
    out[2 * nbytes] = '\0';
}

static void rand_uuid(char out[37]) {
    char raw[33];
    rand_hex(raw, 16);
    snprintf(out, 37, "%.8s-%.4s-4%.3s-a%.3s-%.12s", raw, raw + 8, raw + 12, raw + 16, raw + 20);
}

int dm_nextendo_mint_baas_id_token(char *out, size_t cap, size_t *len_out) {
    if (!out || !cap) return -1;
    pthread_mutex_lock(&g_nx_mu);
    load_config_locked();
    if (!g_nx_cfg.nex_token[0] || !g_nx_cfg.pid) {
        pthread_mutex_unlock(&g_nx_mu);
        return -1;
    }
    int64_t now = (int64_t)time(NULL);
    if (g_nx_jwt_cache[0] && g_nx_jwt_exp - now > 600) {
        size_t len = strlen(g_nx_jwt_cache);
        if (len + 1 > cap) {
            pthread_mutex_unlock(&g_nx_mu);
            return -1;
        }
        memcpy(out, g_nx_jwt_cache, len + 1);
        if (len_out) *len_out = len;
        pthread_mutex_unlock(&g_nx_mu);
        return 0;
    }
    if (!g_nx_ids_ready) {
        rand_hex(g_nx_sub, 16);
        rand_hex(g_nx_di, 16);
        rand_hex(g_nx_bsdid, 16);
        g_nx_ids_ready = 1;
    }
    dm_nextendo_config cfg = g_nx_cfg;
    char sub[33], di[33], bsdid[33];
    strlcpy(sub, g_nx_sub, sizeof sub);
    strlcpy(di, g_nx_di, sizeof di);
    strlcpy(bsdid, g_nx_bsdid, sizeof bsdid);
    pthread_mutex_unlock(&g_nx_mu);

    const char *hdr = "{\"alg\":\"RS256\",\"kid\":\"nextendo-baas-key-1\",\"typ\":\"id_token\"}";
    char jti[37];
    rand_uuid(jti);
    const char *baas = cfg.baas_host[0] ? cfg.baas_host : k_default_baas;
    const char *tv = "1.0.0";
#ifdef DM_GAME_DISPLAY_VERSIONS
    static const struct { const char *build, *ver; } k_vers[] = {DM_GAME_DISPLAY_VERSIONS, {NULL, NULL}};
    if (k_vers[0].ver && k_vers[0].ver[0]) tv = k_vers[0].ver;
#endif
    int64_t exp = now + 10800;
    char payload[2048];
    int pn = snprintf(payload, sizeof payload,
                      "{\"sub\":\"%s\",\"aud\":\"ed9e2f05d286f7b8\",\"iss\":\"https://%s\","
                      "\"jku\":\"https://%s/1.0.0/certificates\",\"iat\":%lld,\"nbf\":%lld,\"exp\":%lld,"
                      "\"jti\":\"%s\",\"di\":\"%s\",\"bs:did\":\"%s\",\"sn\":\"XAW10000000000\","
                      "\"hm\":true,\"tv\":\"%s\",\"app_id\":\"%s\",\"nnex\":\"%s\"}",
                      sub, baas, baas, (long long)now, (long long)now, (long long)exp,
                      jti, di, bsdid, tv, DM_GAME_ID, cfg.nex_token);
    if (pn <= 0 || (size_t)pn >= sizeof payload) return -1;

    char b64_hdr[128], b64_pay[3072];
    if (dm_nextendo_base64url_encode((const uint8_t *)hdr, strlen(hdr), b64_hdr, sizeof b64_hdr) < 0 ||
        dm_nextendo_base64url_encode((const uint8_t *)payload, (size_t)pn, b64_pay, sizeof b64_pay) < 0)
        return -1;

    char signing_input[3300];
    int sn = snprintf(signing_input, sizeof signing_input, "%s.%s", b64_hdr, b64_pay);
    if (sn <= 0 || (size_t)sn >= sizeof signing_input) return -1;

    uint8_t sig_raw[512];
    size_t sig_len = sizeof sig_raw;
    if (!cfg.baas_key_path[0] ||
        dm_nextendo_platform_sign_rs256(cfg.baas_key_path, (const uint8_t *)signing_input, (size_t)sn, sig_raw, &sig_len) != 0) {
        sig_len = 256;
        arc4random_buf(sig_raw, sig_len);
    }
    char b64_sig[700];
    if (dm_nextendo_base64url_encode(sig_raw, sig_len, b64_sig, sizeof b64_sig) < 0) return -1;

    char jwt[4096];
    int jn = snprintf(jwt, sizeof jwt, "%s.%s", signing_input, b64_sig);
    if (jn <= 0 || (size_t)jn >= sizeof jwt || (size_t)jn + 1 > cap) return -1;

    pthread_mutex_lock(&g_nx_mu);
    strlcpy(g_nx_jwt_cache, jwt, sizeof g_nx_jwt_cache);
    g_nx_jwt_exp = exp;
    pthread_mutex_unlock(&g_nx_mu);

    memcpy(out, jwt, (size_t)jn + 1);
    if (len_out) *len_out = (size_t)jn;
    return 0;
}

// ---------- NSD & DNS Redirection ----------
int dm_nextendo_nsd_resolve(const char *in, char *out, size_t cap) {
    if (!in || !out || !cap) return -1;
    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    const char *baas = cfg.baas_host[0] ? cfg.baas_host : k_default_baas;

    // Substitute '%' with "lp1" (retail environment identifier).
    char tmp[256];
    size_t j = 0;
    for (const char *p = in; *p && j + 1 < sizeof tmp; p++) {
        if (*p == '%') {
            if (j + 3 >= sizeof tmp) return -1;
            memcpy(tmp + j, "lp1", 3);
            j += 3;
        } else {
            tmp[j++] = *p;
        }
    }
    tmp[j] = '\0';

    if (!strcasecmp(tmp, "accounts.nintendo.com") || !strcasecmp(tmp, "api.accounts.nintendo.com")) {
        if (strlen(baas) + 1 > cap) return -1;
        strlcpy(out, baas, cap);
        return 0;
    }
    if (j + 1 > cap) return -1;
    memcpy(out, tmp, j + 1);
    return 0;
}

static int wildcard_match(const char *pat, const char *str) {
    const char *star = NULL, *ss = NULL;
    while (*str) {
        if (*pat == '*') {
            star = pat++;
            ss = str;
        } else if (tolower((unsigned char)*pat) == tolower((unsigned char)*str)) {
            pat++;
            str++;
        } else if (star) {
            pat = star + 1;
            str = ++ss;
        } else {
            return 0;
        }
    }
    while (*pat == '*') pat++;
    return *pat == '\0';
}

static int has_suffix_ci(const char *host, const char *suffix) {
    size_t hl = strlen(host), sl = strlen(suffix);
    if (hl < sl) return 0;
    return strcasecmp(host + hl - sl, suffix) == 0;
}

static int is_ipv4_literal(const char *host, uint32_t *out_host_order) {
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) != 1) return 0;
    if (out_host_order) *out_host_order = ntohl(a.s_addr);
    return 1;
}

dm_nextendo_host_kind dm_nextendo_classify_host(const char *host) {
    if (!host || !*host) return DM_NX_HOST_BLOCKED;
    if (is_ipv4_literal(host, NULL)) return DM_NX_HOST_LITERAL_IP;

    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);

    // Built-in NAT-check domain pattern + optional user-configured NAT domains (comma-separated).
    if (wildcard_match("nncs2-*.n.n.srv.nintendo.net", host)) return DM_NX_HOST_NAT;
    if (cfg.nat_domains[0]) {
        const char *p = cfg.nat_domains;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
            while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
            if (len > 0 && len < 256) {
                char pat[256];
                memcpy(pat, p, len);
                pat[len] = '\0';
                if (wildcard_match(pat, host)) return DM_NX_HOST_NAT;
            }
            p = comma ? comma + 1 : p + len;
        }
    }

    if (has_suffix_ci(host, ".nintendo.net") || !strcasecmp(host, "nintendo.net") ||
        has_suffix_ci(host, ".nintendo.com") || !strcasecmp(host, "nintendo.com") ||
        has_suffix_ci(host, ".nintendowifi.net") || !strcasecmp(host, "nintendowifi.net") ||
        has_suffix_ci(host, ".nintendo.co.jp") || !strcasecmp(host, "nintendo.co.jp"))
        return DM_NX_HOST_SERVER;

    return cfg.allow_passthrough ? DM_NX_HOST_PASSTHROUGH : DM_NX_HOST_BLOCKED;
}

static int resolve_ipv4_system(const char *target, uint32_t *out_host_order) {
    if (!target || !*target) return -1;
    if (is_ipv4_literal(target, out_host_order)) return 0;
    struct addrinfo hint = {0}, *res = NULL;
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(target, NULL, &hint, &res) != 0 || !res) return -1;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)res->ai_addr;
    *out_host_order = ntohl(sin->sin_addr.s_addr);
    freeaddrinfo(res);
    return 0;
}

static int resolve_cached_target(const char *target, int is_nat, uint32_t *out_host_order) {
    if (!target || !*target) return -1;
    if (is_ipv4_literal(target, out_host_order)) return 0;
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_nx_mu);
    if (is_nat ? (!strcmp(g_nx_dns_nat.host, target) && g_nx_dns_nat.ip && now - g_nx_dns_nat.ts < NX_DNS_TTL)
               : (!strcmp(g_nx_dns_server.host, target) && g_nx_dns_server.ip && now - g_nx_dns_server.ts < NX_DNS_TTL)) {
        *out_host_order = is_nat ? g_nx_dns_nat.ip : g_nx_dns_server.ip;
        pthread_mutex_unlock(&g_nx_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_nx_mu);

    uint32_t ip = 0;
    if (resolve_ipv4_system(target, &ip) != 0) return -1;
    pthread_mutex_lock(&g_nx_mu);
    if (is_nat) {
        strlcpy(g_nx_dns_nat.host, target, sizeof g_nx_dns_nat.host);
        g_nx_dns_nat.ip = ip;
        g_nx_dns_nat.ts = now;
    } else {
        strlcpy(g_nx_dns_server.host, target, sizeof g_nx_dns_server.host);
        g_nx_dns_server.ip = ip;
        g_nx_dns_server.ts = now;
    }
    pthread_mutex_unlock(&g_nx_mu);
    *out_host_order = ip;
    return 0;
}

static void record_sni(uint32_t ip, const char *host) {
    if (!ip || !host || !*host) return;
    pthread_mutex_lock(&g_nx_mu);
    for (unsigned i = 0; i < NX_SNI_MAX; i++) {
        if (g_nx_sni[i].ip == ip) {
            strlcpy(g_nx_sni[i].host, host, sizeof g_nx_sni[i].host);
            pthread_mutex_unlock(&g_nx_mu);
            return;
        }
    }
    unsigned slot = g_nx_sni_next++ % NX_SNI_MAX;
    g_nx_sni[slot].ip = ip;
    strlcpy(g_nx_sni[slot].host, host, sizeof g_nx_sni[slot].host);
    pthread_mutex_unlock(&g_nx_mu);
}

int dm_nextendo_lookup_sni(uint32_t ipv4_host_order, char *out, size_t cap) {
    if (!ipv4_host_order || !out || !cap) return -1;
    pthread_mutex_lock(&g_nx_mu);
    for (unsigned i = 0; i < NX_SNI_MAX; i++) {
        if (g_nx_sni[i].ip == ipv4_host_order && g_nx_sni[i].host[0]) {
            strlcpy(out, g_nx_sni[i].host, cap);
            pthread_mutex_unlock(&g_nx_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_nx_mu);
    return -1;
}

int dm_nextendo_resolve_host(const char *host, uint32_t *ipv4_host_order, char *sni_out, size_t sni_cap) {
    if (!host || !*host || !ipv4_host_order) return -1;
    char resolved[256];
    if (dm_nextendo_nsd_resolve(host, resolved, sizeof resolved) != 0) strlcpy(resolved, host, sizeof resolved);

    dm_nextendo_host_kind kind = dm_nextendo_classify_host(resolved);
    if (kind == DM_NX_HOST_BLOCKED) return -1;
    if (kind == DM_NX_HOST_LITERAL_IP) {
        if (!is_ipv4_literal(resolved, ipv4_host_order)) return -1;
        if (sni_out && sni_cap) sni_out[0] = '\0';
        return 0;
    }

    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    uint32_t ip = 0;
    if (kind == DM_NX_HOST_NAT) {
        const char *target = cfg.nat_host[0] ? cfg.nat_host : cfg.server_host;
        if (resolve_cached_target(target, 1, &ip) != 0) return -1;
    } else if (kind == DM_NX_HOST_SERVER) {
        if (resolve_cached_target(cfg.server_host, 0, &ip) != 0) return -1;
    } else {
        if (resolve_ipv4_system(resolved, &ip) != 0) return -1;
    }

    record_sni(ip, resolved);
    *ipv4_host_order = ip;
    if (sni_out && sni_cap) strlcpy(sni_out, resolved, sni_cap);
    return 0;
}

int dm_nextendo_is_allowed_tls_peer(uint32_t ipv4_host_order) {
    if (!ipv4_host_order) return 0;
    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    if (cfg.allow_passthrough) return 1;
    uint32_t srv = 0, nat = 0;
    if (cfg.server_host[0] && resolve_cached_target(cfg.server_host, 0, &srv) == 0 && srv == ipv4_host_order) return 1;
    if (cfg.nat_host[0] && resolve_cached_target(cfg.nat_host, 1, &nat) == 0 && nat == ipv4_host_order) return 1;
    return 0;
}

int dm_nextendo_host_prefers_h2(const char *sni) {
    if (!sni || !*sni) return 0;
    if (strstr(sni, "npln") != NULL) return 1;
    if (has_suffix_ci(sni, "gs.nintendo.net")) return 1;
    return 0;
}

// ---------- Friends, Presence, and BCAT ----------
void dm_nextendo_set_friends_cache(const dm_nextendo_friend *friends, int count) {
    if (count < 0) count = 0;
    if (count > DM_NEXTENDO_MAX_FRIENDS) count = DM_NEXTENDO_MAX_FRIENDS;
    pthread_mutex_lock(&g_nx_mu);
    if (friends && count > 0) memcpy(g_nx_friends, friends, (size_t)count * sizeof *friends);
    g_nx_friends_count = count;
    g_nx_friends_ts = (int64_t)time(NULL);
    pthread_mutex_unlock(&g_nx_mu);
}

int dm_nextendo_refresh_friends(void) {
    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    if (!cfg.api_url[0] || (!cfg.access_token[0] && !cfg.nex_token[0])) return -1;
    dm_nextendo_friend tmp[DM_NEXTENDO_MAX_FRIENDS];
    int n = dm_nextendo_platform_fetch_friends(&cfg, tmp, DM_NEXTENDO_MAX_FRIENDS);
    if (n < 0) return -1;
    dm_nextendo_set_friends_cache(tmp, n);
    return n;
}

int dm_nextendo_get_friends(dm_nextendo_friend *out, int max) {
    if (!out || max <= 0) return 0;
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_nx_mu);
    int stale = (g_nx_friends_ts == 0) || (now - g_nx_friends_ts > 10);
    pthread_mutex_unlock(&g_nx_mu);
    if (stale) (void)dm_nextendo_refresh_friends();

    pthread_mutex_lock(&g_nx_mu);
    int n = g_nx_friends_count < max ? g_nx_friends_count : max;
    if (n > 0) memcpy(out, g_nx_friends, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&g_nx_mu);
    return n;
}

int dm_nextendo_update_presence(int status, const uint8_t *app_field, size_t app_field_len, const char *detail) {
    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    if (!cfg.api_url[0] || (!cfg.access_token[0] && !cfg.nex_token[0])) return -1;
    if (app_field_len > DM_NEXTENDO_APP_FIELD_SIZE) app_field_len = DM_NEXTENDO_APP_FIELD_SIZE;
    while (app_field && app_field_len > 0 && app_field[app_field_len - 1] == 0) app_field_len--;
    char b64[300] = "";
    if (app_field && app_field_len > 0) {
        if (dm_nextendo_base64url_encode(app_field, app_field_len, b64, sizeof b64) < 0) b64[0] = '\0';
    }
    return dm_nextendo_platform_post_presence(&cfg, status, DM_GAME_ID, b64, detail ? detail : "");
}

int dm_nextendo_sync_bcat(const char *title_id, const char *dest_dir) {
    if (!title_id || !*title_id || !dest_dir || !*dest_dir) return -1;
    dm_nextendo_config cfg;
    dm_nextendo_get_config(&cfg);
    if (!cfg.api_url[0]) return -1;
    return dm_nextendo_platform_sync_bcat(&cfg, title_id, dest_dir);
}

// ---------- Weak Platform Hooks (overridden by nextendo_apple.m in the host) ----------
__attribute__((weak)) int dm_nextendo_platform_sign_in(const dm_nextendo_config *cfg, char *nex_out, size_t nex_cap,
                                                       char *access_out, size_t access_cap, char *refresh_out, size_t refresh_cap) {
    (void)cfg; (void)nex_out; (void)nex_cap; (void)access_out; (void)access_cap; (void)refresh_out; (void)refresh_cap;
    return -1;
}
__attribute__((weak)) int dm_nextendo_platform_fetch_friends(const dm_nextendo_config *cfg, dm_nextendo_friend *out, int max) {
    (void)cfg; (void)out; (void)max; return -1;
}
__attribute__((weak)) int dm_nextendo_platform_post_presence(const dm_nextendo_config *cfg, int status, const char *app_id,
                                                             const char *app_field_b64, const char *detail) {
    (void)cfg; (void)status; (void)app_id; (void)app_field_b64; (void)detail; return -1;
}
__attribute__((weak)) int dm_nextendo_platform_sync_bcat(const dm_nextendo_config *cfg, const char *title_id, const char *dest_dir) {
    (void)cfg; (void)title_id; (void)dest_dir; return -1;
}
__attribute__((weak)) int dm_nextendo_platform_sign_rs256(const char *pem_path, const uint8_t *msg, size_t msg_len,
                                                          uint8_t *sig_out, size_t *sig_len) {
    (void)pem_path; (void)msg; (void)msg_len; (void)sig_out; (void)sig_len; return -1;
}
__attribute__((weak)) dm_ssl_conn *dm_nextendo_ssl_create(void) { return NULL; }
__attribute__((weak)) void dm_nextendo_ssl_destroy(dm_ssl_conn *c) { (void)c; }
__attribute__((weak)) int dm_nextendo_ssl_set_fd(dm_ssl_conn *c, int host_fd) { (void)c; (void)host_fd; return -1; }
__attribute__((weak)) int dm_nextendo_ssl_set_hostname(dm_ssl_conn *c, const char *host) { (void)c; (void)host; return -1; }
__attribute__((weak)) int dm_nextendo_ssl_set_alpn(dm_ssl_conn *c, const char *proto) { (void)c; (void)proto; return -1; }
__attribute__((weak)) int dm_nextendo_ssl_set_io_mode(dm_ssl_conn *c, int nonblock) { (void)c; (void)nonblock; return -1; }
__attribute__((weak)) int dm_nextendo_ssl_handshake(dm_ssl_conn *c) { (void)c; return -1; }
__attribute__((weak)) int dm_nextendo_ssl_read(dm_ssl_conn *c, void *buf, size_t len, int peek) { (void)c; (void)buf; (void)len; (void)peek; return -1; }
__attribute__((weak)) int dm_nextendo_ssl_write(dm_ssl_conn *c, const void *buf, size_t len) { (void)c; (void)buf; (void)len; return -1; }
__attribute__((weak)) int dm_nextendo_ssl_pending(dm_ssl_conn *c) { (void)c; return 0; }
__attribute__((weak)) void *guest_sym(const char *name) { (void)name; return NULL; }
__attribute__((weak)) void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; }
__attribute__((weak)) uint32_t dm_nextendo_builtin_certs_bufsize(unsigned max_certs) { (void)max_certs; return 0; }
__attribute__((weak)) int dm_nextendo_builtin_certs_fill(void **out_infos, uint8_t *buf, uint32_t buf_size, unsigned max_certs, uint32_t *out_count) {
    (void)out_infos; (void)buf; (void)buf_size; (void)max_certs; (void)out_count; return -1;
}
__attribute__((weak)) int dm_nextendo_ssl_poll(dm_ssl_conn *c, int want_read, int want_write, int timeout_ms) { (void)c; (void)want_read; (void)want_write; (void)timeout_ms; return -1; }
__attribute__((weak)) uint32_t dm_nextendo_ssl_last_error(dm_ssl_conn *c) { (void)c; return 0x2A2u; }
