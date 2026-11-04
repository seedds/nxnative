// NVN2 task/mesh programs execute in Metal object/mesh stages. The normal raster
// state is shared with vertex draws; guest stages retain separate resource tables.
static int draw_raster_state(id<MTL4RenderCommandEncoder> encoder) {
    {
        uint32_t stencil_reference = S.st_ref[0]; // enc_r_stencil_ref sends the front value only
        if (!(g_ed.ok & ED_SREF) || g_ed.sref != stencil_reference) {
            enc_r_stencil_ref(encoder, stencil_reference, stencil_reference);
            g_ed.sref = stencil_reference;
            g_ed.ok |= ED_SREF;
        }
    }
    // Viewport / scissor (clamped within the target size)
    NSUInteger target_width = g_pass.tw, target_height = g_pass.th;
    {
        MTLViewport viewport = S.has_vp
                                   ? (MTLViewport){S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0], S.dr[1]}
                                   : (MTLViewport){0, 0, target_width, target_height, 0, 1};
        if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) {
            viewport.originY += viewport.height;
            viewport.height = -viewport.height;
        } // lower-left origin (see g_origin_ll)
        // Viewport swizzle: NEGATIVE_Y negates clip y, the same as the viewport flipped about its centre; facing is
        // decided after it on the GPU as in Metal, so the winding stays. Other swizzles (axis exchanges, negated x / z
        // / w) are no viewport change and are drawn unswizzled
        {
            uint32_t swizzle;
            memcpy(&swizzle, S.vp_sw[0], 4);
            if (swizzle == 0x100) {
                viewport.originY += viewport.height;
                viewport.height = -viewport.height;
            } else if (swizzle) {
                RED_ONCE(108, "viewport swizzle %d,%d,%d,%d not implemented (drawn unswizzled)", S.vp_sw[0][0],
                         S.vp_sw[0][1] ^ 2, S.vp_sw[0][2] ^ 4, S.vp_sw[0][3] ^ 6);
            }
        }
        if (!(g_ed.ok & ED_VP) || memcmp(&g_ed.vp, &viewport, sizeof viewport)) { // NOLINT(bugprone-suspicious-memory-comparison):
                                                                                  // exact state bits are the cache key.
            enc_r_viewport(encoder, viewport);
            g_ed.vp = viewport;
            g_ed.ok |= ED_VP;
        }
    }
    MTLScissorRect scissor = {0, 0, target_width, target_height};
    if (R.has_sc && !S.rast_discard) { // raster state cannot suppress vertex storage writes
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w,
             y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)target_width) {
            x1 = target_width;
        }
        if (y1 > (long)target_height) {
            y1 = target_height;
        }
        if (x1 <= x0 || y1 <= y0) {
            PD("Z %llu draw skipped: empty scissor (%d,%d %dx%d, target %lux%lu)", g_stat_pass, R.sc.x, R.sc.y, R.sc.w,
               R.sc.h, (unsigned long)target_width, (unsigned long)target_height);
            return 0;
        }
        scissor = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    if (!(g_ed.ok & ED_SC) || memcmp(&g_ed.sc, &scissor, sizeof scissor)) {
        enc_r_scissor(encoder, scissor);
        g_ed.sc = scissor;
        g_ed.ok |= ED_SC;
    }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK && !S.rast_discard) {
        PD("Z %llu draw skipped: both faces culled", g_stat_pass);
        return 0;
    }
    {
        int cull_mode = S.poly.cull == NVN_FACE_FRONT  ? MTLCullModeFront
                        : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack
                                                       : MTLCullModeNone;
        int winding = (S.poly.front == NVN_FRONT_FACE_CCW) != g_queue_origin_ll
                          ? MTLWindingCounterClockwise
                          : MTLWindingClockwise; // NVN's raster flip is captured when the queue is initialized, rather
                                                 // than when the device origin changes.
        int fill_mode = S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill,
            depth_clip = !S.depth_clamp; // depth clip off = SetDepthClamp
        float depth_bias[3] = {0, 0, 0};
        if (S.poly.offset_en & 4) {
            depth_bias[0] = S.po.units;
            depth_bias[1] = S.po.factor;
            depth_bias[2] = S.po.clamp;
        }
        if (!(g_ed.ok & ED_CULL) || g_ed.cull != cull_mode) {
            enc_r_cull(encoder, (MTLCullMode)cull_mode);
            g_ed.cull = cull_mode;
            g_ed.ok |= ED_CULL;
        }
        if (!(g_ed.ok & ED_WIND) || g_ed.wind != winding) {
            enc_r_winding(encoder, (MTLWinding)winding);
            g_ed.wind = winding;
            g_ed.ok |= ED_WIND;
        }
        if (!(g_ed.ok & ED_FILL) || g_ed.fill != fill_mode) {
            enc_r_fill(encoder, (MTLTriangleFillMode)fill_mode);
            g_ed.fill = fill_mode;
            g_ed.ok |= ED_FILL;
        }
        if (!(g_ed.ok & ED_BIAS) ||
            memcmp(g_ed.bias, depth_bias, sizeof depth_bias)) { // NOLINT(bugprone-suspicious-memory-comparison): exact
                                                                // state bits are the cache key.
            enc_r_depth_bias(encoder, depth_bias[0], depth_bias[1], depth_bias[2]);
            memcpy(g_ed.bias, depth_bias, sizeof depth_bias);
            g_ed.ok |= ED_BIAS;
        }
        if (!(g_ed.ok & ED_CLIP) || g_ed.clip != depth_clip) {
            enc_r_depth_clip(encoder, depth_clip);
            g_ed.clip = depth_clip;
            g_ed.ok |= ED_CLIP;
        }
        if (!(g_ed.ok & ED_BLEND) ||
            memcmp(g_ed.blend, S.blend_color, sizeof g_ed.blend)) { // NOLINT(bugprone-suspicious-memory-comparison):
                                                                    // exact state bits are the cache key.
            enc_r_blend_color(encoder, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]);
            memcpy(g_ed.blend, S.blend_color, sizeof g_ed.blend);
            g_ed.ok |= ED_BLEND;
        }
    }
    return 1;
}

