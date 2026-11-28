"""Ordinary VS viewport-zero raster output keeps earlier-stage captures unculled.

NV_viewport_array2 applies only the last active VTG stage's mask, after feedback.
These synthetic sources use the indexed store emitted for issue #1131; no game
shader or data is needed. Build the actual finish() vertex-entry section.
"""
import ast
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from typing import Any

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain
import dmsh_entry
import dmsh_geometry
import dmsh_pull
import dmsh_reflect
import dmsh_rewrite


def source(mask=True):
    return '''#include <metal_stdlib>
using namespace metal;
struct main0_out
{
    float4 gl_Position [[position]];
    float gl_PointSize [[point_size]];
    float4 m_1 [[user(locn1)]];
};
struct main0_in
{
    float4 m_0 [[attribute(0)]];
};
vertex main0_out main0(main0_in in [[stage_in]], uint vid [[vertex_id]], uint gl_InstanceIndex [[instance_id]], constant uint& mask [[buffer(0)]], constant uint& points [[buffer(1)]])
{
    main0_out out = {};
    float2 p[] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    out.gl_Position = float4(points ? float2(0.0) : p[vid], 0.25, 1.0) + in.m_0;
    out.gl_PointSize = 16.0;
    out.m_1.x = as_type<float>(int(gl_InstanceIndex));
    out.m_1.yzw = float3(13.0, 17.0, 19.0);
''' + ('    gl_BuiltIn_5253[0] = as_type<int>(mask);\n' if mask else '') + '''    if (mask == 3u) { return out; }
    return out;
}
'''


def cbuf_source():
    text = source().replace('struct main0_out', 'struct Words { float4 _m0[4096]; };\nstruct Descriptors { constant Words* vp_c5 [[id(1)]]; };\nstruct main0_out', 1)
    text = text.replace('constant uint& mask [[buffer(0)]]', 'constant Descriptors& spvDescriptorSet0 [[buffer(0)]]')
    word = '(*spvDescriptorSet0.vp_c5)._m0[50].y'
    return text.replace('as_type<int>(mask)', f'as_type<int>({word})').replace('if (mask == 3u)', f'if (as_type<uint>({word}) == 3u)')


def entries(src, directory):
    """Execute production finish's entry construction, without a game build."""
    path = TOOLS / 'dmsh_build.py'
    tree = ast.parse(path.read_text())
    finish = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'finish')
    start = next(i for i, n in enumerate(finish.body) if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'pad' for t in n.targets))
    end = next(i for i, n in enumerate(finish.body) if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'fu' for t in n.targets))
    tf = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'vs_tf')
    scope: dict[str, Any] = dict(src=src, re=re, sys=sys, dmsh_entry=dmsh_entry, dmsh_geometry=dmsh_geometry, dmsh_reflect=dmsh_reflect,
                 key='fixture', PAIRS={}, msl=str(directory / 'built.metal'), res={'sbufs': [], 'textures': [], 'images': []},
                 TF_SLOT=8, tess=None, j={'stage': 'vs', 'tf': [{'stride': 16, 'outputs': [[1, 4]]}, [], [], []]})
    wrapper = ast.parse('def build(src):\n    pass\n').body[0]
    assert isinstance(wrapper, ast.FunctionDef)
    wrapper.body = [*finish.body[start:end], *ast.parse('return full, locals()').body]
    module = ast.fix_missing_locations(ast.Module(body=[tf, wrapper], type_ignores=[]))
    exec(compile(module, str(path), 'exec'), scope)
    return scope['build'](src)


