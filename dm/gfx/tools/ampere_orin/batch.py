#!/usr/bin/env python3
"""On the Jetson: for every op in manifest.json, JIT its PTX to a cubin, disassemble it with nvdisasm, and run it on the op's inputs.

  batch.py manifest.json        (run from the folder holding the ops' .ptx and .in.bin files)

Writes <op>.cubin, <op>.sass and <op>.orin.bin beside them, and <op>.err when a step fails. Stdlib only, through cu.py."""
import json
import os
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cu  # noqa: E402
import prog  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    import ctypes as C
    cu.cu.cuGetErrorString.argtypes = [C.c_int, C.POINTER(C.c_char_p)]
    cu.cu.cuLinkAddData_v2.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_size_t, C.c_char_p, C.c_uint, C.c_void_p, C.c_void_p]
    cu.cu.cuMemcpyHtoD_v2.argtypes = [C.c_uint64, C.c_char_p, C.c_size_t]
    cu.cu.cuMemcpyDtoH_v2.argtypes = [C.c_void_p, C.c_uint64, C.c_size_t]
    cu.cu.cuMemsetD8_v2.argtypes = [C.c_uint64, C.c_ubyte, C.c_size_t]
    cu.cu.cuMemFree_v2.argtypes = [C.c_uint64]
    cu.cu.cuLaunchKernel.argtypes = [C.c_void_p] + [C.c_uint] * 7 + [C.c_void_p, C.c_void_p, C.c_void_p]
    cu.init()
    ops = json.load(open(sys.argv[1]))
    ok = 0
    name, drv = C.create_string_buffer(256), C.c_int()
    cu.cu.cuDeviceGetName(name, 256, 0)
    cu.cu.cuDriverGetVersion(C.byref(drv))
    maj, mnr = C.c_int(), C.c_int()
    cu.cu.cuDeviceGetAttribute(C.byref(maj), 75, 0)   # CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
    cu.cu.cuDeviceGetAttribute(C.byref(mnr), 76, 0)   # ..._MINOR
    nvd = subprocess.run([os.path.join(HERE, 'nvdisasm'), '--version'], capture_output=True, text=True).stdout.strip().splitlines()[-1:]
    json.dump({'gpu': name.value.decode(), 'sm': f'{maj.value}{mnr.value}', 'cuda_driver': f'{drv.value // 1000}.{drv.value % 1000 // 10}',
               'nvdisasm': nvd[0] if nvd else ''}, open('device.info', 'w'))
    for op in ops:
        n = op['name']
        for ext in ('.err', '.orin.bin', '.prog.bin', '.prog.sass'):
            if os.path.exists(n + ext):
                os.remove(n + ext)
        try:
            cubin = cu.ptx2cubin(open(n + '.ptx', 'rb').read())
            open(n + '.cubin', 'wb').write(cubin)
            sass = subprocess.run([os.path.join(HERE, 'nvdisasm'), '-hex', '-c', n + '.cubin'], capture_output=True, text=True, check=True).stdout
            open(n + '.sass', 'w').write(sass)
            out = cu.run(cubin, 'k', op['groups'], 32, 0, [open(n + '.in.bin', 'rb').read()], op['groups'] * 32 * op['nout'] * 4)
            open(n + '.orin.bin', 'wb').write(out)
            code, rb, notes = prog.rewrite(cubin, sass, op['nin'], op['nout'])   # the Metal side's program, checked by the disassembler here
            open(n + '.prog.bin', 'wb').write(code)
            dis = subprocess.run([os.path.join(HERE, 'nvdisasm'), '--binary', 'SM87', '-hex', n + '.prog.bin'], capture_output=True, text=True, check=True).stdout
            open(n + '.prog.sass', 'w').write(f'// base register R{rb}\n' + ''.join(f'// {x}\n' for x in notes) + dis)
            ok += 1
        except BaseException as e:   # cu.ck raises SystemExit
            open(n + '.err', 'w').write(f'{type(e).__name__}: {e}\n{traceback.format_exc()}')
            print(f'{n}: {e}', file=sys.stderr)
            if 'kernel' in str(e) or 'cuLaunchKernel' in str(e):   # a fault poisons the context: start a fresh one
                cu.init()
    print(f'{ok}/{len(ops)} ops ran')


if __name__ == '__main__':
    main()
