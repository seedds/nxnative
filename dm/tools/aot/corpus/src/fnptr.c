// Function pointers and vtable-like tables: functions reached only through a relocated table in data (vtables, a dispatch table), only through an
// address built in code (adrp + add, a callback), only from the init and fini arrays (constructors, destructors), direct and indirect tail calls
// (b, br through a loaded pointer), mutual recursion, and cleanup landing pads (built with -fexceptions: `cleanup` variables around calls give the
// FDEs an LSDA with call-site entries; nothing throws, so the pads never run). Discovery must find every one of these as a function entry.
#include "rt.h"

typedef struct shape shape;
typedef struct {
    uint64_t (*area)(const shape *);
    uint64_t (*perimeter)(const shape *);
    uint64_t (*scale)(shape *, uint64_t);
    const char *name;
} shape_vtable;
struct shape {
    const shape_vtable *vt;
    uint64_t w, h;
};

static uint64_t rect_area(const shape *s) { return s->w * s->h; }
static uint64_t rect_perimeter(const shape *s) { return 2 * (s->w + s->h); }
static uint64_t rect_scale(shape *s, uint64_t k) { s->w *= k; s->h *= k; return s->w + s->h; }
static uint64_t tri_area(const shape *s) { return s->w * s->h / 2; }
static uint64_t tri_perimeter(const shape *s) { return s->w + s->h + (s->w > s->h ? s->w : s->h); }
static uint64_t tri_scale(shape *s, uint64_t k) { s->w += k; return s->w * 3; }
static uint64_t sq_area(const shape *s) { return s->w * s->w; }
static uint64_t sq_perimeter(const shape *s) { return 4 * s->w; }
static uint64_t sq_scale(shape *s, uint64_t k) { s->w <<= k & 7; return s->w; }

// The vtables: in .data.rel.ro, every slot a relative relocation
static const shape_vtable rect_vt = {rect_area, rect_perimeter, rect_scale, "rect"};
static const shape_vtable tri_vt = {tri_area, tri_perimeter, tri_scale, "tri"};
static const shape_vtable sq_vt = {sq_area, sq_perimeter, sq_scale, "square"};

RT_NOINLINE uint64_t shape_total(shape *shapes, unsigned n) {
    uint64_t t = 0;
    for (unsigned i = 0; i < n; ++i) {
        t += shapes[i].vt->area(&shapes[i]) * 3 + shapes[i].vt->perimeter(&shapes[i]);
        t ^= shapes[i].vt->scale(&shapes[i], i + 1);
    }
    return t;
}

// A dispatch table of operations and an indirect tail call through it (br through the loaded pointer)
typedef uint64_t (*unop)(uint64_t);
static uint64_t op_inc(uint64_t x) { return x + 1; }
static uint64_t op_dbl(uint64_t x) { return x * 2; }
static uint64_t op_sq(uint64_t x) { return x * x; }
static uint64_t op_neg(uint64_t x) { return -x; }
static uint64_t op_rev(uint64_t x) { return __builtin_bswap64(x); }
static uint64_t op_pop(uint64_t x) { return (uint64_t)__builtin_popcountll(x); }
static unop const unops[] = {op_inc, op_dbl, op_sq, op_neg, op_rev, op_pop};

RT_NOINLINE uint64_t apply_op(unsigned i, uint64_t x) {
    return unops[i % 6](x);  // an indirect tail call: ldr + br
}

// Callbacks whose addresses are built in code (adrp + add) and passed on
static RT_NOINLINE uint64_t fold(uint64_t (*f)(uint64_t, uint64_t), const uint64_t *v, unsigned n, uint64_t init) {
    uint64_t acc = init;
    for (unsigned i = 0; i < n; ++i) {
        acc = f(acc, v[i]);
    }
    return acc;
}
static uint64_t cb_sum(uint64_t a, uint64_t b) { return a + b; }
static uint64_t cb_xor_rot(uint64_t a, uint64_t b) { return ((a << 5) | (a >> 59)) ^ b; }
static uint64_t cb_max(uint64_t a, uint64_t b) { return a > b ? a : b; }

// Direct tail calls and mutual recursion: is_even and is_odd call each other in tail position (b)
static RT_NOINLINE int is_odd(uint64_t n);
static RT_NOINLINE int is_even(uint64_t n) { return n == 0 ? 1 : is_odd(n - 1); }
static RT_NOINLINE int is_odd(uint64_t n) { return n == 0 ? 0 : is_even(n - 1); }
static RT_NOINLINE uint64_t collatz_steps(uint64_t n, uint64_t steps) {
    if (n <= 1) {
        return steps;
    }
    return collatz_steps(n & 1 ? 3 * n + 1 : n / 2, steps + 1);  // a self tail call: a loop or a b to the entry
}
static RT_NOINLINE uint64_t helper_tail(uint64_t x) { return x * 7 + 3; }
RT_NOINLINE uint64_t tail_caller(uint64_t x) { return helper_tail(x ^ 0x55); }

