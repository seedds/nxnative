// Virtual amiibo: the amiibo file store and the nn::nfp take-overs, after Ryujinx's virtual amiibo (VirtualAmiibo.cs, INfp.cs): states, events, results and file format.
// One JSON file per amiibo, <amiibo folder>/<amiibo ID>.json, in the format of Ryujinx's VirtualAmiiboFile
// (FileVersion, TagUuid, AmiiboId, FirstWriteDate, LastWriteDate, WriteCounter, ApplicationAreas, NickName), so that Ryujinx's files can be used as they are.
// The folder is $DM_AMIIBO_DIR, else <$DM_SAVE>_amiibo, else ./amiibo. Unknown top-level members are kept verbatim and written back.
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>

typedef struct { uint32_t id; uint8_t *data; size_t size; } amiibo_area;
typedef struct { uint32_t version; char id[17]; uint8_t uuid[9]; int uuid_len; char first[40], last[40]; uint16_t writes; char nick[64]; amiibo_area *areas; int n_areas; char *extra; } amiibo_file;

// ---- base64 ----
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { size_t cap = (b->n + n + 1) * 2; char *t = realloc(b->p, cap); if (!t) die("amiibo: string buffer allocation failed"); b->p = t; b->cap = cap; }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void b64_encode(sbuf *b, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        char o[4] = { B64[v >> 18 & 63], B64[v >> 12 & 63], i + 1 < n ? B64[v >> 6 & 63] : '=', i + 2 < n ? B64[v & 63] : '=' };
        sb_put(b, o, 4);
    }
}
// Returns a heap buffer (never NULL on success) and its size; NULL on bad input.
static uint8_t *b64_decode(const char *s, size_t *out) {
    size_t L = strlen(s); uint8_t *r = malloc(L / 4 * 3 + 3); size_t n = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < L; i++) {
        if (s[i] == '=') { if (i + 2 < L || (i + 1 < L && s[i + 1] != '=')) goto bad; break; }
        const char *q = s[i] ? strchr(B64, s[i]) : NULL;
        if (!q) goto bad;
        acc = acc << 6 | (uint32_t)(q - B64); bits += 6;
        if (bits >= 8) { bits -= 8; r[n++] = acc >> bits & 255; }
    }
    *out = n; return r;
bad: free(r); return NULL;
}

// ---- JSON reader for one object ----
static void js_ws(const char **p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++; }
static int js_hex4(const char *p, uint32_t *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) { char c = p[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (d < 0) return -1; *v = *v << 4 | d; }
    return 0;
}
static int utf8_put(char *o, uint32_t c) {
    if (c < 0x80) { o[0] = c; return 1; }
    if (c < 0x800) { o[0] = 0xc0 | c >> 6; o[1] = 0x80 | (c & 63); return 2; }
    if (c < 0x10000) { o[0] = 0xe0 | c >> 12; o[1] = 0x80 | (c >> 6 & 63); o[2] = 0x80 | (c & 63); return 3; }
    o[0] = 0xf0 | c >> 18; o[1] = 0x80 | (c >> 12 & 63); o[2] = 0x80 | (c >> 6 & 63); o[3] = 0x80 | (c & 63); return 4;
}
// Parses a string at *p into a heap buffer; NULL on a syntax error.
static char *js_string(const char **p) {
    if (**p != '"') return NULL;
    const char *s = *p + 1; char *r = malloc(strlen(s) + 1); size_t n = 0;
    while (*s && *s != '"') {
        if (*s != '\\') { r[n++] = *s++; continue; }
        s++;
        switch (*s) {
        case '"': case '\\': case '/': r[n++] = *s++; break;
        case 'b': r[n++] = '\b'; s++; break;
        case 'f': r[n++] = '\f'; s++; break;
        case 'n': r[n++] = '\n'; s++; break;
        case 'r': r[n++] = '\r'; s++; break;
        case 't': r[n++] = '\t'; s++; break;
        case 'u': {
            uint32_t c, lo;
            if (js_hex4(s + 1, &c)) goto bad;
            s += 5;
            if (c >= 0xd800 && c < 0xdc00 && s[0] == '\\' && s[1] == 'u' && !js_hex4(s + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); s += 6; }
            else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
            n += utf8_put(r + n, c); break; }
        default: goto bad;
        }
    }
    if (*s != '"') goto bad;
    r[n] = 0; *p = s + 1; return r;
bad: free(r); return NULL;
}
// Skips one value of any kind; returns 0 on success.
static int js_skip(const char **p, int depth) {
    js_ws(p);
    if (depth > 64) return -1;
    if (**p == '"') { char *s = js_string(p); if (!s) return -1; free(s); return 0; }
    if (**p == '{' || **p == '[') {
        char close = **p == '{' ? '}' : ']'; int obj = **p == '{';
        (*p)++; js_ws(p);
        if (**p == close) { (*p)++; return 0; }
        for (;;) {
            js_ws(p);
            if (obj) { char *k = js_string(p); if (!k) return -1; free(k); js_ws(p); if (*(*p)++ != ':') return -1; }
            if (js_skip(p, depth + 1)) return -1;
            js_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == close) { (*p)++; return 0; }
            return -1;
        }
    }
    const char *s = *p;
    while (**p && strchr("+-.0123456789eEtrufalsn", **p)) (*p)++;
    return *p == s ? -1 : 0;
}
static int js_uint(const char **p, unsigned long long *v) {
    js_ws(p);
    if (**p < '0' || **p > '9') return -1;
    char *e; errno = 0; *v = strtoull(*p, &e, 10);
    if (errno) return -1;
    *p = e; return 0;
}
static int js_null(const char **p) { js_ws(p); if (strncmp(*p, "null", 4)) return 0; *p += 4; return 1; }

