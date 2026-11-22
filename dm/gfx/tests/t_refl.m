// Reflection conversion unit tests: B reflection json (dmsh_build.py's <key>_b.json, the translation of the original Switch shaders) → C arrays. Every case is written here: the two
// real-shader cases are cut down from Super Mario Bros. Wonder's translation (chain f0f7a7b25de7), their field values copied from the json text, so the test needs no game files
// Dummy texture type MTLTextureType: 1D=0 1DArray=1 2D=2 2DArray=3 Cube=5 CubeArray=6 3D=7
#import "../nvn/nvn_refl.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include "../nvn/dmsh_abi.h"

enum { C = NVN_RB_CBUF, S = NVN_RB_SAMP, T = NVN_RB_TEX, I = NVN_RB_IMG, TB = NVN_RB_TBUF };
enum { F = NVN_DPF_FLOAT, U = NVN_DPF_UINT, D = NVN_DPF_DEPTH };
static int fail, n;
static void check(const char *what, long want, long got) {
    n++; int ok = want == got; fail += !ok;
    if (!ok) printf("  %-40s expected %ld → got %ld ✗\n", what, want, got);
}

int main(void) { @autoreleasepool {
    nvn_refl r; char err[160] = "";
    {   // Pointer discovery is metadata on the existing pool-table binding; there is no new GPU argument.
        NSMutableDictionary *b = [@{@"kind": @"gmem_map", @"arg_id": @0, @"msl_name": @"cp_s0", @"pointers": @[@[@1, @8], @[@0, @0]]} mutableCopy];
        NSDictionary *j = @{@"route": @"B", @"bindings": @[b]};
        check("raw pointers: conversion", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("raw pointers: existing table kind", NVN_RB_GMEM, r.b[0].kind); check("raw pointers: count", 2, r.ngptr);
        check("raw pointers: slot", 1, r.gptr[0].reg); check("raw pointers: byte", 8, r.gptr[0].off);
        for (id invalid in @[@[@[@16, @0]], @[@[@0, @65529]], @[@[@0]], @[@[@"bad", @0]], @"bad"]) {
            b[@"pointers"] = invalid; check("raw pointers: malformed source rejected", 1, nvn_refl_from_json(j, &r, err, sizeof err) != 0);
        }
        NSMutableArray *many = [NSMutableArray array]; for (int i = 0; i <= DMSH_GPTR_MAX; i++) [many addObject:@[@0, @(8 * i)]];
        b[@"pointers"] = many; check("raw pointers: too many rejected", 1, nvn_refl_from_json(j, &r, err, sizeof err) != 0);
    }
    {   // A row-wrapped BUFFER binding has a lead group followed by logical widths.
        NSArray *forms = @[
            @{@"kind": @"tex", @"reg": @0, @"msl_name": @"cp_t_tcb_8", @"msl_type": @"texture2d<uint>"},
            @{@"kind": @"tex_ubo", @"reg": @0, @"msl_name": @"cp_t_cb3_A", @"msl_type": @"texture2d<uint>"},
            @{@"kind": @"tex_ubo", @"reg": @0, @"msl_name": @"cp_t_cb3_A", @"msl_type": @"array<texture2d<uint>, 2>"},
            @{@"kind": @"tex", @"msl_name": @"cp_t_cb0_40000000", @"msl_type": @"array<texture2d<uint>, 2>"},
            @{@"kind": @"tex_c2a", @"off": @0, @"soff": @0, @"msl_name": @"cp_t_cb2_0", @"msl_type": @"array<texture2d<uint>, 2>"}];
        for (NSDictionary *form in forms) {
            NSMutableDictionary *binding = [form mutableCopy]; binding[@"fetch1d"] = @YES; binding[@"tbx"] = @0;
            NSMutableDictionary *table = [@{@"kind": @"tbias", @"n": @4, @"msl_name": @"cp_c17"} mutableCopy];
            NSDictionary *j = @{@"route": @"B", @"bindings": @[table, binding]};
            check("row binding: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
            check("row binding: fetch1d preserved", 1, r.b[1].f1d);
            check("row binding: lead index", 1, r.b[1].tbx);
            binding[@"tbx"] = @3;
            check("row binding: width past declared table rejected", 1, nvn_refl_from_json(j, &r, err, sizeof err) != 0);
            binding[@"tbx"] = @511; table[@"n"] = @512;
            check("row binding: width past capacity rejected", 1, nvn_refl_from_json(j, &r, err, sizeof err) != 0);
            binding[@"tbx"] = @0; [binding removeObjectForKey:@"fetch1d"];
            check("ordinary 2D offset metadata rejected", 1, nvn_refl_from_json(j, &r, err, sizeof err) != 0);
        }
    }
    {   // native image atomics (dmsh_atomic.py): one read_write image, marked atomic by the build; an image the shader only stores to is not
        NSDictionary *j = @{@"route": @"B", @"bindings": @[
            @{@"kind": @"img", @"reg": @2, @"arg_id": @0, @"msl_name": @"cp_i_cb2_4C_r32ui", @"msl_type": @"texture_buffer<uint, access::read_write>", @"atomic": @YES},
            @{@"kind": @"img", @"reg": @3, @"arg_id": @1, @"msl_name": @"cp_i_cb2_4E_r32ui", @"msl_type": @"texture3d<uint, access::write>"}]};
        check("atomic image: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("atomic image: IMG kind", NVN_RB_IMG, r.b[0].kind);
        check("atomic image: texture type", MTLTextureTypeTextureBuffer, r.b[0].dtt);
        check("atomic image: unsigned format", NVN_DPF_UINT, r.b[0].dpf);
        check("atomic image: written", 1, r.b[0].rw);
        check("atomic image: atomic flag", 1, r.b[0].atom);
        check("stored image: 3D texture type", MTLTextureType3D, r.b[1].dtt);
        check("stored image: no atomic flag", 0, r.b[1].atom);
    }
    {   // readable-tile calls (dmsh_fetch.py): one at an interpolated output, one projective, one at the pixel position times c[6].xy (frag_pos, Splatoon Raiders' ink stamps)
        NSDictionary *j = @{@"route": @"B", @"stage": @"ps", @"bindings": @[], @"fetch": @{@"calls": @[@{@"fc": @0, @"tex_reg": @1, @"varying_locn": @3},
            @{@"fc": @1, @"tex_reg": @2, @"varying_locn": @4, @"proj": @1}, @{@"fc": @2, @"tex_reg": @3, @"frag_pos": @[@[@0, @6, @0], @[@0, @6, @1]]}]}};
        check("fetch calls: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("fetch calls: three", 3, r.nfc);
        check("fetch calls: projective bit", 2, r.fc_proj);
        check("fetch calls: pixel-position bit", 4, r.fc_fpos);
        check("fetch calls: pixel-position has no interpolated output", 0xff, r.fc_locn[2]);
        check("fetch calls: x scale c[6].x", 6 << 2, r.fc_scale[2][0]);
        check("fetch calls: y scale c[6].y", 6 << 2 | 1, r.fc_scale[2][1]);
        NSDictionary *far = @{@"route": @"B", @"stage": @"ps", @"bindings": @[], @"fetch": @{@"calls": @[@{@"fc": @0, @"tex_reg": @6, @"frag_pos": @[@[@6, @989, @0], @[@6, @989, @1]]}]}};
        check("fetch calls: a vector past c[x][63] converts (Mario Kart World's c6[989])", 0, nvn_refl_from_json(far, &r, err, sizeof err));
        check("fetch calls: slot 6, vector 989, x", 6 << 16 | 989 << 2, r.fc_scale[0][0]);
        NSDictionary *bad = @{@"route": @"B", @"stage": @"ps", @"bindings": @[], @"fetch": @{@"calls": @[@{@"fc": @0, @"tex_reg": @3, @"frag_pos": @[@[@16, @6, @0], @[@0, @6, @1]]}]}};
        check("fetch calls: a uniform slot out of range is refused", 1, nvn_refl_from_json(bad, &r, err, sizeof err) != 0);
    }
    {   int f0 = fail;   // Route A is retired: PC-chain reflection (no route = B) must be rejected
        NSDictionary *pc = @{@"arg_buffer_index": @0, @"bindings": @[@{@"type": @"cbuffer", @"reg": @10, @"arg_id": @0, @"access": @"constant", @"msl_type": @"constant cb1_struct*"}], @"vertex_inputs": @[]};
        check("PC chain reflection (no route = B) fails", 1, nvn_refl_from_json(pc, &r, err, sizeof err) != 0);
        printf("%-60s %s\n", "negative control: PC chain reflection rejected (route A retired)", fail == f0 ? "✓" : "✗");
    }

    // Real shaders of Super Mario Bros. Wonder, cut down: ps_models_p#0 (5c1ca4e7c3e4657c_b, 12 of its 44 bindings: one of each kind and texture type, their argument IDs as the build wrote them)
    {   int f1 = fail;
        NSString *c1hex = [@"71c49cbf0000803f6f12833a83f9a23e818080bb8180803c0ad7233dcdcccc3dacc52737f343be3dcdcc4c3e800080b79a99993e9a99193fabaa2a3e" stringByPaddingToLength:512 withString:@"0" startingAtIndex:0];   // 256 bytes, 15 words of constants then zeros
        NSDictionary *j = @{@"route": @"B", @"stage": @"ps", @"c1": c1hex, @"vertex_inputs": @[], @"drv_unfillable": @[],
            @"drv_reads": @{@"0": @{@"vec4_offsets": @[@1376], @"dynamic": @NO}, @"2": @{@"vec4_offsets": @[@592, @1392], @"dynamic": @NO}},
            @"bindings": @[
                @{@"kind": @"support", @"arg_id": @0, @"msl_name": @"support_buffer", @"msl_type": @"constant _10*"},
                @{@"kind": @"cbuf", @"reg": @10, @"arg_id": @1, @"msl_name": @"fp_c13", @"msl_type": @"constant _16*"},
                @{@"kind": @"c1", @"bank": @1, @"arg_id": @4, @"msl_name": @"fp_c1", @"msl_type": @"constant _16*"},
                @{@"kind": @"drvcb", @"bank": @0, @"arg_id": @7, @"msl_name": @"fp_c0", @"msl_type": @"constant _16*"},
                @{@"kind": @"drvcb", @"bank": @2, @"arg_id": @8, @"msl_name": @"fp_c2", @"msl_type": @"constant _16*"},
                @{@"kind": @"sbuf", @"index": @0, @"reg": @4, @"rw": @NO, @"arg_id": @11, @"msl_name": @"fp_s0", @"msl_type": @"const device _39*"},
                @{@"kind": @"tbuf", @"reg": @8, @"arg_id": @13, @"msl_name": @"fp_t_tcb_6A", @"msl_type": @"texture_buffer<float>"},
                @{@"kind": @"tex", @"reg": @41, @"arg_id": @14, @"msl_name": @"fp_t_cb196610_105800AC", @"msl_type": @"texture2d<float>"},
                @{@"kind": @"samp", @"reg": @3, @"arg_id": @15, @"msl_name": @"fp_t_cb196610_105800ACSmplr", @"msl_type": @"sampler"},
                @{@"kind": @"tex", @"reg": @39, @"arg_id": @28, @"msl_name": @"fp_t_cb196610_105980A8", @"msl_type": @"depth2d_array<float>"},
                @{@"kind": @"tex", @"reg": @33, @"arg_id": @34, @"msl_name": @"fp_t_cb196610_105A009C", @"msl_type": @"depthcube<float>"},
                @{@"kind": @"tex", @"reg": @46, @"arg_id": @36, @"msl_name": @"fp_t_cb196610_105880B6", @"msl_type": @"texture3d<float>"}]};
        check("B fragment: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("B fragment: B flag", 1, r.route_b); check("B fragment: argument buffer slot", 0, r.abi); check("B fragment: binding count", 12, r.nb); check("B fragment: vertex input count", 0, r.nv);
        check("B fragment: has c1", 1, r.has_c1); check("B fragment: c1 length 256 (control section field)", 256, r.c1_len);
        uint32_t c1w; memcpy(&c1w, r.c1, 4); check("B fragment: first c1 word = −√1.5 (0xbf9cc471)", 0xbf9cc471u, c1w);
        memcpy(&c1w, r.c1 + 252, 4); check("B fragment: last c1 word zero", 0, c1w);
        check("B fragment: reads driver bank c0", 1, r.drv_read[0]); check("B fragment: reads driver bank c2", 1, r.drv_read[2]);
        struct { int i, kind, reg, aid, rw, dtt, dpf; } w[] = {
            {0, NVN_RB_SUPPORT, 0, 0, 0, -1, -1}, {1, C, 10, 1, 0, -1, -1}, {2, NVN_RB_C1, 0, 4, 0, -1, -1}, {3, NVN_RB_DRV0, 0, 7, 0, -1, -1},
            {4, NVN_RB_DRV2, 0, 8, 0, -1, -1}, {5, NVN_RB_SBUFP, 4, 11, 0, -1, -1}, {6, TB, 8, 13, 0, -1, -1}, {7, T, 41, 14, 0, 2, F},
            {8, S, 3, 15, 0, -1, -1}, {9, T, 39, 28, 0, 3, D}, {10, T, 33, 34, 0, 5, D}, {11, T, 46, 36, 0, 7, F}};
        for (unsigned k = 0; k < sizeof w / sizeof *w; k++) {
            const nvn_rbind *g = &r.b[w[k].i]; char m[64];
            snprintf(m, sizeof m, "B binding[%d] kind", w[k].i); check(m, w[k].kind, g->kind);
            snprintf(m, sizeof m, "B binding[%d] slot", w[k].i); check(m, w[k].reg, g->reg);
            snprintf(m, sizeof m, "B binding[%d] argument index", w[k].i); check(m, w[k].aid, g->aid);
            snprintf(m, sizeof m, "B binding[%d] read-write", w[k].i); check(m, w[k].rw, g->rw);
            if (w[k].dtt >= 0) { snprintf(m, sizeof m, "B binding[%d] dummy texture type", w[k].i); check(m, w[k].dtt, g->dtt);
                                 snprintf(m, sizeof m, "B binding[%d] dummy texture pixel class", w[k].i); check(m, w[k].dpf, g->dpf); }
        }
        printf("%-60s %s\n", "B fragment ps_models_p#0: support / c1 / driver banks / storage pointer / textures", fail == f1 ? "✓" : "✗");
    }
    {   int f1 = fail;   // vs_models_v#0 (54f730a6c0efc118_b), whole: two uniform buffers, four float4 attributes, no c1
        NSDictionary *j = @{@"route": @"B", @"stage": @"vs", @"c1": @"", @"drv_reads": @{}, @"drv_unfillable": @[],
            @"bindings": @[@{@"kind": @"cbuf", @"reg": @0, @"arg_id": @0, @"msl_name": @"vp_c3", @"msl_type": @"constant _16*"},
                           @{@"kind": @"cbuf", @"reg": @3, @"arg_id": @1, @"msl_name": @"vp_c6", @"msl_type": @"constant _16*"}],
            @"vertex_inputs": @[@{@"location": @0, @"msl_type": @"float4"}, @{@"location": @1, @"msl_type": @"float4"},
                                @{@"location": @2, @"msl_type": @"float4"}, @{@"location": @3, @"msl_type": @"float4"}],
            @"raster": @{@"pos_attr": @-1}};
        check("B vertex: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("B vertex: binding count", 2, r.nb); check("B vertex: input count", 4, r.nv); check("B vertex: c1 length 0 (control section +0x704 − +0x700) → no c1", 0, r.has_c1);
        check("B vertex: reads no driver bank", 0, r.drv_read[0] | r.drv_read[2]);
        for (int i = 0; i < 4 && i < r.nv; i++) { char m[48]; snprintf(m, sizeof m, "B vertex input[%d] attribute = location", i); check(m, i, r.v[i].nvn); check(m, i, r.v[i].loc); check("B vertex component float", NVN_CT_FLOAT, r.v[i].ct); }
        check("B vertex: binding[0] uniform slot 0", 0, r.b[0].reg); check("B vertex: binding[1] uniform slot 3", 3, r.b[1].reg);
        printf("%-60s %s\n", "B vertex vs_models_v#0: 4 attributes (attribute = MSL attribute)", fail == f1 ? "✓" : "✗");
    }
    {   int f1 = fail;   // an array of handles in a uniform buffer (Paper Mario: The Thousand-Year Door b352f7af1c86e9fa_b, its last four bindings): array<…, 32> takes 32 argument entries each
        NSDictionary *j = @{@"route": @"B", @"bindings": @[
            @{@"kind": @"tex_ubo", @"reg": @11, @"off": @16, @"arg_id": @14, @"msl_name": @"fp_t_cb14_4", @"msl_type": @"texture2d<float>"},
            @{@"kind": @"samp_ubo", @"reg": @11, @"off": @16, @"arg_id": @15, @"msl_name": @"fp_t_cb14_4Smplr", @"msl_type": @"sampler"},
            @{@"kind": @"tex_ubo", @"reg": @11, @"off": @0, @"arg_id": @16, @"msl_name": @"fp_t2d_cb14_0", @"msl_type": @"array<texture2d<float>, 32>"},
            @{@"kind": @"samp_ubo", @"reg": @11, @"off": @0, @"arg_id": @48, @"msl_name": @"fp_t2d_cb14_0Smplr", @"msl_type": @"array<sampler, 32>"}]};
        check("B handle array: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("B handle array: single texture n", 0, r.b[0].n); check("B handle array: single texture offset", 16, r.b[0].off); check("B handle array: single sampler n", 0, r.b[1].n);
        check("B handle array: texture kind", NVN_RB_TEXU, r.b[2].kind); check("B handle array: texture n", 32, r.b[2].n); check("B handle array: texture dummy type 2D", 2, r.b[2].dtt); check("B handle array: texture slot", 11, r.b[2].reg);
        check("B handle array: sampler kind", NVN_RB_SAMPU, r.b[3].kind); check("B handle array: sampler n", 32, r.b[3].n); check("B handle array: bindless", 1, r.bindless);
        printf("%-60s %s\n", "B array of 32 handles in uniform slot 11 (texture + sampler)", fail == f1 ? "✓" : "✗");
    }
    {   int f1 = fail;   // a SeparateSamplerId (handle type 2, The Witcher 3): the texture from c2, the sampler ID itself in word 0 of uniform slot 7
        NSDictionary *j = @{@"route": @"B", @"bindings": @[
            @{@"kind": @"tex", @"reg": @128, @"arg_id": @0, @"msl_name": @"fp_t_cb720898_2000005A", @"msl_type": @"texture2d<float>"},
            @{@"kind": @"samp_ubo", @"reg": @7, @"off": @0, @"raw_id": @YES, @"arg_id": @1, @"msl_name": @"fp_t_cb720898_2000005ASmplr", @"msl_type": @"sampler"},
            @{@"kind": @"samp_ubo", @"reg": @11, @"off": @16, @"arg_id": @2, @"msl_name": @"fp_t_cb14_4Smplr", @"msl_type": @"sampler"}]};
        check("B sampler ID: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("B sampler ID: kind", NVN_RB_SAMPU, r.b[1].kind); check("B sampler ID: uniform slot", 7, r.b[1].reg); check("B sampler ID: raw_id", 1, r.b[1].raw_id);
        check("B sampler ID: a handle's sampler has no raw_id", 0, r.b[2].raw_id);
        printf("%-60s %s\n", "B separate sampler ID in uniform slot 7 (raw_id)", fail == f1 ? "✓" : "✗");
    }
    {   int f1 = fail;   // handles in a uniform buffer beyond textures: Unreal Engine 5.4 on the Switch 2 keeps images and buffer textures there, and ORs a texture and a sampler word in either order
        NSDictionary *j = @{@"route": @"B", @"bindings": @[
            @{@"kind": @"img_ubo", @"reg": @0, @"off": @8, @"arg_id": @0, @"msl_name": @"cp_i_cb3_2_", @"msl_type": @"texture_buffer<float, access::write>", @"atomic": @YES},
            @{@"kind": @"tbuf_ubo", @"reg": @1, @"off": @16, @"soff": @0, @"sreg": @2, @"arg_id": @1, @"msl_name": @"cp_tb_cb4_4", @"msl_type": @"texture_buffer<uint>"},
            @{@"kind": @"tex_ubo", @"reg": @0, @"off": @88, @"soff": @0, @"sreg": @2, @"arg_id": @2, @"msl_name": @"fp_t_cb262147_10000016", @"msl_type": @"texture2d<float>"},
            @{@"kind": @"tex_ubo", @"reg": @0, @"off": @24, @"arg_id": @3, @"msl_name": @"fp_t_cb3_6", @"msl_type": @"texture2d<float>"}]};
        check("B uniform handles: conversion succeeds", 0, nvn_refl_from_json(j, &r, err, sizeof err));
        check("B uniform handles: image kind", NVN_RB_IMGU, r.b[0].kind); check("B uniform handles: image offset", 8, r.b[0].off); check("B uniform handles: image written", 1, r.b[0].rw); check("B uniform handles: image atomic", 1, r.b[0].atom);
        check("B uniform handles: buffer texture kind", NVN_RB_TBUFU, r.b[1].kind); check("B uniform handles: buffer texture slot", 1, r.b[1].reg); check("B uniform handles: bindless", 1, r.bindless);
        check("B uniform handles: buffer texture's other word at byte 0", 1, r.b[1].soff); check("B uniform handles: buffer texture's other slot", 2, r.b[1].sreg);
        check("B uniform handles: other word at byte 0 of slot 2", 1, r.b[2].soff); check("B uniform handles: its slot", 2, r.b[2].sreg); check("B uniform handles: one word has no other", 0, r.b[3].soff);
        printf("%-60s %s\n", "B images, buffer textures and two-word handles in uniform slots", fail == f1 ? "✓" : "✗");
    }
    {   int f1 = fail;
        NSDictionary *b1 = @{@"route": @"B", @"bindings": @[@{@"kind": @"foo", @"arg_id": @0, @"msl_name": @"x", @"msl_type": @"constant _16*"}]};
        NSDictionary *b2 = @{@"route": @"B", @"bindings": @[@{@"kind": @"tex", @"reg": [NSNull null], @"arg_id": @0, @"msl_name": @"fp_t_x", @"msl_type": @"texture2d<float>"}]};
        check("B unknown kind fails", 1, nvn_refl_from_json(b1, &r, err, sizeof err) != 0);
        check("B texture without a resolved slot fails", 1, nvn_refl_from_json(b2, &r, err, sizeof err) != 0);
        printf("%-60s %s\n", "B negative control: unknown kind / missing slot", fail == f1 ? "✓" : "✗");
    }
    printf("== %d checks, %d failed ==\n", n, fail);
    return fail != 0;
} }
