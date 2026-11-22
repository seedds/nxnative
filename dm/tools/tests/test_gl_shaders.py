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
             'float f(float x) { if (x > 0.0) { return x; } return 0.0; } ;\nvoid main()\n{\n\tgl_Position = vec4(f(1.0));\n};\n')
        g = gl_shaders.function_semicolons(t)
        self.assertIn('struct L { vec4 c; };', g); self.assertIn('} b;', g); self.assertIn('uniform C { vec4 w; };', g)
        self.assertIn('return 0.0; }\nvoid main', g)
        self.assertNotIn('} ;', g); self.assertTrue(g.rstrip().endswith('}'))
        self.assertEqual(gl_shaders.function_semicolons(g), g)


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
            (Path(d) / 'shaders/a.vert').write_bytes(b'#version 320 es\nvoid main() {}\n')
            got = sorted((w, st, s) for s, st, w in gl_shaders.sources([d]))
            self.assertEqual(got, [('/shaders/a.vert', 'vert', b'#version 320 es\nvoid main() {}\n'), ('/shaders/default_ps.glsl', None, b'#version 320 es\nvoid main() {}')])


@unittest.skipUnless(shutil.which('glslangValidator'), 'glslangValidator is not installed')
class Translate(unittest.TestCase):
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
