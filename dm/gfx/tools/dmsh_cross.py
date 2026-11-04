"""The pinned SPIRV-Cross C API, with one independent context per call.
ctypes releases the GIL inside the compiler. Missing libraries and options not mapped here use the CLI.
Errors also go through the CLI, retaining its diagnostics. No shader stage is omitted.
"""
import ctypes as c
import hashlib
import os

LIBRARY = os.path.join(os.path.dirname(__file__), 'spirv_cross_bin/libspirv-cross-c-shared.dylib')
MSL_BIT = 0x8000000
BOOL_OPTIONS = {'--msl-argument-buffers': 32, '--msl-texture-buffer-native': 34, '--msl-pad-fragment-output': 29}


def options(args):
    parsed, at = [], 0
    while at < len(args):
        flag = args[at]; at += 1
        if flag == '--msl': continue
        if flag in BOOL_OPTIONS: parsed.append((BOOL_OPTIONS[flag] | MSL_BIT, 1, True))
        elif flag == '--msl-version' and at < len(args):
            try: value = int(args[at])
            except ValueError: return None
            at += 1; parsed.append((17 | MSL_BIT, value, False))
        else: return None
    return parsed


class Error(Exception):
    pass


class Compiler:
    def __init__(self, path=LIBRARY):
        self.library, self.record = None, 'cli'
        if os.environ.get('DMSH_CROSS', 'auto') == 'cli' or not os.path.isfile(path): return
        lib = c.CDLL(path)
        signatures = {
            'spvc_context_create': ([c.POINTER(c.c_void_p)], c.c_int),
            'spvc_context_destroy': ([c.c_void_p], None),
            'spvc_context_get_last_error_string': ([c.c_void_p], c.c_char_p),
            'spvc_context_parse_spirv': ([c.c_void_p, c.POINTER(c.c_uint32), c.c_size_t, c.POINTER(c.c_void_p)], c.c_int),
            'spvc_context_create_compiler': ([c.c_void_p, c.c_int, c.c_void_p, c.c_int, c.POINTER(c.c_void_p)], c.c_int),
            'spvc_compiler_create_compiler_options': ([c.c_void_p, c.POINTER(c.c_void_p)], c.c_int),
            'spvc_compiler_options_set_bool': ([c.c_void_p, c.c_int, c.c_ubyte], c.c_int),
            'spvc_compiler_options_set_uint': ([c.c_void_p, c.c_int, c.c_uint], c.c_int),
            'spvc_compiler_install_compiler_options': ([c.c_void_p, c.c_void_p], c.c_int),
            'spvc_compiler_compile': ([c.c_void_p, c.POINTER(c.c_char_p)], c.c_int),
        }
        for name, (arguments, result) in signatures.items():
            fn = getattr(lib, name); fn.argtypes, fn.restype = arguments, result
        with open(path, 'rb') as f: self.record = 'c-api ' + hashlib.sha256(f.read()).hexdigest()
        self.library = lib

    def compile(self, binary, args):
        config = options(args)
        if self.library is None or config is None: return None
        if len(binary) < 20 or len(binary) % 4 or binary[:4] != b'\x03\x02\x23\x07': raise Error('not a SPIR-V module')
        lib, context = self.library, c.c_void_p()
        def check(status):
            if status:
                message = lib.spvc_context_get_last_error_string(context) if context.value else None
                raise Error(message.decode(errors='replace') if message else 'SPIRV-Cross context creation failed')
        check(lib.spvc_context_create(c.byref(context)))
        try:
            words = (c.c_uint32 * (len(binary) // 4)).from_buffer_copy(binary)
            ir, compiler, opts, source = c.c_void_p(), c.c_void_p(), c.c_void_p(), c.c_char_p()
            check(lib.spvc_context_parse_spirv(context, words, len(words), c.byref(ir)))
            check(lib.spvc_context_create_compiler(context, 3, ir, 1, c.byref(compiler)))  # MSL, take ownership
            check(lib.spvc_compiler_create_compiler_options(compiler, c.byref(opts)))
            for option, value, boolean in config:
                fn = lib.spvc_compiler_options_set_bool if boolean else lib.spvc_compiler_options_set_uint
                check(fn(opts, option, value))
            check(lib.spvc_compiler_install_compiler_options(compiler, opts))
            check(lib.spvc_compiler_compile(compiler, c.byref(source)))
            return source.value.decode() if source.value is not None else ''
        finally: lib.spvc_context_destroy(context)