static void amiibo_free(amiibo_file *f) {
    for (int i = 0; i < f->n_areas; i++) free(f->areas[i].data);
    free(f->areas); free(f->extra);
    f->areas = NULL; f->extra = NULL; f->n_areas = 0;
}

static int parse_areas(const char **p, amiibo_file *f) {
    js_ws(p);
    if (js_null(p)) return 0;
    if (*(*p)++ != '[') return -1;
    js_ws(p);
    if (**p == ']') { (*p)++; return 0; }
    for (;;) {
        js_ws(p);
        if (*(*p)++ != '{') return -1;
        amiibo_area a = {0}; int have_id = 0;
        for (;;) {
            js_ws(p);
            char *k = js_string(p); if (!k) goto bad;
            js_ws(p);
            if (*(*p)++ != ':') { free(k); goto bad; }
            int rc;
            if (!strcmp(k, "ApplicationAreaId")) { unsigned long long v = 0; rc = js_uint(p, &v); if (!rc && v > 0xffffffffull) rc = -1; a.id = (uint32_t)v; have_id = !rc; }
            else if (!strcmp(k, "ApplicationArea")) {
                if (js_null(p)) rc = 0;
                else { char *s = js_string(p); rc = -1; if (s) { free(a.data); a.data = b64_decode(s, &a.size); rc = a.data ? 0 : -1; free(s); } }
            } else rc = js_skip(p, 0);
            free(k);
            if (rc) goto bad;
            js_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == '}') { (*p)++; break; }
            goto bad;
        }
        if (!have_id) goto bad;
        amiibo_area *t = realloc(f->areas, (f->n_areas + 1) * sizeof *f->areas);
        if (!t) die("amiibo: application area allocation failed");
        f->areas = t; f->areas[f->n_areas++] = a;
        js_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') { (*p)++; return 0; }
        return -1;
    bad: free(a.data); return -1;
    }
}

static void copy_str(char *dst, size_t cap, const char *s) {   // a cut text ends on a UTF-8 character boundary
    snprintf(dst, cap, "%s", s);
    size_t n = cap - 1; if (strlen(s) <= n) return;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
    dst[n] = 0;
}

