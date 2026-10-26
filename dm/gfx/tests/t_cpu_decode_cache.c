// Cache parity uses the actual CPU BC decoder. A source write after capture
// must not change the bytes decoded or the cache baseline; GPU staging retains
// an independent copy after eviction. The cache never decides to skip uploads.
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
static void quiet_log(const char *fmt, ...) { (void)fmt; }
#define NVN_CPU_CACHE_LOG quiet_log
#define NVN_CPU_CACHE_BUDGET 512u
#define NVN_CPU_CACHE_SOURCE_MAX 128u
#define NVN_CPU_CACHE_DECODED_MAX 384u
#include "../nvn/nvn_cpu_decode_cache.h"
#include "../tex/nvn_bcdec.h"

static void decode(nvn_cpu_decode_ticket *t) {
    assert(t->entry && !t->hit);
    assert(!nvn_bc_decode(0x4e,t->source,16,4,4,t->decoded,16));
    nvn_cpu_decode_finish(t,1);
}
int main(void) {
    uint8_t live[16] = {0x40,0x12,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,0x31,0x24,0x57,0x68,0x9b,0xac,0xdf};
    uint8_t original[16], expected[64], staging[64]; memcpy(original,live,sizeof live);
    assert(!nvn_bc_decode(0x4e,original,16,4,4,expected,16));
    nvn_cpu_decode_key k = {.serial=1,.source_bytes=16,.decoded_bytes=64,.mem=live,.pool=live,
        .desc={.target=1,.format=0x4e,.flags=4,.width=4,.height=4,.depth=1,.levels=1}};
    unsetenv("DM_NVN_CPU_DECODE_CACHE");
    nvn_cpu_decode_ticket t=nvn_cpu_decode_begin(0,&k); assert(!t.entry);
    setenv("DM_NVN_CPU_DECODE_CACHE","1",1);
    t=nvn_cpu_decode_begin(1,&k); assert(t.entry && !t.hit && t.source!=live);
    memset(live,0x80,sizeof live); // A real write after the source snapshot.
    decode(&t); assert(!memcmp(t.decoded,expected,sizeof expected));
    memcpy(live,original,sizeof live);
    t=nvn_cpu_decode_begin(2,&k); assert(t.hit && !memcmp(t.decoded,expected,sizeof expected));
    memcpy(staging,t.decoded,sizeof staging);
    live[1]^=0xff;
    t=nvn_cpu_decode_begin(3,&k); assert(t.entry && !t.hit);
    uint8_t changed[64]; assert(!nvn_bc_decode(0x4e,live,16,4,4,changed,16));
    decode(&t); assert(!memcmp(t.decoded,changed,sizeof changed));
    k.desc.levels=2;
    t=nvn_cpu_decode_begin(4,&k); assert(t.entry && !t.hit); nvn_cpu_decode_finish(&t,0);
    k.desc.levels=1;
    t=nvn_cpu_decode_begin(5,&k); assert(t.entry && !t.hit); decode(&t);
    k.pool=original;
    t=nvn_cpu_decode_begin(6,&k); assert(t.entry && !t.hit); nvn_cpu_decode_finish(&t,0);
    t=nvn_cpu_decode_begin(7,&k); assert(t.entry && !t.hit); decode(&t);
    // 80 bytes per entry: the seventh distinct key must evict within 512 bytes.
    for (unsigned i=2;i<12;i++) {
        k.serial=i; t=nvn_cpu_decode_begin(8+i,&k); assert(t.entry && !t.hit); decode(&t);
        assert(g_cpu_decode_cache.bytes<=NVN_CPU_CACHE_BUDGET);
    }
    assert(g_cpu_decode_cache.evictions>0 && !memcmp(staging,expected,sizeof staging));
    k.source_bytes=NVN_CPU_CACHE_SOURCE_MAX+1;
    t=nvn_cpu_decode_begin(30,&k); assert(!t.entry); // Never reads this invalid extent.
    k.source_bytes=16;k.decoded_bytes=NVN_CPU_CACHE_DECODED_MAX+1;
    t=nvn_cpu_decode_begin(31,&k); assert(!t.entry);
    setenv("DM_NVN_CPU_DECODE_CACHE","0",1);k.decoded_bytes=64;
    t=nvn_cpu_decode_begin(32,&k);assert(!t.entry && !g_cpu_decode_cache.bytes);
    for (int i=0;i<CPU_CACHE_SLOTS;i++) assert(!g_cpu_decode_cache.entries[i].source);
    // Small entries reach the independent 32-slot cap before the byte cap.
    setenv("DM_NVN_CPU_DECODE_CACHE","1",1);k.source_bytes=k.decoded_bytes=1;
    for (unsigned i=0;i<CPU_CACHE_SLOTS+5;i++) {
        k.serial=100+i;t=nvn_cpu_decode_begin(100+i,&k);assert(t.entry && !t.hit);
        *t.decoded=*t.source;nvn_cpu_decode_finish(&t,1);
    }
    unsigned slots=0;for(int i=0;i<CPU_CACHE_SLOTS;i++)slots+=g_cpu_decode_cache.entries[i].source!=NULL;
    assert(slots==CPU_CACHE_SLOTS && g_cpu_decode_cache.bytes==CPU_CACHE_SLOTS*2);
    nvn_cpu_decode_clear();assert(!g_cpu_decode_cache.bytes);
    puts("CPU decode cache: BC7 parity, exact changes, owned snapshots, layout/storage invalidation, failed decode, independent staging, LRU byte/slot/size bounds and disabling passed");
    return 0;
}
