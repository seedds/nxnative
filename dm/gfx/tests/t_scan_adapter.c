#include "../nvn/hle_nvn.c"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int checks,failures;
#define CHECK(c,label) do { checks++; if(!(c)) { failures++; fprintf(stderr,"FAIL: %s\n",label); } } while(0)
static void scan_put32(uint8_t *p,unsigned off,uint32_t v) { memcpy(p+off,&v,4); }
static uint32_t scan_get32(const void *p,unsigned off) { uint32_t v;memcpy(&v,(const uint8_t *)p+off,4);return v; }
int main(void) {
    module_t module={.name="main",.build_id="8275fbe1ec5191c279f3a943ad8524f4"};g_mod=&module;g_nmod=1;
    NVNcommandBuffer commands;h_nvnCommandBufferInitialize(&commands,NULL);
    uint8_t original[0x58]={0},saved[0x58];
    tex_t source={.magic=MAGIC('T','E','X',0),.serial=1001,
        .d={.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_DEPTH24_STENCIL8,.width=3200,.height=1800,.depth=1,.levels=1,.samples=1}};
    const tex_t *ptr=&source;memcpy(original+0x38,&ptr,sizeof(const tex_t *));
    view_t view={.set_levels=1,.nlevels=1,.set_layers=1,.nlayers=1,.set_dsmode=1,.dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL,.set_swizzle=1,.swz={2,2,2,2}};
    memcpy(original+0x48,&view,sizeof view);scan_put32(original,0xc,0x12);scan_put32(original,0x14,3200);scan_put32(original,0x18,1800);
    CHECK(nvn_lookup("nxnativeMp4ScanSourceV1")==mp4_scan_source_v1,"extension available only for registered main build");
    strcpy(module.build_id,"unknown");
    CHECK(!nvn_lookup("nxnativeMp4ScanSourceV1"),"unregistered build cannot look up private entry");
    CHECK(mp4_scan_source_v1(&commands,original)==original,"unregistered build cannot invoke private entry");
    strcpy(module.build_id,"8275fbe1ec5191c279f3a943ad8524f4");
    CHECK(mp4_scan_source_v1(&commands,original)==original,"inactive command buffer refused");
    h_nvnCommandBufferBeginRecording(&commands);
    scan_put32(original,0x14,1280);scan_put32(original,0x18,720);
    CHECK(mp4_scan_source_v1(&commands,original)==original,"native-size source unchanged");
    scan_put32(original,0x14,2560);scan_put32(original,0x18,1440);
    CHECK(mp4_scan_source_v1(&commands,original)==original,"1440p source unchanged");
    scan_put32(original,0x14,3840);scan_put32(original,0x18,2160);
    CHECK(mp4_scan_source_v1(&commands,original)==original,"2160p descriptor with a 1800p texture refused");
    scan_put32(original,0x14,3200);scan_put32(original,0x18,1800);
    source.d.samples=4;CHECK(mp4_scan_source_v1(&commands,original)==original,"multisample layout refused");source.d.samples=1;
    source.d.width=3199;CHECK(mp4_scan_source_v1(&commands,original)==original,"descriptor/texture extent mismatch refused");source.d.width=3200;
    scan_put32(original,0xc,0x9);CHECK(mp4_scan_source_v1(&commands,original)==original,"unvalidated integer format refused");scan_put32(original,0xc,0x12);
    memcpy(saved,original,sizeof saved);uint8_t saved_source[sizeof source];memcpy(saved_source,&source,sizeof source);
    const void *result=mp4_scan_source_v1(&commands,original);
    CHECK(result!=original && scan_get32(result,0x14)==1600 && scan_get32(result,0x18)==900,"borrowed view has the native query extent");
    uint8_t current_source[sizeof source];memcpy(current_source,&source,sizeof source);
    CHECK(!memcmp(original,saved,sizeof saved) && !memcmp(current_source,saved_source,sizeof source),"engine view and scene texture remain byte-identical");
    CHECK(!memcmp((const uint8_t *)result+0x48,&view,sizeof view),"stencil mode and swizzles preserved");
    memcpy(&ptr,(const uint8_t *)result+0x38,sizeof(const tex_t *));
    CHECK(ptr!=&source && ptr->d.width==1600 && ptr->d.height==900 && ptr->readback_proxy,"independent query texture requests explicit packed readback");
    // An ordinary command before/after the copy must retain its place.
    h_nvnCommandBufferBarrier(&commands,0);
    uint64_t handle=h_nvnCommandBufferEndRecording(&commands);size_t n;uint8_t *linear=nvn_rec_linear(handle,&n);
    op_hdr h;memcpy(&h,linear,sizeof h);
    CHECK(h.op==OP_COPY_T2T,"scaled copy is recorded before the next query command");
    const p_copy_t2t *copy=(const void *)(linear+sizeof h);
    CHECK(copy->src->serial==source.serial && copy->dst->serial==ptr->serial,"command owns source/destination snapshots");
    CHECK(copy->sr.width==3200 && copy->sr.height==1800 && copy->dr.width==1600 && copy->dr.height==900 && !copy->flags,"copy uses full scene, native query extent and nearest sampling");
    op_hdr next;memcpy(&next,linear+h.size,sizeof next);CHECK(next.op==OP_BARRIER,"following query command is not reordered");
    free(linear);
    // Switching from the confirmed mode to 4K reuses only the native-size query
    // texture. The full-resolution source is a distinct game-owned allocation.
    h_nvnCommandBufferBeginRecording(&commands);
    source.serial=2001;source.d.width=3840;source.d.height=2160;
    scan_put32(original,0x14,3840);scan_put32(original,0x18,2160);
    memcpy(saved,original,sizeof saved);memcpy(saved_source,&source,sizeof source);
    const tex_t *prior_query=ptr;
    result=mp4_scan_source_v1(&commands,original);
    CHECK(result!=original && scan_get32(result,0x14)==1600 && scan_get32(result,0x18)==900,"4K source obtains native query dimensions");
    memcpy(&ptr,(const uint8_t *)result+0x38,sizeof(const tex_t *));
    CHECK(ptr==prior_query,"resolution switch reuses the compatible native query texture");
    memcpy(current_source,&source,sizeof source);
    CHECK(!memcmp(original,saved,sizeof saved) && !memcmp(current_source,saved_source,sizeof source),"4K scene and original view remain unchanged");
    handle=h_nvnCommandBufferEndRecording(&commands);linear=nvn_rec_linear(handle,&n);memcpy(&h,linear,sizeof h);copy=(const void *)(linear+sizeof h);
    CHECK(h.op==OP_COPY_T2T && copy->src->d.width==3840 && copy->src->d.height==2160
        && copy->sr.width==3840 && copy->sr.height==2160 && copy->dr.width==1600 && copy->dr.height==900 && !copy->flags,"4K query records a nearest 3840x2160 to 1600x900 copy");
    free(linear);h_nvnCommandBufferFinalize(&commands);
    mp4_scan_source *state=pthread_getspecific(mp4_scan_key);pthread_setspecific(mp4_scan_key,NULL);mp4_scan_destroy(state);
    printf("SCAN_ADAPTER %d checks, %d failed\n",checks,failures);return failures!=0;
}
