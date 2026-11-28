"""gl_shaders.py: what NVIDIA's GLSL compiler takes and glslang does not (DOOM 3's GLSL 1.50: built-ins redeclared as plain varyings, the compatibility colour pair, `sampler` as a
name; Mega Man Legacy Collection's `};` after a function), a named uniform block's table row, only the samplers the MSL reads, .glsl files, and main's '#version' strings that are only a prefix. The shaders here are written for the test, in the form DOOM 3's renderer makes them."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/gfx/tools'), str(REPO / 'dm/games')]
import gl_shaders   # noqa: E402
import profile_guess as pg   # noqa: E402
import romfs_image   # noqa: E402

PREFIX = ('#version 150\n#define PC\n'
          'vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture( sampler, texcoord.xy ); }\n'
          'float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n')
VERT = PREFIX + ('uniform vec4 _va_[4];\nuniform matrices_ubo {vec4 matrices [ 408 ] ; } ;\n'
                 'in vec4 in_Position;\nin vec4 in_Color;\n'
                 'out vec4 gl_Position;\nout vec4 vofi_TexCoord0;\nout vec4 gl_FrontColor;\n'
                 'void main() {\n\tint j = int ( in_Color . x * 255.1 * 3 ) ;\n'
                 '\tgl_Position = vec4 ( dot ( in_Position , matrices [ j ] ) , dot ( in_Position , _va_[1] ) , dot ( in_Position , _va_[2] ) , dot ( in_Position , _va_[3] ) ) ;\n'
                 '\tvofi_TexCoord0 = in_Position ;\n\tgl_FrontColor = saturate ( _va_[0] . x ) * in_Color ;\n}\n')
FRAG = PREFIX + ('uniform vec4 _fa_[1];\nuniform sampler2D samp0;\n'
                 'in vec4 gl_FragCoord;\nin vec4 vofi_TexCoord0;\nin vec4 gl_Color;\n'
                 'out vec4 gl_FragColor;\nout float gl_FragDepth;\n'
                 'void main() {\n\tgl_FragColor = tex2D ( samp0 , vofi_TexCoord0 . xy + gl_FragCoord . xy ) * gl_Color * _fa_[0] ;\n\tgl_FragDepth = 0.5 ;\n}\n')


class Fixups(unittest.TestCase):
    def test_builtins_declared_as_varyings(self):
        v = gl_shaders.fixups(VERT, 'vert')
        self.assertNotIn('out vec4 gl_Position;', v)
        self.assertIn('gl_Position = vec4', v)   # the built-in itself stays
        self.assertIn('out vec4 dm_Color;', v); self.assertNotIn('gl_FrontColor', v)
        f = gl_shaders.fixups(FRAG, 'frag')
        for decl in ('in vec4 gl_FragCoord;', 'out float gl_FragDepth;', 'in vec4 gl_Color;', 'out vec4 gl_FragColor;'): self.assertNotIn(decl, f)
        self.assertIn('in vec4 dm_Color;', f); self.assertIn('out vec4 dm_FragColor;', f)
        self.assertIn('dm_FragColor = tex2D', f); self.assertIn('gl_FragCoord . xy', f); self.assertIn('gl_FragDepth = 0.5', f)

    def test_sampler_as_a_name(self):
        f = gl_shaders.fixups(FRAG, 'frag')
        self.assertIn('sampler2D dm_sampler', f); self.assertIn('texture( dm_sampler', f)
        self.assertIn('uniform sampler2D samp0;', f)   # the sampler types are other words

    def test_undeclared_compatibility_names_stay(self):   # only a declaration as a plain varying is NVIDIA's own reading; a compatibility shader's built-in is left to glslang
        t = '#version 150\nvoid main() { gl_FrontColor = vec4(1.0); }\n'
        self.assertEqual(gl_shaders.fixups(t, 'vert'), t)


    def test_semicolon_after_a_function_body(self):   # Mega Man Legacy Collection's .glsl files end main with `};`; a struct's and a block's `};` stay
        t = ('struct L { vec4 c; };\nuniform B { vec4 v; } b;\nlayout(std140) uniform C { vec4 w; };\n'
             'float f(float x)\n#line 10\n{ if (x > 0.0) { return x; } return 0.0; } ;;\nvoid main()\n{\n\tgl_Position = vec4(f(1.0));\n};\n')
        g = gl_shaders.empty_declarations(t)
        self.assertIn('struct L { vec4 c; };', g); self.assertIn('} b;', g); self.assertIn('uniform C { vec4 w; };', g)
        self.assertIn('return 0.0; } \nvoid main', g)
        self.assertNotIn('} ;', g); self.assertTrue(g.rstrip().endswith('}'))
        self.assertEqual(gl_shaders.empty_declarations(g), g)

    def test_empty_declarations_at_file_scope(self):   # Mark of the Ninja: Remastered's DECL_*_REGISTER macros end with `;` and are called with one; in-body `;;` stays
        t = ('#version 450\n;\n'
             'layout(binding = 6) uniform sampler2D FOWTexture;;\n'
             'float FOWSampler; ;;\n'
             'struct S { vec4 v; };;\n'
             'const float k[2] = { 1.0, 2.0 };;\n'
             'uniform float kept;\n;#line 42\n'
             'void main()\n{\n\tfor (;;) break;\n\tif (gl_FragCoord.x < 0.0) discard;;\n};;\n')
        g = gl_shaders.empty_declarations(t)
        self.assertEqual(g, ('#version 450\n\n'
                             'layout(binding = 6) uniform sampler2D FOWTexture;\n'
                             'float FOWSampler; \n'
                             'struct S { vec4 v; };\n'
                             'const float k[2] = { 1.0, 2.0 };\n'
                             'uniform float kept;\n#line 42\n'
                             'void main()\n{\n\tfor (;;) break;\n\tif (gl_FragCoord.x < 0.0) discard;;\n}\n'))
        self.assertEqual(gl_shaders.empty_declarations(g), g)

    def test_named_block_members(self):   # Mark of the Ninja: Remastered finds cbuffer_wvp by its member mtxPVW (glGetActiveUniform, glGetActiveUniformsiv)
        refl = {'types': {'_S': {'name': 'S', 'members': [{'name': 'a', 'type': 'float', 'offset': 0}, {'name': 'm', 'type': 'mat2', 'offset': 16, 'matrix_stride': 8, 'row_major': True}]}}}
        ms = [{'name': 'mtxPVW', 'type': 'mat4', 'offset': 0, 'matrix_stride': 16},
              {'name': 'arr', 'type': 'vec4', 'offset': 64, 'array': [3], 'array_stride': 16},
              {'name': 's', 'type': '_S', 'offset': 112},
              {'name': 'flag', 'type': 'uint', 'offset': 160},
              {'name': 'aa', 'type': 'vec4', 'offset': 176, 'array': [2, 2]}]
        f = []
        gl_shaders.block_members(refl, ms, '', 0, 2, f, {'flag'})
        self.assertEqual(f, ['m:mtxPVW=2:0:mat4:0:0:16:0', 'm:arr=2:64:vec4:3:16:0:0',
                             'm:s.a=2:112:float:0:0:0:0', 'm:s.m=2:128:mat2:0:0:8:1', 'm:flag=2:160:bool:0:0:0:0'])


# GLSL ES 1.00 as Fate/hollow ataraxia REMASTERED's built shaders write it: a sampler named `texture` (a built-in function from ES 3.00 on) and, as GameMaker's, a variable `sample`
ES100_FRAG = (b'precision mediump float;\nvarying vec2 v_uv;\nuniform sampler2D texture;\nuniform float sample;\n'
              b'void main() { gl_FragColor = texture2D(texture, v_uv) * sample; }\n')


class GameNames(unittest.TestCase):
    def test_es100_renames_texture_names_not_calls(self):
        t = gl_shaders.es100(ES100_FRAG.decode(), 'frag')
        self.assertIn('uniform sampler2D dm_texture;', t); self.assertIn('texture2D(dm_texture, v_uv)', t)
        self.assertIn('vec4 c = texture (s, v);', gl_shaders.es100('vec4 c = texture (s, v);', 'frag'))   # a call (NVIDIA takes one in ES 1.00 too) stays

    def test_table_rows_give_the_names_back(self):
        rows = ['t:dm_texture=0:sampler2D', 'u:L.gm_sample=16:float:0:0', 'a:dm_sampler=1:vec4', 'b:0:32', 'o:gm_FragColor=0', 'u:dm_texture_scale=0:float:0:0']
        self.assertEqual(gl_shaders.game_names(rows, {'dm_texture': 'texture', 'gm_sample': 'sample', 'dm_sampler': 'sampler'}),
                         ['t:texture=0:sampler2D', 'u:L.sample=16:float:0:0', 'a:sampler=1:vec4', 'b:0:32', 'o:gm_FragColor=0',
                                                       'u:dm_texture_scale=0:float:0:0'])


class Sources(unittest.TestCase):
    def test_packer_and_translator_use_the_same_standalone_pairs(self):
        vertex = b'attribute vec4 p;\nvoid main() { gl_Position = p; }\n'
        fragment = b'precision mediump float;\nvoid /* entry */ main (void) { gl_FragColor = vec4(1.0); }\n'
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'ExeFS').mkdir()
            image = root / 'RomFS.img'
            # These are distinct RomFS directories even on a case-insensitive Mac volume.
            entries = [[b'', None, True, 1, 5],
                       [b'Effect', None, True, 6, 1], [b'effect', None, True, 7, 1],
                       [b'love', None, True, 8, 1], [b'snippets', None, True, 9, 2], [b'standalone', None, True, 11, 0],
                       [b'vertex.vs', vertex, False, 0, 0], [b'frag.fs', fragment, False, 0, 0], [b'CRT.fs', fragment, False, 0, 0],
                       [b'frag.fs', b'// void main() {}\nvoid main();\n', False, 0, 0], [b'vertex.vs', vertex, False, 0, 0]]
            managed = 'glShaderSource'.encode('utf-16-le')
            imports = [('eglCreateWindowSurface', False, 1)]
            with mock.patch.object(pg, 'nso', return_value=(managed, '', [0, 0, 0], 0)), mock.patch.object(pg.nso_syms, 'dynsyms', return_value=imports):
                Path(romfs_image.write(entries, str(image), lambda data, out: out.write(data))).replace(image)
                romfs = pg.source(root)
                capture = pg.build_sources(root / 'ExeFS', romfs, ['main'])
                self.assertEqual(capture['graphics'][0], 'gl')
                self.assertTrue(capture['gl_capture'][0])
                self.assertEqual(list(gl_shaders.sources([romfs])), [])

                entries[5][4] = 2
                entries.extend([[b'fragShader.FS', fragment + b'\0', False, 0, 0], [b'vertexShader.vs', vertex, False, 0, 0]])
                Path(romfs_image.write(entries, str(image), lambda data, out: out.write(data))).replace(image)
                offline = pg.build_sources(root / 'ExeFS', romfs, ['main'])
                self.assertFalse(offline['gl_capture'][0])
                self.assertEqual(list(gl_shaders.sources([romfs])), [(fragment, 'frag', '/standalone/fragShader.FS'), (vertex, 'vert', '/standalone/vertexShader.vs')])

    def test_glsl_files_are_read_with_no_stage(self):   # Mega Man Legacy Collection: shaders/nx/*.glsl, whose names say nothing of the stage
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'shaders').mkdir()
            (Path(d) / 'shaders/default_ps.glsl').write_bytes(b'#version 320 es\nvoid main() {}\0junk')
            (Path(d) / 'shaders/helper.glsl').write_bytes(b'vec4 helper(vec4 c) { return c; }\n')   # snippet without main() is skipped
            (Path(d) / 'shaders/a.vert').write_bytes(b'#version 320 es\nvoid main() {}\n')
            got = sorted((w, st, s) for s, st, w in gl_shaders.sources([d]))
            self.assertEqual(got, [('/shaders/a.vert', 'vert', b'#version 320 es\nvoid main() {}\n'), ('/shaders/default_ps.glsl', None, b'#version 320 es\nvoid main() {}')])

    def test_glsl_snippets_without_main_do_not_suppress_capture(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'ExeFS').mkdir(); (root / 'RomFS/shaders').mkdir(parents=True)
            (root / 'RomFS/shaders/common.glsl').write_bytes(b'vec4 shade(vec4 c) { return c; }\n')
            embedded = b'\0#version 320 es\nvoid main() {}\0glShaderSource\0'
            imports = [('eglCreateWindowSurface', False, 1)]
            with mock.patch.object(pg, 'nso', return_value=(embedded, '', [0, 0, 0], 0)), mock.patch.object(pg.nso_syms, 'dynsyms', return_value=imports):
                got = pg.build_sources(root / 'ExeFS', str(root / 'RomFS'), ['main'])
                self.assertEqual(got['graphics'][0], 'gl')
                self.assertTrue(got['gl_capture'][0])
                (root / 'RomFS/shaders/sprite.glsl').write_bytes(b'#version 120\nvoid main() { gl_FragColor = vec4(1, 0, 0, 1); }\n')
                got2 = pg.build_sources(root / 'ExeFS', str(root / 'RomFS'), ['main'])
                self.assertFalse(got2['gl_capture'][0])


@unittest.skipUnless(shutil.which('glslangValidator'), 'glslangValidator is not installed')
class Translate(unittest.TestCase):
    def test_es300_preprocesses_before_promoting_for_spirv(self):
        src = (b'#version 300 es\n#if __VERSION__ != 300\n#error expected ES 3.00\n#endif\n'
               b'precision mediump float;\nout vec4 color;\nvoid main() { color = vec4(1.0); }\n')
        with tempfile.TemporaryDirectory() as tmp:
            result = gl_shaders.translate(src, 'frag', '3' * 16, tmp)
            self.assertIsInstance(result, tuple, result)

    def test_sampler_buffer_uses_metal_texture_buffer(self):
        src = (b'#version 330\nuniform samplerBuffer lookup;\n'
               b'void main() { gl_Position = texelFetch(lookup, gl_VertexID); }\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'vert', 'b' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertIn('t:lookup=0:-1:samplerBuffer', r[1])
            msl = Path(r[0]).read_text()
            self.assertIn('texture_buffer<float>', msl)
            self.assertNotIn('spvTexelBufferCoord', msl)

    def test_many_samplers_and_uniforms_have_independent_argument_ids(self):
        for count in (16, 17, 32):
            src = ('#version 320 es\nprecision highp float;\n'
                   + ''.join(f'uniform sampler2D tex{i};\n' for i in range(count))
                   + 'uniform vec4 weight;\nout vec4 color;\nvoid main() { color = weight * ('
                   + ' + '.join(f'texture(tex{i}, vec2(0.5))' for i in range(count)) + '); }\n').encode()
            with self.subTest(count=count), tempfile.TemporaryDirectory() as tmp:
                r = gl_shaders.translate(src, 'frag', '9' * 16, tmp)
                self.assertIsInstance(r, tuple, r)
                args = [int(x) for f in r[1] if f.startswith('t:') for x in f.split('=')[1].split(':')[:2]]
                args += [int(f.split(':')[1]) for f in r[1] if f.startswith('b:')]
                self.assertEqual(len(args), 2 * count + 1)
                self.assertEqual(len(set(args)), len(args))
                self.assertIn(f'r:0:{len(args)}', r[1])
                self.assertNotIn('[[sampler(', Path(r[0]).read_text())

    def test_original_names_that_resemble_rewrite_names(self):
        src = (b'#version 320 es\nprecision highp float;\nuniform sampler2D dm_texture;\nuniform float gm_sample;\n'
               b'out vec4 color;\nvoid main() { color = texture(dm_texture, vec2(0.5)) * gm_sample; }\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', '0' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertIn('t:dm_texture=0:1:sampler2D', r[1])
            self.assertTrue(any(x.startswith('u:gm_sample=') for x in r[1]), r[1])

    def test_rewritten_names_do_not_collide_with_original_names(self):
        src = (b'precision highp float;\nuniform sampler2D texture;\nuniform sampler2D dm_texture;\n'
               b'void main() { gl_FragColor = texture2D(texture, vec2(0.5)) + texture2D(dm_texture, vec2(0.5)); }\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', '1' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertEqual({x.split('=')[0] for x in r[1] if x.startswith('t:')}, {'t:texture', 't:dm_texture'})

    def test_varyings_keep_the_same_name_across_stages_with_different_collisions(self):
        vert = b'attribute vec4 p; uniform float dm_texture; varying float texture; void main() { gl_Position = p; texture = dm_texture; }\n'
        frag = b'precision highp float;\nvarying float texture; void main() { gl_FragColor = vec4(texture); }\n'
        with tempfile.TemporaryDirectory() as tmp:
            for src, stage, key in [(vert, 'vert', '2'), (frag, 'frag', '3')]:
                r = gl_shaders.translate(src, stage, key * 16, tmp)
                self.assertIsInstance(r, tuple, r)
                self.assertIn('[[user(texture)]]', Path(r[0]).read_text())

    def test_doom3_style_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            v = gl_shaders.translate(VERT.encode(), 'vert', 'v' * 16, tmp)
            f = gl_shaders.translate(FRAG.encode(), 'frag', 'f' * 16, tmp)
            self.assertIsInstance(v, tuple, v); self.assertIsInstance(f, tuple, f)
            rows = v[1]
            self.assertIn('n:matrices_ubo=0:6528', rows)   # 408 vec4s; IDs come from the Metal argument layout, independent of the GLSL bindings
            self.assertTrue(any(r.startswith('b:1:') for r in rows))
            self.assertIn('o:dm_FragColor=0', f[1])
            # the colour pair is one varying, matched by name between the stages
            self.assertIn('[[user(dm_Color)]]', Path(v[0]).read_text()); self.assertIn('[[user(dm_Color)]]', Path(f[0]).read_text())

    def test_a_sampler_never_read_is_not_listed(self):   # Mega Man Legacy Collection's default_ps.glsl: T1 only under #if 0, given T0's binding by glslang
        src = (b'#version 320 es\nprecision highp float;\nin vec2 Texcoord;\nout vec4 outColor;\nuniform sampler2D T0;\nuniform sampler2D T1;\n'
               b'void main()\n{\n\toutColor = texture(T0, Texcoord);\n};\n#if 0\nvoid xmain() { outColor = texture(T1, Texcoord); }\n#endif\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', 'a' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertEqual([x for x in r[1] if x.startswith('t:')], ['t:T0=0:1:sampler2D'])

    def test_default_uniforms_expose_only_active_members_at_their_original_offsets(self):
        src = b'''#version 320 es
precision highp float;
struct Light { vec4 unused; vec4 color[4]; };
in vec4 position;
uniform vec4 inactive;
uniform vec4 uncalled;
uniform vec4 palette[4];
uniform Light light;
uniform int index;
uniform Materials { vec4 value; };
vec4 unusedHelper() { return uncalled; }
vec4 lighting() { return light.color[2]; }
void main() { gl_Position = position + value + palette[index] + lighting(); }
'''
        with tempfile.TemporaryDirectory() as tmp:
            result = gl_shaders.translate(src, 'vert', 'a' * 16, tmp)
            self.assertIsInstance(result, tuple, result)
            rows = result[1]
            self.assertEqual({row for row in rows if row.startswith('u:')}, {
                'u:palette=32:vec4:4:16', 'u:light.color=112:vec4:4:16', 'u:index=176:int:0:0',
            })
            self.assertTrue(any(row.startswith('b:') and row.endswith(':180') for row in rows), rows)
            self.assertTrue(any(row.startswith('n:Materials=') and row.endswith(':16') for row in rows), rows)

    def test_named_block_member_reflection_layout_and_instanced_names(self):
        src = b'''#version 450 core
#extension GL_EXT_scalar_block_layout : enable
layout(std140) uniform Uninst {
    mat4 mtxPVW;
    layout(row_major) mat3 rm3;
    vec4 arr[3];
    bool flag;
};
layout(std140, row_major) uniform BlockA {
    mat4 shared_m;
    mat3x4 m3x4;
} instA;
layout(std140) uniform BlockB {
    mat4 shared_m;
} instB;
layout(scalar) uniform Tight {
    mat2 m2;
    layout(row_major) mat2x3 m2x3r;
};
out vec4 color;
void main() {
    color = mtxPVW[0] + vec4(rm3[0], 0.0) + arr[0] + float(flag)
          + instA.shared_m[0] + instA.m3x4[0] + instB.shared_m[0]
          + vec4(m2[0], m2x3r[0].xy);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', '7' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            rows = r[1]
            blocks = {row.split('=')[0][2:]: row.split('=')[1].split(':')[0] for row in rows if row.startswith('n:')}
            members = {row.split('=')[0][2:]: row.split('=')[1] for row in rows if row.startswith('m:')}
            self.assertEqual(members['mtxPVW'], f'{blocks["Uninst"]}:0:mat4:0:0:16:0')
            self.assertEqual(members['rm3'], f'{blocks["Uninst"]}:64:mat3:0:0:16:1')
            self.assertEqual(members['arr'], f'{blocks["Uninst"]}:112:vec4:3:16:0:0')
            self.assertEqual(members['flag'], f'{blocks["Uninst"]}:160:bool:0:0:0:0')
            self.assertEqual(members['BlockA.shared_m'], f'{blocks["BlockA"]}:0:mat4:0:0:16:1')
            self.assertEqual(members['BlockA.m3x4'], f'{blocks["BlockA"]}:64:mat3x4:0:0:16:1')
            self.assertEqual(members['BlockB.shared_m'], f'{blocks["BlockB"]}:0:mat4:0:0:16:0')
            self.assertEqual(members['m2'], f'{blocks["Tight"]}:0:mat2:0:0:8:0')
            self.assertEqual(members['m2x3r'], f'{blocks["Tight"]}:16:mat2x3:0:0:8:1')

    def test_texture_query_keeps_its_resource_arguments(self):
        src = b'#version 320 es\nprecision highp float;\nuniform sampler2D tex;\nout vec4 c;\nvoid main() { c = vec4(textureSize(tex, 0), 0, 1); }\n'
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', 'b' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertIn('t:tex=0:1:sampler2D', r[1])

    def test_resource_names_do_not_collide_with_generated_names(self):
        src = (b'#version 320 es\nprecision highp float;\nuniform sampler2D dm_gl_resource_21;\nuniform sampler2D dm_gl_resource_21Smplr;\n'
               b'out vec4 c;\nvoid main() { c = texture(dm_gl_resource_21, vec2(0.5)) + texture(dm_gl_resource_21Smplr, vec2(0.5)); }\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', 'c' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertEqual({x.split('=')[0] for x in r[1] if x.startswith('t:')}, {'t:dm_gl_resource_21', 't:dm_gl_resource_21Smplr'})


    def test_es100_sampler_named_texture(self):   # Fate/hollow ataraxia REMASTERED: 16 of its 78 fragment shaders failed with `'texture' : redefinition`
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(ES100_FRAG, 'frag', 'e' * 16, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertIn('t:texture=0:1:sampler2D', r[1])
            self.assertTrue(any(x.startswith('u:sample=') for x in r[1]), r[1])

    def test_precision_statements_with_comments_macros_or_inline_code(self):
        body = b'varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor = texture2D(t, v); }\n'
        cases = (
            b'precision mediump float; // default\n' + body,
            b'precision mediump float; ' + body,
            b'precision\tlowp int; precision highp float;\n' + body,
            b'#define LOWPREC lowp\nprecision LOWPREC float; // default\nLOWPREC vec4 tint(LOWPREC vec4 c){ return c; }\n'
            b'varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor = tint(texture2D(t, v)); }\n',
            b'precision /* c */ mediump /* d */ float;\n' + body,
            b'#version 120\nprecision mediump float; // default\n' + body,
        )
        for src in cases:
            with self.subTest(src=src[:44]), tempfile.TemporaryDirectory() as d:
                self.assertIsInstance(gl_shaders.translate(src, 'frag', 'k', d), tuple)

    def test_precision_words_in_identifiers_stay(self):
        src = '#version 310 es\nfloat precision_scale = 1.0;\nfloat highp_bias = 0.5;\n'
        out = gl_shaders.strip_legacy_precision(src)
        self.assertIn('float precision_scale = 1.0;', out)
        self.assertIn('float highp_bias = 0.5;', out)

    def test_legacy_desktop_varyings_link_after_macro_expansion(self):
        vert = b'#version 120\nattribute vec4 p;\n#define COLOR gl_FrontColor\n#define TEX(i) gl_TexCoord[i]\nvoid main() { gl_Position = p; COLOR = p; TEX(0) = p; }\n'
        frag = b'#version 110\n#define COLOR gl_Color\n#define TEX(i) gl_TexCoord[i]\nvoid main() { gl_FragData[0] = COLOR * TEX(0); }\n'
        with tempfile.TemporaryDirectory() as tmp:
            for source, stage in ((vert, 'vert'), (frag, 'frag')):
                r = gl_shaders.translate(source, stage, '1864' + stage, tmp)
                self.assertIsInstance(r, tuple, r)
                msl = Path(r[0]).read_text()
                self.assertIn('[[user(dm_LegacyColor)]]', msl)
                self.assertIn('[[user(dm_LegacyTexCoord0)]]', msl)
                if stage == 'frag': self.assertIn('[[color(0)]]', msl)

    def test_desktop_glsl_120_implicit_conversions_and_texture3d(self):
        frag = (b'#version 120\nvarying vec3 uvw;\nuniform sampler3D lut;\n'
                b'void main() { float k = 1; gl_FragColor = texture3D(lut, uvw) * k + vec4(0, 0, 0, 1); }\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(frag, 'frag', '3021' + 'f' * 12, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertIn('t:lut=0:1:sampler3D', r[1])
            self.assertIn('texture3d<float>', Path(r[0]).read_text())

    def test_explicit_sampler_layout_bindings_preserve_units_and_default_others_to_zero(self):
        frag = (b'#version 450 core\nlayout(binding = 0) uniform sampler2D u_y;\nlayout(binding = 1) uniform sampler2D u_uv;\n'
                b'uniform layout(binding = 3) sampler2D u_lut;\nuniform sampler2D u_plain;\nout vec4 c;\n'
                b'void main() { c = texture(u_y, vec2(0)) + texture(u_uv, vec2(0)) + texture(u_lut, vec2(0)) + texture(u_plain, vec2(0)); }\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(frag, 'frag', '3021' + 'b' * 12, tmp)
            self.assertIsInstance(r, tuple, r)
            samplers = {x.split('=')[0]: x.split('=')[1] for x in r[1] if x.startswith('t:')}
            self.assertEqual(samplers['t:u_y'].count(':'), 2)       # binding 0 omitted (defaults to 0)
            self.assertEqual(samplers['t:u_plain'].count(':'), 2)   # no explicit binding: defaults to 0, NOT glslang's --amb slot
            self.assertTrue(samplers['t:u_uv'].endswith(':sampler2D:1'), samplers['t:u_uv'])
            self.assertTrue(samplers['t:u_lut'].endswith(':sampler2D:3'), samplers['t:u_lut'])

    def test_es_versions_without_profile_get_es_and_keep_original_key(self):
        # FINAL FANTASY VII (#3292): ES 3.20 shaders with precision/layout declare `#version 320` without `es`.
        body = (b'precision highp float;\nlayout(location = 0) in vec2 uv;\n'
                b'uniform sampler2D tex;\nout vec4 color;\nvoid main() { color = texture(tex, uv); }\n')
        for header in (b'#version 320\n', b'#version 320\r\n', b'#version 320 // ff7\n',
                       b'#version 320 /* ff7 */\n', b'#version 310\n', b'#version 320 es\n'):
            src = header + body
            with self.subTest(header=header), tempfile.TemporaryDirectory() as tmp:
                rows = gl_shaders.one((src, 'frag', '/ff7/shaders/test.glsl'), tmp)
                self.assertEqual(len(rows), 1)
                key, stage, fields, msl, _ = rows[0]
                self.assertEqual(stage, 'frag', fields)
                self.assertEqual(key, gl_shaders.hashlib.sha256(src).hexdigest()[:16])
                self.assertIn('t:tex=0:1:sampler2D', fields)
                self.assertIn(f'gl_{key}_f', Path(msl).read_text())

        with tempfile.TemporaryDirectory() as tmp:
            err = gl_shaders.translate(b'#version 320 core\n' + body, 'frag', '3292' + 'e' * 12, tmp)
            self.assertIsInstance(err, str)
            self.assertIn('glslang -E:', err)

    def test_empty_file_scope_declarations_translate_under_glsl_450(self):
        # Mark of the Ninja: Remastered (#3415): semicolon-terminated macros produce `;;` at file scope in `#version 450`, which glslang rejects before 4.60.
        src = (b'#version 450\n'
               b'#define EMPTY_DECL(x)\n'
               b'#define DECL_TEXTURE_REGISTER(name, slot) layout(binding = slot) uniform sampler2D name;\n'
               b'EMPTY_DECL(a);\n'
               b'DECL_TEXTURE_REGISTER(FOWTexture, 6);\n'
               b'struct S { vec4 tint; };;;\n'
               b'in vec2 uv;\nout vec4 color;\n'
               b'void main() {\n'
               b'\tif (uv.x < 0.0) discard;;\n'
               b'\tcolor = texture(FOWTexture, uv);\n'
               b'};;\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(src, 'frag', '3415' + 'f' * 12, tmp)
            self.assertIsInstance(r, tuple, r)
            self.assertIn('t:FOWTexture=0:1:sampler2D:6', r[1])

    def test_variable_texel_offsets_in_every_offset_function(self):
        # NVIDIA's compiler takes any expression as a texel offset; GLSL and glslang want a constant ("'texel offset' : argument must be compile-time constant").
        # Digimon's FXAA calls textureOffset with one; FINAL FANTASY VII's fxaanv5_p.glsl (#3292) fails the same way on textureLodOffset.
        decl = {'2D': 'sampler2D tex', '2DArray': 'highp sampler2DArray tex', '3D': 'highp sampler3D tex', '2DShadow': 'highp sampler2DShadow tex',
                'i2D': 'highp isampler2D tex'}
        coord = {'2D': 'uv', '2DArray': 'vec3(uv, 1.0)', '3D': 'vec3(uv, 0.5)', '2DShadow': 'vec3(uv, 0.5)'}
        off = {'2D': 'o', '2DArray': 'o', '3D': 'ivec3(o, 1)', '2DShadow': 'o'}
        grad = {'2D': 'vec2(0.01)', '2DArray': 'vec2(0.01)', '3D': 'vec3(0.01)', '2DShadow': 'vec2(0.01)'}
        calls = [(k, f'textureOffset(tex, {coord[k]}, {off[k]})') for k in coord]
        calls += [(k, f'textureOffset(tex, {coord[k]}, {off[k]}, 0.5)') for k in ('2D', '2DArray', '3D', '2DShadow')]   # with a bias
        calls += [(k, f'textureLodOffset(tex, {coord[k]}, 1.0, {off[k]})') for k in ('2D', '2DArray', '3D', '2DShadow')]
        calls += [(k, f'textureGradOffset(tex, {coord[k]}, {grad[k]}, {grad[k]}, {off[k]})') for k in coord]
        calls += [('2D', 'texelFetchOffset(tex, ivec2(uv * 64.0), 0, o)'), ('2DArray', 'texelFetchOffset(tex, ivec3(uv * 64.0, 1), 0, o)'),
                  ('3D', 'texelFetchOffset(tex, ivec3(uv * 64.0, 1), 0, ivec3(o, 1))'), ('i2D', 'vec4(texelFetchOffset(tex, ivec2(uv * 64.0), 0, o))')]
        calls += [('2D', 'textureProjOffset(tex, vec3(uv, 1.0), o)'), ('2D', 'textureProjOffset(tex, vec4(uv, 0.0, 1.0), o, 0.5)'),
                  ('2D', 'textureProjLodOffset(tex, vec3(uv, 1.0), 1.0, o)'), ('2D', 'textureProjGradOffset(tex, vec4(uv, 0.0, 1.0), vec2(0.01), vec2(0.01), o)'),
                  ('3D', 'textureProjOffset(tex, vec4(uv, 0.5, 1.0), ivec3(o, 1))'),
                  ('2DShadow', 'textureProjOffset(tex, vec4(uv, 0.5, 1.0), o)')]
        for kind, call in calls:
            src = (f'#version 320 es\nprecision highp float;\nprecision highp int;\nuniform {decl[kind]};\nuniform ivec2 step_;\nin vec2 uv;\nout vec4 color;\n'
                   f'void main() {{ ivec2 o = step_ * 2; color = vec4({call}); }}\n').encode()
            with self.subTest(call=call), tempfile.TemporaryDirectory() as tmp:
                r = gl_shaders.translate(src, 'frag', gl_shaders.hashlib.sha256(src).hexdigest()[:16], tmp)
                self.assertIsInstance(r, tuple, r)
                fixed = gl_shaders.fixups(src.decode(), 'frag')
                self.assertNotRegex(fixed, gl_shaders.OFFSET_RE, 'raw offset call is routed to helper')
                self.assertIn('dm_var_', fixed)
        # A constant offset stays untouched with no helpers injected (glslang takes it, and the sampler applies it at each mip level it reads)
        const = 'textureLodOffset(tex, uv, 1.0, ivec2(1, -1))'
        src_const = f'uniform sampler2D tex;\nvoid main() {{ c = {const}; }}\n'
        self.assertEqual(gl_shaders.fixups(src_const, 'frag'), src_const)
        # textureLodOffset helper clamps lod and size against negative/out-of-range levels; texelFetchOffset preserves array layer.
        lod = gl_shaders.fixups('#version 320 es\nuniform sampler2D tex;\nvoid main() { c = textureLodOffset(tex, uv, lv, o); }\n', 'frag')
        self.assertIn('dm_var_textureLodOffset(tex, uv, lv, o)', lod)
        self.assertIn('max(textureSize(s, max(int(l), 0)), ivec2(1))', lod)
        fetch = gl_shaders.fixups('#version 320 es\nuniform highp sampler2DArray tex;\nvoid main() { c = texelFetchOffset(tex, p, 2, o); }\n', 'frag')
        self.assertIn('dm_var_texelFetchOffset(tex, p, 2, o)', fetch)
        self.assertIn('texelFetch(s, p + ivec3(o, 0), l)', fetch)

    def test_variable_texel_offsets_more_forms(self):
        head = '#version 320 es\nprecision highp float;\nprecision highp int;\nuniform ivec2 step_;\nin vec2 uv;\nout vec4 color;\n'
        cases = [  # (declarations, the expression; it must compile)
            ('uniform highp sampler2DArrayShadow tex;', 'vec4(textureGradOffset(tex, vec4(uv, 1.0, 0.5), vec2(0.01), vec2(0.01), o))'),
            ('uniform highp isampler2DArray tex;', 'vec4(texelFetchOffset(tex, ivec3(uv * 64.0, 1), 0, o))'),
            ('uniform highp usampler2DArray tex;', 'vec4(texelFetchOffset(tex, ivec3(uv * 64.0, 1), 0, o))'),
            ('uniform sampler2D tex;\nuniform sampler2D t2;', 'textureLodOffset(tex, uv + vec2(texelFetchOffset(t2, ivec2(uv * 8.0), 0, o).xy), 0.0, o)'),   # nested variable calls
            ('uniform sampler2D tex;\nuniform sampler2D t2;', 'textureLodOffset(tex, uv + vec2(texelFetchOffset(t2, ivec2(uv * 8.0), 0, o).xy), 0.0, ivec2(1, 0))'),  # inner only
            ('uniform sampler2D tex;\nuniform highp sampler2DArray arr;\nvec4 f(sampler2D s, ivec2 d) { return textureOffset(s, uv, d, 0.5); }\n'
             'vec4 h(highp sampler2DArray s, ivec2 d) { return textureGradOffset(s, vec3(uv, 1.0), vec2(0.01), vec2(0.01), d); }', 'f(tex, o) + h(arr, o)'),   # one name, two types
            # Comma-separated uniform samplers, and an earlier function's parameter not shadowing a global uniform in main
            ('uniform sampler2D s0, s1;\nvec4 f(highp sampler2DArray s1, ivec2 d) { return textureLodOffset(s1, vec3(uv, 0.0), 0.0, d); }',
             'textureLodOffset(s1, uv, 0.0, o)'),
            # mediump sampler and dynamic sampler-array element work through highp helpers
            ('uniform mediump sampler2D arr[2];\nuniform int idx;', 'textureLodOffset(arr[idx], uv, 0.0, o)'),
            # A const function parameter is read-only, not a compile-time constant expression (common in FXAA helpers)
            ('uniform sampler2D tex;\nvec4 fxaaOff(sampler2D s, vec2 p, const ivec2 d) { return textureLodOffset(s, p, 0.0, d); }', 'fxaaOff(tex, uv, o)'),
            # A const name in one function must not suppress lowering when another function uses a non-const local with the same name
            ('uniform sampler2D tex;\nint helper() { const int o = 1; return o; }', 'textureLodOffset(tex, uv, 0.0, o) + float(helper())'),
            # Ternary offset in texelFetchOffset keeps precedence
            ('uniform sampler2D tex;', 'texelFetchOffset(tex, ivec2(uv * 64.0), 0, o.x > 0 ? o : ivec2(1, 0))'),
        ]
        for decl, expr in cases:
            src = f'{head}{decl}\nvoid main() {{ ivec2 o = step_ * 2; color = {expr}; }}\n'.encode()
            with self.subTest(expr=expr), tempfile.TemporaryDirectory() as tmp:
                r = gl_shaders.translate(src, 'frag', gl_shaders.hashlib.sha256(src).hexdigest()[:16], tmp)
                self.assertIsInstance(r, tuple, r)
        # Vertex stage emits no fragment-only bias overloads
        vert_src = (b'#version 320 es\nprecision highp float;\nprecision highp int;\nuniform sampler2D tex;\nuniform ivec2 step_;\nin vec2 uv;\n'
                    b'void main() { gl_Position = textureOffset(tex, uv, step_) + textureProjOffset(tex, vec3(uv, 1.0), step_); }\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(vert_src, 'vert', gl_shaders.hashlib.sha256(vert_src).hexdigest()[:16], tmp)
            self.assertIsInstance(r, tuple, r)
        # Desktop GLSL: non-constant `const` locals, plus 1D, 1DArray, 2DRect (3-arg fetch), and shadow/projective desktop samplers
        desktop_src = (b'#version 450 core\nuniform sampler1D s1;\nuniform sampler1DArray s1a;\nuniform sampler2DRect sr;\n'
                       b'uniform sampler1DShadow s1s;\nuniform sampler1DArrayShadow s1as;\nuniform sampler2DRectShadow srs;\n'
                       b'uniform ivec2 step_;\nin vec4 uv;\nout vec4 color;\n'
                       b'void main() {\n'
                       b'    const ivec2 o = step_ * 2;\n'
                       b'    color = textureLodOffset(s1, uv.x, 0.0, o.x) + textureOffset(s1a, uv.xy, o.x)\n'
                       b'          + textureOffset(sr, uv.xy, o) + texelFetchOffset(sr, ivec2(uv.xy), o)\n'
                       b'          + textureProjOffset(s1, uv.xy, o.x) + textureProjOffset(sr, uv.xyz, o)\n'
                       b'          + vec4(textureOffset(s1s, uv.xyz, o.x) + textureLodOffset(s1as, uv.xyz, 0.0, o.x) + textureProjOffset(srs, uv, o));\n'
                       b'}\n')
        with tempfile.TemporaryDirectory() as tmp:
            r = gl_shaders.translate(desktop_src, 'frag', gl_shaders.hashlib.sha256(desktop_src).hexdigest()[:16], tmp)
            self.assertIsInstance(r, tuple, r)
        # Constant expressions stay as written with no helpers injected
        for call in ('textureGradOffset(tex, uv, vec2(0.01), vec2(0.01), K)', 'textureOffset(tex, uv, ivec2(1u, 0u), 0.5)', 'textureOffset(tex, uv, K)',
                     'textureLodOffset(tex, uv, 0.0, K2)'):
            t = f'const int DX = 1, DY = -1;\nconst ivec2 K = ivec2(1, -1);\nconst ivec2 K2 = ivec2(DX, DY);\nuniform sampler2D tex;\nvoid main() {{ c = {call}; }}\n'
            self.assertEqual(gl_shaders.fixups(t, 'frag'), t)


class MainStrings(unittest.TestCase):
    def test_a_prefix_is_not_a_shader(self):
        self.assertIsNone(gl_shaders.GL_ENTRY.search(PREFIX.encode()))
        self.assertIsNotNone(gl_shaders.GL_ENTRY.search(VERT.encode()))


def gpu_fixture(out):
    """Original GLSL → table and metallib → GL imports and GPU pixels in t_gl_imports.m."""
    out = Path(out); out.mkdir(exist_ok=True)
    vert = ('#version 320 es\nprecision highp float;\nlayout(location=15) in vec4 position;\nout vec2 uv;\n'
            'uniform vec4 shift;\nuniform sampler2D tex0;\nuniform Block { vec4 factors; };\n'
            'void main() { gl_Position = position + shift + vec4(factors.x, 0, 0, 0); uv = vec2(0.75 + texture(tex0, vec2(0.25, 0.5)).g, shift.y + factors.z); }\n')
    frag = ('#version 320 es\nprecision highp float;\nin vec2 uv;\nout vec4 color;\nuniform vec4 weight;\nuniform Block { vec4 factors; };\n'
            + ''.join(f'uniform sampler2D tex{i};\n' for i in range(17))
            + 'void main() { int i = int(gl_FragCoord.x) % 17; color = vec4(0);\n'
            + ''.join(f'if (i == {i}) color = texture(tex{i}, uv) * weight * factors.y;\n' for i in range(17)) + 'color.g = uv.y * factors.y; }\n')
    (out / 'resource.vert').write_text(vert); (out / 'resource.frag').write_text(frag)
    sys.argv = ['gl_shaders.py', str(out), str(out)]; gl_shaders.main()


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': gpu_fixture(sys.argv[2])
    else: unittest.main()
