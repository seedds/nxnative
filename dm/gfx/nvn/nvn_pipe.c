// Neutral render-pipeline description and the prewarm lists' pipeline rows (see nvn_pipe.h for the description and the P row format)
#include "nvn_pipe.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int say(char *why, size_t n, int rc, const char *fmt, ...) {
    if (why && n) { va_list ap; va_start(ap, fmt); vsnprintf(why, n, fmt, ap); va_end(ap); }
    return rc;
}

// The state objects field by field into zeroed ones: the bits outside NVN's fields stay zero, so a description compares byte for byte and its P row says everything it holds
static blend_st blend_clean(blend_st b) {
    blend_st o; memset(&o, 0, sizeof o);
    o.target = b.target; o.src_rgb = b.src_rgb; o.dst_rgb = b.dst_rgb; o.src_a = b.src_a; o.dst_a = b.dst_a;
    o.eq_rgb = b.eq_rgb; o.eq_a = b.eq_a; o.adv = b.adv; o.adv_npm = b.adv_npm; o.adv_ovl = b.adv_ovl;
    return o;
}
static va_st attr_clean(va_st a) { va_st o; memset(&o, 0, sizeof o); o.fmt = a.fmt; o.offset = a.offset; o.stream = a.stream; return o; }
static vs_st stream_clean(vs_st s) { vs_st o; memset(&o, 0, sizeof o); o.stride = s.stride; o.divisor = s.divisor; return o; }
static ds_st ds_clean(ds_st d) {
    ds_st o; memset(&o, 0, sizeof o);
    o.dt = d.dt; o.dw = d.dw; o.df = d.df; o.st = d.st; o.sf_f = d.sf_f; o.sf_b = d.sf_b;
    o.fail_f = d.fail_f; o.zfail_f = d.zfail_f; o.zpass_f = d.zpass_f; o.fail_b = d.fail_b; o.zfail_b = d.zfail_b; o.zpass_b = d.zpass_b;
    return o;
}

// A shader key a P row can carry: printable ASCII without the row's separators, the bytes after its end zero
static int key_text(const char k[24]) {
    int i = 0;
    for (; i < 24 && k[i]; i++) if (k[i] < 0x21 || k[i] > 0x7e || k[i] == ',' || k[i] == '@' || k[i] == '=') return 0;
    if (i == 1 && k[0] == '-') return 0;   // '-' is the row's "no shader"
    for (; i < 24; i++) if (k[i]) return 0;
    return 1;
}

static uint32_t to_mtl(uint16_t f) { uint32_t m = 0; if (f) nvn_fmt_to_mtl(f, &m, NULL); return m; }
// A Metal format of the key → NVN: 0 for 0; the first candidate, with *guess set when there are several; -1 when none
static int from_mtl(uint32_t m, uint16_t *f, int *guess) {
    *f = 0; *guess = 0;
    if (!m) return 0;
    uint16_t c[1]; int n = nvn_fmt_from_mtl(m, NULL, c, 1);
    if (n < 1) return -1;
    *f = c[0]; *guess = n > 1;
    return 0;
}

