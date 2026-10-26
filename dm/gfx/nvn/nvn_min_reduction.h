// Metal 3 has no sampler reduction mode. These two source-hash-checked legacy
// companions implement the observed integer explicit-LOD, linear MIN sample.
// Keep the ordinary function alive: selection depends on each actual binding,
// and the live opt-in switch permits an otherwise identical A/B comparison.
#pragma once

static const char *min_reduction_original(NvShader *sh) {
    if (!sh || strcmp(DMSH_CHAIN, "f80f842de321")) return NULL;
    if (sh->stage == 1 && !strcmp(sh->key, "151607c7a92386cc_b"))
        return "7c6e776f1cccd8ecf7310e5083664697e35de1ab85a9f21466cd307426de297f";
    if (sh->stage == 5 && !strcmp(sh->key, "ec5ebd6d9902a9a1_b"))
        return "bcd3d2c291a896ee1a22a828b7a461cc6501a33e835465df24d5c3cb71610435";
    return NULL;
}
static int min_reduction_candidate(NvShader *sh) { return min_reduction_original(sh) != NULL; }
static int min_reduction_sampler(const samp_par *p) {
    return p->reduction == 1 && p->minf == 3 && p->magf == 1 &&
        p->ws == 7 && p->wt == 7 && p->wr == 7 && !p->cmp_mode &&
        p->lod_min == 0.0f && p->lod_max == 1000.0f && p->lod_bias == 0.0f && p->aniso == 1.0f;
}
typedef struct { int textures, samplers, texture_ok, sampler_ok, tid, sid; } min_reduction_binding;
static void min_reduction_bind_sampler(min_reduction_binding *v, const nvn_rbind *b, int sid, const samp_par *p, int real) {
    if (b->reg != 17 || b->kind != NVN_RB_SAMPT) return;
    v->samplers++; v->sid = sid; v->sampler_ok = real && p && min_reduction_sampler(p);
}
static void min_reduction_bind_texture(min_reduction_binding *v, const nvn_rbind *b, int tid, id<MTLTexture> t) {
    if (b->reg != 17 || b->kind != NVN_RB_TEX) return;
    v->textures++; v->tid = tid;
    v->texture_ok = b->dtt == MTLTextureType2D && b->dpf == NVN_DPF_FLOAT && t && t.textureType == MTLTextureType2D && t.sampleCount == 1 &&
        (t.pixelFormat == MTLPixelFormatDepth16Unorm || t.pixelFormat == MTLPixelFormatDepth32Float || t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8);
}
static void min_reduction_binding_finish(NvShader *sh, const min_reduction_binding *v) {
    // This decision belongs to the encoded argument block, including reuse of
    // that block. Later descriptor registration must not change its function.
    sh->min_reduction_bound = v->textures == 1 && v->samplers == 1 && v->texture_ok && v->sampler_ok;
    sh->min_reduction_sid = v->sid; sh->min_reduction_tid = v->tid;
}
static BOOL min_reduction_record(NSDictionary *r, NvShader *sh, NSData *source, NSData *library, NSString *src) {
    if (![r isKindOfClass:NSDictionary.class]) return NO;
    NSDictionary *s = r[@"sampler"];
    if (![s isKindOfClass:NSDictionary.class]) return NO;
    const char *original = min_reduction_original(sh);
    if (!original || ![r[@"schema"] isEqual:@1] || ![r[@"chain"] isEqual:@DMSH_CHAIN] ||
        ![r[@"kind"] isEqual:@"linear-min-reduction-v1"] || ![r[@"key"] isEqual:@(sh->key)] ||
        ![r[@"stage"] isEqual:sh->stage == 1 ? @"ps" : @"cs"] ||
        ![r[@"entry"] isEqual:sh->fn.name] || ![r[@"reg"] isEqual:@17] || ![r[@"calls"] isEqual:@1] ||
        ![r[@"integer_explicit_lod"] isEqual:@YES] || ![r[@"texture_dimension"] isEqual:@"2d"] ||
        ![r[@"original_source_sha256"] isEqual:@(original)] ||
        ![r[@"source_sha256"] isEqual:nonpoint_digest(source)] ||
        ![r[@"library_sha256"] isEqual:nonpoint_digest(library)] ||
        ![s[@"reduction"] isEqual:@1] || ![s[@"minf"] isEqual:@3] || ![s[@"magf"] isEqual:@1] ||
        ![s[@"ws"] isEqual:@7] || ![s[@"wt"] isEqual:@7] || ![s[@"wr"] isEqual:@7] ||
        ![s[@"cmp_mode"] isEqual:@0] || ![s[@"lod_min"] isEqual:@0] || ![s[@"lod_max"] isEqual:@1000] ||
        ![s[@"lod_bias"] isEqual:@0] || ![s[@"aniso"] isEqual:@1] || ![r[@"helper_file"] isEqual:@"embedded"]) return NO;
    // The exporter hashes the exact UTF-8 helper body, excluding both markers.
    NSString *begin = @"// DM_MIN_REDUCTION_HELPER_BEGIN\n", *end = @"// DM_MIN_REDUCTION_HELPER_END\n";
    NSArray *parts = [src componentsSeparatedByString:begin];
    if (parts.count != 2) return NO;
    NSArray *tail = [parts[1] componentsSeparatedByString:end];
    if (tail.count != 2) return NO;
    return [r[@"helper_sha256"] isEqual:nonpoint_digest([tail[0] dataUsingEncoding:NSUTF8StringEncoding])];
}
static id<MTLFunction> min_reduction_compile(NvShader *sh) {
    @synchronized (sh) {
        if (!sh->min_reduction_tried) {
            sh->min_reduction_tried = 1;
            NSString *root = [g_msl_dir stringByAppendingPathComponent:@"min-reduction-v1"], *key = @(sh->key);
            NSData *meta = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:[key stringByAppendingString:@".json"]]];
            NSDictionary *record = meta ? [NSJSONSerialization JSONObjectWithData:meta options:0 error:nil] : nil;
            NSData *source = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:[key stringByAppendingString:@".metal"]]];
            NSData *library = [NSData dataWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]] options:NSDataReadingMappedIfSafe error:nil];
            NSString *src = source ? [[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding] : nil;
            NSError *e = nil;
            if (!src || !min_reduction_record(record, sh, source, library, src)) {
                e = [NSError errorWithDomain:@"NVNMinReduction" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Missing or mismatched min-reduction-v1 companion source/manifest"}];
            } else {
                MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion3_0;
                options.preserveInvariance = YES; options.fastMathEnabled = NO;
                CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
                sh->min_reduction_lib = [g_dev newLibraryWithSource:src options:options error:&e];
                if (sh->min_reduction_lib) {
                    sh->min_reduction_fn = sh->specialized ?
                        [sh->min_reduction_lib newFunctionWithName:sh->fn.name constantValues:[MTLFunctionConstantValues new] error:&e] :
                        [sh->min_reduction_lib newFunctionWithName:sh->fn.name];
                    MTLFunctionType want = sh->stage == 1 ? MTLFunctionTypeFragment : MTLFunctionTypeKernel;
                    if (!sh->min_reduction_fn || sh->min_reduction_fn.functionType != want ||
                        (sh->rf.nb && [sh->min_reduction_fn newArgumentEncoderWithBufferIndex:sh->rf.abi].encodedLength != sh->arg_len)) {
                        sh->min_reduction_fn = nil;
                        e = [NSError errorWithDomain:@"NVNMinReduction" code:2 userInfo:@{NSLocalizedDescriptionKey:@"Min-reduction entry point type or argument layout changed"}];
                    }
                }
                if (sh->min_reduction_fn) {
                    sh->min_reduction_fn.label = [NSString stringWithFormat:@"%s:min-reduction-v1:%@", sh->key, record[@"source_sha256"]];
                    MLOG("Min-reduction variant %s stage=%d entry=%s source=%s compiled in %.1fms; original function retained", sh->key, sh->stage,
                        sh->fn.name.UTF8String, [record[@"source_sha256"] UTF8String], (CFAbsoluteTimeGetCurrent() - t0) * 1000);
                }
            }
            if (!sh->min_reduction_fn) MLOG("[RED] Min-reduction variant %s failed: %s (ordinary function retained)", sh->key, e.localizedDescription.UTF8String);
        }
        return sh->min_reduction_fn;
    }
}
static void min_reduction_note(NvShader *sh, int state, int sid, int tid) {
    if (state != sh->min_reduction_last) {
        sh->min_reduction_last = state;
        if (sh->min_reduction_notes++ < 16) MLOG("Min-reduction selection frame=%llu key=%s stage=%d state=%s sid=%d tid=%d",
            (unsigned long long)atomic_load(&g_cur_frame), sh->key, sh->stage,
            state == 1 ? "explicit-lod-min" : state == 2 ? "binding-mismatch" : state == 3 ? "companion-failed" : state == 4 ? "pipeline-failed" : "ordinary", sid, tid);
    }
}
static id<MTLFunction> min_reduction_function(NvShader *sh) {
    if (!min_reduction_candidate(sh)) return nil;
    if (!gfx_diagnostic_bool(&g_min_reduction, "DM_NVN_MIN_REDUCTION") ||
        [MTLSamplerDescriptor instancesRespondToSelector:NSSelectorFromString(@"setReductionMode:")]) {
        min_reduction_note(sh, -1, 0, 0); return nil;
    }
    // encode_args captured this contract while binding the actual sampler and
    // final texture view under g_rmu. Never re-read the mutable registry here.
    int sid = sh->min_reduction_sid, tid = sh->min_reduction_tid;
    if (sh->min_reduction_pso_failed) { min_reduction_note(sh, 4, sid, tid); return nil; }
    if (!sh->min_reduction_bound) { min_reduction_note(sh, 2, sid, tid); return nil; }
    id<MTLFunction> fn = min_reduction_compile(sh);
    min_reduction_note(sh, fn ? 1 : 3, sid, tid);
    return fn;
}
