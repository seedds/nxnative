"""The ops the batch checks: each a PTX body over 32-bit inputs %i0.. that writes 32-bit outputs %o0...

Input kinds (gen.py makes the values): f = f32, h = f16x2, b = bf16x2, u = 32-bit integer, s = shift or bit count, l = lane index, p = predicate-ish bit.
Output kinds (compare.py reads them): f, h, b, u. An op is (name, family, inputs, outputs, body)."""

OPS = []


def op(name, family, ins, outs, body):
    OPS.append(dict(name=name, family=family, ins=ins, outs=outs, body=body))


def fbin(o, extra=''):   # f32 a op b
    return f'mov.b32 %f0, %i0; mov.b32 %f1, %i1; {o} %f2, %f0, %f1; mov.b32 %o0, %f2;'


def fun(o):   # f32 op a
    return f'mov.b32 %f0, %i0; {o} %f2, %f0; mov.b32 %o0, %f2;'


def fter(o):
    return f'mov.b32 %f0, %i0; mov.b32 %f1, %i1; mov.b32 %f3, %i2; {o} %f2, %f0, %f1, %f3; mov.b32 %o0, %f2;'


def hbin(o):   # f16x2 a op b
    return f'mov.b32 %x0, %i0; mov.b32 %x1, %i1; {o} %x2, %x0, %x1; mov.b32 %o0, %x2;'


def hter(o):
    return f'mov.b32 %x0, %i0; mov.b32 %x1, %i1; mov.b32 %x3, %i2; {o} %x2, %x0, %x1, %x3; mov.b32 %o0, %x2;'


# --- f32 arithmetic (FADD / FMUL / FFMA / FMNMX), every rounding mode, ftz and sat
for rnd in ('rn', 'rz', 'rm', 'rp'):
    op(f'fadd_{rnd}', 'f32 arith', 'ff', 'f', fbin(f'add.{rnd}.f32'))
    op(f'fmul_{rnd}', 'f32 arith', 'ff', 'f', fbin(f'mul.{rnd}.f32'))
    op(f'ffma_{rnd}', 'f32 arith', 'fff', 'f', fter(f'fma.{rnd}.f32'))
op('fadd_ftz', 'f32 arith', 'ff', 'f', fbin('add.rn.ftz.f32'))
op('fadd_sat', 'f32 arith', 'ff', 'f', fbin('add.rn.sat.f32'))
op('fsub', 'f32 arith', 'ff', 'f', fbin('sub.rn.f32'))
op('fmul_ftz', 'f32 arith', 'ff', 'f', fbin('mul.rn.ftz.f32'))
op('fmul_sat', 'f32 arith', 'ff', 'f', fbin('mul.rn.sat.f32'))
op('ffma_ftz', 'f32 arith', 'fff', 'f', fter('fma.rn.ftz.f32'))
op('ffma_sat', 'f32 arith', 'fff', 'f', fter('fma.rn.sat.f32'))
op('ffma_ftz_sat', 'f32 arith', 'fff', 'f', fter('fma.rn.ftz.sat.f32'))
op('fadd_abs_neg', 'f32 arith', 'ff', 'f', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; abs.f32 %f2, %f0; neg.f32 %f3, %f1; add.rn.f32 %f4, %f2, %f3; mov.b32 %o0, %f4;')
op('fmul_by_imm', 'f32 arith', 'f', 'f', 'mov.b32 %f0, %i0; mul.rn.f32 %f2, %f0, 0f3EAAAAAB; mov.b32 %o0, %f2;')
op('fmin', 'f32 arith', 'ff', 'f', fbin('min.f32'))
op('fmax', 'f32 arith', 'ff', 'f', fbin('max.f32'))
op('fmin_ftz', 'f32 arith', 'ff', 'f', fbin('min.ftz.f32'))
op('fmax_nan', 'f32 arith', 'ff', 'f', fbin('max.NaN.f32'))
op('fmin_nan', 'f32 arith', 'ff', 'f', fbin('min.NaN.f32'))
op('fcopysign', 'f32 arith', 'ff', 'f', fbin('copysign.f32'))
op('fabs', 'f32 arith', 'f', 'f', fun('abs.f32'))
op('fneg', 'f32 arith', 'f', 'f', fun('neg.f32'))

# --- f32 compares (FSETP / FSET), many predicates in one kernel
CMPS = ('eq', 'ne', 'lt', 'le', 'gt', 'ge', 'equ', 'neu', 'ltu', 'leu', 'gtu', 'geu', 'num', 'nan')
op('fsetp', 'f32 compare', 'ff', 'u' * len(CMPS), 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; ' + ' '.join(
    f'setp.{c}.f32 %p0, %f0, %f1; selp.u32 %o{k}, 1, 0, %p0;' for k, c in enumerate(CMPS)))
