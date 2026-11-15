// The OpenGL layer's fixed pipeline (hle_gl.m, "Fixed function"): what OpenGL 1.x draws with when no program is current (UnMetal: client arrays, the matrix stack and
// glTexEnv, no shader at all). The arrays are read at NVIDIA's aliased attribute numbers: 0 the vertex, 3 the colour, 8 texture unit 0's coordinates.
// One texture unit; its environment follows the GL 1.5 table (section 3.8.13) by the texture's base format: has_c / has_a say whether it gives colour / alpha
#include <metal_stdlib>
using namespace metal;

struct ff_vu { float4x4 mvp, tm; };   // projection × modelview; texture unit 0's texture matrix
struct ff_fu {
    int tex, env, has_c, has_a;   // texturing on; GL_TEXTURE_ENV_MODE (0 MODULATE, 1 REPLACE, 2 DECAL, 3 BLEND, 4 ADD); what the base format gives
    int atest, afunc; float aref, pad;   // GL_ALPHA_TEST: glAlphaFunc's function (0 NEVER .. 7 ALWAYS, GL's order) and reference
    float4 envc;   // GL_TEXTURE_ENV_COLOR
};
// One argument buffer per stage at buffer 0, as the translated shaders have (gl_shaders.py): the uniforms at ID 0, the fragment's texture and sampler at 1 and 2
struct ff_va { constant ff_vu *u [[id(0)]]; };
struct ff_fa { constant ff_fu *u [[id(0)]]; texture2d<float> t [[id(1)]]; sampler s [[id(2)]]; };
struct ff_in { float4 pos [[attribute(0)]]; float4 col [[attribute(3)]]; float4 tc [[attribute(8)]]; };
struct ff_v2f { float4 pos [[position]]; float4 col; float4 tc; float psize [[point_size]]; };

vertex ff_v2f ff_vs(ff_in i [[stage_in]], constant ff_va &args [[buffer(0)]]) {
    constant ff_vu &u = *args.u;
    ff_v2f o;
    o.pos = u.mvp * i.pos;
    o.pos.z = (o.pos.z + o.pos.w) * 0.5;   // GL's clip z (−w..w) → Metal's (0..w)
    o.col = saturate(i.col);               // GL clamps the vertex colour
    o.tc = u.tm * i.tc;
    o.psize = 1;
    return o;
}

fragment float4 ff_fs(ff_v2f i [[stage_in]], constant ff_fa &args [[buffer(0)]]) {
    constant ff_fu &u = *args.u;
    float4 f = i.col, c = f;
    if (u.tex) {
        float4 x = args.t.sample(args.s, i.tc.xy / (i.tc.w != 0 ? i.tc.w : 1));
        float3 cc = u.has_c ? x.rgb : f.rgb; float a = f.a;
        switch (u.env) {
            case 0: cc = u.has_c ? f.rgb * x.rgb : f.rgb; a = u.has_a ? f.a * x.a : f.a; break;                        // MODULATE
            case 1: cc = u.has_c ? x.rgb : f.rgb; a = u.has_a ? x.a : f.a; break;                                       // REPLACE
            case 2: cc = u.has_c ? mix(f.rgb, x.rgb, u.has_a ? x.a : 1.0) : f.rgb; a = f.a; break;                      // DECAL
            case 3: cc = u.has_c ? mix(f.rgb, u.envc.rgb, x.rgb) : f.rgb; a = u.has_a ? f.a * x.a : f.a; break;         // BLEND
            case 4: cc = u.has_c ? f.rgb + x.rgb : f.rgb; a = u.has_a ? f.a * x.a : f.a; break;                         // ADD
        }
        c = saturate(float4(cc, a));
    }
    if (u.atest) {
        bool pass;
        switch (u.afunc) {
            case 0: pass = false; break; case 1: pass = c.a < u.aref; break; case 2: pass = c.a == u.aref; break; case 3: pass = c.a <= u.aref; break;
            case 4: pass = c.a > u.aref; break; case 5: pass = c.a != u.aref; break; case 6: pass = c.a >= u.aref; break; default: pass = true; break;
        }
        if (!pass) discard_fragment();
    }
    return c;
}
