// coop disc (MIT, our own code): DNS-SD on the LAN (research/06 §5.5, docs/coop-step1c.md). A hosting node registers "Pokopia-<8 hex>" as
// _pokopia-ldn._udp with its control port and a TXT record: p = the coop protocol, b = main's build ID (16 hex digits, "-" unknown), m = the mods
// fingerprint, a = the host's best LAN IPv4, x = its other LAN IPv4s, comma-separated (a PC on two networks, such as Wi-Fi and a phone's USB link).
// Nothing else is published: no player, town or computer names. A scanning node browses for the type and turns each instance into an address and
// port: of the responder's A record and the TXT's addresses, the first on one of its own subnets (else the first), the others tried in turn.
// Backends: Apple dns_sd (macOS, iOS: one shared connection to mDNSResponder that the I/O thread polls; unicast and Bonjour need no multicast
// entitlement), Windows 10 1809+ DnsServiceRegister / DnsServiceBrowse (asynchronous, callbacks on system threads, loaded from dnsapi.dll at run
// time; its browse delivers PTR, SRV and TXT records but resolves no A record for a registration on the same machine, hence a=), nothing elsewhere.
// Everything from the network is untrusted: names, TXT values, ports and addresses are checked before an entry is kept, an entry only names a host
// to try, and the control link checks its WELCOME and every NetworkInfo again (coop_mpnet.c, coop_ldn.c).
#include "coop_int.h"
#include <stdio.h>

#define DISC_RETRY_MS 5000        // after a failed start
#define RESOLVE_MS 5000           // Apple: a resolve or address lookup that has not answered is dropped
#define GAI_GRACE_MS 1000         // Apple: the responder's A record gets this long before the TXT's a= is used
#define WBROWSE_REFRESH_MS 15000  // Windows: the browse is restarted (fresh answers from every live host) ...
#define WENTRY_STALE_MS 35000     // ... and an entry not reported again for this long is dropped

static int64_t now_ms(void) { return cplat_now_ms(); }

