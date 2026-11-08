#include "../nvn/nvn_predicate_map.h"
#include <assert.h>
#include <stdio.h>
static uint64_t rng=0x98512d0e;
static uint64_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
int main(void){
    nvn_predicate_map m={0};uint64_t checks=0;
    enum {N=4096};uint64_t addresses[N];unsigned char alive[N];
    // Dense aligned queries exercise direct-key invalidation. Dense/distant
    // unaligned fields retain the general page path, including page crossings.
    for(int mode=0;mode<4;mode++){
    npm_reset(&m);uint64_t stride=(uint64_t[]){16,15,8192,8191}[mode];
    for(int i=0;i<N;i++){addresses[i]=UINT64_C(0x10000000)+(uint64_t)i*stride;alive[i]=1;
        assert(npm_record(&m,addresses[i],2,0x80000000+(uint64_t)i*8,1,0));}
    assert(m.unaligned==(unsigned)(mode&1));
    for(int t=0;t<600;t++){
        uint64_t index=next()%N,lo=addresses[index]+(next()%32)-16,size=next()%160;
        if(t%17==0)size=UINT64_C(1)<<25;
        npm_invalidate(&m,lo,size);
        for(int i=0;i<N;i++){if(size&&addresses[i]<lo+size&&lo<addresses[i]+8)alive[i]=0;
            nvn_predicate_operand *e=npm_find(&m,addresses[i]);assert(e&&!!e->kind==alive[i]);checks++;}
        // A later report supersedes the invalidated value; a cached version cannot match.
        nvn_predicate_operand old=*npm_find(&m,addresses[index]);
        assert(npm_record(&m,addresses[index],1,0,0,(uint64_t)t));alive[index]=1;
        nvn_predicate_operand *e=npm_find(&m,addresses[index]);assert(e->version>old.version&&e->constant==(uint64_t)t&&e->kind==1);checks++;
    }
    }
    uint64_t version=m.version;npm_reset(&m);assert(!npm_find(&m,addresses[0]));
    assert(npm_record(&m,4093,2,0x80000000,1,0));assert(m.version>version);
    npm_invalidate(&m,4096,1);assert(!npm_find(&m,4093)->kind);checks++;
    assert(npm_record(&m,4093,1,0,0,0));npm_invalidate(&m,4101,1);assert(npm_find(&m,4093)->kind);checks++;
    npm_invalidate(&m,UINT64_MAX-1,3);assert(!npm_find(&m,4093)->kind);checks++;
    assert(!npm_record(&m,UINT64_MAX-6,1,0,0,0));checks++;
    assert(npm_record(&m,0,1,0,0,42));assert(npm_find(&m,0)->constant==42);checks++;
    m.version=UINT64_MAX;assert(!npm_record(&m,0,1,0,0,43));assert(!npm_find(&m,0)->kind);checks++;
    npm_destroy(&m);printf("predicate map: %llu reference comparisons/checks PASS\n",(unsigned long long)checks);return 0;
}