// Parses the text of one amiibo file into f (f->id is set by the caller); returns 0 on success.
static int amiibo_parse(const char *text, amiibo_file *f) {
    const char *p = text; sbuf extra = {0};
    js_ws(&p);
    if (*p++ != '{') return -1;
    js_ws(&p);
    if (*p == '}') { p++; goto done; }
    for (;;) {
        js_ws(&p);
        const char *kstart = p;
        char *k = js_string(&p); if (!k) goto bad;
        js_ws(&p);
        if (*p++ != ':') { free(k); goto bad; }
        int rc = 0; unsigned long long v = 0;
        if (!strcmp(k, "TagUuid")) {
            if (js_null(&p)) f->uuid_len = 0;
            else { char *s = (js_ws(&p), js_string(&p)); size_t n; uint8_t *d = s ? b64_decode(s, &n) : NULL; free(s); if (!d || n > sizeof f->uuid) rc = -1; else { memcpy(f->uuid, d, n); f->uuid_len = (int)n; } free(d); }
        } else if (!strcmp(k, "FirstWriteDate") || !strcmp(k, "LastWriteDate") || !strcmp(k, "NickName") || !strcmp(k, "AmiiboId")) {
            if (js_null(&p)) { if (k[0] == 'N') f->nick[0] = 0; }
            else {
                char *s = (js_ws(&p), js_string(&p));
                if (!s) rc = -1;
                else if (k[0] == 'F') copy_str(f->first, sizeof f->first, s);
                else if (k[0] == 'L') copy_str(f->last, sizeof f->last, s);
                else if (k[0] == 'N') copy_str(f->nick, sizeof f->nick, s);
                free(s);
            }
        } else if (!strcmp(k, "WriteCounter")) { rc = js_uint(&p, &v); if (!rc) f->writes = (uint16_t)v; }
        else if (!strcmp(k, "ApplicationAreas")) rc = parse_areas(&p, f);
        else if (!strcmp(k, "FileVersion")) { rc = js_uint(&p, &v); if (!rc && v > 0xffffffffull) rc = -1; f->version = (uint32_t)v; }
        else {
            js_ws(&p); rc = js_skip(&p, 0);
            if (!rc) { if (extra.n) sb_str(&extra, ","); sb_put(&extra, kstart, (size_t)(p - kstart)); }
        }
        free(k);
        if (rc) goto bad;
        js_ws(&p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        goto bad;
    }
done:
    js_ws(&p);
    if (*p) goto bad;
    f->extra = extra.p;
    return 0;
bad: free(extra.p); amiibo_free(f); return -1;
}

// ---- files ----
static const char *amiibo_dir(void) {
    static char dir[1024];
    const char *e = getenv("DM_AMIIBO_DIR"), *s = getenv("DM_SAVE");
    if (e && *e) snprintf(dir, sizeof dir, "%s", e);
    else if (s && *s) { size_t n = strlen(s); while (n > 1 && s[n - 1] == '/') n--; snprintf(dir, sizeof dir, "%.*s_amiibo", (int)n, s); }
    else snprintf(dir, sizeof dir, "./amiibo");
    mkdir(dir, 0755);
    return dir;
}

static int amiibo_id_ok(const char *id) {
    if (!id || strlen(id) != 16) return 0;
    for (int i = 0; i < 16; i++) if (!strchr("0123456789abcdefABCDEF", id[i])) return 0;
    return 1;
}

static void amiibo_now(char *out, size_t cap) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    long off = tm.tm_gmtoff; char sign = off < 0 ? '-' : '+'; if (off < 0) off = -off;
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, sign, off / 3600, off / 60 % 60);
}

static void js_put_string(sbuf *b, const char *s) {
    sb_str(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s; char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = c; sb_put(b, e, 2); }
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); sb_str(b, e); }
        else sb_put(b, s, 1);
    }
    sb_str(b, "\"");
}

static int amiibo_save(const amiibo_file *f) {
    sbuf b = {0}; char num[96];
    snprintf(num, sizeof num, "{\"FileVersion\":%u,\"TagUuid\":\"", f->version); sb_str(&b, num);
    b64_encode(&b, f->uuid, (size_t)f->uuid_len);
    sb_str(&b, "\",\"AmiiboId\":"); js_put_string(&b, f->id);
    sb_str(&b, ",\"FirstWriteDate\":"); js_put_string(&b, f->first);
    sb_str(&b, ",\"LastWriteDate\":"); js_put_string(&b, f->last);
    snprintf(num, sizeof num, ",\"WriteCounter\":%u,\"ApplicationAreas\":[", f->writes); sb_str(&b, num);
    for (int i = 0; i < f->n_areas; i++) {
        snprintf(num, sizeof num, "%s{\"ApplicationAreaId\":%u,\"ApplicationArea\":\"", i ? "," : "", f->areas[i].id); sb_str(&b, num);
        b64_encode(&b, f->areas[i].data, f->areas[i].size);
        sb_str(&b, "\"}");
    }
    sb_str(&b, "],\"NickName\":"); js_put_string(&b, f->nick);
    if (f->extra && *f->extra) { sb_str(&b, ","); sb_str(&b, f->extra); }
    sb_str(&b, "}");
    char path[1200], tmp[1210];
    snprintf(path, sizeof path, "%s/%s.json", amiibo_dir(), f->id); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb"); int ok = fp && fwrite(b.p, 1, b.n, fp) == b.n;
    if (fp && fclose(fp)) ok = 0;
    free(b.p);
    if (!ok || rename(tmp, path)) { LOG("amiibo: cannot write %s (errno=%d)", path, errno); unlink(tmp); return -1; }
    return 0;
}

