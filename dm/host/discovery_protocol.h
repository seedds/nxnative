// Original framing recognizer for verified discovery queries, never game data.
// PiaLan facts were checked in the SDK's serializer; LDN uses the public wire.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
enum { DM_DISCOVERY_PIA=1, DM_DISCOVERY_LDN=2 };
static inline unsigned dm_discovery_query_protocol(unsigned port,const void *bytes,size_t size) {
    if(!bytes)return 0;
    const uint8_t *p=bytes;
    if(port==30000) {
        if(size<0x12f||size>1364||p[0])return 0;
        uint32_t payload=(uint32_t)p[1]<<24|(uint32_t)p[2]<<16|(uint32_t)p[3]<<8|p[4];
        if(payload>1364-0x12f||size!=payload+0x12f||p[payload+5]!=1||p[payload+6]>1)return 0;
        unsigned nonce=0;for(unsigned i=0;i<8;++i)nonce|=p[payload+7+i];
        return nonce?DM_DISCOVERY_PIA:0;
    }
    if(port==11452) {
        static const uint8_t query[12]={0,0x14,0x45,0x11};
        return size==sizeof query&&!memcmp(p,query,sizeof query)?DM_DISCOVERY_LDN:0;
    }
    return 0;
}
