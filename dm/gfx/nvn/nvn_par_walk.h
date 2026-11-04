// A submission cursor owns its return stack and remaining handles. Positions identify an invocation,
// not merely an opcode address: a shared command list can be called or submitted several times.
enum { PAR_CALLS = 16, PAR_WALK_LIMIT = 1000000 };
typedef struct {
    const uint8_t *op, *ret[PAR_CALLS];
    const uint64_t *rest; int nrest, depth;
} par_pos;
typedef struct { par_pos pos; unsigned left; int bad; } par_walk;
static int par_pos_equal(const par_pos *a, const par_pos *b) {
    return a->op == b->op && a->nrest == b->nrest && (!a->nrest || a->rest == b->rest)
        && a->depth == b->depth && a->depth >= 0 && a->depth <= PAR_CALLS
        && !memcmp(a->ret, b->ret, (size_t)a->depth * sizeof *a->ret);
}
static par_walk par_walk_begin(const uint64_t *handles, int n) {
    par_walk c = {.left = PAR_WALK_LIMIT};
    if (n < 0 || (n && (!handles || !handles[0]))) { c.bad = 1; return c; }
    if (n) { c.pos.op = (const uint8_t *)handles[0]; c.pos.rest = handles + 1; c.pos.nrest = n - 1; }
    return c;
}
static par_walk par_walk_from(par_pos pos) { return (par_walk){.pos = pos, .left = PAR_WALK_LIMIT}; }
static int par_walk_next(par_walk *c, par_pos *where, op_hdr *out, const void **args) {
    if (c->pos.nrest < 0 || (c->pos.nrest && !c->pos.rest)) c->bad = 1;
    if (c->bad) return 0;
    while (c->pos.op) {
        if (!c->left || c->pos.depth < 0 || c->pos.depth > PAR_CALLS) { c->bad = 1; return 0; }
        c->left--;
        const uint8_t *p = c->pos.op; op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < sizeof h) { c->bad = 1; return 0; }
        switch (h.op) {
            case OP_MULTI_DRAW: c->bad = 1; return 0;   // GPU-dependent live arguments need serial replay
            case OP_END:
                if (c->pos.depth) c->pos.op = c->pos.ret[--c->pos.depth];
                else if (c->pos.nrest) { c->pos.op = (const uint8_t *)*c->pos.rest++; c->pos.nrest--; if (!c->pos.op) c->bad = 1; }
                else c->pos.op = NULL;
                if (c->bad) return 0;
                continue;
            case OP_CALL: case OP_JUMP: {
                uint64_t target;
                if (h.size < sizeof h + sizeof target || (h.op == OP_CALL && c->pos.depth == PAR_CALLS)) { c->bad = 1; return 0; }
                memcpy(&target, p + sizeof h, sizeof target);
                if (!target) { c->bad = 1; return 0; }
                if (h.op == OP_CALL) c->pos.ret[c->pos.depth++] = p + h.size;
                c->pos.op = (const uint8_t *)target; continue;
            }
            default: break;
        }
        *where = c->pos; *out = h; *args = p + sizeof h; c->pos.op = p + h.size; return 1;
    }
    return 0;
}