// Cuts s to at most n Unicode characters (UTF-8).
static void utf8_cut(char *s, int n) {
    for (; *s; s++) if (((unsigned char)*s & 0xc0) != 0x80 && n-- == 0) { *s = 0; return; }
}

static int amiibo_load(const char *id, const char *default_nick, amiibo_file *f) {
    memset(f, 0, sizeof *f);
    if (!amiibo_id_ok(id)) { LOG("amiibo: bad amiibo ID"); return -1; }
    snprintf(f->id, sizeof f->id, "%s", id);
    char path[1200]; snprintf(path, sizeof path, "%s/%s.json", amiibo_dir(), id);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        char *text = n >= 0 && n < (16 << 20) ? malloc((size_t)n + 1) : NULL;
        int ok = text && fread(text, 1, (size_t)n, fp) == (size_t)n;
        fclose(fp);
        if (ok) { text[n] = 0; ok = !amiibo_parse(text, f); }
        free(text);
        if (ok) { if (!f->first[0]) amiibo_now(f->first, sizeof f->first); if (!f->last[0]) copy_str(f->last, sizeof f->last, f->first); return 0; }
        char bad[1210]; snprintf(bad, sizeof bad, "%s.bad", path);
        LOG("amiibo: %s does not parse; moved to %s, a new file replaces it", path, bad);
        rename(path, bad);
        memset(f, 0, sizeof *f); snprintf(f->id, sizeof f->id, "%s", id);
    }
    amiibo_now(f->first, sizeof f->first); copy_str(f->last, sizeof f->last, f->first);
    copy_str(f->nick, sizeof f->nick, default_nick ? default_nick : ""); utf8_cut(f->nick, 10);
    return amiibo_save(f);
}

// ---- nn::nfp take-overs ----
// From Mario 1.2.1 the game initializes nfp at boot (v0 did not use it). The real SDK would connect to the sm: service, which the host lacks → abort, so the whole family is taken over.
// The host reports one NFC device on the currently connected controller (same as hle_hid.c). StartDetection searches; dm_amiibo_scan (the front end's Amiibo panel) places the virtual amiibo,
// signals the activate event, then the deactivate event 125 ms later, once per scan; Super Mario attaches no events and polls GetDeviceState. The amiibo settings applets return "tag not found".
// The Legend of Zelda: Breath of the Wild (SDK 20) attaches the activate / deactivate events: without the take-over the SDK's code reads a null pointer at sdk+0x3f2a78
#define NFP_RES(d) ((uint32_t)(d) << 9 | 115u)   // module 115 (nfp)
#define NFP_RES_DEVICE_NOT_FOUND NFP_RES(64)
#define NFP_RES_WRONG_ARGUMENT NFP_RES(65)
#define NFP_RES_WRONG_STATE NFP_RES(73)
#define NFP_RES_TAG_NOT_FOUND NFP_RES(97)
#define NFP_RES_AREA_NULL NFP_RES(128)
#define NFP_RES_AREA_EXISTS NFP_RES(168)
enum { NFP_STATE_INIT = 0, NFP_STATE_SEARCH = 1, NFP_STATE_FOUND = 2, NFP_STATE_REMOVED = 3, NFP_STATE_MOUNTED = 4 };
typedef struct { uint32_t id; uint8_t reserved[4]; } NfpDeviceHandle;
#define NFP_DEVICE_ID 0x4e465030u   // 'NFP0'
#define NFP_AREA_SIZE 0xd8u
uint32_t hle_hid_connected_npad(void);
uint64_t dm_entropy64(void);

// The nfp state; g_nfp_mu guards all of it (guest threads in the take-overs, the front end in dm_amiibo_scan)
static pthread_mutex_t g_nfp_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_nfp_state, g_nfp_loaded, g_nfp_area_open;
static uint32_t g_nfp_area_id;
static uint8_t g_nfp_uuid[9];
static char g_nfp_name[64];
static amiibo_file g_nfp_file;
static void *g_nfp_ev_act, *g_nfp_ev_deact;