int nvn_pipe_from_key(const pso_skey *k, nvn_pipe_desc *out, char *why, size_t n) {
    memset(out, 0, sizeof *out);
    const pso_key *p = &k->k;
    int lm = 0; for (int i = 0; i < 8; i++) lm |= p->lmap[i];
    if (k->merged || lm) return say(why, n, -1, "merged pass");   // lmap maps physical attachments to logical targets of a pass the Metal backend merged
    { pso_fz_desc z; memset(&z, 0, sizeof z); if (k->fz || memcmp(&k->fz_desc, &z, sizeof z)) return say(why, n, -1, "readable-tile variant"); }
    if (p->vs || p->fs || p->fsfn) return say(why, n, -1, "live pointers in the key");
    if (!key_text(k->vs) || !key_text(k->fs)) return say(why, n, -1, "shader key is not text");
    { const uint8_t *b = (const uint8_t *)k; for (size_t i = offsetof(pso_skey, fz_desc) + sizeof k->fz_desc; i < offsetof(pso_skey, k); i++) if (b[i]) return say(why, n, -1, "bits outside the NVN fields"); }
    if (p->samples > 0xff || p->blend_en > 0xff || p->a2c > 0xff) return say(why, n, -1, "field out of range");
    memcpy(out->vs.key, k->vs, 24); out->vs.stage = k->vs_stage;
    memcpy(out->fs.key, k->fs, 24); out->fs.stage = k->fs_stage;
    for (int i = 0; i < 10; i++) {
        uint32_t m = i < 8 ? p->cfmt[i] : i == 8 ? p->dfmt : p->sfmt;
        uint16_t *f = i < 8 ? &out->cfmt[i] : i == 8 ? &out->dfmt : &out->sfmt; int g;
        if (from_mtl(m, f, &g)) { memset(out, 0, sizeof *out); return say(why, n, -1, "Metal format %u has no NVN source", m); }
        out->fmt_guess |= (uint16_t)(g << i);
    }
    out->samples = (uint8_t)p->samples; out->blend_en = (uint8_t)p->blend_en; out->a2c = (uint8_t)p->a2c; out->chmask = p->chmask;
    for (int i = 0; i < 8; i++) out->blend[i] = blend_clean(p->blend[i]);
    for (int i = 0; i < 16; i++) { out->attr[i] = attr_clean(p->attr[i]); out->stream[i] = stream_clean(p->stream[i]); }
    if (memcmp(out->blend, p->blend, sizeof out->blend) || memcmp(out->attr, p->attr, sizeof out->attr) || memcmp(out->stream, p->stream, sizeof out->stream)) {
        memset(out, 0, sizeof *out); return say(why, n, -1, "bits outside the NVN fields");
    }
    return say(why, n, 0, "");
}

int nvn_pipe_to_key(const nvn_pipe_desc *d, pso_skey *out) {
    memset(out, 0, sizeof *out);
    memcpy(out->vs, d->vs.key, 24); out->vs_stage = d->vs.stage;
    memcpy(out->fs, d->fs.key, 24); out->fs_stage = d->fs.stage;
    pso_key *k = &out->k; int bad = 0;
    for (int i = 0; i < 8; i++) { k->cfmt[i] = to_mtl(d->cfmt[i]); bad |= d->cfmt[i] && !k->cfmt[i]; }
    k->dfmt = to_mtl(d->dfmt); k->sfmt = to_mtl(d->sfmt); bad |= (d->dfmt && !k->dfmt) || (d->sfmt && !k->sfmt);
    k->samples = d->samples; k->blend_en = d->blend_en; k->chmask = d->chmask; k->a2c = d->a2c;
    memcpy(k->blend, d->blend, sizeof k->blend); memcpy(k->attr, d->attr, sizeof k->attr); memcpy(k->stream, d->stream, sizeof k->stream);
    pso_key_norm(k, 0);
    return bad ? -1 : 0;
}