PIXEL = '\nfragment float4 pixel() { return float4(1.0, 0.0, 0.0, 1.0); }\n'
RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>
int main(int argc,const char **argv) {@autoreleasepool {
 id<MTLDevice>d=MTLCreateSystemDefaultDevice();if(!d)return 77;NSError*e=nil;
 id<MTLLibrary>l=[d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];if(!l){fprintf(stderr,"%s\n",e.description.UTF8String);return 1;}
 int capture=atoi(argv[3]),has_mask=atoi(argv[4]),pulling=argc>5?atoi(argv[5]):0,layer=strstr(argv[2],"_l")!=NULL;
 MTLRenderPipelineDescriptor*p=[MTLRenderPipelineDescriptor new];p.vertexFunction=[l newFunctionWithName:@(argv[2])];
 p.vertexDescriptor=[MTLVertexDescriptor vertexDescriptor];p.vertexDescriptor.attributes[0].format=MTLVertexFormatFloat4;p.vertexDescriptor.attributes[0].bufferIndex=2;p.vertexDescriptor.layouts[2].stride=16;
 if(pulling)p.vertexDescriptor=nil;if(layer)p.inputPrimitiveTopology=MTLPrimitiveTopologyClassTriangle;
 p.rasterizationEnabled=!capture;if(!capture){p.fragmentFunction=[l newFunctionWithName:@"pixel"];p.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;}
 id<MTLRenderPipelineState>ps=[d newRenderPipelineStateWithDescriptor:p error:&e];if(!ps){fprintf(stderr,"%s\n",e.description.UTF8String);return 1;}
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;
 id<MTLTexture>texture=[d newTextureWithDescriptor:td];id<MTLCommandQueue>q=[d newCommandQueue];
 id<MTLBuffer>zero=[d newBufferWithLength:48 options:MTLResourceStorageModeShared],out=[d newBufferWithLength:3*144*4 options:MTLResourceStorageModeShared];
 uint32_t masks[]={0,1,2,3,63,0x80000000u,0xffffffffu};int checks=0;
 for(unsigned clamp=0;clamp<2;clamp++)for(uint32_t points=0;points<2;points++)for(unsigned k=0;k<7;k++){
  if(layer&&points)continue;
  memset(out.contents,0xcc,out.length);MTLRenderPassDescriptor*r=[MTLRenderPassDescriptor new];r.renderTargetWidth=16;r.renderTargetHeight=16;r.defaultRasterSampleCount=1;if(layer)r.renderTargetArrayLength=1;
  if(!capture){r.colorAttachments[0].texture=texture;r.colorAttachments[0].loadAction=MTLLoadActionClear;r.colorAttachments[0].storeAction=MTLStoreActionStore;r.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);}
  id<MTLCommandBuffer>c=[q commandBuffer];id<MTLRenderCommandEncoder>en=[c renderCommandEncoderWithDescriptor:r];[en setRenderPipelineState:ps];[en setDepthClipMode:clamp?MTLDepthClipModeClamp:MTLDepthClipModeClip];
  [en setVertexBytes:&masks[k] length:4 atIndex:0];[en setVertexBytes:&points length:4 atIndex:1];[en setVertexBuffer:zero offset:0 atIndex:2];
  struct {struct {uint64_t base;uint32_t stride,divisor;} streams[16];uint32_t attr[32];} layout={0};layout.streams[0].base=zero.gpuAddress;layout.streams[0].stride=16;layout.attr[0]=0x2e;
  if(pulling){[en setVertexBytes:&layout length:sizeof layout atIndex:16];[en useResource:zero usage:MTLResourceUsageRead stages:MTLRenderStageVertex];}
  struct {uint64_t dst,pad;simd_uint4 info;uint32_t stride;int16_t slot[144];} gc={out.gpuAddress,0,{0,3,0,1},144,{0}};
  for(int i=0;i<144;i++)gc.slot[i]=i;
  struct {simd_uint4 offset,args;uint64_t order;uint32_t first,count,base,pad;uint64_t reserved;} tf={{0,0,0,0},{3,0,0,0},0,0,3,0,0,0};
  if(capture==1){[en setVertexBytes:&gc length:sizeof gc atIndex:13];[en useResource:out usage:MTLResourceUsageWrite stages:MTLRenderStageVertex];}
  if(capture==2){[en setVertexBuffer:out offset:0 atIndex:8];[en setVertexBytes:&tf length:sizeof tf atIndex:14];}
  [en drawPrimitives:points?MTLPrimitiveTypePoint:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:points?1:3];[en endEncoding];[c commit];[c waitUntilCompleted];if(c.error){fprintf(stderr,"%s\n",c.error.description.UTF8String);return 1;}
  if(!capture){uint8_t pixel[4];[texture getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(8,8,1,1) mipmapLevel:0];if((pixel[0]!=0)!=(has_mask?((masks[k]&1)!=0):1)){fprintf(stderr,"raster mask=%x entry=%s point=%u clamp=%u red=%u\n",masks[k],argv[2],points,clamp,pixel[0]);return 1;}checks++;}
  else {float*got=out.contents;for(unsigned v=0;v<(points?1:3);v++){
   float*at=got+v*(capture==1?144:4);float want[]={points?0:(v==1?3:-1),points?0:(v==2?3:-1),.25,1};
   if(capture==1){for(int j=0;j<4;j++){if(at[j]!=want[j]){fprintf(stderr,"capture mask=%x v=%u c=%d got=%g want=%g\n",masks[k],v,j,at[j],want[j]);return 1;}checks++;}at+=20;}
   float attrs[]={0,13,17,19};for(int j=0;j<4;j++){if(at[j]!=attrs[j]){fprintf(stderr,"feedback mask=%x v=%u c=%d got=%g\n",masks[k],v,j,at[j]);return 1;}checks++;}
  }}
 }
 printf("%s: %d checks\n",argv[2],checks);return 0;
}}
'''


class VertexViewportMask(unittest.TestCase):
    def test_absent_mask_is_unchanged(self):
        original = source(False)
        self.assertEqual(dmsh_entry.vs_viewport_mask(original), (original, original))

    def test_literals_and_declared_fixed_constant_buffer_are_uniform(self):
        for value in ('0', '1u', '63', '0x3fU', '-1', 'int(1)', 'uint(63)'):
            with self.subTest(value=value):
                raster, capture = dmsh_entry.vs_viewport_mask(source().replace('as_type<int>(mask)', value))
                self.assertNotEqual(raster, capture)
        raster, capture = dmsh_entry.vs_viewport_mask(cbuf_source())
        self.assertNotEqual(raster, capture)

    def test_uniform_store_after_bit0_rewrite(self):
        store = '    gl_BuiltIn_5253[0] = '
        preceding = '    int _10 = int(vid) & 1;\n    bool _11 = _10 != 0;\n    int _12 = _11 ? (-1) : 0;\n'
        src = cbuf_source().replace(store, preceding + store)
        src = src.replace('    return out;\n}', '    out.gl_Position.x += float(_12);\n    return out;\n}')
        rewritten = dmsh_rewrite.bit0_mask(src)
        self.assertIn('int _12 = -_10;   // bit 0 as a mask', rewritten)
        with tempfile.TemporaryDirectory(prefix='vs_mask_bit0_') as tmp:
            result = entries(rewritten, Path(tmp))
        self.assertEqual(len(result), 2, result)
        full, scope = result
        self.assertTrue(scope['viewport_mask'])
        self.assertIn('int _12 = -_10;   // bit 0 as a mask', full)

    def test_unproven_and_per_vertex_values_are_reported_unsupported(self):
        base = source()
        store = 'gl_BuiltIn_5253[0] = as_type<int>(mask);'
        cases = [base.replace('as_type<int>(mask)', rhs) for rhs in
                 ('int(vid)', 'as_type<int>(in.m_0.x)', 'int(mask + 1u)', 'choose(mask)')]
        cases += [base.replace('constant uint& mask', space + ' uint& mask') for space in ('device', 'thread')]
        cases += [base.replace(store, text) for text in
                  ('if (vid == 0u) { ' + store + ' }', 'if (vid == 0u)\n    ' + store,
                   'if (vid == 0u) /* ; */\n    ' + store,
                   store + '\n    ' + store, 'if (vid == 0u) { return out; }\n    ' + store)]
        fixed = cbuf_source()
        cases += [fixed.replace('constant Words* vp_c5', 'device Words* vp_c5'),
                  fixed.replace('constant Descriptors&', 'device Descriptors&'),
                  fixed.replace('_m0[50].y', '_m0[vid].y'),
                  fixed.replace('_m0[50].y', '_m0[50][vid]')]
        with tempfile.TemporaryDirectory(prefix='vs_mask_uniform_') as tmp:
            for i, text in enumerate(cases):
                with self.subTest(case=i):
                    result = entries(text, Path(tmp))
                    self.assertEqual(result[0], 'vertex viewport mask failed')
                    self.assertIn('draw-uniform', result[1])

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_reported_constant_buffer_store_compiles(self):
        src = cbuf_source()
        with_write = src.replace('[[buffer(1)]])', '[[buffer(1)]], texture2d<uint, access::write> image [[texture(0)]])')
        with_write = with_write.replace('    gl_BuiltIn_5253', '    image.write(uint4(vid), uint2(0));\n    gl_BuiltIn_5253')
        with tempfile.TemporaryDirectory(prefix='vs_mask_cbuf_') as tmp:
            root = Path(tmp)
            path = root / 'v.metal'
            for discard, variant in ((False, src), (True, with_write)):
                full, scope = entries(variant, root)
                self.assertEqual(scope['discard'], discard)
                if discard:
                    discard_body = full.split('vertex void main0_discard', 1)[1].split('vertex void main0_tf', 1)[0]
                    self.assertIn('image.write(uint4(vid), uint2(0));', discard_body)
                    self.assertNotIn('dm_vertex_viewport_mask & 1u', discard_body)
                path.write_text(full)
                for sdk in ('macosx', 'iphoneos'):
                    with self.subTest(sdk=sdk, discard=discard):
                        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-fsyntax-only', str(path)], capture_output=True, text=True)
                        self.assertEqual(r.returncode, 0, r.stderr)

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_all_entry_variants_compile(self):
        with tempfile.TemporaryDirectory(prefix='vs_mask_compile_') as tmp:
            root=Path(tmp)
            for mask in (False, True):
                full, scope = entries(source(mask), root)
                self.assertEqual(scope["viewport_mask"], mask)
                pull, why = dmsh_pull.variant(full)
                self.assertIsNotNone(pull, why)
                assert pull is not None
                for variant, text in (('native',full),('pull',pull)):
                    path=root/'v.metal';path.write_text(text)
                    for sdk in ('macosx','iphoneos'):
                        with self.subTest(mask=mask,variant=variant,sdk=sdk):
                            r=subprocess.run(['xcrun','-sdk',sdk,'metal','-fsyntax-only',str(path)],capture_output=True,text=True)
                            self.assertEqual(r.returncode,0,r.stderr)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and sys.platform=='darwin', 'set DMSH_TEST_GPU=1 on Mac')
    def test_raster_and_unculled_captures(self):
        with tempfile.TemporaryDirectory(prefix='vs_mask_gpu_') as tmp:
            root=Path(tmp); runner=root/'run.m';runner.write_text(RUNNER)
            subprocess.run(['clang','-fobjc-arc',str(runner),'-framework','Foundation','-framework','Metal','-o',str(root/'run')],check=True,capture_output=True)
            for mask in (False,True):
                full, scope=entries(source(mask),root)
                pull, why=dmsh_pull.variant(full);self.assertIsNotNone(pull,why)
                assert pull is not None
                for pulling,text in ((False,full),(True,pull)):
                    path=root/'v.metal';path.write_text(text+PIXEL)
                    subprocess.run(['xcrun','-sdk','macosx','metal','-c',str(path),'-o',str(root/'v.air')],check=True,capture_output=True)
                    subprocess.run(['xcrun','-sdk','macosx','metallib',str(root/'v.air'),'-o',str(root/'v.metallib')],check=True,capture_output=True)
                    cases=[('main0',0),('main0_z',0)]
                    if not pulling:cases += [('main0_gs',1),('main0_tf',2),('main0_l',0),('main0_lz',0)]
                    for entry,capture in cases:
                        with self.subTest(mask=mask,entry=entry,pulling=pulling):
                            applied=mask and not entry.startswith('main0_l')
                            r=subprocess.run([str(root/'run'),str(root/'v.metallib'),entry,str(capture),str(int(applied)),str(int(pulling))],capture_output=True,text=True)
                            if r.returncode==77:self.skipTest('no Metal device')
                            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
                            print(('pull ' if pulling else '')+r.stdout.strip())
                    if not pulling and os.environ.get('DMSH_TEST_VIEWPORT_HOST'):
                        metadata={'route':'B','chain':dmsh_chain.version(),'stage':'vs','entry':'main0','entry_z':'main0_z',
                                  'entry_layer':'main0_l','entry_layer_z':'main0_lz','viewport_mask':scope['viewport_mask'],
                                  'bindings':[],'vertex_inputs':[],'c1':'','raster':{}}
                        (root/'v.json').write_text(json.dumps(metadata))
                        r=subprocess.run([os.environ['DMSH_TEST_VIEWPORT_HOST'],str(root),str(int(mask))],capture_output=True,text=True)
                        self.assertEqual(r.returncode,0,r.stdout+r.stderr);print(r.stdout.strip())


if __name__=='__main__':unittest.main()
