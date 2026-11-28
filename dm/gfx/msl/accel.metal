#include <metal_stdlib>
using namespace metal;
// NVN2's packed instance is a row-major3x4 transform, two packed24/8 words,
// and a guest child address. Metal indirect instances use separate32-bit fields
// and a resource ID. Read on the GPU: earlier shaders may have written instances.
kernel void instances(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]],
                      device const ulong2 *map [[buffer(2)]], constant uint2 &n [[buffer(3)]],
                      device atomic_uint *error [[buffer(4)]],
                      device uint *sbt_offsets [[buffer(5)]],
                      uint i [[thread_position_in_grid]]) {
    if (i >= n.x) return;
    src += i * 16; dst += i * 18;
    for (uint j=0;j<12;j++) dst[j]=src[j];
    uint flags=src[13]>>24;
    // Mapping inferred from the exact Vulkan/DXR packed instance ABI, including
    // both 24/8 words; not from a recovered native NVN enum decoder. Metal uses
    // the corresponding disable-cull, CCW, opaque and nonopaque option bits.
    dst[12]=((flags&1)?1u:0u)|((flags&2)?2u:0u)|((flags&4)?4u:0u)|((flags&8)?8u:0u);
    dst[13]=src[12]>>24;
    dst[14]=src[13]&0xffffff; dst[15]=src[12]&0xffffff;
    // The ordinary Metal hit result exposes IDs/transforms, not this word. Read
    // it at the same GPU execution point as the built instance, before reuse.
    sbt_offsets[i]=src[13]&0xffffff;
    ulong address=ulong(src[14])|(ulong(src[15])<<32), rid=0;
    uint lo=0,hi=n.y;
    while(lo<hi) { uint m=lo+(hi-lo)/2; if(map[m].x<address)lo=m+1;else hi=m; }
    bool found=lo<n.y && map[lo].x==address;
    uint code=!found?1:((flags&~15u) || (flags&12u)==12u)?2:0;
    if(code) {
        uint zero=0;
        while(!atomic_compare_exchange_weak_explicit(error,&zero,code,memory_order_relaxed,memory_order_relaxed) && !zero) {}
        if(!zero) {
            atomic_store_explicit(error+1,i,memory_order_relaxed);
            atomic_store_explicit(error+2,code==1?uint(address):flags,memory_order_relaxed);
            atomic_store_explicit(error+3,uint(address>>32),memory_order_relaxed);
        }
        // Keep invalid GPU descriptors safe until completion reports the error.
        // The CPU rejects an empty map before dispatch, so map[0] is valid.
        rid=map[0].y; dst[12]=0; dst[13]=0;
    } else rid=map[lo].y;
    dst[16]=uint(rid); dst[17]=uint(rid>>32);
}
kernel void indices8(device const uchar *src [[buffer(0)]], device ushort *dst [[buffer(1)]],
                     constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    if(i<n)dst[i]=src[i];
}