int nvn_pipe_from_state(nvn_pipe_desc *out, const nvn_draw_state *s, const char vs_key[24], int vs_stage, uint32_t vs_inputs, const char *fs_key, int fs_stage,
                        const uint16_t cfmt[8], uint16_t dfmt, uint16_t sfmt, uint32_t samples) {
    memset(out, 0, sizeof *out);
    if (samples > 0xff) return -1;
    if (vs_key) memcpy(out->vs.key, vs_key, 24);
    out->vs.stage = (uint8_t)vs_stage;
    if (fs_key) { memcpy(out->fs.key, fs_key, 24); out->fs.stage = (uint8_t)fs_stage; }
    memcpy(out->cfmt, cfmt, sizeof out->cfmt); out->dfmt = dfmt; out->sfmt = sfmt; out->samples = (uint8_t)samples;
    // pso_key_fill: blend enables, masks and alpha to coverage from S, a target's blend state where the pass attaches it, the vertex shader's attributes and their streams
    out->blend_en = (uint8_t)s->color.blend_en; out->chmask = s->chmask.mask; out->a2c = s->ms.a2c;
    for (int i = 0; i < 8; i++) if (cfmt[i]) out->blend[i] = blend_clean(s->blend[i]);
    for (int a = 0; a < 16; a++) {
        if (!(vs_inputs >> a & 1) || a >= s->nattr) continue;
        out->attr[a] = attr_clean(s->attr[a]);
        int st = s->attr[a].stream; if (st < 16) out->stream[st] = stream_clean(s->stream[st]);
    }
    // pso_key_norm for a normal pass, on the NVN formats
    for (int i = 0; i < 8; i++) {
        if (!cfmt[i]) { out->blend_en &= (uint8_t)~(1u << i); out->chmask &= ~(0xfu << (4 * i)); }
        if (!(out->blend_en >> i & 1)) memset(&out->blend[i], 0, sizeof out->blend[i]);
    }
    out->xstate = 1;
    out->ds = ds_clean(s->ds);
    out->cull = (uint8_t)s->poly.cull; out->front = (uint8_t)s->poly.front; out->poly_mode = (uint8_t)s->poly.mode; out->bias_en = (uint8_t)s->poly.offset_en;
    out->depth_clamp = s->depth_clamp != 0; out->rast_discard = s->rast_discard != 0; out->restart = s->restart.on != 0;
    out->logic_op = (uint8_t)s->color.logic_op; out->alpha_func = (uint8_t)s->color.alpha_func;
    return 0;
}

// ---------- P rows ----------
typedef struct { char *p; size_t left; int bad; } wbuf;
static void put(wbuf *w, const char *fmt, ...) {
    if (w->bad) return;
    va_list ap; va_start(ap, fmt); int r = vsnprintf(w->p, w->left, fmt, ap); va_end(ap);
    if (r < 0 || (size_t)r >= w->left) { w->bad = 1; return; }
    w->p += r; w->left -= (size_t)r;
}
static void put_key(wbuf *w, const char *name, const nvn_pipe_stage *s) {
    if (!key_text(s->key)) { w->bad = 1; return; }
    if (s->key[0]) put(w, "\t%s=%.24s@%u", name, s->key, s->stage); else put(w, "\t%s=-@%u", name, s->stage);
}
static void put_fmt(wbuf *w, uint16_t f) { if (f) put(w, "%#x", f); else put(w, "0"); }