static void nfp_lock(void) { pthread_mutex_lock(&g_nfp_mu); }
static Result nfp_unlock(Result r) { pthread_mutex_unlock(&g_nfp_mu); return r; }
static int nfp_bad(const NfpDeviceHandle *h) { return !h || h->id != NFP_DEVICE_ID; }
// Handle and state check for the calls that need a mounted tag; 0 when the call may go on
static Result nfp_need_mount(const NfpDeviceHandle *h) { return nfp_bad(h) ? NFP_RES_DEVICE_NOT_FOUND : g_nfp_state != NFP_STATE_MOUNTED ? NFP_RES_WRONG_STATE : 0; }
static amiibo_area *nfp_area(uint32_t id) {
    for (int i = 0; i < g_nfp_file.n_areas; i++) if (g_nfp_file.areas[i].id == id) return &g_nfp_file.areas[i];
    return NULL;
}
static void nfp_area_set(amiibo_area *a, const void *buf, size_t n) {
    free(a->data); a->data = malloc(n ? n : 1); if (n) memcpy(a->data, buf, n); a->size = n;
}
static void nfp_date(const char *iso, uint8_t *out) {   // u16 year, u8 month, u8 day
    int y = 2000, m = 1, d = 1; sscanf(iso, "%4d-%2d-%2d", &y, &m, &d);
    uint16_t yy = (uint16_t)y; memcpy(out, &yy, 2); out[2] = (uint8_t)m; out[3] = (uint8_t)d;
}
static void nfp_uuid_new(uint8_t *u) {
    uint64_t a = dm_entropy64(), b = dm_entropy64(); memcpy(u, &a, 8); u[8] = (uint8_t)b;
    u[3] = 0x88 ^ u[0] ^ u[1] ^ u[2]; u[8] = u[3] ^ u[4] ^ u[5] ^ u[6];
}

typedef struct { void *act, *deact; } nfp_events;
static void *nfp_event_thread(void *arg) {
    nfp_events e = *(nfp_events *)arg; free(arg);
    if (e.act) hle_signal_bound_system_event(e.act);
    nanosleep(&(struct timespec){0, 125000000}, NULL);
    if (e.deact) hle_signal_bound_system_event(e.deact);
    return NULL;
}

int dm_amiibo_scan(const char *amiibo_id, const char *name, int random_uuid) {
    nfp_lock();
    if (g_nfp_state != NFP_STATE_SEARCH) { LOG("nfp: scan %s ignored: the game is not searching for a tag (state %d)", amiibo_id ? amiibo_id : "?", g_nfp_state); return (int)nfp_unlock(0); }
    if (!amiibo_id_ok(amiibo_id)) { LOG("nfp: scan ignored: bad amiibo ID"); return (int)nfp_unlock(0); }
    if (g_nfp_loaded) amiibo_free(&g_nfp_file);
    amiibo_load(amiibo_id, name, &g_nfp_file); g_nfp_loaded = 1;   // on a write error the tag still works from memory
    if (g_nfp_file.uuid_len != 9) { nfp_uuid_new(g_nfp_file.uuid); g_nfp_file.uuid_len = 9; amiibo_save(&g_nfp_file); }
    if (random_uuid) nfp_uuid_new(g_nfp_uuid); else memcpy(g_nfp_uuid, g_nfp_file.uuid, 9);
    copy_str(g_nfp_name, sizeof g_nfp_name, name ? name : "");
    g_nfp_state = NFP_STATE_FOUND; g_nfp_area_open = 0;
    nfp_events *e = malloc(sizeof *e); *e = (nfp_events){ g_nfp_ev_act, g_nfp_ev_deact };
    pthread_t th; if (pthread_create(&th, NULL, nfp_event_thread, e)) { free(e); LOG("[RED] nfp: cannot start the event thread"); } else pthread_detach(th);
    LOG("nfp: scan %s (%s) → tag found, %s tag UUID", amiibo_id, g_nfp_name, random_uuid ? "random" : "stored");
    return (int)nfp_unlock(1);
}
int dm_amiibo_searching(void) { nfp_lock(); int r = g_nfp_state == NFP_STATE_SEARCH; nfp_unlock(0); return r; }