// Constructors and destructors: functions only the init and fini arrays name
static volatile uint64_t ctor_trace;
__attribute__((constructor(200))) static void ctor_first(void) { ctor_trace = ctor_trace * 10 + 1; }
__attribute__((constructor(300))) static void ctor_second(void) { ctor_trace = ctor_trace * 10 + 2; }
__attribute__((constructor)) static void ctor_plain(void) { ctor_trace = ctor_trace * 10 + 3; }
__attribute__((destructor)) static void dtor_plain(void) { ctor_trace = 0; }
extern volatile unsigned rt_init_ran;

// Cleanup landing pads: release() runs on the normal path; on an unwind through work() it would run from the pad
static volatile uint64_t resources, released;
typedef struct {
    uint64_t id;
} resource;
static RT_NOINLINE void release(resource *r) {
    released += r->id;
}
static RT_NOINLINE uint64_t work(resource *r, uint64_t x) {
    resources += r->id;
    return x * r->id + 1;
}
// Called through a pointer the compilers cannot see through: they must assume it may unwind, so the calls get call-site entries and pads
static uint64_t (*volatile work_ptr)(resource *, uint64_t) = work;
RT_NOINLINE uint64_t with_cleanup(uint64_t x) {
    __attribute__((cleanup(release))) resource a = {x & 0xff};
    const uint64_t y = work_ptr(&a, x);
    __attribute__((cleanup(release))) resource b = {y & 0xf};
    return work_ptr(&b, y) + work_ptr(&a, 3);
}

// A function no direct call names, reached through a pointer that lives in writable data
static uint64_t hidden_target(uint64_t x) { return x ^ 0x5a5a5a5a5a5a5a5aull; }
uint64_t (*volatile hidden_ptr)(uint64_t) = hidden_target;

int main(void) {
    shape shapes[] = {{&rect_vt, 3, 4}, {&tri_vt, 6, 8}, {&sq_vt, 5, 0}, {&rect_vt, 10, 1}, {&sq_vt, 2, 0}};
    RT_CHECK64("vtable.total", shape_total(shapes, 5), 174);
    RT_CHECK64("vtable.after_scale", shapes[0].w + shapes[1].w + shapes[2].w + shapes[3].w + shapes[4].w, 3 + 8 + 40 + 40 + 64);
    uint64_t h = RT_MIX_INIT;
    for (unsigned i = 0; i < 12; ++i) {
        h = rt_mix(h, apply_op(RT_OPAQUE(i), 0x1234567 + i));
    }
    rt_note("dispatch.digest", h, 16);
    RT_CHECK64("dispatch.sq", apply_op(RT_OPAQUE(2u), 12), 144);
    RT_CHECK64("dispatch.pop", apply_op(RT_OPAQUE(11u), 0xff), 8);
    static const uint64_t v[] = {5, 9, 1, 30, 2, 7};
    RT_CHECK64("callback.sum", fold(cb_sum, v, 6, 0), 54);
    RT_CHECK64("callback.max", fold(cb_max, v, 6, 3), 30);
    RT_CHECK64("callback.xor_rot", fold(cb_xor_rot, v, 2, 1), ((((1ull << 5) ^ 5) << 5) ^ 9));
    RT_CHECK32("recursion.even", is_even(RT_OPAQUE(1000ull)), 1);
    RT_CHECK32("recursion.odd", is_odd(RT_OPAQUE(777ull)), 1);
    RT_CHECK64("tail.collatz27", collatz_steps(RT_OPAQUE(27ull), 0), 111);
    RT_CHECK64("tail.helper", tail_caller(RT_OPAQUE(1ull)), (1 ^ 0x55) * 7 + 3);
    RT_CHECK64("init.order", ctor_trace, 123);
    RT_CHECK32("init.dt_init", rt_init_ran, 1);
    RT_CHECK64("cleanup.value", with_cleanup(RT_OPAQUE(0x105ull)), (0x105 * 5 + 1) * ((0x105 * 5 + 1) & 0xf) + 1 + 3 * 5 + 1);
    RT_CHECK64("cleanup.released", released, 5 + (((0x105 * 5 + 1) & 0xf)));
    RT_CHECK64("pointer.hidden", hidden_ptr(RT_OPAQUE(0xffull)), 0x5a5a5a5a5a5a5aa5ull);
    return rt_finish();
}
