// Submission-local versions of GPU counter operands. The caller owns slot-buffer
// lifetimes; this table never dereferences GPU pointers or guesses a pending value.
#ifndef NVN_PREDICATE_MAP_H
#define NVN_PREDICATE_MAP_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
typedef struct {
    uint64_t address, slots, constant, version;
    uint32_t count, kind, next_page; // kind: 0 invalid, 1 constant, 2 GPU slots
} nvn_predicate_operand;
typedef struct {
    nvn_predicate_operand *entries;
    uint32_t *addresses, *pages;
    uint32_t count, capacity, unaligned;
    uint64_t version;
} nvn_predicate_map;
static uint64_t npm_hash(uint64_t x) {
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb); return x ^ (x >> 31);
}
static void npm_invalidate_all(nvn_predicate_map *m) {
    for (uint32_t i=0;i<m->count;i++) m->entries[i].kind=0;
}
static void npm_reset(nvn_predicate_map *m) {
    m->count=0;m->unaligned=0;
    if(m->capacity){memset(m->addresses,0,m->capacity*sizeof(uint32_t));memset(m->pages,0,m->capacity*sizeof(uint32_t));}
}
static void npm_destroy(nvn_predicate_map *m) {
    free(m->entries);free(m->addresses);free(m->pages);memset(m,0,sizeof *m);
}
static void npm_link(nvn_predicate_map *m,uint32_t i) {
    nvn_predicate_operand *e=&m->entries[i];
    uint32_t h=(uint32_t)npm_hash(e->address)&(m->capacity-1);
    while(m->addresses[h])h=(h+1)&(m->capacity-1);m->addresses[h]=i+1;
    h=(uint32_t)npm_hash(e->address>>12)&(m->capacity-1);
    e->next_page=m->pages[h];m->pages[h]=i+1;
}
static int npm_reserve(nvn_predicate_map *m) {
    if(m->capacity && m->count<m->capacity/2)return 1;
    if(m->capacity>UINT32_MAX/2)return 0;
    uint32_t n=m->capacity?m->capacity*2:64;
    nvn_predicate_operand *e=calloc(n/2,sizeof *e);
    uint32_t *a=calloc(n,sizeof *a),*p=calloc(n,sizeof *p);
    if(!e||!a||!p){free(e);free(a);free(p);return 0;}
    if(m->count)memcpy(e,m->entries,m->count*sizeof *e);
    free(m->entries);free(m->addresses);free(m->pages);
    m->entries=e;m->addresses=a;m->pages=p;m->capacity=n;
    for(uint32_t i=0;i<m->count;i++)npm_link(m,i);return 1;
}
static nvn_predicate_operand *npm_find(nvn_predicate_map *m,uint64_t address) {
    if(!m->capacity)return NULL;
    uint32_t h=(uint32_t)npm_hash(address)&(m->capacity-1);
    while(m->addresses[h]){nvn_predicate_operand *e=&m->entries[m->addresses[h]-1];
        if(e->address==address)return e;h=(h+1)&(m->capacity-1);}
    return NULL;
}
static int npm_record(nvn_predicate_map *m,uint64_t address,uint32_t kind,uint64_t slots,uint32_t count,uint64_t constant) {
    // Counter fields must fit; unsupported operand kinds invalidate, never guess.
    if(address>UINT64_MAX-8||kind>2||m->version==UINT64_MAX){npm_invalidate_all(m);return 0;}
    nvn_predicate_operand *e=npm_find(m,address);
    if(!e){if(!npm_reserve(m)){npm_invalidate_all(m);return 0;}
        uint32_t i=m->count++;e=&m->entries[i];e->address=address;npm_link(m,i);}
    if(address&7)m->unaligned=1;
    e->kind=kind;e->slots=slots;e->count=count;e->constant=constant;e->version=++m->version;return 1;
}
static void npm_invalidate(nvn_predicate_map *m,uint64_t address,uint64_t size) {
    if(!size||!m->count)return;
    if(address>UINT64_MAX-size){npm_invalidate_all(m);return;}
    uint64_t end=address+size;
    // Native query records are aligned. Small writes then overlap only a few
    // possible keys, even when hundreds of records share a page. Keep the page
    // path for maps containing unaligned operands and for larger writes.
    if(!m->unaligned&&size<=64){
        uint64_t last=(end-1)&~UINT64_C(7);
        for(uint64_t a=address&~UINT64_C(7);;a+=8){
            nvn_predicate_operand *e=npm_find(m,a);if(e)e->kind=0;
            if(a==last)break;
        }
        return;
    }
    // An operand beginning up to seven bytes before this write can overlap it.
    uint64_t first=(address>7?address-7:0)>>12,last=(end-1)>>12;
    if(last-first>=m->count){
        for(uint32_t i=0;i<m->count;i++){nvn_predicate_operand *e=&m->entries[i];
            if(e->address<end&&address<e->address+8)e->kind=0;}
    }else for(uint64_t page=first;;page++){
        uint32_t h=(uint32_t)npm_hash(page)&(m->capacity-1);
        for(uint32_t i=m->pages[h];i;i=m->entries[i-1].next_page){nvn_predicate_operand *e=&m->entries[i-1];
            if((e->address>>12)==page&&e->address<end&&address<e->address+8)e->kind=0;}
        if(page==last)break;
    }
}
#endif
