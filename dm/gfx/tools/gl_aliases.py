#!/usr/bin/env python3
"""Writes dm/gfx/gl/gl_aliases.h: the extension names of GL functions that are the same function as another name, which the GL layer's eglGetProcAddress answers with that
other name's entry (hle_gl.m gl_lookup). A game's loader may ask only for an extension's name (Fate/hollow ataraxia REMASTERED: glGenFramebuffersEXT; DOOM 3: glBindBufferARB).

The source is Khronos's registry, gl.xml: a command's <alias name="..."/> says that it is the same function as that one (same arguments, same behaviour). Stripping a vendor
suffix is not the same thing: glVertexAttribPointerNV takes 5 arguments and glVertexAttribPointer 6, glGetProgramivARB queries an ARB assembly program by its target,
glPolygonOffsetEXT's bias is in other units, so a name becomes another only where the registry or the list below says so.

gl.xml records no aliases for the OpenGL ES extensions, so the ES extension functions that the ES core took over unchanged are listed here, each reviewed against its
extension's specification (ES_SAME). EXT_framebuffer_object's binds are left out of gl.xml because a bind there makes the object of a name glGen* never gave, which core GL
forbids; the layer's binds make it (as GL ES's and the compatibility profile's do), so they are listed too.

Usage: gl_aliases.py <gl.xml>   (https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml); writes dm/gfx/gl/gl_aliases.h"""
import hashlib
import os
import sys
import xml.etree.ElementTree as ET

ES_SAME = {
    # OES_framebuffer_object, taken unchanged by GL ES 2.0
    'glIsRenderbufferOES': 'glIsRenderbuffer', 'glBindRenderbufferOES': 'glBindRenderbuffer', 'glDeleteRenderbuffersOES': 'glDeleteRenderbuffers',
    'glGenRenderbuffersOES': 'glGenRenderbuffers', 'glRenderbufferStorageOES': 'glRenderbufferStorage', 'glGetRenderbufferParameterivOES': 'glGetRenderbufferParameteriv',
    'glIsFramebufferOES': 'glIsFramebuffer', 'glBindFramebufferOES': 'glBindFramebuffer', 'glDeleteFramebuffersOES': 'glDeleteFramebuffers',
    'glGenFramebuffersOES': 'glGenFramebuffers', 'glCheckFramebufferStatusOES': 'glCheckFramebufferStatus', 'glFramebufferRenderbufferOES': 'glFramebufferRenderbuffer',
    'glFramebufferTexture2DOES': 'glFramebufferTexture2D', 'glGetFramebufferAttachmentParameterivOES': 'glGetFramebufferAttachmentParameteriv',
    'glGenerateMipmapOES': 'glGenerateMipmap',
    # EXT_framebuffer_object's binds (the rest of it is in gl.xml): a name never generated is made at its first bind, as the layer's binds do
    'glBindFramebufferEXT': 'glBindFramebuffer', 'glBindRenderbufferEXT': 'glBindRenderbuffer',
    # OES_texture_3D, taken unchanged by GL ES 3.0
    'glTexImage3DOES': 'glTexImage3D', 'glTexSubImage3DOES': 'glTexSubImage3D', 'glCopyTexSubImage3DOES': 'glCopyTexSubImage3D',
    'glCompressedTexImage3DOES': 'glCompressedTexImage3D', 'glCompressedTexSubImage3DOES': 'glCompressedTexSubImage3D', 'glFramebufferTexture3DOES': 'glFramebufferTexture3D',
    # OES_blend_subtract, OES_blend_equation_separate, OES_blend_func_separate (GL ES 2.0)
    'glBlendEquationOES': 'glBlendEquation', 'glBlendEquationSeparateOES': 'glBlendEquationSeparate', 'glBlendFuncSeparateOES': 'glBlendFuncSeparate',
    # ANGLE_framebuffer_blit, ANGLE_framebuffer_multisample, APPLE_framebuffer_multisample's storage call, NV_draw_buffers, NV_read_buffer (GL ES 3.0)
    'glBlitFramebufferANGLE': 'glBlitFramebuffer', 'glRenderbufferStorageMultisampleANGLE': 'glRenderbufferStorageMultisample',
    'glRenderbufferStorageMultisampleAPPLE': 'glRenderbufferStorageMultisample', 'glDrawBuffersNV': 'glDrawBuffers', 'glReadBufferNV': 'glReadBuffer',
}


def aliases(xml_path):
    """{name: the name it is the same function as}, gl.xml's aliases followed to their end, with ES_SAME added"""
    cmds = {}
    for c in ET.parse(xml_path).getroot().iterfind('commands/command'):
        a = c.find('alias')
        cmds[c.findtext('proto/name')] = a.get('name') if a is not None else None
    out = {}
    for n, a in cmds.items():
        seen = {n}
        while a and cmds.get(a) and a not in seen: seen.add(a); a = cmds[a]
        if a: out[n] = a
    for n, a in ES_SAME.items():
        if n not in cmds or a not in cmds: sys.exit(f'[RED] {n} -> {a}: not a gl.xml command')
        if out.get(n, a) != a: sys.exit(f'[RED] {n}: gl.xml says {out[n]}, ES_SAME says {a}')
        out[n] = a
    return out


def main():
    if len(sys.argv) != 2: sys.exit(__doc__)
    xml_path = sys.argv[1]
    al = aliases(xml_path)
    sha = hashlib.sha256(open(xml_path, 'rb').read()).hexdigest()[:16]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'gl', 'gl_aliases.h')
    with open(out, 'w') as f:
        f.write('// Written by dm/gfx/tools/gl_aliases.py from Khronos gl.xml (sha256 %s) and its reviewed GL ES list: do not edit.\n' % sha)
        f.write('// An extension name and the name of the same function (hle_gl.m gl_lookup), sorted by name for a binary search: %d names.\n' % len(al))
        f.write('static const struct { const char *name, *same; } gl_aliases[] = {\n')
        for n in sorted(al): f.write('    {"%s", "%s"},\n' % (n, al[n]))
        f.write('};\n')
    print(f'{os.path.relpath(out)}: {len(al)} names')


if __name__ == '__main__':
    main()
