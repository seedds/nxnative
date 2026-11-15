// Original v131072 UI_PauseMenu UICamera owner template, independently
// identified in ROOM 87d3f1d9d735214790b8606cd8794bf8. Never select the HUD.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
static inline int mp4_pause_template(const void *properties) {
    static const uint8_t id[16]={0x68,0xd8,0x47,0xf0,0x68,0x20,0x88,0x4a,0x8f,0x84,0x6c,0x55,0xb0,0x2f,0x73,0x9d};
    return properties && !memcmp((const uint8_t *)properties+0xc,id,16);
}
// The existing 16:10 UICamera postlude expands +/-8,+/-4.5 to +/-8.8889,+/-5.
// Undo that safe fit only for this menu's 2D canvas, before its native copy and
// inverse calculation. Full-screen artwork must keep its authored coverage.
static inline int mp4_pause_bounds(void *descriptor) {
    if(!descriptor) return 0;
    uint32_t mode;memcpy(&mode,descriptor,4);
    float b[4];memcpy(b,(uint8_t *)descriptor+0xc,sizeof b);
    const float expected[]={8.f/.9f,-8.f/.9f,-5.f,5.f};
    if(mode!=2) return 0;
    for(int i=0;i<4;i++) if(!isfinite(b[i]) || fabsf(b[i]-expected[i])>.0001f) return 0;
    for(int i=0;i<4;i++) b[i]*=.9f;
    memcpy((uint8_t *)descriptor+0xc,b,sizeof b);return 1;
}
// Perspective models use the separate Camera component (4fe57689), not the
// UICamera view callback. Borrow its constructor parameter record after the
// native copy: near/far at +0/+4, FOV degrees at +8, aspect at +c. The native
// constructors copy this record to camera+1e4 before the camera is published.
// Preserve aspect and near/far. Scope to the observed native 55-degree input;
// other FOVs are unvalidated and retain their native behavior.
static inline int mp4_pause_fov(void *frame) {
    if(!frame) return 0;
    float fov;memcpy(&fov,(uint8_t *)frame+8,4);
    if(!isfinite(fov) || fabsf(fov-55.f)>.01f) return 0;
    const double pi=3.14159265358979323846;
    fov=(float)(2.*atan(tan(fov*pi/360.)/.9)*180./pi);
    memcpy((uint8_t *)frame+8,&fov,4);return 1;
}

static inline int mp4_pause_apply(const void *component,void *payload,unsigned event) {
    if(!component || !payload || event>1) return 0;
    const uint8_t *c=component;const uint8_t *owner;const void *properties;
    // Both sites are initialization paths with a live owner; no event-enabled
    // callback flag applies to the Camera constructor. Do not read game assets.
    memcpy(&owner,c+0x20,sizeof owner);if(!owner) return 0;
    memcpy(&properties,owner+0x30,sizeof properties);
    if(!mp4_pause_template(properties)) return 0;
    return event?mp4_pause_fov(payload):mp4_pause_bounds(payload);
}