typedef struct {
    pso_key raster;
    void *object_fn, *mesh_fn;
} taskmesh_pso_key;
static nvn_hmap g_taskmesh_pso = {.klen = sizeof(taskmesh_pso_key)};
static id<MTLRenderPipelineState> taskmesh_pipeline(NvProg *program) {
    NvShader *mesh = program->sh[NVN_SHADER_STAGE_MESH], *task = program->sh[NVN_SHADER_STAGE_TASK],
             *fragment = program->sh[1];
    id<MTLFunction> mesh_function = task ? mesh->fn : mesh->mesh_direct_fn;
    if (!mesh_function || (task && !task->fn)) {
        return nil;
    }
    taskmesh_pso_key key = {0};
    pso_key_fill(&key.raster, mesh, fragment, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
    if (S.rast_discard) {
        key.raster.fsfn = DISCARD_MARK;
    }
    memset(key.raster.attr, 0, sizeof key.raster.attr);
    memset(key.raster.stream, 0, sizeof key.raster.stream);
    key.object_fn = task ? (__bridge void *)task->fn : NULL;
    key.mesh_fn = (__bridge void *)mesh_function;
    id cached = hm_get_id(&g_taskmesh_pso, &key);
    if (cached) {
        return cached == NSNull.null ? nil : cached;
    }
    MTLRenderPipelineDescriptor *raster_descriptor =
        pso_desc(&key.raster, mesh, S.rast_discard ? nil : fragment, nil, 0);
    MTLMeshRenderPipelineDescriptor *mesh_descriptor = [MTLMeshRenderPipelineDescriptor new];
    mesh_descriptor.objectFunction = task ? task->fn : nil;
    mesh_descriptor.meshFunction = mesh_function;
    mesh_descriptor.fragmentFunction = raster_descriptor.fragmentFunction;
    if (task) {
        mesh_descriptor.maxTotalThreadsPerObjectThreadgroup = task->mesh_threads;
    }
    mesh_descriptor.maxTotalThreadsPerMeshThreadgroup = mesh->mesh_threads;
    mesh_descriptor.payloadMemoryLength = task ? task->mesh_payload : 0;
    mesh_descriptor.rasterSampleCount = raster_descriptor.rasterSampleCount;
    mesh_descriptor.alphaToCoverageEnabled = raster_descriptor.alphaToCoverageEnabled;
    mesh_descriptor.rasterizationEnabled = raster_descriptor.rasterizationEnabled;
    for (int i = 0; i < 8; i++) {
        mesh_descriptor.colorAttachments[i] = raster_descriptor.colorAttachments[i];
    }
    mesh_descriptor.depthAttachmentPixelFormat = raster_descriptor.depthAttachmentPixelFormat;
    mesh_descriptor.stencilAttachmentPixelFormat = raster_descriptor.stencilAttachmentPixelFormat;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithMeshDescriptor:mesh_descriptor
                                                                                  options:MTLPipelineOptionNone
                                                                               reflection:nil
                                                                                    error:&error];
    hm_put_id(&g_taskmesh_pso, &key, pipeline ?: NSNull.null);
    MLOG("%stask/mesh pipeline: task %s mesh %s fragment %s%s%s", pipeline ? "" : "[RED] ", task ? task->key : "-",
         mesh->key, fragment ? fragment->key : "-", pipeline ? "" : ": ",
         pipeline ? "" : error.localizedDescription.UTF8String);
    return pipeline;
}

