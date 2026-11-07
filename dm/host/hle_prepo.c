// Native play-report sessions. Reports are validated and consumed locally under
// the host's offline telemetry policy; completion does not imply an upload.
#include "sf_service.h"
#include <mach/mach.h>
extern uint64_t dm_entropy64(void);

#define PP_ARGUMENT 0x281u
#define PP_NAME 0xa81u
#define PP_RESOURCE 0xc81u
#define PP_PAYLOAD 0x1281u
#define PP_PERMISSION 0xb481u
#define PP_SHAPE 0x1ce0au
#define PP_COMMAND 0x1ba0au

enum { PP_BOOL = 1, PP_BIN, PP_EXT, PP_F32, PP_F64, PP_UINT, PP_INT, PP_STR, PP_ARRAY, PP_MAP };
typedef struct { const uint8_t *p, *end; } pp_reader;
typedef struct { unsigned type, ext; uint64_t n; const uint8_t *data; } pp_token;
static int pp_take(pp_reader *r, unsigned n, uint64_t *value) {
    if ((size_t)(r->end - r->p) < n) return 0;
    uint64_t v = 0; for (unsigned k = 0; k < n; k++) v = v << 8 | *r->p++;
    *value = v; return 1;
}
static int pp_utf8(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], count, minimum;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; minimum = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; minimum = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; minimum = 0x10000; c &= 7; }
        else return 0;
        if (count > n - i) return 0;
        while (count--) { unsigned b = s[i++]; if ((b & 0xc0) != 0x80) return 0; c = c << 6 | (b & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}
static int pp_next(pp_reader *r, pp_token *t) {
    uint64_t b; if (!pp_take(r, 1, &b)) return 0;
    *t = (pp_token){0}; unsigned width = 0;
    if (b <= 0x7f || b >= 0xe0) { t->type = b <= 0x7f ? PP_UINT : PP_INT; t->n = b; }
    else if (b >= 0xa0 && b <= 0xbf) { t->type = PP_STR; t->n = b & 31; }
    else if (b >= 0x90 && b <= 0x9f) { t->type = PP_ARRAY; t->n = b & 15; }
    else if (b >= 0x80 && b <= 0x8f) { t->type = PP_MAP; t->n = b & 15; }
    else if (b == 0xc2 || b == 0xc3) t->type = PP_BOOL;
    else if (b >= 0xc4 && b <= 0xc6) { t->type = PP_BIN; width = 1u << (b - 0xc4); }
    else if (b >= 0xc7 && b <= 0xc9) { t->type = PP_EXT; width = 1u << (b - 0xc7); }
    else if (b == 0xca || b == 0xcb) { t->type = b == 0xca ? PP_F32 : PP_F64; width = b == 0xca ? 4 : 8; }
    else if (b >= 0xcc && b <= 0xcf) { t->type = PP_UINT; width = 1u << (b - 0xcc); }
    else if (b >= 0xd0 && b <= 0xd3) { t->type = PP_INT; width = 1u << (b - 0xd0); }
    else if (b >= 0xd4 && b <= 0xd8) { t->type = PP_EXT; t->n = 1u << (b - 0xd4); }
    else if (b >= 0xd9 && b <= 0xdb) { t->type = PP_STR; width = 1u << (b - 0xd9); }
    else if (b == 0xdc || b == 0xdd) { t->type = PP_ARRAY; width = b == 0xdc ? 2 : 4; }
    else if (b == 0xde || b == 0xdf) { t->type = PP_MAP; width = b == 0xde ? 2 : 4; }
    else return 0; // nil and reserved tokens are not report values
    if (width && !pp_take(r, width, &t->n)) return 0;
    if (t->type == PP_EXT) { if (!pp_take(r, 1, &b)) return 0; t->ext = (unsigned)b; }
    if (t->type == PP_STR || t->type == PP_BIN || t->type == PP_EXT) {
        if (t->n > (uint64_t)(r->end - r->p)) return 0;
        t->data = r->p; r->p += t->n;
        if (t->type == PP_STR && !pp_utf8(t->data, t->n)) return 0;
    }
    return 1;
}
static unsigned pp_class(unsigned type) { return type == PP_INT ? PP_UINT : type == PP_F64 ? PP_F32 : type; }
static int pp_key(const pp_token *t, int extended) {
    if (t->type != PP_STR || !t->data || !t->n || t->n > 64) return 0;
    for (size_t i = 0; i < t->n; i++) {
        unsigned c = t->data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') continue;
        if ((!extended || i) && c >= '0' && c <= '9') continue;
        if (!extended && c == ':') continue;
        return 0;
    }
    return 1;
}
static int pp_key_equal(const pp_token *a, const pp_token *b) {
    if (!a->data || !b->data || a->n != b->n) return 0;
    for (size_t i = 0; i < a->n; i++) {
        unsigned x = a->data[i], y = b->data[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return 1;
}
// Compare already-validated map/array schemas, including key order and nested
// lengths. Numeric widths and signedness do not change the schema.
static int pp_schema(pp_reader *a, pp_reader *b, unsigned depth) {
    pp_token x, y;
    if (depth > 4 || !pp_next(a, &x) || !pp_next(b, &y) || pp_class(x.type) != pp_class(y.type)) return 0;
    if (x.type != PP_ARRAY && x.type != PP_MAP) return 1;
    if (x.n != y.n) return 0;
    for (uint64_t i = 0; i < x.n; i++) {
        if (x.type == PP_MAP) {
            pp_token k, l;
            if (!pp_next(a, &k) || !pp_next(b, &l) || k.n != l.n || memcmp(k.data, l.data, k.n)) return 0;
        }
        if (!pp_schema(a, b, depth + 1)) return 0;
    }
    return 1;
}
static int pp_value(pp_reader *r, pp_token t, unsigned depth, int extended);
static int pp_map(pp_reader *r, uint64_t count, unsigned depth, int extended) {
    if (depth > 3 || count > (uint64_t)(r->end - r->p) / 2 || (!depth && (!count || count > 100))) return 0;
    pp_token *keys = count ? calloc((size_t)count, sizeof *keys) : NULL;
    if (count && !keys) return 0;
    int valid = 1;
    for (uint64_t i = 0; valid && i < count; i++) {
        if (!pp_next(r, &keys[i]) || !pp_key(&keys[i], extended)) { valid = 0; break; }
        for (uint64_t j = 0; j < i; j++) if (pp_key_equal(&keys[i], &keys[j])) { valid = 0; break; }
        pp_token v;
        if (valid && (!pp_next(r, &v) || !pp_value(r, v, depth, extended))) valid = 0;
    }
    free(keys); return valid;
}
static int pp_array(pp_reader *r, uint64_t count, unsigned depth) {
    if (depth > 3 || count > (uint64_t)(r->end - r->p)) return 0;
    unsigned type = 0; const uint8_t *first = r->p;
    for (uint64_t i = 0; i < count; i++) {
        const uint8_t *start = r->p; pp_token t;
        if (!pp_next(r, &t) || t.type == PP_ARRAY || (i && pp_class(t.type) != type)) return 0;
        type = pp_class(t.type);
        if (!pp_value(r, t, depth, 1)) return 0;
        if (i && type == PP_MAP) {
            pp_reader a = {first, r->end}, b = {start, r->end};
            if (!pp_schema(&a, &b, depth)) return 0;
        }
    }
    return 1;
}
static int pp_value(pp_reader *r, pp_token t, unsigned depth, int extended) {
    if (!extended) return t.type == PP_UINT || t.type == PP_INT || t.type == PP_F64 || t.type == PP_STR || (t.type == PP_EXT && !t.ext && t.n == 8);
    switch (t.type) {
    case PP_BOOL: case PP_UINT: case PP_INT: case PP_STR: return 1;
    case PP_BIN: return !depth;
    case PP_F32: return (t.n & 0x7fffffff) <= 0x7f7fffff;
    case PP_F64: return (t.n & UINT64_C(0x7fffffffffffffff)) <= UINT64_C(0x7fefffffffffffff);
    case PP_EXT: return (!t.ext && t.n == 8) || (!depth && ((t.ext == 0x40 && t.n == 16) || (t.ext == 0x41 && t.n == 1) || (t.ext == 0x42 && t.n == 10)));
    case PP_ARRAY: return pp_array(r, t.n, depth + 1);
    case PP_MAP: return pp_map(r, t.n, depth + 1, 1);
    default: return 0;
    }
}
static int pp_payload(const void *data, size_t size, int extended) {
    if (!data || !size) return 0;
    pp_reader r = {data, (const uint8_t *)data + size}; pp_token t;
    return pp_next(&r, &t) && t.type == PP_MAP && pp_map(&r, t.n, 0, extended) && r.p == r.end;
}
static int pp_read(void *out, const void *data, size_t size) {
    if (!data || size > UINTPTR_MAX - (uintptr_t)data) return 0;
    vm_size_t copied = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)data, size, (vm_address_t)out, &copied) == KERN_SUCCESS && copied == size;
}
static Result pp_name(sf_buffer name) {
    char text[32];
    if (!name.size || name.size > sizeof text || !pp_read(text, name.data, name.size) || text[name.size - 1]) return PP_ARGUMENT;
    if (!text[0]) return PP_NAME;
    for (size_t i = 0; text[i]; i++) if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) return PP_NAME;
    return RES_OK;
}
static int pp_shape(const sf_request *q, size_t bytes, unsigned x, unsigned a, unsigned copy, int pid) {
    return q->size >= bytes && q->nx == x && q->na == a && !q->nb && !q->nw && !q->nc &&
           q->ncopy == copy && !q->nmove && !q->nobjects && q->send_pid == pid && (!x || !q->x[0].mode) && (!a || !q->a[0].mode);
}
static uint32_t pp_u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int pp_uid(const uint8_t *p) { uint64_t v[2]; memcpy(v, p, sizeof v); return v[0] || v[1]; }
typedef struct { uint32_t read, write; dm_transfer_memory *memory; } pp_task;
static void pp_task_close(void *state) {
    pp_task *t = state;
    sf_close_handle(t->read); sf_close_handle(t->write); dm_transfer_unmap(t->memory); free(t);
}
static Result pp_task_request(void *state, const sf_request *q, sf_reply *r) {
    pp_task *t = state;
    if (q->command > 2) return PP_COMMAND;
    if (!pp_shape(q, 0, 0, 0, 0, 0)) return PP_SHAPE;
    if (!q->command) { r->copy = hle_dup_readable_handle(t->read); if (!r->copy) return PP_RESOURCE; }
    // Consumption finishes before publication in the offline host. Command 1
    // is an acknowledgment, not cancellation; command 2 returns the CMIF result.
    return RES_OK;
}
static Result pp_submit(unsigned permissions, const sf_request *q, sf_reply *r) {
    unsigned cmd = q->command;
    int async = cmd == 10500 || cmd == 10501, system = cmd >= 20100;
    int uid = async ? cmd == 10500 : cmd & 1;
    int flag = async || (system ? cmd >= 20102 : cmd >= 10106);
    size_t bytes = async ? (uid ? 40 : 24) : 8 + (uid ? 16 : 0) + (flag ? 8 : 0);
    if (!pp_shape(q, bytes, 1, !async, async, !system)) return PP_SHAPE;
    if (!(permissions & (system ? 2u : 1u))) return PP_PERMISSION;
    if (!system && q->pid != DM_GUEST_PROCESS_ID) return 0xca81;
    if (uid && !pp_uid(q->data + (async ? 16 : flag ? 8 : 0))) return PP_ARGUMENT;
    Result result = pp_name(q->x[0]); if (result) return result;
    dm_transfer_memory *memory = NULL;
    const void *data = NULL; void *copy = NULL;
    size_t size = async ? pp_u32(q->data + 8) : q->a[0].size;
    if (async) {
        size_t mapped = pp_u32(q->data + 4);
        result = dm_transfer_map(q->copies[0], mapped, 0, &memory, &data);
        if (result) return result;
        if (size > mapped) result = PP_PAYLOAD;
    } else {
        if (!size || size > 65536) return PP_PAYLOAD;
        copy = malloc(size); if (!copy) return PP_RESOURCE;
        if (!pp_read(copy, q->a[0].data, size)) result = PP_PAYLOAD;
        data = copy;
    }
    if (!result && !pp_payload(data, size, async || cmd > 10101)) result = PP_PAYLOAD;
    free(copy);
    if (result || !async) { dm_transfer_unmap(memory); return result; }
    pp_task *task = calloc(1, sizeof *task);
    if (!task) { dm_transfer_unmap(memory); return PP_RESOURCE; }
    task->memory = memory; task->read = hle_new_event(&task->write);
    extern int svc_thread(int num, uint64_t *regs);
    uint64_t regs[32] = {task->write}; svc_thread(0x11, regs);
    if (regs[0]) { pp_task_close(task); return (Result)regs[0]; }
    r->child = (sf_object){pp_task_request, pp_task_close, task};
    return RES_OK;
}
static Result pp_request(void *state, const sf_request *q, sf_reply *r) {
    unsigned permissions = (unsigned)(uintptr_t)state, cmd = q->command;
    if ((cmd >= 10100 && cmd <= 10107) || cmd == 10500 || cmd == 10501 || (cmd >= 20100 && cmd <= 20103)) return pp_submit(permissions, q, r);
    unsigned required = cmd == 10400 ? 1 : cmd == 20200 ? 2 : (cmd >= 30100 && cmd <= 50100) ? 4 : cmd == 90100 ? 0x80000000u : 0;
    if (required && !(permissions & required)) return PP_PERMISSION;
    if (cmd == 10200 || cmd == 10300 || cmd == 10400 || cmd == 20200) {
        if (!pp_shape(q, cmd == 20200 ? 8 : 0, 0, 0, 0, 0)) return PP_SHAPE;
        if (cmd == 10300) { r->size = 4; memset(r->data, 0, 4); } // no queued uploads
        if (cmd == 10400) {
            static uint64_t session; if (!session) session = dm_entropy64() | 1;
            memcpy(r->data, &session, 8); r->size = 8;
        }
        return RES_OK;
    }
    LOG("[RED] prepo: unsupported command %u", cmd);
    return PP_COMMAND;
}
sf_object sf_prepo_root(unsigned permissions) { return (sf_object){pp_request, NULL, (void *)(uintptr_t)permissions}; }