HLE_FORCE("_ZN2nn3nfp10InitializeEv", "amiibo library init; one NFC device with the virtual amiibo", void, h_nfp_Init, void) {
    nfp_lock(); g_nfp_state = NFP_STATE_INIT; g_nfp_area_open = 0;
    LOG("nfp::Initialize: virtual amiibo, folder %s", amiibo_dir()); nfp_unlock(0);
}
HLE_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "NFC availability change event; bound to a kernel event, never signalled", void, h_nfp_AttachAvail, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "NFC device list: one device", Result, h_nfp_List, NfpDeviceHandle *out, int *n, int max) {
    *n = 0; if (max >= 1) { memset(&out[0], 0, sizeof out[0]); out[0].id = NFP_DEVICE_ID; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", "controller the NFC device is on: the one connected now", Result, h_nfp_NpadId, uint32_t *out, const NfpDeviceHandle *h) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    *out = hle_hid_connected_npad(); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", "start tag detection; the Amiibo panel's scan places the tag", Result, h_nfp_Start, const NfpDeviceHandle *h) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    nfp_lock(); g_nfp_state = NFP_STATE_SEARCH; g_nfp_area_open = 0; return nfp_unlock(RES_OK);
}
HLE_FORCE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", "stop tag detection; the scanned tag is gone", Result, h_nfp_Stop, const NfpDeviceHandle *h) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    nfp_lock(); g_nfp_state = NFP_STATE_INIT; g_nfp_area_open = 0; return nfp_unlock(RES_OK);
}
HLE_FORCE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", "device state: initialized / searching / tag found / tag mounted", int, h_nfp_State, const NfpDeviceHandle *h) {
    (void)h; nfp_lock(); int s = g_nfp_state; nfp_unlock(0); return s;
}
HLE_FORCE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", "tag info: the virtual amiibo's 9-byte UUID", Result, h_nfp_TagInfo, void *out, const NfpDeviceHandle *h) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    nfp_lock();
    if (g_nfp_state != NFP_STATE_FOUND && g_nfp_state != NFP_STATE_MOUNTED) return nfp_unlock(NFP_RES_WRONG_STATE);
    uint8_t *o = out; memset(o, 0, 0x58); memcpy(o, g_nfp_uuid, 9); o[0x0a] = 9;
    uint32_t any = 0xffffffffu; memcpy(o + 0x20, &any, 4); memcpy(o + 0x24, &any, 4);   // protocol, tag type
    return nfp_unlock(RES_OK);
}
HLE_FORCE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", "model info: from the amiibo ID's digits", Result, h_nfp_ModelInfo, void *out, const NfpDeviceHandle *h) {
    nfp_lock(); Result r = nfp_need_mount(h); if (r) return nfp_unlock(r);
    unsigned v[5]; const char *id = g_nfp_file.id;
    sscanf(id, "%4x%2x%2x%4x%2x", &v[0], &v[1], &v[2], &v[3], &v[4]);
    uint8_t *o = out; memset(o, 0, 0x40);
    o[0] = (uint8_t)(v[0] >> 8); o[1] = (uint8_t)v[0]; o[2] = (uint8_t)v[1]; o[3] = (uint8_t)v[2];
    uint16_t model = (uint16_t)v[3]; memcpy(o + 4, &model, 2); o[6] = (uint8_t)v[4];
    return nfp_unlock(RES_OK);
}
static Result nfp_mount(const NfpDeviceHandle *h, int model, int target) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    if (model != 0 || (target & 3) == 0) return NFP_RES_WRONG_ARGUMENT;
    nfp_lock();
    if (g_nfp_state != NFP_STATE_FOUND && g_nfp_state != NFP_STATE_MOUNTED) { LOG("nfp: Mount → no tag (state %d)", g_nfp_state); return nfp_unlock(NFP_RES_WRONG_STATE); }
    g_nfp_state = NFP_STATE_MOUNTED; LOG("nfp: Mount %s → ok", g_nfp_file.id);
    return nfp_unlock(RES_OK);
}
HLE_FORCE_BIG("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", "mount the virtual amiibo", Result, h_nfp_Mount, const NfpDeviceHandle *h, int model, int target) { return nfp_mount(h, model, target); }
HLE_FORCE_BIG("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeE", "mount the virtual amiibo (two arguments: all targets)", Result, h_nfp_Mount2, const NfpDeviceHandle *h, int model) { return nfp_mount(h, model, 3); }
HLE_FORCE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", "tag placed event; signalled by the host after a scan", Result, h_nfp_AttachAct, void *ev, const NfpDeviceHandle *h) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    hle_bind_system_event(ev, 0); nfp_lock(); g_nfp_ev_act = ev; return nfp_unlock(RES_OK);
}
HLE_FORCE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", "tag removed event; signalled by the host 125 ms after a scan", Result, h_nfp_AttachDeact, void *ev, const NfpDeviceHandle *h) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    hle_bind_system_event(ev, 0); nfp_lock(); g_nfp_ev_deact = ev; return nfp_unlock(RES_OK);
}
HLE_FORCE("_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", "unmount the virtual amiibo", Result, h_nfp_Unmount, const NfpDeviceHandle *h) {
    if (nfp_bad(h)) return NFP_RES_DEVICE_NOT_FOUND;
    nfp_lock();
    if (g_nfp_state != NFP_STATE_FOUND && g_nfp_state != NFP_STATE_MOUNTED) { LOG("nfp: Unmount → no tag (state %d)", g_nfp_state); return nfp_unlock(NFP_RES_WRONG_STATE); }
    g_nfp_state = NFP_STATE_FOUND; g_nfp_area_open = 0; LOG("nfp: Unmount %s → ok", g_nfp_file.id);
    return nfp_unlock(RES_OK);
}
HLE_FORCE("_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", "common info: the amiibo file's last write date and write counter", Result, h_nfp_CommonInfo, void *out, const NfpDeviceHandle *h) {
    nfp_lock(); Result r = nfp_need_mount(h); if (r) return nfp_unlock(r);
    uint8_t *o = out; memset(o, 0, 0x40); nfp_date(g_nfp_file.last, o);
    uint16_t writes = g_nfp_file.writes, version = 1; uint32_t size = NFP_AREA_SIZE;
    memcpy(o + 4, &writes, 2); memcpy(o + 6, &version, 2); memcpy(o + 8, &size, 4);
    return nfp_unlock(RES_OK);
}
// UTF-8 → UTF-16, at most n units without the terminator; out has room for n + 1
static void nfp_utf16(uint16_t *out, int n, const char *s) {
    int k = 0; const unsigned char *p = (const unsigned char *)s;
    while (*p && k < n) {
        uint32_t c = *p++; int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more) c &= 0x3f >> more;
        while (more-- && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 63);
        out[k++] = c > 0xffff ? 0xfffd : (uint16_t)c;
    }
    out[k] = 0;
}
HLE_FORCE("_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", "register info: the default Mii with the user's nickname, the amiibo's nickname", Result, h_nfp_RegInfo, void *out, const NfpDeviceHandle *h) {
    nfp_lock(); Result r = nfp_need_mount(h); if (r) return nfp_unlock(r);
    uint8_t *o = out; memset(o, 0, 0x100);
    dm_mii_default_charinfo(o);
    const char *nick = getenv("DM_NICK"); uint16_t mii_nick[11] = {0};
    nfp_utf16(mii_nick, 10, nick ? nick : "Player"); memcpy(o + 0x10, mii_nick, sizeof mii_nick);   // CharInfo nickname, UTF-16
    nfp_date(g_nfp_file.first, o + 0x58);
    snprintf((char *)o + 0x5c, 0x29, "%s", g_nfp_file.nick);   // amiibo nickname, UTF-8; font region 0 at 0x85
    return nfp_unlock(RES_OK);
}
HLE_FORCE("_ZN2nn3nfp8GetStateEv", "amiibo library state: initialized (1)", int, h_nfp_LibState, void) { return 1; }
HLE_FORCE("_ZN2nn3nfp8FinalizeEv", "amiibo library finalize: the scanned tag is gone", void, h_nfp_Fini, void) {
    nfp_lock(); g_nfp_state = NFP_STATE_INIT; g_nfp_area_open = 0;
    if (g_nfp_loaded) amiibo_free(&g_nfp_file);
    g_nfp_loaded = 0; nfp_unlock(0);
}
HLE_FORCE_BIG("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", "flush tag: the host writes the amiibo file on each change; nothing to do", Result, h_nfp_Flush, const NfpDeviceHandle *h) {
    nfp_lock(); return nfp_unlock(nfp_need_mount(h));
}
HLE_FORCE_BIG("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", "open the virtual amiibo's application area", Result, h_nfp_OpenArea, const NfpDeviceHandle *h, uint32_t id) {
    nfp_lock(); Result r = nfp_need_mount(h); if (r) return nfp_unlock(r);
    if (!nfp_area(id)) r = NFP_RES_AREA_NULL; else { g_nfp_area_open = 1; g_nfp_area_id = id; }
    LOG("nfp: OpenApplicationArea %s area %#x → %#x", g_nfp_file.id, id, r);
    return nfp_unlock(r);
}
HLE_FORCE_BIG("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", "read the open application area: at most the buffer size", Result, h_nfp_GetArea, void *buf, uint64_t *n, const NfpDeviceHandle *h, uint64_t sz) {
    if (n) *n = 0;
    nfp_lock(); Result r = nfp_need_mount(h); if (r) return nfp_unlock(r);
    amiibo_area *a = g_nfp_area_open ? nfp_area(g_nfp_area_id) : NULL;
    if (!a || !a->size) return nfp_unlock(NFP_RES_AREA_NULL);
    size_t k = a->size < sz ? a->size : (size_t)sz; memcpy(buf, a->data, k); if (n) *n = k;
    return nfp_unlock(RES_OK);
}
HLE_FORCE_BIG("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", "write the open application area; saved to the amiibo file", Result, h_nfp_SetArea, const NfpDeviceHandle *h, const void *buf, uint64_t sz) {
    nfp_lock(); Result r = nfp_need_mount(h); if (r) return nfp_unlock(r);
    amiibo_area *a = g_nfp_area_open ? nfp_area(g_nfp_area_id) : NULL;
    if (sz > NFP_AREA_SIZE) r = NFP_RES_WRONG_ARGUMENT; else if (!a) r = NFP_RES_AREA_NULL; else { nfp_area_set(a, buf, (size_t)sz); amiibo_save(&g_nfp_file); }
    LOG("nfp: SetApplicationArea %s area %#x, %llu bytes → %#x", g_nfp_file.id, g_nfp_area_id, (unsigned long long)sz, r);
    return nfp_unlock(r);
}
typedef struct { uint32_t id, size; const void *data; } NfpAreaCreateInfo;
static Result nfp_create_area(const NfpDeviceHandle *h, const NfpAreaCreateInfo *ci, int recreate) {
    nfp_lock(); Result r = nfp_need_mount(h); if (r) return nfp_unlock(r);
    amiibo_area *a = nfp_area(ci->id);
    if (ci->size > NFP_AREA_SIZE) r = NFP_RES_WRONG_ARGUMENT;
    else if (a && !recreate) r = NFP_RES_AREA_EXISTS;
    else {
        if (!a) { amiibo_area *t = realloc(g_nfp_file.areas, (g_nfp_file.n_areas + 1) * sizeof *g_nfp_file.areas); if (!t) die("amiibo: application area allocation failed"); g_nfp_file.areas = t; a = &g_nfp_file.areas[g_nfp_file.n_areas++]; *a = (amiibo_area){ ci->id, NULL, 0 }; }
        nfp_area_set(a, ci->data, ci->size); amiibo_save(&g_nfp_file);
    }
    LOG("nfp: %sApplicationArea %s area %#x, %u bytes → %#x", recreate ? "Recreate" : "Create", g_nfp_file.id, ci->id, ci->size, r);
    return nfp_unlock(r);
}
HLE_FORCE_BIG("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "create an application area on the virtual amiibo", Result, h_nfp_CreateArea, const NfpDeviceHandle *h, const void *info) { return nfp_create_area(h, info, 0); }
HLE_FORCE_BIG("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "replace an application area on the virtual amiibo", Result, h_nfp_RecreateArea, const NfpDeviceHandle *h, const void *info) { return nfp_create_area(h, info, 1); }
// The amiibo settings applets are out of scope: no tag found
HLE_FORCE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo restorer applet: no tag", Result, h_nfp_Restorer, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return NFP_RES_TAG_NOT_FOUND; }
HLE_FORCE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo game data eraser applet: no tag", Result, h_nfp_Eraser, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return NFP_RES_TAG_NOT_FOUND; }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo nickname and owner settings applet: no tag", Result, h_nfp_Nick, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag) { (void)h; (void)reg; (void)p; (void)tag; if (changed) *changed = 0; return NFP_RES_TAG_NOT_FOUND; }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_", "amiibo nickname and owner settings applet (with the old register info): no tag", Result, h_nfp_Nick2, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag, const void *old) { (void)h; (void)reg; (void)p; (void)tag; (void)old; if (changed) *changed = 0; return NFP_RES_TAG_NOT_FOUND; }
