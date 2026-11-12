#include "../dsu_protocol.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t p[100]={0},req[28];dsu_sample s;
    dsu_request(req,123,1);assert(!memcmp(req,"DSUC",4));assert(req[20]==1 && req[21]==1);assert(dsu_crc(req,28)==dsu_u32(req+8));
    memcpy(p,"DSUS",4);p[4]=0xe9;p[5]=3;p[6]=84;dsu_put(p+16,0x100002);p[20]=1;p[21]=2;p[31]=1;dsu_put(p+68,1000);dsu_put(p+8,dsu_crc(p,100));
    assert(dsu_decode(p,100,&s) && s.slot==1 && s.timestamp_us==1000);
    assert(!dsu_decode(p,99,&s));p[90]^=1;assert(!dsu_decode(p,100,&s));p[90]^=1;
    dsu_put(p+88,0x7fc00000);dsu_put(p+8,dsu_crc(p,100));assert(!dsu_decode(p,100,&s));
    puts("DSU protocol: valid request, reply, length, CRC and finite-value checks passed");
}