// ---------- TXT records and names (shared, tested directly) ----------
static int hexval(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
static int hex16(const uint8_t *s, size_t n, uint64_t *v, char *text) {   // exactly 16 hex digits
    if (n != 16) return 0;
    uint64_t x = 0;
    for (size_t i = 0; i < 16; i++) { int h = hexval(s[i]); if (h < 0) return 0; x = x << 4 | (uint64_t)h; if (text) text[i] = "0123456789abcdef"[h]; }
    if (text) text[16] = 0;
    if (v) *v = x;
    return 1;
}
static int dotted4(const uint8_t *s, size_t n, uint32_t *ip) {
    uint32_t v = 0; size_t i = 0;
    for (int part = 0; part < 4; part++) {
        unsigned b = 0, digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 3) { b = b * 10 + (unsigned)(s[i++] - '0'); digits++; }
        if (!digits || b > 255) return 0;
        v = v << 8 | b;
        if (part < 3) { if (i >= n || s[i] != '.') return 0; i++; }
    }
    if (i != n) return 0;
    *ip = v; return 1;
}
static int dotted_list(const uint8_t *s, size_t n, uint32_t *ip, int max) {   // "a.b.c.d,e.f.g.h": the count, -1 when malformed or too long
    int k = 0;
    for (size_t i = 0; i <= n;) {
        size_t j = i; while (j < n && s[j] != ',') j++;
        if (k == max || !dotted4(s + i, j - i, &ip[k])) return -1;
        k++; i = j + 1;
    }
    return k;
}
int cdisc_txt_parse(const uint8_t *t, size_t len, cdisc_entry *e) {
    int have = 0;
    e->proto = 0; e->build[0] = 0; e->mods = 0; e->txt_ip = 0; e->txt_nx = 0; e->has_txt = 0;
    if (!t || len > 1300) return 0;   // one mDNS packet's worth
    for (size_t i = 0; i < len;) {
        size_t l = t[i++];
        if (l > len - i) return 0;    // a string runs past the record
        const uint8_t *s = t + i; i += l;
        if (l < 2 || s[1] != '=') continue;
        const uint8_t *v = s + 2; size_t vl = l - 2;
        switch (s[0]) {
        case 'p': { unsigned p = 0; if (!vl || vl > 5) return 0; for (size_t k = 0; k < vl; k++) { if (v[k] < '0' || v[k] > '9') return 0; p = p * 10 + (unsigned)(v[k] - '0'); }
                    if (!p || p > 65535) return 0; e->proto = (int)p; have |= 1; break; }
        case 'b': if (vl == 1 && v[0] == '-') e->build[0] = 0; else if (!hex16(v, vl, NULL, e->build)) return 0; have |= 2; break;
        case 'm': if (!hex16(v, vl, &e->mods, NULL)) return 0; have |= 4; break;
        case 'a': if (!dotted4(v, vl, &e->txt_ip)) return 0; break;
        case 'x': if ((e->txt_nx = dotted_list(v, vl, e->txt_x, DISC_ADDRS - 1)) < 0) return 0; break;
        default: break;   // keys of later protocols
        }
    }
    e->has_txt = have == 7;
    return e->has_txt;
}
static size_t txt_put(uint8_t *out, size_t cap, size_t at, const char *kv) {
    size_t l = strlen(kv);
    if (l > 255 || at + 1 + l > cap) return at;
    out[at] = (uint8_t)l; memcpy(out + at + 1, kv, l);
    return at + 1 + l;
}
static int addr_list(char *o, size_t cap, const uint32_t *ip, int n) {   // "a.b.c.d,e.f.g.h"; the length
    size_t at = 0;
    for (int i = 0; i < n && at < cap; i++)
        at += (size_t)snprintf(o + at, cap - at, "%s%u.%u.%u.%u", i ? "," : "", ip[i] >> 24, ip[i] >> 16 & 0xFF, ip[i] >> 8 & 0xFF, ip[i] & 0xFF);
    return (int)(at < cap ? at : cap - 1);
}
size_t cdisc_txt_make(uint8_t *out, size_t cap, int proto, const char *build, uint64_t mods, const uint32_t *ip, int nip) {
    char kv[80]; size_t at = 0;
    snprintf(kv, sizeof kv, "p=%d", proto); at = txt_put(out, cap, at, kv);
    snprintf(kv, sizeof kv, "b=%s", build && build[0] ? build : "-"); at = txt_put(out, cap, at, kv);
    snprintf(kv, sizeof kv, "m=%016llx", (unsigned long long)mods); at = txt_put(out, cap, at, kv);
    if (nip > DISC_ADDRS) nip = DISC_ADDRS;
    if (nip > 0) { memcpy(kv, "a=", 2); addr_list(kv + 2, sizeof kv - 2, ip, 1); at = txt_put(out, cap, at, kv); }
    if (nip > 1) { memcpy(kv, "x=", 2); addr_list(kv + 2, sizeof kv - 2, ip + 1, nip - 1); at = txt_put(out, cap, at, kv); }
    return at;
}
int cdisc_host_addrs(const caddr *mine, int n, uint32_t route, uint32_t *out, int max) {
    int rank[64], idx[64], k = 0;
    for (int i = 0; i < n && k < 64; i++) {
        int cls = coop_addr4_class(mine[i].ip), dup = 0;
        if (cls != COOP_ADDR_PRIVATE && cls != COOP_ADDR_LINKLOCAL) continue;
        for (int j = 0; j < k; j++) dup |= mine[idx[j]].ip == mine[i].ip;
        if (dup) continue;
        rank[k] = (mine[i].is_public ? 4 : 0) + (cls == COOP_ADDR_LINKLOCAL ? 2 : 0) + (mine[i].ip != route);
        idx[k++] = i;
    }
    for (int i = 1; i < k; i++)   // stable: equals keep the system's order
        for (int j = i; j > 0 && rank[j] < rank[j - 1]; j--) { int t = rank[j]; rank[j] = rank[j - 1]; rank[j - 1] = t; t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    int m = k < max ? k : max;
    for (int i = 0; i < m; i++) out[i] = mine[idx[i]].ip;
    return m;
}
int cdisc_pick(const uint32_t *cand, int n, const caddr *mine, int nm) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < nm; j++)
            if (mine[j].mask && mine[j].ip != cand[i] && (mine[j].ip & mine[j].mask) == (cand[i] & mine[j].mask)) return i;
    return 0;
}
int cdisc_name_ok(const char *s, size_t max) {
    size_t k = 0;
    for (; k < max && s[k]; k++) if ((unsigned char)s[k] < 0x20 || s[k] == 0x7F) return 0;
    return k > 0 && k < max;
}