op('fsetp_ftz', 'f32 compare', 'ff', 'uuuu', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; ' + ' '.join(
    f'setp.{c}.ftz.f32 %p0, %f0, %f1; selp.u32 %o{k}, 1, 0, %p0;' for k, c in enumerate(('eq', 'lt', 'gtu', 'ne'))))
op('fset_bf', 'f32 compare', 'ff', 'ff', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; set.lt.f32.f32 %f2, %f0, %f1; set.neu.f32.f32 %f3, %f0, %f1; mov.b32 %o0, %f2; mov.b32 %o1, %f3;')
op('fset_u32', 'f32 compare', 'ff', 'uu', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; set.ge.u32.f32 %o0, %f0, %f1; set.nan.u32.f32 %o1, %f0, %f1;')
op('ftestp', 'f32 compare', 'f', 'uuuuu', 'mov.b32 %f0, %i0; ' + ' '.join(
    f'testp.{t}.f32 %p0, %f0; selp.u32 %o{k}, 1, 0, %p0;' for k, t in enumerate(('finite', 'infinite', 'number', 'normal', 'subnormal'))))
op('fsel_cmp', 'f32 compare', 'fff', 'f', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; mov.b32 %f3, %i2; setp.gt.f32 %p0, %f0, 0f00000000; selp.f32 %f2, %f1, %f3, %p0; mov.b32 %o0, %f2;')

# --- MUFU and the precise sequences built on it
for o in ('ex2.approx.f32', 'ex2.approx.ftz.f32', 'lg2.approx.f32', 'lg2.approx.ftz.f32', 'rcp.approx.f32', 'rcp.approx.ftz.f32', 'rsqrt.approx.f32',
          'rsqrt.approx.ftz.f32', 'sqrt.approx.f32', 'sqrt.approx.ftz.f32', 'sin.approx.f32', 'cos.approx.f32', 'tanh.approx.f32'):
    op('mufu_' + o.replace('.approx', '').replace('.f32', '').replace('.', '_'), 'mufu', 'f', 'f', fun(o))
op('mufu_ex2_f16x2', 'mufu', 'h', 'h', 'mov.b32 %x0, %i0; ex2.approx.f16x2 %x2, %x0; mov.b32 %o0, %x2;')
op('mufu_tanh_f16x2', 'mufu', 'h', 'h', 'mov.b32 %x0, %i0; tanh.approx.f16x2 %x2, %x0; mov.b32 %o0, %x2;')
op('fdiv_rn', 'f32 precise', 'ff', 'f', fbin('div.rn.f32'))
op('fdiv_full', 'f32 precise', 'ff', 'f', fbin('div.full.f32'))
op('fdiv_approx', 'f32 precise', 'ff', 'f', fbin('div.approx.f32'))
op('frcp_rn', 'f32 precise', 'f', 'f', fun('rcp.rn.f32'))
op('fsqrt_rn', 'f32 precise', 'f', 'f', fun('sqrt.rn.f32'))

# --- conversions (F2I, I2F, F2F, FRND, F2FP, I2I)
for rnd in ('rni', 'rzi', 'rmi', 'rpi'):
    op(f'f2i_s32_{rnd}', 'convert', 'f', 'u', f'mov.b32 %f0, %i0; cvt.{rnd}.s32.f32 %o0, %f0;')
    op(f'frnd_{rnd}', 'convert', 'f', 'f', f'mov.b32 %f0, %i0; cvt.{rnd}.f32.f32 %f2, %f0; mov.b32 %o0, %f2;')
op('f2i_u32_rzi', 'convert', 'f', 'u', 'mov.b32 %f0, %i0; cvt.rzi.u32.f32 %o0, %f0;')
op('f2i_s32_ftz', 'convert', 'f', 'u', 'mov.b32 %f0, %i0; cvt.rzi.ftz.s32.f32 %o0, %f0;')
op('f2i_s16_sat', 'convert', 'f', 'u', 'mov.b32 %f0, %i0; cvt.rzi.sat.s16.f32 %w0, %f0; cvt.s32.s16 %o0, %w0;')
op('f2i_u8_sat', 'convert', 'f', 'u', 'mov.b32 %f0, %i0; cvt.rni.sat.u8.f32 %w0, %f0; cvt.u32.u8 %o0, %w0;')
op('f2i_s64', 'convert', 'f', 'uu', 'mov.b32 %f0, %i0; cvt.rzi.s64.f32 %d0, %f0; mov.b64 {%o0, %o1}, %d0;')
for rnd in ('rn', 'rz', 'rm', 'rp'):
    op(f'i2f_s32_{rnd}', 'convert', 'u', 'f', f'cvt.{rnd}.f32.s32 %f2, %i0; mov.b32 %o0, %f2;')
op('i2f_u32_rn', 'convert', 'u', 'f', 'cvt.rn.f32.u32 %f2, %i0; mov.b32 %o0, %f2;')
op('i2f_s16', 'convert', 'u', 'f', 'cvt.u16.u32 %w0, %i0; cvt.rn.f32.s16 %f2, %w0; mov.b32 %o0, %f2;')
op('i2f_s64', 'convert', 'uu', 'f', 'mov.b64 %d0, {%i0, %i1}; cvt.rn.f32.s64 %f2, %d0; mov.b32 %o0, %f2;')
op('f2f_f16_rn', 'convert', 'f', 'h', 'mov.b32 %f0, %i0; cvt.rn.f16.f32 %y0, %f0; mov.b16 %w0, %y0; cvt.u32.u16 %o0, %w0;')
op('f2f_f16_rz', 'convert', 'f', 'h', 'mov.b32 %f0, %i0; cvt.rz.f16.f32 %y0, %f0; mov.b16 %w0, %y0; cvt.u32.u16 %o0, %w0;')
op('f2f_f32_from_f16', 'convert', 'h', 'ff', 'mov.b32 {%y0, %y1}, %i0; cvt.f32.f16 %f2, %y0; cvt.f32.f16 %f3, %y1; mov.b32 %o0, %f2; mov.b32 %o1, %f3;')
op('f2fp_pack_f16x2', 'convert', 'ff', 'h', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; cvt.rn.f16x2.f32 %x2, %f0, %f1; mov.b32 %o0, %x2;')
op('f2fp_pack_f16x2_relu', 'convert', 'ff', 'h', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; cvt.rn.relu.f16x2.f32 %x2, %f0, %f1; mov.b32 %o0, %x2;')
op('f2fp_pack_bf16x2', 'convert', 'ff', 'b', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; cvt.rn.bf16x2.f32 %x2, %f0, %f1; mov.b32 %o0, %x2;')
op('f2f_tf32', 'convert', 'f', 'u', 'mov.b32 %f0, %i0; cvt.rna.tf32.f32 %o0, %f0;')
# directed roundings to f16 and the f16x2 pack's .RZ (PR #1800 fixes these; recorded so its claims can be checked on the GPU)
for rnd in ('rz', 'rm', 'rp'):
    op(f'i2f_f16_{rnd}', 'convert', 'u', 'h', f'cvt.{rnd}.f16.s32 %w0, %i0; cvt.u32.u16 %o0, %w0;')
    op(f'f2f_f16_{rnd}x', 'convert', 'f', 'h', f'mov.b32 %f0, %i0; cvt.{rnd}.f16.f32 %w0, %f0; cvt.u32.u16 %o0, %w0;')
op('i2f_f16_u32_rz', 'convert', 'u', 'h', 'cvt.rz.f16.u32 %w0, %i0; cvt.u32.u16 %o0, %w0;')
op('f2fp_pack_f16x2_rz', 'convert', 'ff', 'h', 'mov.b32 %f0, %i0; mov.b32 %f1, %i1; cvt.rz.f16x2.f32 %x2, %f0, %f1; mov.b32 %o0, %x2;')
op('i2i_s8_sat', 'convert', 'u', 'u', 'cvt.sat.s8.s32 %w0, %i0; cvt.s32.s8 %o0, %w0;')
op('i2i_u16_sat', 'convert', 'u', 'u', 'cvt.sat.u16.s32 %w0, %i0; cvt.u32.u16 %o0, %w0;')
op('sext_s8', 'convert', 'u', 'u', 'cvt.s32.s8 %o0, %i0;')

# --- f16x2 / bf16x2 (HADD2 / HMUL2 / HFMA2 / HMNMX2 / HSETP2 / HSET2)
op('hadd2', 'f16x2', 'hh', 'h', hbin('add.rn.f16x2'))
op('hadd2_ftz', 'f16x2', 'hh', 'h', hbin('add.rn.ftz.f16x2'))
op('hadd2_sat', 'f16x2', 'hh', 'h', hbin('add.rn.sat.f16x2'))
op('hsub2', 'f16x2', 'hh', 'h', hbin('sub.rn.f16x2'))
op('hmul2', 'f16x2', 'hh', 'h', hbin('mul.rn.f16x2'))
op('hmul2_ftz', 'f16x2', 'hh', 'h', hbin('mul.rn.ftz.f16x2'))
op('hfma2', 'f16x2', 'hhh', 'h', hter('fma.rn.f16x2'))
op('hfma2_ftz', 'f16x2', 'hhh', 'h', hter('fma.rn.ftz.f16x2'))
op('hfma2_sat', 'f16x2', 'hhh', 'h', hter('fma.rn.sat.f16x2'))
op('hfma2_relu', 'f16x2', 'hhh', 'h', hter('fma.rn.relu.f16x2'))
op('hmin2', 'f16x2', 'hh', 'h', hbin('min.f16x2'))
op('hmax2', 'f16x2', 'hh', 'h', hbin('max.f16x2'))
op('hmin2_nan', 'f16x2', 'hh', 'h', hbin('min.NaN.f16x2'))
op('hneg2', 'f16x2', 'h', 'h', 'mov.b32 %x0, %i0; neg.f16x2 %x2, %x0; mov.b32 %o0, %x2;')
op('habs2', 'f16x2', 'h', 'h', 'mov.b32 %x0, %i0; abs.f16x2 %x2, %x0; mov.b32 %o0, %x2;')
op('hadd2_imm', 'f16x2', 'h', 'h', 'mov.b32 %x0, %i0; mov.b32 %x1, 0x3C00BC00; add.rn.f16x2 %x2, %x0, %x1; mov.b32 %o0, %x2;')
op('hadd_f16', 'f16x2', 'hh', 'hh', 'mov.b32 {%y0, %y1}, %i0; mov.b32 {%y2, %y3}, %i1; add.rn.f16 %y4, %y0, %y2; mul.rn.f16 %y5, %y1, %y3; mov.b16 %w0, %y4; cvt.u32.u16 %o0, %w0; mov.b16 %w1, %y5; cvt.u32.u16 %o1, %w1;')
op('hsetp2', 'f16x2', 'hh', 'uuuuuuuu', 'mov.b32 %x0, %i0; mov.b32 %x1, %i1; ' + ' '.join(
    f'setp.{c}.f16x2 %p0|%p1, %x0, %x1; selp.u32 %o{2*k}, 1, 0, %p0; selp.u32 %o{2*k+1}, 1, 0, %p1;' for k, c in enumerate(('lt', 'eq', 'gtu', 'nan'))))
op('hset2', 'f16x2', 'hh', 'hu', 'mov.b32 %x0, %i0; mov.b32 %x1, %i1; set.lt.f16x2.f16x2 %x2, %x0, %x1; mov.b32 %o0, %x2; set.geu.u32.f16x2 %o1, %x0, %x1;')
op('bfma2', 'bf16x2', 'bbb', 'b', hter('fma.rn.bf16x2'))
op('bmin2', 'bf16x2', 'bb', 'b', hbin('min.bf16x2'))
op('bmax2_nan', 'bf16x2', 'bb', 'b', hbin('max.NaN.bf16x2'))

# --- integer ALU
op('iadd', 'int', 'uu', 'u', 'add.u32 %o0, %i0, %i1;')
op('iadd3', 'int', 'uuu', 'u', 'add.u32 %t0, %i0, %i1; sub.u32 %o0, %t0, %i2;')
op('iadd_sat', 'int', 'uu', 'u', 'add.sat.s32 %o0, %i0, %i1;')
op('isub_sat', 'int', 'uu', 'u', 'sub.sat.s32 %o0, %i0, %i1;')
op('iadd64', 'int', 'uuuu', 'uu', 'mov.b64 %d0, {%i0, %i1}; mov.b64 %d1, {%i2, %i3}; add.u64 %d2, %d0, %d1; mov.b64 {%o0, %o1}, %d2;')
op('isub64', 'int', 'uuuu', 'uu', 'mov.b64 %d0, {%i0, %i1}; mov.b64 %d1, {%i2, %i3}; sub.u64 %d2, %d0, %d1; mov.b64 {%o0, %o1}, %d2;')
op('imul_lo', 'int', 'uu', 'u', 'mul.lo.u32 %o0, %i0, %i1;')
op('imul_hi_u32', 'int', 'uu', 'u', 'mul.hi.u32 %o0, %i0, %i1;')
op('imul_hi_s32', 'int', 'uu', 'u', 'mul.hi.s32 %o0, %i0, %i1;')
op('imul_wide_s32', 'int', 'uu', 'uu', 'mul.wide.s32 %d0, %i0, %i1; mov.b64 {%o0, %o1}, %d0;')
op('imul_wide_u32', 'int', 'uu', 'uu', 'mul.wide.u32 %d0, %i0, %i1; mov.b64 {%o0, %o1}, %d0;')
op('imad_lo', 'int', 'uuu', 'u', 'mad.lo.u32 %o0, %i0, %i1, %i2;')
op('imad_hi_s32', 'int', 'uuu', 'u', 'mad.hi.s32 %o0, %i0, %i1, %i2;')
op('imul64', 'int', 'uuuu', 'uu', 'mov.b64 %d0, {%i0, %i1}; mov.b64 %d1, {%i2, %i3}; mul.lo.u64 %d2, %d0, %d1; mov.b64 {%o0, %o1}, %d2;')
op('imul24', 'int', 'uu', 'u', 'mul24.lo.s32 %o0, %i0, %i1;')
op('imin_s32', 'int', 'uu', 'u', 'min.s32 %o0, %i0, %i1;')
op('imax_u32', 'int', 'uu', 'u', 'max.u32 %o0, %i0, %i1;')
op('imin_s64', 'int', 'uuuu', 'uu', 'mov.b64 %d0, {%i0, %i1}; mov.b64 %d1, {%i2, %i3}; min.s64 %d2, %d0, %d1; mov.b64 {%o0, %o1}, %d2;')
op('iabs', 'int', 'u', 'u', 'abs.s32 %o0, %i0;')
op('ineg', 'int', 'u', 'u', 'neg.s32 %o0, %i0;')
op('isad', 'int', 'uuu', 'u', 'sad.u32 %o0, %i0, %i1, %i2;')
op('idiv_u32', 'int div', 'uu', 'u', 'div.u32 %o0, %i0, %i1;')
op('irem_u32', 'int div', 'uu', 'u', 'rem.u32 %o0, %i0, %i1;')
op('idiv_s32', 'int div', 'uu', 'u', 'div.s32 %o0, %i0, %i1;')
op('irem_s32', 'int div', 'uu', 'u', 'rem.s32 %o0, %i0, %i1;')
op('idiv_u32_by_7', 'int div', 'u', 'u', 'div.u32 %o0, %i0, 7;')
op('isetp', 'int compare', 'uu', 'uuuuuu', ' '.join(
    f'setp.{c} %p0, %i0, %i1; selp.u32 %o{k}, 1, 0, %p0;' for k, c in enumerate(('lt.s32', 'lt.u32', 'ge.s32', 'hi.u32', 'eq.s32', 'ne.u32'))))
op('isetp_and', 'int compare', 'uuuu', 'uu', 'setp.lt.s32 %p1, %i2, %i3; setp.lt.and.s32 %p0, %i0, %i1, %p1; selp.u32 %o0, 1, 0, %p0; '
   'setp.ge.or.u32 %p2, %i0, %i1, %p1; selp.u32 %o1, 1, 0, %p2;')
op('isetp64', 'int compare', 'uuuu', 'uu', 'mov.b64 %d0, {%i0, %i1}; mov.b64 %d1, {%i2, %i3}; setp.lt.s64 %p0, %d0, %d1; selp.u32 %o0, 1, 0, %p0; '
   'setp.lt.u64 %p1, %d0, %d1; selp.u32 %o1, 1, 0, %p1;')
op('plop3', 'int compare', 'uuu', 'u', 'setp.ne.u32 %p0, %i0, 0; setp.ne.u32 %p1, %i1, 0; setp.ne.u32 %p2, %i2, 0; xor.pred %p3, %p0, %p1; or.pred %p3, %p3, %p2; '
   'not.pred %p3, %p3; selp.u32 %o0, 1, 0, %p3;')

# --- bits
op('shl', 'bits', 'us', 'u', 'shl.b32 %o0, %i0, %i1;')
op('shr_u32', 'bits', 'us', 'u', 'shr.u32 %o0, %i0, %i1;')
op('shr_s32', 'bits', 'us', 'u', 'shr.s32 %o0, %i0, %i1;')
op('shl_imm', 'bits', 'u', 'uu', 'shl.b32 %o0, %i0, 7; shr.s32 %o1, %i0, 13;')
op('shf_l_wrap', 'bits', 'uus', 'u', 'shf.l.wrap.b32 %o0, %i0, %i1, %i2;')
op('shf_l_clamp', 'bits', 'uus', 'u', 'shf.l.clamp.b32 %o0, %i0, %i1, %i2;')
op('shf_r_wrap', 'bits', 'uus', 'u', 'shf.r.wrap.b32 %o0, %i0, %i1, %i2;')
op('shf_r_clamp', 'bits', 'uus', 'u', 'shf.r.clamp.b32 %o0, %i0, %i1, %i2;')
op('shl64', 'bits', 'uus', 'uu', 'mov.b64 %d0, {%i0, %i1}; shl.b64 %d1, %d0, %i2; mov.b64 {%o0, %o1}, %d1;')
op('shr64_u', 'bits', 'uus', 'uu', 'mov.b64 %d0, {%i0, %i1}; shr.u64 %d1, %d0, %i2; mov.b64 {%o0, %o1}, %d1;')
op('shr64_s', 'bits', 'uus', 'uu', 'mov.b64 %d0, {%i0, %i1}; shr.s64 %d1, %d0, %i2; mov.b64 {%o0, %o1}, %d1;')
LUTS = (0x96, 0xe8, 0xca, 0x01, 0x80, 0x6a, 0xf8, 0x1e, 0xb4, 0x3c)
op('lop3', 'bits', 'uuu', 'u' * len(LUTS), ' '.join(f'lop3.b32 %o{k}, %i0, %i1, %i2, {hex(l)};' for k, l in enumerate(LUTS)))
op('logic', 'bits', 'uu', 'uuuu', 'and.b32 %o0, %i0, %i1; or.b32 %o1, %i0, %i1; xor.b32 %o2, %i0, %i1; not.b32 %t0, %i0; and.b32 %o3, %t0, %i1;')
PRMT = ('', '.f4e', '.b4e', '.rc8', '.ecl', '.ecr', '.rc16')
op('prmt', 'bits', 'uuu', 'u' * len(PRMT), ' '.join(f'prmt.b32{m} %o{k}, %i0, %i1, %i2;' for k, m in enumerate(PRMT)))
op('prmt_imm', 'bits', 'uu', 'uuu', 'prmt.b32 %o0, %i0, %i1, 0x3210; prmt.b32 %o1, %i0, %i1, 0x5140; prmt.b32 %o2, %i0, %i1, 0x8880;')
op('bfe_u32', 'bits', 'uss', 'u', 'bfe.u32 %o0, %i0, %i1, %i2;')
op('bfe_s32', 'bits', 'uss', 'u', 'bfe.s32 %o0, %i0, %i1, %i2;')
op('bfi', 'bits', 'uuss', 'u', 'bfi.b32 %o0, %i0, %i1, %i2, %i3;')
op('bmsk_clamp', 'bits', 'ss', 'u', 'bmsk.clamp.b32 %o0, %i0, %i1;')
op('bmsk_wrap', 'bits', 'ss', 'u', 'bmsk.wrap.b32 %o0, %i0, %i1;')
op('szext_s', 'bits', 'us', 'u', 'szext.clamp.s32 %o0, %i0, %i1;')
op('popc', 'bits', 'u', 'u', 'popc.b32 %o0, %i0;')
op('popc64', 'bits', 'uu', 'u', 'mov.b64 %d0, {%i0, %i1}; popc.b64 %o0, %d0;')
op('clz', 'bits', 'u', 'u', 'clz.b32 %o0, %i0;')
op('clz64', 'bits', 'uu', 'u', 'mov.b64 %d0, {%i0, %i1}; clz.b64 %o0, %d0;')
op('bfind_u32', 'bits', 'u', 'u', 'bfind.u32 %o0, %i0;')
op('bfind_s32', 'bits', 'u', 'u', 'bfind.s32 %o0, %i0;')
op('bfind_shiftamt', 'bits', 'u', 'u', 'bfind.shiftamt.u32 %o0, %i0;')
op('brev', 'bits', 'u', 'u', 'brev.b32 %o0, %i0;')
op('fns', 'bits', 'uus', 'u', 'and.b32 %t0, %i2, 31; fns.b32 %o0, %i0, %i1, %t0;')
op('dp4a_u32', 'bits', 'uuu', 'u', 'dp4a.u32.u32 %o0, %i0, %i1, %i2;')
op('dp4a_s32', 'bits', 'uuu', 'u', 'dp4a.s32.s32 %o0, %i0, %i1, %i2;')
op('dp4a_su', 'bits', 'uuu', 'u', 'dp4a.s32.u32 %o0, %i0, %i1, %i2;')
op('dp2a_lo', 'bits', 'uuu', 'u', 'dp2a.lo.s32.s32 %o0, %i0, %i1, %i2;')
op('dp2a_hi', 'bits', 'uuu', 'u', 'dp2a.hi.u32.u32 %o0, %i0, %i1, %i2;')

# --- warp (SHFL / VOTE / MATCH / REDUX): 32 threads to a group on both sides, so a lane's neighbours are the same
op('shfl_idx', 'warp', 'ul', 'u', 'shfl.sync.idx.b32 %o0, %i0, %i1, 0x1f, 0xffffffff;')
op('shfl_up', 'warp', 'us', 'uu', 'and.b32 %t0, %i1, 31; shfl.sync.up.b32 %o0|%p0, %i0, %t0, 0, 0xffffffff; selp.u32 %o1, 1, 0, %p0;')
op('shfl_down', 'warp', 'us', 'uu', 'and.b32 %t0, %i1, 31; shfl.sync.down.b32 %o0|%p0, %i0, %t0, 0x1f, 0xffffffff; selp.u32 %o1, 1, 0, %p0;')
op('shfl_up_clamp', 'warp', 'us', 'uu', 'and.b32 %t0, %i1, 31; shfl.sync.up.b32 %o0|%p0, %i0, %t0, 0x10, 0xffffffff; selp.u32 %o1, 1, 0, %p0;')
op('shfl_up_seg', 'warp', 'us', 'uu', 'and.b32 %t0, %i1, 31; shfl.sync.up.b32 %o0|%p0, %i0, %t0, 0x1808, 0xffffffff; selp.u32 %o1, 1, 0, %p0;')
op('shfl_bfly', 'warp', 'ul', 'u', 'shfl.sync.bfly.b32 %o0, %i0, %i1, 0x1f, 0xffffffff;')
op('shfl_bfly_imm', 'warp', 'u', 'u', 'shfl.sync.bfly.b32 %o0, %i0, 1, 0x1f, 0xffffffff;')
op('shfl_idx_seg', 'warp', 'ul', 'u', 'shfl.sync.idx.b32 %o0, %i0, %i1, 0x1807, 0xffffffff;')
op('vote', 'warp', 'p', 'uuuu', 'and.b32 %t0, %i0, 1; setp.ne.u32 %p0, %t0, 0; vote.sync.ballot.b32 %o0, %p0, 0xffffffff; vote.sync.any.pred %p1, %p0, 0xffffffff; '
   'selp.u32 %o1, 1, 0, %p1; vote.sync.all.pred %p2, %p0, 0xffffffff; selp.u32 %o2, 1, 0, %p2; vote.sync.uni.pred %p3, %p0, 0xffffffff; selp.u32 %o3, 1, 0, %p3;')
op('activemask', 'warp', 'p', 'u', 'activemask.b32 %o0;')
op('match_any', 'warp', 'p', 'u', 'and.b32 %t0, %i0, 3; match.any.sync.b32 %o0, %t0, 0xffffffff;')
op('redux_add', 'warp', 'u', 'u', 'redux.sync.add.u32 %o0, %i0, 0xffffffff;')
op('redux_min_s32', 'warp', 'u', 'u', 'redux.sync.min.s32 %o0, %i0, 0xffffffff;')
op('redux_or', 'warp', 'u', 'u', 'redux.sync.or.b32 %o0, %i0, 0xffffffff;')

# --- tensor (HMMA / IMMA): each thread holds its fragments, as the PTX layouts give them
op('hmma_16816_f16', 'tensor', 'HHHHHHHH', 'hh', 'mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%o0, %o1}, {%i0, %i1, %i2, %i3}, {%i4, %i5}, {%i6, %i7};')
op('hmma_16816_f32', 'tensor', 'HHHHHHFFFF', 'ffff', 'mov.b32 %f0, %i6; mov.b32 %f1, %i7; mov.b32 %f2, %i8; mov.b32 %f3, %i9; '
   'mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f4, %f5, %f6, %f7}, {%i0, %i1, %i2, %i3}, {%i4, %i5}, {%f0, %f1, %f2, %f3}; '
   'mov.b32 %o0, %f4; mov.b32 %o1, %f5; mov.b32 %o2, %f6; mov.b32 %o3, %f7;')
op('hmma_1688_f16', 'tensor', 'HHHHH', 'hh', 'mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 {%o0, %o1}, {%i0, %i1}, {%i2}, {%i3, %i4};')
op('hmma_16816_bf16', 'tensor', 'BBBBBBFFFF', 'ffff', 'mov.b32 %f0, %i6; mov.b32 %f1, %i7; mov.b32 %f2, %i8; mov.b32 %f3, %i9; '
   'mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {%f4, %f5, %f6, %f7}, {%i0, %i1, %i2, %i3}, {%i4, %i5}, {%f0, %f1, %f2, %f3}; '
   'mov.b32 %o0, %f4; mov.b32 %o1, %f5; mov.b32 %o2, %f6; mov.b32 %o3, %f7;')
op('hmma_1688_tf32', 'tensor', 'FFFFFFFFFF', 'ffff', 'mov.b32 %f0, %i6; mov.b32 %f1, %i7; mov.b32 %f2, %i8; mov.b32 %f3, %i9; '
   'mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32 {%f4, %f5, %f6, %f7}, {%i0, %i1, %i2, %i3}, {%i4, %i5}, {%f0, %f1, %f2, %f3}; '
   'mov.b32 %o0, %f4; mov.b32 %o1, %f5; mov.b32 %o2, %f6; mov.b32 %o3, %f7;')
op('imma_16832_s8', 'tensor', 'UUUUUUUUUU', 'uuuu', 'mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 {%o0, %o1, %o2, %o3}, {%i0, %i1, %i2, %i3}, {%i4, %i5}, {%i6, %i7, %i8, %i9};')

# --- f64 (DADD / DMUL / DFMA): rare in shaders, here for the coverage list
op('dadd', 'f64', 'uuuu', 'uu', 'mov.b64 %g0, {%i0, %i1}; mov.b64 %g1, {%i2, %i3}; add.rn.f64 %g2, %g0, %g1; mov.b64 {%o0, %o1}, %g2;')
op('dmul', 'f64', 'uuuu', 'uu', 'mov.b64 %g0, {%i0, %i1}; mov.b64 %g1, {%i2, %i3}; mul.rn.f64 %g2, %g0, %g1; mov.b64 {%o0, %o1}, %g2;')
