// Cemuhook DSU v1001 wire decoder. No network or host state, safe for unit tests.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
static uint32_t dsu_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint64_t dsu_u64(const uint8_t *p) { return dsu_u32(p) | (uint64_t)dsu_u32(p+4)<<32; }
static void dsu_put(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }
static uint32_t dsu_crc(const uint8_t *p, size_t n) {
    uint32_t c=~0u;
    for (size_t i=0;i<n;i++) { c ^= i>=8 && i<12 ? 0 : p[i]; for (int j=0;j<8;j++) c=(c>>1)^((0u-(c&1))&0xedb88320u); }
    return ~c;
}
typedef struct { uint32_t server, sequence; uint64_t timestamp_us; uint8_t slot; uint16_t buttons; int16_t sticks[4]; float acceleration[3], degrees[3]; } dsu_sample;
static int dsu_decode(const uint8_t *p, size_t n, dsu_sample *s) {
    if (n<100 || memcmp(p,"DSUS",4) || p[4]!=0xe9 || p[5]!=3) return 0;
    size_t size=16+(size_t)p[6]+((size_t)p[7]<<8);
    if (size<100 || size>n || dsu_u32(p+16)!=0x100002 || dsu_crc(p,size)!=dsu_u32(p+8) || p[20]>3 || p[21]!=2 || p[31]!=1) return 0;
    memset(s,0,sizeof *s);s->server=dsu_u32(p+12);s->sequence=dsu_u32(p+32);s->slot=p[20];s->timestamp_us=dsu_u64(p+68);
    if (!s->timestamp_us) return 0;
    // DSU uses DualShock positions: east/south/north/west -> Switch A/B/X/Y.
    static const uint8_t first[8]={11,4,5,10,13,14,15,12};
    static const uint8_t second[8]={2,0,1,3,6,7,8,9};
    for(int bit=0;bit<8;bit++) { if(p[36]>>bit&1)s->buttons|=(uint16_t)(1u<<first[bit]);if(p[37]>>bit&1)s->buttons|=(uint16_t)(1u<<second[bit]); }
    for(int a=0;a<4;a++){ int v=(int)p[40+a]-128;s->sticks[a]=(int16_t)(v<0?v*256:v*32767/127); }
    for (int i=0;i<3;i++) {
        uint32_t a=dsu_u32(p+76+i*4),g=dsu_u32(p+88+i*4);memcpy(&s->acceleration[i],&a,4);memcpy(&s->degrees[i],&g,4);
        if (!isfinite(s->acceleration[i]) || !isfinite(s->degrees[i]) || fabsf(s->acceleration[i])>100 || fabsf(s->degrees[i])>100000) return 0;
    }
    return 1;
}
static void dsu_request(uint8_t p[28], uint32_t id, int slot) {
    memset(p,0,28);memcpy(p,"DSUC",4);p[4]=0xe9;p[5]=3;p[6]=12;dsu_put(p+12,id);dsu_put(p+16,0x100002);
    if (slot>=0 && slot<=3) { p[20]=1;p[21]=(uint8_t)slot; }
    dsu_put(p+8,dsu_crc(p,28));
}