int nvn_pipe_write_row(const nvn_pipe_desc *d, char *out, size_t n) {
    if (!out || !n) return -1;
    wbuf w = {out, n, 0};
    put(&w, "P\tv%d", NVN_PIPE_ROW_VERSION);
    put_key(&w, "vs", &d->vs); put_key(&w, "fs", &d->fs);
    put(&w, "\tc=");
    for (int i = 0; i < 8; i++) { if (i) put(&w, ","); put_fmt(&w, d->cfmt[i]); }
    put(&w, "\tds="); put_fmt(&w, d->dfmt); put(&w, ","); put_fmt(&w, d->sfmt);
    put(&w, "\tguess=%#x\tms=%u,%u\tblend_en=%#x\tchmask=%#x", d->fmt_guess, d->samples, d->a2c, d->blend_en, d->chmask);
    put(&w, "\tblend=");
    for (int i = 0; i < 8; i++) {
        const blend_st *b = &d->blend[i]; blend_st z; memset(&z, 0, sizeof z);
        if (i) put(&w, ",");
        if (!memcmp(b, &z, sizeof z)) put(&w, "-");
        else put(&w, "%u.%u.%u.%u.%u.%u.%u.%u.%u.%u", (unsigned)b->target, (unsigned)b->src_rgb, (unsigned)b->dst_rgb, (unsigned)b->src_a, (unsigned)b->dst_a,
                 (unsigned)b->eq_rgb, (unsigned)b->eq_a, (unsigned)b->adv, (unsigned)b->adv_npm, (unsigned)b->adv_ovl);
    }
    put(&w, "\tattr=");
    for (int i = 0; i < 16; i++) {
        const va_st *a = &d->attr[i];
        if (i) put(&w, ",");
        if (!a->fmt && !a->offset && !a->stream) put(&w, "-"); else put(&w, "%u.%u.%u", (unsigned)a->fmt, (unsigned)a->offset, (unsigned)a->stream);
    }
    put(&w, "\tstream=");
    for (int i = 0; i < 16; i++) {
        const vs_st *s = &d->stream[i];
        if (i) put(&w, ",");
        if (!s->stride && !s->divisor) put(&w, "-"); else put(&w, "%u.%u", s->stride, s->divisor);
    }
    if (!d->xstate) put(&w, "\trast=-\tdss=-");
    else {
        const ds_st *s = &d->ds;
        put(&w, "\trast=%u.%u.%u.%u.%u.%u.%u.%u.%u", d->cull, d->front, d->poly_mode, d->bias_en, d->depth_clamp, d->rast_discard, d->restart, d->logic_op, d->alpha_func);
        put(&w, "\tdss=%u.%u.%u.%u.%u.%u.%u.%u.%u.%u.%u.%u", (unsigned)s->dt, (unsigned)s->dw, (unsigned)s->df, (unsigned)s->st, (unsigned)s->sf_f, (unsigned)s->sf_b,
            (unsigned)s->fail_f, (unsigned)s->zfail_f, (unsigned)s->zpass_f, (unsigned)s->fail_b, (unsigned)s->zfail_b, (unsigned)s->zpass_b);
    }
    // A description a P row cannot say exactly is refused: fields beyond the key without xstate, reserved bytes, bits outside the state objects' fields
    { uint8_t z[sizeof d->rsv] = {0}; if (memcmp(d->rsv, z, sizeof z)) w.bad = 1; }
    if (!d->xstate) { nvn_pipe_desc t; memset(&t, 0, sizeof t); if (memcmp(&d->ds, &t.ds, offsetof(nvn_pipe_desc, rsv) - offsetof(nvn_pipe_desc, ds))) w.bad = 1; }
    if (d->xstate > 1) w.bad = 1;
    for (int i = 0; i < 8 && !w.bad; i++) { blend_st c = blend_clean(d->blend[i]); if (memcmp(&c, &d->blend[i], sizeof c)) w.bad = 1; }
    { ds_st c = ds_clean(d->ds); if (memcmp(&c, &d->ds, sizeof c)) w.bad = 1; }
    if (w.bad) { out[0] = 0; return -1; }
    return (int)(w.p - out);
}

