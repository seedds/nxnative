#!/usr/bin/env python3
"""Runs a kernel on the Jetson through libcuda alone (no toolkit, no numpy): the driver JIT-compiles PTX for this GPU, or loads a cubin as given.

  cu.py ptx2cubin   k.ptx k.cubin                      JIT the PTX for this GPU and save the cubin the linker produced
  cu.py run         k.(ptx|cubin) kernel grid block shared in.bin out_bytes out.bin [more in.bin ...]
                    launches kernel(in0, [in1, ...,] out) with grid x block threads and `shared` bytes of dynamic shared memory

Every buffer is a plain device allocation; the kernel gets their addresses in argument order, the output last."""
import ctypes as C
import sys

cu = C.CDLL('libcuda.so.1')


def ck(r, what):
    if r:
        s = C.c_char_p()
        cu.cuGetErrorString(r, C.byref(s))
        raise SystemExit(f'{what}: {r} {s.value.decode() if s.value else ""}')


def init():
    ck(cu.cuInit(0), 'cuInit')
    dev = C.c_int()
    ck(cu.cuDeviceGet(C.byref(dev), 0), 'cuDeviceGet')
    ctx = C.c_void_p()
    ck(cu.cuDevicePrimaryCtxRetain(C.byref(ctx), dev), 'cuDevicePrimaryCtxRetain')
    ck(cu.cuCtxSetCurrent(ctx), 'cuCtxSetCurrent')
    return dev


CU_JIT_INFO_LOG_BUFFER, CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES = 3, 4
CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 5, 6
CU_JIT_INPUT_PTX = 1


def ptx2cubin(ptx: bytes) -> bytes:
    log = C.create_string_buffer(1 << 16)
    opts = (C.c_int * 2)(CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES)
    vals = (C.c_void_p * 2)(C.cast(log, C.c_void_p), C.c_void_p(len(log)))
    st = C.c_void_p()
    ck(cu.cuLinkCreate_v2(2, opts, vals, C.byref(st)), 'cuLinkCreate')
    data = C.create_string_buffer(ptx + b'\0')
    r = cu.cuLinkAddData_v2(st, CU_JIT_INPUT_PTX, data, C.c_size_t(len(ptx) + 1), b'k.ptx', 0, None, None)
    if r:
        sys.stderr.write(log.value.decode(errors='replace'))
    ck(r, 'cuLinkAddData')
    out, size = C.c_void_p(), C.c_size_t()
    ck(cu.cuLinkComplete(st, C.byref(out), C.byref(size)), 'cuLinkComplete')
    cubin = C.string_at(out, size.value)
    cu.cuLinkDestroy(st)
    return cubin


def run(image: bytes, kernel: str, grid: int, block: int, shared: int, inputs, out_bytes: int) -> bytes:
    mod = C.c_void_p()
    img = C.create_string_buffer(image + (b'\0' if image[:4] != b'\x7fELF' else b''))
    ck(cu.cuModuleLoadData(C.byref(mod), img), 'cuModuleLoadData')
    fn = C.c_void_p()
    ck(cu.cuModuleGetFunction(C.byref(fn), mod, kernel.encode()), 'cuModuleGetFunction')
    if shared > 48 * 1024:
        ck(cu.cuFuncSetAttribute(fn, 8, shared), 'cuFuncSetAttribute(max dynamic shared)')   # CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES
    ptrs = []
    for data in inputs:
        p = C.c_uint64()
        ck(cu.cuMemAlloc_v2(C.byref(p), C.c_size_t(max(len(data), 4))), 'cuMemAlloc')
        ck(cu.cuMemcpyHtoD_v2(p, data, C.c_size_t(len(data))), 'cuMemcpyHtoD')
        ptrs.append(p)
    o = C.c_uint64()
    ck(cu.cuMemAlloc_v2(C.byref(o), C.c_size_t(out_bytes)), 'cuMemAlloc')
    ck(cu.cuMemsetD8_v2(o, 0xcd, C.c_size_t(out_bytes)), 'cuMemsetD8')   # a fill no result is likely to equal, so an unwritten word shows
    ptrs.append(o)
    args = (C.c_void_p * len(ptrs))(*[C.cast(C.byref(p), C.c_void_p) for p in ptrs])
    ck(cu.cuLaunchKernel(fn, grid, 1, 1, block, 1, 1, shared, None, args, None), 'cuLaunchKernel')
    ck(cu.cuCtxSynchronize(), 'kernel')
    buf = C.create_string_buffer(out_bytes)
    ck(cu.cuMemcpyDtoH_v2(buf, o, C.c_size_t(out_bytes)), 'cuMemcpyDtoH')
    for p in ptrs:
        cu.cuMemFree_v2(p)
    cu.cuModuleUnload(mod)
    return buf.raw


if __name__ == '__main__':
    cu.cuGetErrorString.argtypes = [C.c_int, C.POINTER(C.c_char_p)]
    cu.cuLinkAddData_v2.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_size_t, C.c_char_p, C.c_uint, C.c_void_p, C.c_void_p]
    cu.cuMemcpyHtoD_v2.argtypes = [C.c_uint64, C.c_char_p, C.c_size_t]
    cu.cuMemcpyDtoH_v2.argtypes = [C.c_void_p, C.c_uint64, C.c_size_t]
    cu.cuMemsetD8_v2.argtypes = [C.c_uint64, C.c_ubyte, C.c_size_t]
    cu.cuMemFree_v2.argtypes = [C.c_uint64]
    cu.cuLaunchKernel.argtypes = [C.c_void_p] + [C.c_uint] * 7 + [C.c_void_p, C.c_void_p, C.c_void_p]
    init()
    cmd, a = sys.argv[1], sys.argv[2:]
    if cmd == 'ptx2cubin':
        open(a[1], 'wb').write(ptx2cubin(open(a[0], 'rb').read()))
    elif cmd == 'run':
        src, kernel, grid, block, shared, in0, out_bytes, out = a[:8]
        image = open(src, 'rb').read()
        if src.endswith('.ptx'):
            image = ptx2cubin(image)
        ins = [open(p, 'rb').read() for p in [in0] + a[8:]]
        open(out, 'wb').write(run(image, kernel, int(grid), int(block), int(shared), ins, int(out_bytes)))
    else:
        raise SystemExit(__doc__)
