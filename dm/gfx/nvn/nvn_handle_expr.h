// Optional, bounded material-handle proof. Unknown values preserve depth splits.
// Metadata is emitted by dmsh_handles.py; operands precede their users.
#pragma once
#include <stdint.h>
#include <limits.h>
enum { NVN_HP_MAX_NODES = 256, NVN_HP_MAX_ROOTS = 128, NVN_HP_VERSION = 1 };
enum { NVN_HP_CONST, NVN_HP_CB, NVN_HP_SB, NVN_HP_NEG_I, NVN_HP_AND,
       NVN_HP_NE, NVN_HP_NOT, NVN_HP_ADD_I, NVN_HP_ADD_U, NVN_HP_SELECT,
       NVN_HP_SHL_I, NVN_HP_SHR_U, NVN_HP_MIN_U, NVN_HP_OPS };
typedef struct { unsigned op; uint32_t a, b, c; } nvn_hp_node;
typedef struct {
    unsigned n, nroots;
    nvn_hp_node nodes[NVN_HP_MAX_NODES];
    uint16_t roots[NVN_HP_MAX_ROOTS];
} nvn_hp_prog;

// Structural validation is also used when accepting optional reflection. Bad or
// newer metadata disables the optimization, without rejecting a valid shader.
static inline int nvn_hp_valid(const nvn_hp_prog *p) {
    if (!p->n || p->n > NVN_HP_MAX_NODES || !p->nroots || p->nroots > NVN_HP_MAX_ROOTS) return 0;
    for (unsigned i = 0; i < p->n; i++) {
        nvn_hp_node n = p->nodes[i];
        if (n.op >= NVN_HP_OPS) return 0;
        if (n.op == NVN_HP_CONST) { if (n.b || n.c) return 0; continue; }
        if (n.op == NVN_HP_CB || n.op == NVN_HP_SB) {
            if (n.a >= 16 || n.b >= i || n.c >= 4) return 0;
            continue;
        }
        if (n.a >= i) return 0;
        if (n.op == NVN_HP_NOT || n.op == NVN_HP_NEG_I) { if (n.b || n.c) return 0; continue; }
        if (n.b >= i) return 0;
        if (n.op == NVN_HP_SELECT) { if (n.c >= i) return 0; }
        else if (n.c) return 0;
    }
    for (unsigned i = 0; i < p->nroots; i++) if (p->roots[i] >= p->n) return 0;
    return 1;
}

typedef int (*nvn_hp_reader)(void *, int, unsigned, uint64_t, uint32_t *);
static inline int nvn_hp_eval(const nvn_hp_prog *p, nvn_hp_reader read, void *ctx, uint32_t *out) {
    uint32_t v[NVN_HP_MAX_NODES] = {0}; unsigned char ok[NVN_HP_MAX_NODES] = {0};
    if (!p->n || p->n > NVN_HP_MAX_NODES || !p->nroots || p->nroots > NVN_HP_MAX_ROOTS) return 0;
    for (unsigned i=0; i<p->n; i++) {
        nvn_hp_node n=p->nodes[i]; uint32_t a=0,b=0; int64_t z=0;
        if (n.op==NVN_HP_CONST) { v[i]=n.a; ok[i]=1; continue; }
        if (n.op==NVN_HP_CB || n.op==NVN_HP_SB) {
            if (n.b>=i || !ok[n.b] || n.a>=16 || n.c>=4) continue;
            uint64_t off=(uint64_t)v[n.b]*(n.op==NVN_HP_CB ? 16 : 4)+n.c*4;
            ok[i]=read(ctx,n.op==NVN_HP_CB,n.a,off,&v[i]); continue;
        }
        if (n.a>=i || !ok[n.a]) continue;
        a=v[n.a];
        if (n.op==NVN_HP_NOT) { v[i]=!a; ok[i]=1; continue; }
        if (n.op==NVN_HP_NEG_I) { if (a!=0x80000000u) { v[i]=0u-a; ok[i]=1; } continue; }
        if (n.op==NVN_HP_SELECT) {
            unsigned which=a?n.b:n.c;
            if (which<i && ok[which]) { v[i]=v[which]; ok[i]=1; } continue;
        }
        if (n.b>=i || !ok[n.b]) continue;
        b=v[n.b];ok[i]=1;
        switch (n.op) {
        case NVN_HP_AND:v[i]=a&b;break;
        case NVN_HP_NE:v[i]=a!=b;break;
        case NVN_HP_ADD_U:v[i]=a+b;break;
        case NVN_HP_ADD_I:z=(int64_t)(int32_t)a+(int32_t)b; if(z<INT32_MIN||z>INT32_MAX)ok[i]=0;else v[i]=(uint32_t)z;break;
        case NVN_HP_SHL_I:if(b>=32||(int32_t)a<0)ok[i]=0;else {z=(int64_t)a<<b;if(z>INT32_MAX)ok[i]=0;else v[i]=(uint32_t)z;}break;
        case NVN_HP_SHR_U:if(b>=32)ok[i]=0;else v[i]=a>>b;break;
        case NVN_HP_MIN_U:v[i]=a<b?a:b;break;
        default:ok[i]=0;break;
        }
    }
    for(unsigned i=0;i<p->nroots;i++){unsigned k=p->roots[i];if(k>=p->n||!ok[k])return 0;out[i]=v[k];}
    return 1;
}