// ---------- Parsing ----------
// A number in [0, max]: decimal, or hexadecimal with 0x; the end pointer after it
static int num(const char **s, uint64_t max, uint64_t *v) {
    const char *p = *s; int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    if (!(base == 16 ? (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') : *p >= '0' && *p <= '9')) return -1;
    uint64_t x = 0;
    for (;; p++) {
        int c = *p >= '0' && *p <= '9' ? *p - '0' : base == 16 && *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : base == 16 && *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
        if (c < 0) break;
        x = x * (uint64_t)base + (uint64_t)c;
        if (x > max) return -1;
    }
    *v = x; *s = p; return 0;
}
static int lit(const char **s, const char *t) { size_t n = strlen(t); if (strncmp(*s, t, n)) return -1; *s += n; return 0; }
// count numbers separated by sep, each within its maximum, into v
static int nums(const char **s, int count, char sep, const uint64_t *max, uint64_t *v) {
    for (int i = 0; i < count; i++) {
        if (i && *(*s)++ != sep) return -1;
        if (num(s, max[i], &v[i])) return -1;
    }
    return 0;
}
static int field_end(const char *s) { return *s == '\t' || *s == 0; }

static int parse_key(const char **s, const char *name, nvn_pipe_stage *st) {
    if (lit(s, name)) return -1;
    const char *p = *s, *at = strchr(p, '@');
    if (!at || at == p || at - p > 24) return -1;
    memset(st->key, 0, sizeof st->key);
    if (!(at - p == 1 && p[0] == '-')) {
        memcpy(st->key, p, (size_t)(at - p));
        for (const char *q = p; q < at; q++) if (*q == '\t') return -1;
        if (!key_text(st->key)) return -1;
    }
    *s = at + 1; uint64_t v;
    if (num(s, 0xff, &v)) return -1;
    st->stage = (uint8_t)v;
    return 0;
}

static int parse_p(const char *s, nvn_pipe_desc *d, char *why, size_t n) {
    memset(d, 0, sizeof *d);
    const char *p = s; uint64_t v[16];
#define BAD(what) do { memset(d, 0, sizeof *d); return say(why, n, -1, "malformed P row: %s", what); } while (0)
    if (lit(&p, "P\t")) BAD("no P");
    if (lit(&p, "v")) BAD("no version");
    if (num(&p, 1000000, &v[0]) || !field_end(p)) BAD("version");
    if (v[0] != NVN_PIPE_ROW_VERSION) { memset(d, 0, sizeof *d); return say(why, n, -1, "P row version %llu is not known (this reader knows v%d)", (unsigned long long)v[0], NVN_PIPE_ROW_VERSION); }
    if (parse_key(&p, "\tvs=", &d->vs) || !field_end(p)) BAD("vs=");
    if (parse_key(&p, "\tfs=", &d->fs) || !field_end(p)) BAD("fs=");
    static const uint64_t f8[8] = {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff};
    if (lit(&p, "\tc=") || nums(&p, 8, ',', f8, v) || !field_end(p)) BAD("c=");
    for (int i = 0; i < 8; i++) d->cfmt[i] = (uint16_t)v[i];
    if (lit(&p, "\tds=") || nums(&p, 2, ',', f8, v) || !field_end(p)) BAD("ds=");
    d->dfmt = (uint16_t)v[0]; d->sfmt = (uint16_t)v[1];
    if (lit(&p, "\tguess=") || num(&p, 0x3ff, &v[0]) || !field_end(p)) BAD("guess=");
    d->fmt_guess = (uint16_t)v[0];
    static const uint64_t ms[2] = {0xff, 0xff};
    if (lit(&p, "\tms=") || nums(&p, 2, ',', ms, v) || !field_end(p)) BAD("ms=");
    d->samples = (uint8_t)v[0]; d->a2c = (uint8_t)v[1];
    if (lit(&p, "\tblend_en=") || num(&p, 0xff, &v[0]) || !field_end(p)) BAD("blend_en=");
    d->blend_en = (uint8_t)v[0];
    if (lit(&p, "\tchmask=") || num(&p, 0xffffffffu, &v[0]) || !field_end(p)) BAD("chmask=");
    d->chmask = (uint32_t)v[0];
    if (lit(&p, "\tblend=")) BAD("blend=");
    static const uint64_t bm[10] = {7, 127, 127, 127, 127, 7, 7, 63, 1, 3};
    for (int i = 0; i < 8; i++) {
        if (i && *p++ != ',') BAD("blend=");
        if (*p == '-') { p++; continue; }
        if (nums(&p, 10, '.', bm, v)) BAD("blend=");
        blend_st *b = &d->blend[i];
        b->target = v[0]; b->src_rgb = v[1]; b->dst_rgb = v[2]; b->src_a = v[3]; b->dst_a = v[4]; b->eq_rgb = v[5]; b->eq_a = v[6]; b->adv = v[7]; b->adv_npm = v[8]; b->adv_ovl = v[9];
    }
    if (!field_end(p)) BAD("blend=");
    if (lit(&p, "\tattr=")) BAD("attr=");
    static const uint64_t am[3] = {0xff, 0x7ffff, 0x1f};
    for (int i = 0; i < 16; i++) {
        if (i && *p++ != ',') BAD("attr=");
        if (*p == '-') { p++; continue; }
        if (nums(&p, 3, '.', am, v)) BAD("attr=");
        d->attr[i].fmt = (uint32_t)v[0]; d->attr[i].offset = (uint32_t)v[1]; d->attr[i].stream = (uint32_t)v[2];
    }
    if (!field_end(p)) BAD("attr=");
    if (lit(&p, "\tstream=")) BAD("stream=");
    static const uint64_t sm[2] = {0xffffffffu, 0xffffffffu};
    for (int i = 0; i < 16; i++) {
        if (i && *p++ != ',') BAD("stream=");
        if (*p == '-') { p++; continue; }
        if (nums(&p, 2, '.', sm, v)) BAD("stream=");
        d->stream[i].stride = (uint32_t)v[0]; d->stream[i].divisor = (uint32_t)v[1];
    }
    if (!field_end(p)) BAD("stream=");
    if (lit(&p, "\trast=")) BAD("rast=");
    if (*p == '-') {
        p++;
        if (lit(&p, "\tdss=-")) BAD("dss= (rast=- needs dss=-)");
    } else {
        static const uint64_t rm[9] = {3, 1, 3, 7, 1, 1, 1, 15, 15};
        if (nums(&p, 9, '.', rm, v) || !field_end(p)) BAD("rast=");
        d->cull = (uint8_t)v[0]; d->front = (uint8_t)v[1]; d->poly_mode = (uint8_t)v[2]; d->bias_en = (uint8_t)v[3]; d->depth_clamp = (uint8_t)v[4];
        d->rast_discard = (uint8_t)v[5]; d->restart = (uint8_t)v[6]; d->logic_op = (uint8_t)v[7]; d->alpha_func = (uint8_t)v[8];
        static const uint64_t dm[12] = {1, 1, 15, 1, 15, 15, 15, 15, 15, 15, 15, 15};
        if (lit(&p, "\tdss=") || nums(&p, 12, '.', dm, v)) BAD("dss=");
        ds_st *s = &d->ds;
        s->dt = v[0]; s->dw = v[1]; s->df = v[2]; s->st = v[3]; s->sf_f = v[4]; s->sf_b = v[5];
        s->fail_f = v[6]; s->zfail_f = v[7]; s->zpass_f = v[8]; s->fail_b = v[9]; s->zfail_b = v[10]; s->zpass_b = v[11];
        d->xstate = 1;
    }
    if (*p) BAD("extra fields");
#undef BAD
    return say(why, n, 0, "");
}

int nvn_pipe_parse_row(const char *line, nvn_pipe_desc *out, char *why, size_t n) {
    memset(out, 0, sizeof *out);
    if (!line) return say(why, n, -1, "no row");
    char buf[NVN_PIPE_ROW_MAX];
    size_t len = strcspn(line, "\r\n");
    if (len >= sizeof buf) return say(why, n, -1, "row longer than %d bytes", NVN_PIPE_ROW_MAX - 1);
    memcpy(buf, line, len); buf[len] = 0;
    if (buf[0] == 'P' && buf[1] == '\t') return parse_p(buf, out, why, n) ? -1 : 0;
    if (buf[0] == 'R' && buf[1] == '\t') {
        pso_skey k;
        if (pso_skey_parse(buf + 2, &k)) return say(why, n, -1, "malformed R row: not %zu hex digits", sizeof k * 2);
        return nvn_pipe_from_key(&k, out, why, n) ? -2 : 0;
    }
    return say(why, n, 1, "not a render pipeline row");
}
