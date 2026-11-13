// The encode thread's recorder (nvn_rec.h DmRecEnc, DM_NVN_ENC_THREAD) stands in for the render encoder and stops the host on a selector it does not record
// (forwardingTargetForSelector). Every draw the NVN layer encodes must be one it records: the indirect task / mesh draw of nvn_geometry.inc was not, and the
// iOS app (where the encode thread is the default) aborted at Ocarina of Time's first one. No GPU work: the selectors are checked on the class.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
int main(void) {
    @autoreleasepool {
        const SEL draws[] = {
            @selector(drawPrimitives:vertexStart:vertexCount:),
            @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:),
            @selector(drawPrimitives:indirectBuffer:),
            @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferLength:instanceCount:baseVertex:baseInstance:),
            @selector(drawIndexedPrimitives:indexType:indexBuffer:indexBufferLength:indirectBuffer:),
            @selector(drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:),
            @selector(drawMeshThreadgroupsWithIndirectBuffer:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:),
        };
        int failed = 0, n = (int)(sizeof draws / sizeof draws[0]);
        for (int i = 0; i < n; i++)
            if (![DmRecEnc instancesRespondToSelector:draws[i]]) { failed++; fprintf(stderr, "FAIL: DmRecEnc does not record -%s\n", sel_getName(draws[i])); }
        printf("REC_DRAWS selectors=%d failed=%d\n", n, failed);
        return failed != 0;
    }
}