// ---------- the entry table (dlk) ----------
static void ipstr(char *o, uint32_t ip) { snprintf(o, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF); }
// A resolved instance. txt has the TXT's fields; ip is the responder's A record (0: none). The addresses to try: the A record, a= and x=, LAN ones
// only, the first on one of this machine's subnets moved to the front. Returns 1 when kept
static int found(coop_node *n, const char *name, uint32_t ifi, uint32_t ip, uint16_t port, const cdisc_entry *txt, int timed) {
    cdisc *d = &n->disc;
    char a[16];
    uint32_t cand[DISC_ADDRS] = {0}, all[2 + DISC_ADDRS - 1] = {ip, txt->txt_ip}; int nc = 0;
    memcpy(all + 2, txt->txt_x, sizeof txt->txt_x);
    for (int i = 0; i < 2 + txt->txt_nx && nc < DISC_ADDRS; i++) {
        int dup = 0;
        for (int j = 0; j < nc; j++) dup |= cand[j] == all[i];
        if (!dup && all[i] && coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(all[i]))) cand[nc++] = all[i];
    }
    if (nc > 1) {
        caddr mine[16]; int nm = cplat_local_addrs(mine, 16, 0), k = cdisc_pick(cand, nc, mine, nm > 0 ? nm : 0);
        uint32_t t = cand[k]; memmove(cand + 1, cand, (size_t)k * sizeof cand[0]); cand[0] = t;
    }
    ip = nc ? cand[0] : ip ? ip : txt->txt_ip;
    if (!cdisc_name_ok(name, sizeof d->e[0].name) || !port || !txt->has_txt || !nc) {
        cplat_lock(&n->dlk); d->bad++; uint64_t k = d->bad; cplat_unlock(&n->dlk);
        ipstr(a, ip);
        if (k <= 4) coop_logf(n, "disc: an instance ignored (%s, %s:%u): not a LAN address, no port or no valid TXT record", cdisc_name_ok(name, 64) ? name : "a bad name", a, port);
        return 0;
    }
    cplat_lock(&n->dlk);
    cdisc_entry *e = NULL, *fr = NULL;
    for (int i = 0; i < DISC_MAX; i++) {
        if (!d->e[i].used) { if (!fr) fr = &d->e[i]; continue; }
        if (!strcmp(d->e[i].name, name) && (d->e[i].ifindex == ifi || !ifi || !d->e[i].ifindex)) { e = &d->e[i]; break; }
    }
    int fresh = !e || e->ip != ip || e->port != port;
    if (!e) e = fr;
    if (e) {
        cdisc_entry t = *txt;
        snprintf(t.name, sizeof t.name, "%s", name);
        t.used = 1; t.ifindex = ifi; t.ip = ip; t.port = port; t.seen = timed ? now_ms() : 0;
        memcpy(t.addrs, cand, sizeof cand); t.naddrs = nc;
        *e = t;
    }
    cplat_unlock(&n->dlk);
    ipstr(a, ip);
    if (!e) coop_logf(n, "disc: %s at %s:%u not kept: %d hosts known already", name, a, port, DISC_MAX);
    else if (fresh) coop_logf(n, "disc: found %s at %s:%u%s (coop protocol %d, build %s, mods %016llx)", name, a, port, nc > 1 ? " (and other addresses)" : "", txt->proto,
                              txt->build[0] ? txt->build : "unknown", (unsigned long long)txt->mods);
    return e != NULL;
}
static void lost(coop_node *n, const char *name, uint32_t ifi) {
    int k = 0;
    cplat_lock(&n->dlk);
    for (int i = 0; i < DISC_MAX; i++) {
        cdisc_entry *e = &n->disc.e[i];
        if (e->used && !strcmp(e->name, name) && (!ifi || !e->ifindex || e->ifindex == ifi)) { memset(e, 0, sizeof *e); k++; }
    }
    cplat_unlock(&n->dlk);
    if (k) coop_logf(n, "disc: %s is gone", name);
}
static void clear_entries(coop_node *n) { cplat_lock(&n->dlk); memset(n->disc.e, 0, sizeof n->disc.e); cplat_unlock(&n->dlk); }
#if defined(__APPLE__) || defined(_WIN32)
static int host_addrs(uint32_t *out, char *shown, size_t cap) {   // this host's LAN addresses for its record, best first, and as text for the log
    caddr mine[16]; uint32_t route = 0; int k = 0;
    if (cplat_route_ip(&route)) route = 0;
    int nm = cplat_local_addrs(mine, 16, 1);
    if (nm > 0) k = cdisc_host_addrs(mine, nm, route, out, DISC_ADDRS);
    else if (route && coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(route)) && coop_addr4_class(route) != COOP_ADDR_LOOPBACK) out[k++] = route;
    addr_list(shown, cap, out, k);
    return k;
}
#endif
static void make_instance(char *out, size_t n) { uint32_t r; cplat_random(&r, sizeof r); snprintf(out, n, "Pokopia-%08x", r); }

