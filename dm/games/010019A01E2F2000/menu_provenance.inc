// Native GUI creation provenance; callers serialize node-map access.
#pragma once
#include "menu_visual.inc"
#include <stdlib.h>
// A shared GUI layer is not ownership. Preserve the room of each native
// TextPane/Model creation until that GUI node leaves its native frame.
typedef struct mp4_menu_node_entry { uintptr_t node;mp4_menu_scope scope;struct mp4_menu_node_entry *next; } mp4_menu_node_entry;
static mp4_menu_node_entry *mp4_menu_nodes[1024];
static unsigned mp4_menu_node_bucket(const void *node) { return ((uintptr_t)node>>4)&1023; }
static mp4_menu_node_entry **mp4_menu_node_slot(const void *node) {
    mp4_menu_node_entry **p=&mp4_menu_nodes[mp4_menu_node_bucket(node)];
    while(*p && (*p)->node!=(uintptr_t)node)p=&(*p)->next;
    return p;
}
typedef struct mp4_menu_scope_entry {mp4_menu_scope scope;struct mp4_menu_scope_entry *previous;} mp4_menu_scope_entry;
static _Thread_local mp4_menu_scope_entry *mp4_menu_scope_top;
static _Thread_local unsigned mp4_menu_scope_skipped;
static inline int mp4_menu_scope_push(const void *component) {
    if(mp4_menu_scope_skipped){mp4_menu_scope_skipped++;return 0;}
    mp4_menu_scope_entry *p=malloc(sizeof *p);
    if(!p){mp4_menu_scope_skipped=1;return 0;}
    *p=(mp4_menu_scope_entry){mp4_menu_scope_read(component),mp4_menu_scope_top};mp4_menu_scope_top=p;return 1;
}
static void mp4_menu_scope_pop(void) {
    if(mp4_menu_scope_skipped){mp4_menu_scope_skipped--;return;}
    mp4_menu_scope_entry *p=mp4_menu_scope_top;if(p){mp4_menu_scope_top=p->previous;free(p);}
}

static inline void mp4_menu_node_forget(const void *node) {
    mp4_menu_node_entry **slot=mp4_menu_node_slot(node),*p=*slot;
    if(p){*slot=p->next;free(p);}
}

static inline int mp4_menu_node_register(const void *node) {
    if(!node)return 1;
    // Native GUI addresses are reused; an unscoped construction replaces any
    // old identity too (including creation after a failed nested scope push).
    if(!mp4_menu_scope_top || mp4_menu_scope_skipped){mp4_menu_node_forget(node);return 1;}
    mp4_menu_node_entry **slot=mp4_menu_node_slot(node);
    if(!*slot)*slot=calloc(1,sizeof **slot);
    if(!*slot)return 0;
    (*slot)->node=(uintptr_t)node;(*slot)->scope=mp4_menu_scope_top->scope;return 1;
}
