// Read-only snapshot of the original pause inventory's left text group.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
typedef struct { float gui[12],world[15],source[3]; uintptr_t pane; } mp4_menu_snapshot;
static inline int mp4_menu_snapshot_read(const void *component,const void *node,mp4_menu_snapshot *out) {
    if(!component || !node || !out) return 0;
    const uint8_t *c=component,*owner,*props,*world;
    if((c[0x1c]&0xf8)!=0xd8) return 0; // same native enabled condition; no detached-owner read
    memcpy(&owner,c+0x20,8);if(!owner) return 0;
    memcpy(&props,owner+0x30,8);if(!props) return 0;
    // UI_PauseMenu: original template containing Menu_InventorySamus and its
    // sibling left captions, independently identified in ROOM property 7428.
    const uint8_t id[]={0xeb,0x1c,0x03,0x70,0xbd,0xd0,0x24,0x4f,0x8e,0x46,0x1a,0x34,0xa4,0x16,0x17,0x08};
    if(memcmp(props+0xc,id,16)) return 0;
    memcpy(&world,owner+0x38,8);if(!world) return 0;
    mp4_menu_snapshot s={.pane=(uintptr_t)component};
    memcpy(s.gui,(const uint8_t *)node+0x60,sizeof s.gui);
    memcpy(s.world,world,sizeof s.world);memcpy(s.source,props+0x1c,sizeof s.source);
    for(unsigned i=0;i<12;i++) if(!isfinite(s.gui[i])) return 0;
    for(unsigned i=0;i<15;i++) if(!isfinite(s.world[i])) return 0;
    for(unsigned i=0;i<3;i++) if(!isfinite(s.source[i])) return 0;
    *out=s;return 1; // component, node, owner and source are never written
}