#if defined(__APPLE__)
// ---------- Apple: dns_sd ----------
#include <arpa/inet.h>
#include <dns_sd.h>
typedef struct aslot { coop_node *n; int used; DNSServiceRef res, gai; char name[64], type[64], dom[64]; uint32_t ifi; uint16_t port; cdisc_entry txt; int64_t t0, txt_at; } aslot;
typedef struct aimpl { DNSServiceRef conn, reg, browse; uint16_t reg_port; char inst[32], addrs[80]; int64_t retry_at; aslot s[DISC_MAX]; } aimpl;
static aimpl *AI(coop_node *n) { return n->disc.impl; }
static const char *policy_hint(DNSServiceErrorType e) {
    return e == kDNSServiceErr_PolicyDenied ? ": local network access is off for this app (Settings, Privacy & Security, Local Network)" : "";
}
static void slot_free(aslot *s) {
    if (s->res) DNSServiceRefDeallocate(s->res);
    if (s->gai) DNSServiceRefDeallocate(s->gai);
    coop_node *n = s->n; memset(s, 0, sizeof *s); s->n = n;
}
static void a_reset(coop_node *n) {
    aimpl *a = AI(n);
    for (int i = 0; i < DISC_MAX; i++) if (a->s[i].used) slot_free(&a->s[i]);
    if (a->reg) DNSServiceRefDeallocate(a->reg);
    if (a->browse) DNSServiceRefDeallocate(a->browse);
    if (a->conn) DNSServiceRefDeallocate(a->conn);
    a->conn = a->reg = a->browse = NULL;
    n->disc.adv_on = n->disc.browse_on = 0;
}
static void DNSSD_API reg_cb(DNSServiceRef r, DNSServiceFlags fl, DNSServiceErrorType err, const char *name, const char *type, const char *dom, void *ctx) {
    coop_node *n = ctx; (void)r; (void)fl; (void)type; (void)dom;
    if (err) { coop_logf(n, "disc: advertising failed (dns_sd %d)%s", (int)err, policy_hint(err)); n->disc.adv_on = 0; return; }
    n->disc.adv_on = 1;
    coop_logf(n, "disc: advertising \"%s\" (%s) on port %u, addresses %s", name ? name : "?", COOP_SERVICE, AI(n)->reg_port, AI(n)->addrs[0] ? AI(n)->addrs : "none");
}
static void DNSSD_API gai_cb(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType err, const char *host, const struct sockaddr *addr, uint32_t ttl, void *ctx) {
    aslot *s = ctx; (void)r; (void)host; (void)ttl;
    if (err || !(fl & kDNSServiceFlagsAdd) || !addr || addr->sa_family != AF_INET) return;   // more answers may come; the slot's time limit ends it
    uint32_t ip = ntohl(((const struct sockaddr_in *)addr)->sin_addr.s_addr);
    if (!coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(ip))) return;   // a public or other address on some interface: wait for a LAN one
    found(s->n, s->name, ifi, ip, s->port, &s->txt, 0);
    slot_free(s);
}
static void DNSSD_API resolve_cb(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType err, const char *full, const char *host, uint16_t port_be,
                                 uint16_t txtlen, const unsigned char *txt, void *ctx) {
    aslot *s = ctx; coop_node *n = s->n; (void)r; (void)fl; (void)full;
    DNSServiceRefDeallocate(s->res); s->res = NULL;
    if (err || !host || strnlen(host, 256) >= 256) { slot_free(s); return; }
    s->port = ntohs(port_be);
    if (!cdisc_txt_parse(txt, txtlen, &s->txt)) { cplat_lock(&n->dlk); n->disc.bad++; cplat_unlock(&n->dlk); coop_logf(n, "disc: %s has no valid TXT record: ignored", s->name); slot_free(s); return; }
    s->txt_at = now_ms();
    s->gai = AI(n)->conn;
    if (DNSServiceGetAddrInfo(&s->gai, kDNSServiceFlagsShareConnection, ifi, kDNSServiceProtocol_IPv4, host, gai_cb, s) != kDNSServiceErr_NoError) s->gai = NULL;
}
static void DNSSD_API browse_cb(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType err, const char *name, const char *type, const char *dom, void *ctx) {
    coop_node *n = ctx; aimpl *a = AI(n); (void)r;
    if (err) { coop_logf(n, "disc: browsing failed (dns_sd %d)%s", (int)err, policy_hint(err)); return; }
    if (!name || !cdisc_name_ok(name, 64) || !type || strnlen(type, 64) >= 64 || !dom || strnlen(dom, 64) >= 64) { cplat_lock(&n->dlk); n->disc.bad++; cplat_unlock(&n->dlk); return; }
    if (!(fl & kDNSServiceFlagsAdd)) {
        for (int i = 0; i < DISC_MAX; i++) if (a->s[i].used && !strcmp(a->s[i].name, name) && a->s[i].ifi == ifi) slot_free(&a->s[i]);
        lost(n, name, ifi); return;
    }
    if (!strcmp(name, a->inst)) return;   // this node's own service
    aslot *s = NULL;
    for (int i = 0; i < DISC_MAX; i++) { if (a->s[i].used && !strcmp(a->s[i].name, name) && a->s[i].ifi == ifi) return; if (!a->s[i].used && !s) s = &a->s[i]; }
    if (!s) return;
    memset(s, 0, sizeof *s); s->n = n; s->used = 1; s->ifi = ifi; s->t0 = now_ms();
    snprintf(s->name, sizeof s->name, "%s", name); snprintf(s->type, sizeof s->type, "%s", type); snprintf(s->dom, sizeof s->dom, "%s", dom);
    s->res = a->conn;
    if (DNSServiceResolve(&s->res, kDNSServiceFlagsShareConnection, ifi, s->name, s->type, s->dom, resolve_cb, s) != kDNSServiceErr_NoError) { s->res = NULL; slot_free(s); }
}
void cdisc_init(coop_node *n) { n->disc.impl = calloc(1, sizeof(aimpl)); }
void cdisc_fini(coop_node *n) { if (n->disc.impl) { a_reset(n); free(n->disc.impl); n->disc.impl = NULL; } }
csock cdisc_fd(coop_node *n) { aimpl *a = AI(n); return a && a->conn ? DNSServiceRefSockFD(a->conn) : CSOCK_BAD; }
void cdisc_process(coop_node *n) {
    aimpl *a = AI(n);
    if (!a || !a->conn) return;
    DNSServiceErrorType e = DNSServiceProcessResult(a->conn);
    if (e != kDNSServiceErr_NoError) { coop_logf(n, "disc: the mDNSResponder connection failed (dns_sd %d); retrying", (int)e); a_reset(n); a->retry_at = now_ms() + DISC_RETRY_MS; }
}
void cdisc_tick(coop_node *n) {
    aimpl *a = AI(n); cdisc *d = &n->disc; int64_t now = now_ms();
    if (!a) return;
    if (a->reg && (!d->adv_want || a->reg_port != d->adv_port)) { DNSServiceRefDeallocate(a->reg); a->reg = NULL; d->adv_on = 0; coop_logf(n, "disc: stopped advertising"); }
    if (a->browse && !d->browse_want) {
        DNSServiceRefDeallocate(a->browse); a->browse = NULL; d->browse_on = 0;
        for (int i = 0; i < DISC_MAX; i++) if (a->s[i].used) slot_free(&a->s[i]);
        clear_entries(n);
    }
    if (!a->reg && !a->browse && a->conn) { DNSServiceRefDeallocate(a->conn); a->conn = NULL; }
    if (!(d->adv_want && !a->reg) && !(d->browse_want && !a->browse)) goto slots;
    if (now < a->retry_at) goto slots;
    if (!a->conn && DNSServiceCreateConnection(&a->conn) != kDNSServiceErr_NoError) {
        a->conn = NULL; a->retry_at = now + DISC_RETRY_MS; coop_logf(n, "disc: mDNSResponder is not reachable; retrying in 5 s"); goto slots;
    }
    if (d->adv_want && !a->reg) {
        uint8_t txt[200]; uint32_t ips[DISC_ADDRS];   // mDNSResponder publishes an A record per interface itself; the TXT lists them all
        int k = host_addrs(ips, a->addrs, sizeof a->addrs);
        size_t tl = cdisc_txt_make(txt, sizeof txt, n->proto, n->build, n->mods, ips, k);
        make_instance(a->inst, sizeof a->inst);
        a->reg = a->conn; a->reg_port = d->adv_port;
        DNSServiceErrorType e = DNSServiceRegister(&a->reg, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny, a->inst, COOP_SERVICE, NULL, NULL,
                                                   htons(d->adv_port), (uint16_t)tl, txt, reg_cb, n);
        if (e != kDNSServiceErr_NoError) { a->reg = NULL; a->retry_at = now + DISC_RETRY_MS; coop_logf(n, "disc: advertising failed to start (dns_sd %d)", (int)e); }
    }
    if (d->browse_want && !a->browse) {
        a->browse = a->conn;
        DNSServiceErrorType e = DNSServiceBrowse(&a->browse, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexAny, COOP_SERVICE, NULL, browse_cb, n);
        if (e != kDNSServiceErr_NoError) { a->browse = NULL; a->retry_at = now + DISC_RETRY_MS; coop_logf(n, "disc: browsing failed to start (dns_sd %d)", (int)e); }
        else { d->browse_on = 1; coop_logf(n, "disc: browsing for %s", COOP_SERVICE); }
    }
slots:
    for (int i = 0; i < DISC_MAX; i++) {   // the responder's A record had its chance: the TXT's a= then; a slot that never answers is dropped
        aslot *s = &a->s[i];
        if (!s->used) continue;
        if (s->txt_at && s->txt.txt_ip && now - s->txt_at >= GAI_GRACE_MS) { found(n, s->name, s->ifi, 0, s->port, &s->txt, 0); slot_free(s); continue; }
        if (now - s->t0 >= RESOLVE_MS) slot_free(s);
    }
}
void cdisc_stop(coop_node *n) { if (n->disc.impl) a_reset(n); clear_entries(n); }

