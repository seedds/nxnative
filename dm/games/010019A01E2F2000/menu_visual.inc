// v131072 pause-room GUI serialization. The game authors a 16:9 foreground;
// the host's 16:10 final crop needs a uniform .9 XY fit. Change the freshly
// copied render packet, never the animated source node or Entity transform.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
typedef struct { uintptr_t camera,frame,room; } mp4_menu_binding;
static inline int mp4_menu_bind(mp4_menu_binding *binding,const void *camera) {
    if(!binding || !camera) return 0;
    const uint8_t *owner,*room;const void *frame;
    memcpy(&owner,(const uint8_t *)camera+0x20,8);if(!owner) return 0;
    memcpy(&room,owner+0x90,8);if(!room) return 0;
    memcpy(&frame,room+0x90,8);if(!frame) return 0;
    // Called only after the exact original pause-camera template matched.
    *binding=(mp4_menu_binding){(uintptr_t)camera,(uintptr_t)frame,(uintptr_t)room};return 1;
}
static inline void mp4_menu_unbind(mp4_menu_binding *binding,const void *camera) {
    // Destruction compares scalar identities; never follow a destroyed owner.
    if(binding && binding->camera==(uintptr_t)camera) *binding=(mp4_menu_binding){0};
}
// TextPane projection property b3e70d14 value 90130085 maps to mode 1.
// Native 18b7c8 / 10074a4 route those TextPane/Model instances into the
// context's shared GUI frame, rather than the room's frame. Keep provenance
// from their native component creation call, not from the chosen GUI layer.
typedef struct { uintptr_t room,frame; int backdrop; } mp4_menu_scope;
static inline mp4_menu_scope mp4_menu_scope_read(const void *component) {
    mp4_menu_scope out={0};if(!component) return out;
    const uint8_t *owner,*room,*props;memcpy(&owner,(const uint8_t *)component+0x20,8);if(!owner)return out;
    memcpy(&room,owner+0x90,8);if(!room)return out;out.room=(uintptr_t)room;memcpy(&out.frame,room+0x90,8);
    memcpy(&props,owner+0x30,8);if(!props)return out;
    // Exact original full-screen background model component in UI_PauseMenu,
    // ROOM 87d3f1d9d735214790b8606cd8794bf8, Entity property 2244.
    // Its scene artwork must retain the accepted native coverage.
    static const uint8_t backgrounds[][16]={{0xc4,0x48,0x2d,0xd8,0x86,0x14,0x56,0x4e,0xb1,0x19,0x14,0x9f,0x03,0x1d,0x18,0x59},
        // The independent full-screen nine-slice backdrop, Entity property
        // 2247: native scale 213.5359 x 122.0729 at z=-16.74455.
        {0xe3,0xfe,0xb7,0xfe,0x81,0x02,0x6e,0x4a,0x84,0xb2,0x22,0x4c,0x14,0x37,0xea,0x12},
        // Entity property 2243: the separate dotted background nine-slice,
        // native scale .84 x 1.9129 at z=-16.662094. Its 1920x476 art
        // spans the screen before fitting; shrinking it leaves 5% bands.
        {0xd5,0xfc,0xee,0x4c,0x3e,0x64,0xba,0x4c,0x90,0x44,0xb8,0x84,0xf4,0xa0,0xe1,0x57},
        // Entity property 8448 in template 25a6ea55085c924f88d5977d577b9fe1:
        // the paused-scene image, native +/-8 x +/-4.5 quad at z=-20.
        // Keep every background layer native, not just its dark overlay.
        {0x9e,0x42,0xac,0x01,0x3f,0x86,0xe9,0x02,0xbe,0x06,0xc0,0xa9,0x59,0xfd,0x69,0x83}};
    uint32_t count;const uint8_t *ids;memcpy(&count,props+0x50,4);memcpy(&ids,props+0x58,8);
    if(ids && count<=256) for(uint32_t i=0;i<count;i++)for(unsigned j=0;j<sizeof backgrounds/sizeof *backgrounds;j++)
        if(!memcmp(ids+16*i,backgrounds[j],16)){out.backdrop=1;break;}
    return out;
}
static inline int mp4_menu_visual_scoped(const mp4_menu_binding *binding,const void *node,void *packet,const mp4_menu_scope *scope) {
    if(!binding || !binding->frame || !node || !packet) return 0;
    const uint8_t *n=node;const void *actual;const uint8_t *frame;
    memcpy(&actual,n+0x50,8);if(actual!=packet || (n[0x48]&3)!=3) return 0;
    if(scope) {
        // Explicit provenance overrides the frame fallback, including the
        // background exclusion and nodes from other rooms on the shared layer.
        if(!binding->room || scope->room!=binding->room || scope->frame!=binding->frame || scope->backdrop) return 0;
    } else {
        memcpy(&frame,n+0x28,8);
        unsigned depth=0;
        while(frame && (uintptr_t)frame!=binding->frame && depth++<32) memcpy(&frame,frame,8);
        if((uintptr_t)frame!=binding->frame) return 0;
    }
    float m[12];memcpy(m,n+0x60,sizeof m);
    for(unsigned i=0;i<12;i++) if(!isfinite(m[i]) || fabsf(m[i])>100000.f) return 0;
    for(unsigned i=0;i<8;i++) m[i]*=.9f;
    memcpy((uint8_t *)packet+8,m,sizeof m);return 1;
}
static inline int mp4_menu_visual(const mp4_menu_binding *binding,const void *node,void *packet) {
    return mp4_menu_visual_scoped(binding,node,packet,NULL);
}

// MapRenderer's orthographic models use the literal at main+1403714 instead
// of UICamera. Both normal/alternate lists copy it into a borrowed draw record.
// Fit this projection once per fresh copy; never move the native model, map
// camera, viewport or world marker projection. Depth and orientation survive.
static inline int mp4_menu_compass_projection(const mp4_menu_binding *binding,void *projection) {
    static const uint32_t native[16]={0x3e000000,0,0,0,0,0x3e638e39,0,0x80000000,0,0,0xb9800000,0x80000000,0,0,0,0x3f800000};
    if(!binding || !binding->frame || !projection || memcmp(projection,native,sizeof native))return 0;
    float m[16];memcpy(m,projection,sizeof m);m[0]*=.9f;m[5]*=.9f;memcpy(projection,m,sizeof m);return 1;
}
