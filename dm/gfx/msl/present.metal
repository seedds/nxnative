// The present: the frame drawn into the drawable, cropped (nvn_mtl.m present_pipeline)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct Crop { float4 rect; float4 edge; };
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }
// DM_PRESENT_AA=fxaa (nvn_mtl.m aa_encode): FXAA 3.11 (Timothy Lottes' quality path, preset 12 steps) over the presented crop at the game's own resolution, before the present scales it.
// Positions are in source texels; every fetch is clamped to the crop's texel centres, so nothing outside it is read. The target is drawn at its origin (viewport = crop size).
// a.rect: crop x, y, width, height in the source; a.opt.x: 1 for a float (linear) source, whose luma is taken from its square root (FXAA wants perceptual luma), a.opt.y: the subpixel amount
struct AA { float4 rect; float4 opt; };
static inline float aa_luma(float3 c, float lin) { float l = dot(c, float3(0.299, 0.587, 0.114)); return lin > 0 ? sqrt(max(l, 0.0)) : l; }
fragment float4 fxaa(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant AA& a [[buffer(0)]]) {
    float2 rcp = 1.0 / float2(t.get_width(), t.get_height()), lo = a.rect.xy + 0.5, hi = a.rect.xy + a.rect.zw - 0.5; float lin = a.opt.x;
#define AA_TEX(p) t.sample(s, clamp((p), lo, hi) * rcp)
#define AA_L(p) aa_luma(AA_TEX(p).rgb, lin)
    float2 posM = floor(in.p.xy) + 0.5 + a.rect.xy;
    float4 rgbyM = AA_TEX(posM);
    float lumaM = aa_luma(rgbyM.rgb, lin);
    float lumaS = AA_L(posM + float2(0, 1)), lumaE = AA_L(posM + float2(1, 0)), lumaN = AA_L(posM + float2(0, -1)), lumaW = AA_L(posM + float2(-1, 0));
    float rangeMax = max(max(max(lumaS, lumaM), lumaE), max(lumaN, lumaW)), rangeMin = min(min(min(lumaS, lumaM), lumaE), min(lumaN, lumaW));
    float range = rangeMax - rangeMin;
    if (range < max(0.0833, rangeMax * 0.166)) return rgbyM;   // edge threshold (min): flat areas, most of the picture, end here
    float lumaNW = AA_L(posM + float2(-1, -1)), lumaSE = AA_L(posM + float2(1, 1)), lumaNE = AA_L(posM + float2(1, -1)), lumaSW = AA_L(posM + float2(-1, 1));
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE, subpixRcpRange = 1.0 / range, subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE, lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE, edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert = abs(edgeVert3) + abs(edgeVert1) * 2.0 + abs(edgeVert2);
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + lumaNWSW + lumaNESE, lengthSign = 1.0;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    float subpixB = subpixA * (1.0 / 12.0) - lumaM, gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = posM, offNP = horzSpan ? float2(1, 0) : float2(0, 1);
    if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP, posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0, subpixE = subpixC * subpixC;
    float lumaEndN = AA_L(posN), lumaEndP = AA_L(posP);
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25, lumaMM = lumaM - lumaNN * 0.5, subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5; lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * 1.5;
    if (!doneP) posP += offNP * 1.5;
    const float steps[3] = {2.0, 4.0, 12.0};
    for (int i = 0; i < 3 && !(doneN && doneP); i++) {
        if (!doneN) lumaEndN = AA_L(posN) - lumaNN * 0.5;
        if (!doneP) lumaEndP = AA_L(posP) - lumaNN * 0.5;
        doneN = doneN || abs(lumaEndN) >= gradientScaled; doneP = doneP || abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * steps[i];
        if (!doneP) posP += offNP * steps[i];
    }
    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y, dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero, goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP), spanLengthRcp = 1.0 / (dstP + dstN);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixH = subpixF * subpixF * a.opt.y, pixelOffset = dst * -spanLengthRcp + 0.5;
    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign; else posM.y += pixelOffsetSubpix * lengthSign;
    return float4(AA_TEX(posM).rgb, rgbyM.a);
#undef AA_TEX
#undef AA_L
}