#elif defined(_WIN32)
// ---------- Windows: DnsServiceRegister / DnsServiceBrowse (Windows 10 1809+), loaded at run time ----------
#include <windns.h>
typedef DWORD (WINAPI *f_reg)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
typedef DNS_STATUS (WINAPI *f_browse)(PDNS_SERVICE_BROWSE_REQUEST, PDNS_SERVICE_CANCEL);
typedef DNS_STATUS (WINAPI *f_bcancel)(PDNS_SERVICE_CANCEL);
typedef PDNS_SERVICE_INSTANCE (WINAPI *f_construct)(PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD, DWORD, PCWSTR *, PCWSTR *);
typedef VOID (WINAPI *f_free)(PDNS_SERVICE_INSTANCE);
typedef VOID (WINAPI *f_rlfree)(PDNS_RECORD, DNS_FREE_TYPE);
// One context per asynchronous operation: it outlives the node's interest in it (a browse's last callback is ERROR_CANCELLED, a registration's
// last is the deregistration's) and is freed by that last callback. g_wpending counts them, so coop_destroy can wait for the stragglers
typedef struct wctx { coop_node *n; volatile LONG dead; int kind; } wctx;
static volatile LONG g_wpending;
typedef struct wimpl {
    HMODULE dll; int loaded;
    f_reg reg, dereg; f_browse browse; f_bcancel bcancel; f_construct construct; f_free ifree; f_rlfree rlfree;
    // dlk: shared with the callbacks
    wctx *bctx; DNS_SERVICE_CANCEL bcancel_h; int64_t browse_at;
    wctx *rctx; DNS_SERVICE_REGISTER_REQUEST rreq; PDNS_SERVICE_INSTANCE inst; int reg_state;   // 0 none, 1 registering, 2 registered, 3 deregistering
    uint16_t reg_port; char inst_name[32], addrs[80]; int64_t retry_at; int warned, noaddr_said;
} wimpl;
static wimpl *WI(coop_node *n) { return n->disc.impl; }
static wctx *wctx_new(coop_node *n, int kind) { wctx *c = calloc(1, sizeof *c); if (c) { c->n = n; c->kind = kind; InterlockedIncrement(&g_wpending); } return c; }
static void wctx_done(wctx *c) { free(c); InterlockedDecrement(&g_wpending); }
static void w8(char *out, size_t n, const WCHAR *w) {   // a wide name from the network as UTF-8, or "" when it does not fit
    out[0] = 0;
    if (!w) return;
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
    if (k <= 0) out[0] = 0;
}
static int wsuffix(const WCHAR *s, const WCHAR *suf) {   // s ends with suf, ignoring case
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && !_wcsicmp(s + a - b, suf);
}
static const WCHAR k_wtype[] = L"._pokopia-ldn._udp.local";
static VOID WINAPI w_browse_cb(DWORD st, PVOID ctx, PDNS_RECORD rec) {
    wctx *c = ctx; coop_node *n = c->n;
    if (st == ERROR_CANCELLED || !rec) { if (rec) WI(n)->rlfree(rec, DnsFreeRecordList); if (st == ERROR_CANCELLED) wctx_done(c); return; }
    if (c->dead) { WI(n)->rlfree(rec, DnsFreeRecordList); return; }
    for (DNS_RECORDW *p = (DNS_RECORDW *)rec; p; p = p->pNext) {
        if (p->wType != DNS_TYPE_PTR || !p->Data.PTR.pNameHost || !wsuffix(p->Data.PTR.pNameHost, k_wtype)) continue;
        const WCHAR *inst = p->Data.PTR.pNameHost;
        char name[64]; WCHAR wn[64]; size_t il = wcslen(inst) - wcslen(k_wtype);
        if (il == 0 || il >= 64) { cplat_lock(&n->dlk); n->disc.bad++; cplat_unlock(&n->dlk); continue; }
        memcpy(wn, inst, il * sizeof(WCHAR)); wn[il] = 0; w8(name, sizeof name, wn);
        if (p->dwTtl == 0) { if (name[0]) lost(n, name, 0); continue; }   // a goodbye
        if (!name[0] || !strcmp(name, WI(n)->inst_name)) continue;
        const WCHAR *target = NULL; WORD port = 0; cdisc_entry txt; memset(&txt, 0, sizeof txt);
        int have_txt = 0;
        for (DNS_RECORDW *q = (DNS_RECORDW *)rec; q; q = q->pNext) {
            if (!q->pName || _wcsicmp(q->pName, inst)) continue;
            if (q->wType == DNS_TYPE_SRV && q->Data.SRV.pNameTarget) { target = q->Data.SRV.pNameTarget; port = q->Data.SRV.wPort; }
            if (q->wType == DNS_TYPE_TEXT && !have_txt) {   // the strings as a TXT record's wire form, for the shared parser
                uint8_t w[512]; size_t at = 0;
                for (DWORD i = 0; i < q->Data.TXT.dwStringCount && i < 16; i++) {
                    char kv[256]; w8(kv, sizeof kv, q->Data.TXT.pStringArray[i]);
                    at = txt_put(w, sizeof w, at, kv);
                }
                have_txt = cdisc_txt_parse(w, at, &txt);
            }
        }
        uint32_t ip = 0;
        for (DNS_RECORDW *q = (DNS_RECORDW *)rec; q && target; q = q->pNext)   // a responder that sends its A record along (Apple's does)
            if (q->wType == DNS_TYPE_A && q->pName && !_wcsicmp(q->pName, target)) {
                uint32_t a = ntohl(q->Data.A.IpAddress);
                if (coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(a))) { ip = a; break; }
            }
        if (!target || !port || !have_txt) continue;   // a later answer carries the rest
        found(n, name, 0, ip, port, &txt, 1);
    }
    WI(n)->rlfree(rec, DnsFreeRecordList);
}
static VOID WINAPI w_reg_cb(DWORD st, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    wctx *c = ctx; coop_node *n = c->n; wimpl *w = WI(n);
    if (inst) w->ifree(inst);
    cplat_lock(&n->dlk);
    int state = w->reg_state, done = 0;
    if (state == 3) { w->reg_state = 0; if (w->inst) { w->ifree(w->inst); w->inst = NULL; } w->rctx = NULL; done = 1; }
    else if (st == ERROR_SUCCESS) w->reg_state = 2;
    else { w->reg_state = 0; if (w->inst) { w->ifree(w->inst); w->inst = NULL; } w->rctx = NULL; done = 1; }
    n->disc.adv_on = w->reg_state == 2;
    cplat_unlock(&n->dlk);
    if (state == 3) coop_logf(n, "disc: stopped advertising");
    else if (st == ERROR_SUCCESS) coop_logf(n, "disc: advertising \"%s\" (%s) on port %u, addresses %s", w->inst_name, COOP_SERVICE, w->reg_port, w->addrs);
    else coop_logf(n, "disc: advertising failed (status %lu)", (unsigned long)st);
    if (done) wctx_done(c);
}
void cdisc_init(coop_node *n) {
    wimpl *w = calloc(1, sizeof *w);
    n->disc.impl = w;
    if (!w) return;
    w->dll = LoadLibraryW(L"dnsapi.dll");
    if (!w->dll) return;
#define LOADF(f, name) (*(FARPROC *)&w->f = GetProcAddress(w->dll, name))
    LOADF(reg, "DnsServiceRegister"); LOADF(dereg, "DnsServiceDeRegister"); LOADF(browse, "DnsServiceBrowse"); LOADF(bcancel, "DnsServiceBrowseCancel");
    LOADF(construct, "DnsServiceConstructInstance"); LOADF(ifree, "DnsServiceFreeInstance"); LOADF(rlfree, "DnsRecordListFree");
#undef LOADF
    w->loaded = w->reg && w->dereg && w->browse && w->bcancel && w->construct && w->ifree && w->rlfree;
}
static void w_stop_browse(coop_node *n, int refresh) {
    wimpl *w = WI(n);
    cplat_lock(&n->dlk); wctx *c = w->bctx; w->bctx = NULL; DNS_SERVICE_CANCEL h = w->bcancel_h; cplat_unlock(&n->dlk);
    if (!c) return;
    c->dead = 1;
    if (w->bcancel(&h) != ERROR_SUCCESS) {}   // its ERROR_CANCELLED callback frees the context
    if (!refresh) n->disc.browse_on = 0;
}
static void w_deregister(coop_node *n) {
    wimpl *w = WI(n);
    cplat_lock(&n->dlk);
    int go = w->reg_state == 2;
    if (go) w->reg_state = 3;
    cplat_unlock(&n->dlk);
    if (go && w->dereg(&w->rreq, NULL) != DNS_REQUEST_PENDING) {   // no callback will come
        cplat_lock(&n->dlk); wctx *c = w->rctx; w->rctx = NULL; w->reg_state = 0; if (w->inst) { w->ifree(w->inst); w->inst = NULL; } n->disc.adv_on = 0; cplat_unlock(&n->dlk);
        if (c) wctx_done(c);
    }
}
void cdisc_fini(coop_node *n) {
    wimpl *w = WI(n);
    if (!w) return;
    if (w->loaded) {
        w_stop_browse(n, 0); w_deregister(n);
        for (int i = 0; i < 200 && g_wpending; i++) Sleep(10);   // the last callbacks name this node
    }
    if (w->dll && !g_wpending) FreeLibrary(w->dll);
    free(w); n->disc.impl = NULL;
}
csock cdisc_fd(coop_node *n) { (void)n; return CSOCK_BAD; }
void cdisc_process(coop_node *n) { (void)n; }
void cdisc_tick(coop_node *n) {
    wimpl *w = WI(n); cdisc *d = &n->disc; int64_t now = now_ms();
    if (!w) return;
    if (!w->loaded) {
        if ((d->adv_want || d->browse_want) && !w->warned) { w->warned = 1; coop_logf(n, "disc: DNS-SD needs Windows 10 1809 or later (dnsapi.dll has no DnsService*): join by address (DM_COOP=join:<IPv4>)"); }
        return;
    }
    cplat_lock(&n->dlk); int rs = w->reg_state; cplat_unlock(&n->dlk);
    if (rs == 2 && (!d->adv_want || w->reg_port != d->adv_port)) w_deregister(n);
    if (w->bctx && (!d->browse_want || now - w->browse_at >= WBROWSE_REFRESH_MS)) w_stop_browse(n, d->browse_want);
    if (!d->browse_want) {
        int any = 0; cplat_lock(&n->dlk); for (int i = 0; i < DISC_MAX; i++) any |= n->disc.e[i].used; cplat_unlock(&n->dlk);
        if (any) clear_entries(n);
    } else {
        cplat_lock(&n->dlk);
        for (int i = 0; i < DISC_MAX; i++) if (d->e[i].used && d->e[i].seen && now - d->e[i].seen > WENTRY_STALE_MS) memset(&d->e[i], 0, sizeof d->e[i]);
        cplat_unlock(&n->dlk);
    }
    if (now < w->retry_at) return;
    if (d->adv_want && rs == 0) {
        uint32_t ips[DISC_ADDRS]; int k = host_addrs(ips, w->addrs, sizeof w->addrs);
        if (!k) {
            w->retry_at = now + DISC_RETRY_MS;
            if (!w->noaddr_said++) coop_logf(n, "disc: no LAN address to advertise (this PC has no private or link-local address): visitors join by address");
        } else {
            WCHAR inst[96], host[48], wv[5][80]; char v[5][80];
            make_instance(w->inst_name, sizeof w->inst_name);
            swprintf(inst, 96, L"%hs%ls", w->inst_name, k_wtype);
            swprintf(host, 48, L"pokopia-%hs.local", w->inst_name + 8);   // a name of its own: the computer's name is not published
            snprintf(v[0], 80, "%d", n->proto); snprintf(v[1], 80, "%s", n->build[0] ? n->build : "-"); snprintf(v[2], 80, "%016llx", (unsigned long long)n->mods);
            addr_list(v[3], 80, ips, 1); addr_list(v[4], 80, ips + 1, k - 1);
            for (int i = 0; i < 5; i++) swprintf(wv[i], 80, L"%hs", v[i]);
            PCWSTR keys[5] = {L"p", L"b", L"m", L"a", L"x"}, vals[5] = {wv[0], wv[1], wv[2], wv[3], wv[4]};
            IP4_ADDRESS a4 = htonl(ips[0]);   // one A record: the best address; a visitor on another of this PC's networks finds its own in x=
            PDNS_SERVICE_INSTANCE si = w->construct(inst, host, &a4, NULL, d->adv_port, 0, 0, k > 1 ? 5 : 4, keys, vals);
            wctx *c = si ? wctx_new(n, 2) : NULL;
            if (!si || !c) { if (si) w->ifree(si); w->retry_at = now + DISC_RETRY_MS; }
            else {
                cplat_lock(&n->dlk);
                w->inst = si; w->rctx = c; w->reg_port = d->adv_port; w->reg_state = 1;
                memset(&w->rreq, 0, sizeof w->rreq);
                w->rreq.Version = DNS_QUERY_REQUEST_VERSION1; w->rreq.pServiceInstance = si; w->rreq.pRegisterCompletionCallback = w_reg_cb; w->rreq.pQueryContext = c;
                cplat_unlock(&n->dlk);
                DWORD st = w->reg(&w->rreq, NULL);
                if (st != DNS_REQUEST_PENDING) {
                    cplat_lock(&n->dlk); w->reg_state = 0; w->rctx = NULL; w->inst = NULL; cplat_unlock(&n->dlk);
                    w->ifree(si); wctx_done(c); w->retry_at = now + DISC_RETRY_MS;
                    coop_logf(n, "disc: advertising failed to start (status %lu)", (unsigned long)st);
                }
            }
        }
    }
    if (d->browse_want && !w->bctx) {
        wctx *c = wctx_new(n, 1);
        if (!c) return;
        DNS_SERVICE_BROWSE_REQUEST br; memset(&br, 0, sizeof br);
        br.Version = DNS_QUERY_REQUEST_VERSION1; br.QueryName = L"_pokopia-ldn._udp.local"; br.pBrowseCallback = w_browse_cb; br.pQueryContext = c;
        DNS_SERVICE_CANCEL h; memset(&h, 0, sizeof h);
        cplat_lock(&n->dlk); w->bctx = c; cplat_unlock(&n->dlk);
        DNS_STATUS st = w->browse(&br, &h);
        if (st != DNS_REQUEST_PENDING) {
            cplat_lock(&n->dlk); w->bctx = NULL; cplat_unlock(&n->dlk);
            wctx_done(c); w->retry_at = now + DISC_RETRY_MS;
            coop_logf(n, "disc: browsing failed to start (status %ld)", (long)st);
        } else {
            cplat_lock(&n->dlk); if (w->bctx == c) w->bcancel_h = h; cplat_unlock(&n->dlk);
            if (!d->browse_on) coop_logf(n, "disc: browsing for %s", COOP_SERVICE);
            d->browse_on = 1; w->browse_at = now;
        }
    }
}
void cdisc_stop(coop_node *n) {
    wimpl *w = WI(n);
    if (w && w->loaded) { w_stop_browse(n, 0); w_deregister(n); }
    clear_entries(n);
}

#else
// ---------- elsewhere (Linux, Android until its NsdManager glue): no DNS-SD; join by address ----------
void cdisc_init(coop_node *n) { n->disc.impl = NULL; }
void cdisc_fini(coop_node *n) { (void)n; }
csock cdisc_fd(coop_node *n) { (void)n; return CSOCK_BAD; }
void cdisc_process(coop_node *n) { (void)n; }
void cdisc_tick(coop_node *n) {
    static int warned;
    if ((n->disc.adv_want || n->disc.browse_want) && !warned) { warned = 1; coop_logf(n, "disc: no DNS-SD on this platform: visitors join by address (DM_COOP=join:<IPv4>)"); }
}
void cdisc_stop(coop_node *n) { clear_entries(n); }
#endif
