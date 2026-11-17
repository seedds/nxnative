// NVN2 direct ray dispatch. The original module code reads its SDK resource
// records; only the backend-owned core/descriptor records are synthesized here.
static void rtd_u32(uint8_t *p, size_t offset, uint32_t value) { memcpy(p + offset, &value, 4); }
static void rtd_u64(uint8_t *p, size_t offset, uint64_t value) { memcpy(p + offset, &value, 8); }
static unsigned rtd_bits(uint32_t n) { return n <= 1 ? 0 : 32u - (unsigned)__builtin_clz(n - 1); }
static void rtd_range(uint64_t address, uint64_t size, const char *name) {
    void *buffer; uint64_t offset, gpu;
    if (!address || !size || address > UINT64_MAX - size || !mtl_gpu_addr(address, size, &buffer, &offset, &gpu))
        MDIE("[RED] ray-tracing %s %#llx + %#llx is outside a memory pool", name, (unsigned long long)address, (unsigned long long)size);
}
static void rtd_table(const nvn_rt_table *table, const char *name) {
    if (!table->size) {
        if (table->address || table->stride) MDIE("[RED] ray-tracing %s has an unsupported zero-size sentinel", name);
        return;
    }
    if ((table->address & (DMSH_RT_RECORD_ALIGN - 1)) || table->stride < DMSH_RT_RECORD_ALIGN ||
        (table->stride & (DMSH_RT_RECORD_ALIGN - 1)) || table->stride > UINT32_MAX || table->size > UINT32_MAX || table->size < DMSH_RT_HANDLE_BYTES)
        MDIE("[RED] ray-tracing %s has an invalid record alignment, stride or token extent", name);
    rtd_range(table->address, table->size, name);
}
static uint8_t rtd_image_class(unsigned slot) { // registration lock held
    int tid = (int)(bound_img(NVN_RT_RESOURCE_STAGE, (int)slot) & 0xfffff);
    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok || g_treg[tid].gone) return 3;
    void *view = NULL; if (!tex_fast_rid(tid, &view) || !view) return 3;
    id<MTLTexture> texture = (__bridge id<MTLTexture>)view;
    int dpf = mtl_pf_dpf(texture.pixelFormat);
    return dpf == NVN_DPF_FLOAT ? 0 : dpf == NVN_DPF_SINT ? 1 : dpf == NVN_DPF_UINT ? 2 : 3;
}
static void dispatch_rays(const p_rt_dispatch *dp) {
    if (!dp->width || !dp->height || !dp->depth) return;
    unsigned xb = rtd_bits(dp->width), zb = rtd_bits(dp->depth);
    if (dp->width > 0x40000000 || dp->height > 0x40000000 || dp->depth > 0x40000000 ||
        (uint64_t)dp->width * dp->height > 0x40000000ull / dp->depth || xb + zb + rtd_bits(dp->height) > 32)
        MDIE("[RED] ray-tracing launch dimensions exceed the supported packed launch index");
    NvProg *program = S.rprog ? serial_get(S.rprog->serial) : nil;
    NvRayProgram *rt = program ? program->raytracing : nil;
    if (!rt) MDIE("[RED] ray-tracing dispatch has no initialized ray-tracing program");
    NvRayLibrary *library = rt->library; NvShader *shader = library->shader;
    const uint16_t *tg = shader->rf.tg;
    if (!tg[0] || !tg[1] || !tg[2] || (uint64_t)tg[0] * tg[1] * tg[2] > library->pipeline.maxTotalThreadsPerThreadgroup)
        MDIE("[RED] ray-tracing library has an invalid or unsupported threadgroup size");
    if ((dp->raygen & (DMSH_RT_RECORD_ALIGN - 1))) MDIE("[RED] ray-generation record is not aligned");
    rtd_range(dp->raygen, DMSH_RT_HANDLE_BYTES, "ray-generation record");
    rtd_table(&dp->miss, "miss table"); rtd_table(&dp->hit, "hit table"); rtd_table(&dp->callable, "callable table");
    if (dp->callable.size) MDIE("[RED] callable ray-tracing shaders are not supported");

    // Raw pointers can read any bound pool. Deferred uniform updates and pending
    // attachment clears must land before the descriptor table is consumed.
    if (g_nubd) ubd_land();
    rp_flush(); rp_end();
    image_prepare_shader(shader, NVN_RT_RESOURCE_STAGE);
    uint64_t root = mtl_as_bound_address(8, (unsigned)rt->asSlot);
    nvn_as_binding scene;
    if (!mtl_as_acquire(root, &scene) || scene.type != NVN_AS_BINDING_TLAS || !scene.resource_id || !scene.instance_sbt_offsets)
        MDIE("[RED] ray-tracing dispatch requires a built top-level AS at slot %d", rt->asSlot);

    id<MTLBuffer> constants, parameters, errors;
    size_t co = arena_alloc(0x10000, &constants), po = arena_alloc(library->parameterWords * 4u, &parameters), eo = arena_alloc(16, &errors);
    uint8_t *bank = (uint8_t *)constants.contents + co;
    uint8_t *p = (uint8_t *)parameters.contents + po;
    memset(bank, 0, 0x10000); memset(p, 0, library->parameterWords * 4u); memset((uint8_t *)errors.contents + eo, 0, 16);
    uint8_t *core = bank + library->coreOffset, *descriptor = bank + library->descriptorOffset, *handles = descriptor + DMSH_RT_DESCRIPTOR_HANDLES;
    // O/E/T/C are zero in this backend: Metal's helper stack is not guest local
    // memory. B and K describe the checked, metadata-sized private arena.
    rtd_u32(core, 0x20, dp->width); rtd_u32(core, 0x24, dp->height); rtd_u32(core, 0x28, dp->depth);
    rtd_u32(core, 0x2c, library->localBytes); rtd_u32(core, 0x34, 1u << 16); // u16 +0x36: supported trace depth 1
    rtd_u64(core, 0x38, dp->raygen); rtd_u64(core, 0x40, dp->miss.address); rtd_u64(core, 0x48, dp->hit.address); rtd_u64(core, 0x50, dp->callable.address);
    rtd_u32(core, 0x60, (uint32_t)dp->miss.stride); rtd_u32(core, 0x64, (uint32_t)dp->hit.stride); rtd_u32(core, 0x68, (uint32_t)dp->callable.stride);
    rtd_u32(core, 0x70, (1u << xb) - 1); rtd_u32(core, 0x74, xb + zb); rtd_u32(core, 0x78, xb); rtd_u32(core, 0x7c, (1u << zb) - 1);
    rtd_u32(core, 0xb0, dp->hit.size ? (uint32_t)dp->hit.size : UINT32_MAX);
    rtd_u32(core, 0xb4, dp->miss.size ? (uint32_t)dp->miss.size : UINT32_MAX);
    rtd_u32(core, 0xb8, dp->callable.size ? (uint32_t)dp->callable.size : UINT32_MAX);
    for (unsigned i = 0; i < 16; i++) {
        const p_bind_buf *u = &S.ubo[NVN_RT_RESOURCE_STAGE][i], *s = &S.ssbo[NVN_RT_RESOURCE_STAGE][i];
        rtd_u64(descriptor, 0x210 + i * 16, u->addr); rtd_u32(descriptor, 0x218 + i * 16, u->size);
        rtd_u64(descriptor, 0x310 + i * 16, s->addr); rtd_u32(descriptor, 0x318 + i * 16, s->size);
    }
    for (unsigned i = 0; i < 8; i++) rtd_u32(handles, i * 4, i);
    for (unsigned i = 0; i < 32; i++) {
        rtd_u64(handles, 0x20 + i * 8, S.tex[NVN_RT_RESOURCE_STAGE][i]);
        rtd_u64(handles, 0x568 + i * 8, S.samp[NVN_RT_RESOURCE_STAGE][i]);
        rtd_u64(handles, 0x668 + i * 8, mtl_as_bound_address(8, i));
    }
    for (unsigned i = 0; i < DMSH_RT_IMAGE_SLOTS; i++) rtd_u64(handles, 0x120 + i * 8, S.img[NVN_RT_RESOURCE_STAGE][i]);
    for (unsigned i = 0; i < 128; i++) rtd_u64(handles, 0x168 + i * 8, S.tex[NVN_RT_RESOURCE_STAGE][NVN_SEPTEX_SLOT0 + i]);
    memcpy(descriptor + DMSH_RT_DESCRIPTOR_PUSH, S.push[1], sizeof S.push[1]);
    rtd_u64(p, 0, rt->tag); rtd_u64(p, 8, root); rtd_u32(p, 24, scene.instance_count);
    rtd_u64(p, 32, dp->miss.size); rtd_u64(p, 40, dp->hit.size); rtd_u64(p, 48, dp->callable.size);
    memcpy(p + DMSH_RT_MEMBERSHIP * 4u, rt->membership.bytes, rt->membership.length);
    rt_argument_values values = {.scene=scene.resource_id, .parameters=parameters.gpuAddress+po, .errors=errors.gpuAddress+eo,
        .instance_sbt=scene.instance_sbt_offsets, .bank0=library->bank0.gpuAddress, .bank3=constants.gpuAddress+co};
    os_unfair_lock_lock(&g_rmu);
    for (unsigned i = 0; i < DMSH_RT_IMAGE_SLOTS; i++) {
        values.image_class[i] = rtd_image_class(i); rtd_u32(p, (DMSH_RT_IMAGE_CLASSES + i) * 4u, values.image_class[i]);
    }
    os_unfair_lock_unlock(&g_rmu);
    cbs_note(shader); za_shader((__bridge const void *)shader, shader->key);
    // Preparing descriptors can upload a sampled view. Finish that before the
    // query barrier; bind the completed argument block after opening the encoder.
    encode_args_full(nil, shader, NVN_RT_RESOURCE_STAGE, 0, &values);
    id<MTL4ComputeCommandEncoder> encoder = dispatch_enc(); as_barrier();
    bind_args((id)encoder, NVN_RT_RESOURCE_STAGE, 0, shader->ab_buf, shader->ab_gpu, shader->ab_off, shader->rf.abi);
    enc_c_pso(encoder, library->pipeline);
    enc_c_dispatch(encoder, MTLSizeMake(((uint64_t)dp->width + tg[0] - 1) / tg[0], ((uint64_t)dp->height + tg[1] - 1) / tg[1], ((uint64_t)dp->depth + tg[2] - 1) / tg[2]), MTLSizeMake(tg[0], tg[1], tg[2]));
    // Raw accesses have unknown reach, like the general compute fallback. Keep
    // subsequent copies/dispatches ordered and indirect/vertex snapshots stale.
    g_bw_lo[0]=0; g_bw_hi[0]=UINT64_MAX; g_bw_n=1; g_br_n=0; g_blit_ops=1;
    g_wr_epoch++; g_mdx_epoch++; g_vbuf_write_epoch++; g_dsp_game_bar=0;
    bufsnap_after(shader, encoder);
    if (g_nbcp) bcp_flush();   // image stores reach their compressed aliases before the next command
    uint64_t tag = rt->tag;
    [g_commit_blocks addObject:^{
        const uint32_t *fault = (const void *)((const uint8_t *)errors.contents + eo);
        if (fault[0]) MDIE("[RED] ray-tracing shader fault %u at packed invocation %#x (detail %#x, function/aux %#x, program %llu)", fault[0], fault[1], fault[2], fault[3], (unsigned long long)tag);
    }];
    static int first; if (!first++) MLOG("ray tracing: first native module dispatch %ux%ux%u, AS slot %d, %u instances, %u private bytes", dp->width, dp->height, dp->depth, rt->asSlot, scene.instance_count, library->localBytes);
}