static void taskmesh_draw(const p_draw_mesh *draw) {
    g_draw_binst = 0; // task/mesh draws have no base instance for c0 + 0x644
    g_mdxc.next = NULL;
    if (!draw->x || !draw->y || !draw->z) {
        return;
    }
    if (draw->ind) {
        RED_ONCE(160, "indirect task/mesh draw has no decoder");
        return;
    }
    NvProg *program = S.prog ? prog_get(S.prog) : nil;
    NvShader *mesh = program ? program->sh[NVN_SHADER_STAGE_MESH] : nil,
             *task = program ? program->sh[NVN_SHADER_STAGE_TASK] : nil;
    if (!mesh) {
        RED_ONCE(161, "task/mesh draw has no mesh program");
        return;
    }
    // The vertex-only pass optimizations do not prove mesh work's coverage or
    // dependencies. Return to the full attachment pass at this command boundary.
    if (g_enc && (g_enc_mode || g_rt_merged || g_pass.layered || g_fz.active || g_sm_pass || g_rt_range_active)) {
        rp_end();
    }
    if (g_enc && g_rp_aw &&
        (!R.has_sc || (long)R.sc.x + R.sc.w > (long)g_rp_aw || (long)R.sc.y + R.sc.h > (long)g_rp_ah)) {
        rp_end();
    }
    if (g_nubd) {
        rp_end();
        ubd_land();
    }
    image_prepare(program, (1u << NVN_SHADER_STAGE_TASK) | (1u << NVN_SHADER_STAGE_MESH) | (1u << 1));
    order_draw(program, 1);
    if (g_nlc && !g_enc) {
        lc_take(program);
    }
    fb_prepare(program);
    if (g_enc && g_vis_counting && g_vis_n >= VIS_SLOTS) {
        rp_end();
    }
    id<MTL4RenderCommandEncoder> encoder = (id<MTL4RenderCommandEncoder>)rp_begin_ex(0);
    if (!encoder) {
        return;
    }
    id<MTLRenderPipelineState> pipeline = taskmesh_pipeline(program);
    if (!pipeline) {
        return;
    }
    enc_r_pso(encoder, pipeline);
    g_enc_pso = pipeline;
    id<MTLDepthStencilState> depth_stencil = dss_get(g_pass.hd, g_pass.hs);
    enc_r_dss(encoder, depth_stencil);
    g_enc_dss = depth_stencil;
    if (!draw_raster_state(encoder)) {
        return;
    }
    args_sync_dirty();
    [encoder setArgumentTable:g_mt atStages:MTLRenderStageMesh];
    if (task) {
        [encoder setArgumentTable:g_ot atStages:MTLRenderStageObject];
    }
    if (task) {
        encode_args(encoder, task, NVN_SHADER_STAGE_TASK, 0);
    }
    encode_args(encoder, mesh, NVN_SHADER_STAGE_MESH, 0);
    if (program->sh[1] && !S.rast_discard) {
        encode_args(encoder, program->sh[1], 1, 0);
    }
    if (g_enc != encoder) {
        // A texture view upgrade ends its old encoder; it is now materialized.
        static int again;
        if (again) {
            RED_ONCE(162, "task/mesh resource binding ended its pass twice");
            return;
        }
        again = 1;
        taskmesh_draw(draw);
        again = 0;
        return;
    }
    id<MTLBuffer> first_buffer;
    size_t first_offset = arena_alloc(16, &first_buffer);
    uint32_t first[4] = {draw->first, 0, 0, 0};
    memcpy((uint8_t *)first_buffer.contents + first_offset, first, sizeof first);
    id<MTL4ArgumentTable> table = task ? g_ot : g_mt;
    if (rec_on()) {
        rec_table_op(RO_T_ADDR, table, first_buffer.gpuAddress + first_offset, DMSH_MESH_FIRST_SLOT);
    } else {
        [table setAddress:first_buffer.gpuAddress + first_offset atIndex:DMSH_MESH_FIRST_SLOT];
    }
    if (g_vis_counting && g_enc_has_vis) {
        if (g_vis_cur < 0 || g_vis_cur_pass != g_pass.serial) {
            g_vis_cur = (int)g_vis_n++;
            g_vis_cur_pass = g_pass.serial;
            if (g_nvs == g_vscap) {
                g_vscap = g_vscap ? g_vscap * 2 : 64;
                vis_slot *next = realloc(g_vs, g_vscap * sizeof *g_vs);
                if (!next) {
                    MDIE("visibility slot allocation failed");
                }
                g_vs = next;
            }
            g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), (uint32_t)g_vis_cur};
        }
        enc_r_visibility(encoder, MTLVisibilityResultModeCounting, (NSUInteger)g_vis_cur * 8);
        g_enc_vis = g_vis_cur + 1;
    }
    udc_note_draw(program, draw_color_writes(program));
    swrite_note(program);
    order_note(program);
    rt_range_note_depth();
    [encoder drawMeshThreadgroups:MTLSizeMake(draw->x, draw->y, draw->z)
        threadsPerObjectThreadgroup:MTLSizeMake(task ? task->mesh_threads : 1, 1, 1)
          threadsPerMeshThreadgroup:MTLSizeMake(mesh->mesh_threads, 1, 1)];
    g_stat_draw++;
    if (passdump()) {
        PD("mesh %llu first=%u groups=%u,%u,%u task=%s mesh=%s", g_stat_pass, draw->first, draw->x, draw->y, draw->z,
           task ? task->key : "-", mesh->key);
    }
}
