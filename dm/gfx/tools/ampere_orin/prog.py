"""The Metal side's program, made from the very SASS the Orin ran (stdlib only, so the Jetson can run it and disassemble the result).

The op's kernel is k(in, out): it loads its NIN input words from in[gid * NIN + k] (LDG), computes, and stores NOUT words to out[gid * NOUT + k] (STG).
On the Metal side the translator gets the same instructions, where:
  - two instructions go first: S2R Rb = SR_TID.X; IMAD Rb = Rb * W * 4 (W = NIN + NOUT words a thread, Rb a register the kernel does not use)
  - each LDG Rd, [Ra + 4k] becomes LDS Rd, [Rb + 4k]; each STG [Ra + 4k], Rs becomes STS [Rb + 4 (NIN + k)], Rs (same guard and control bits)
  - each instruction that reads the CUDA parameter bank c[0x0] (the stack pointer, the global window, the two pointers) becomes a NOP
so a thread's words lie at shared memory word tid * W: its inputs, then its outputs. Branches are relative, so the two inserted instructions move nothing."""
import re
import struct

CTRL = ~((1 << 41) - 1) & (2 ** 64 - 1)   # the control bits (stall, yield, barriers, wait mask, reuse): bits 105..127 of the instruction, 41..63 of its high word
WAIT_ALL = 0x3f << 52


def text_section(cubin, kernel='k'):
    shoff, = struct.unpack_from('<Q', cubin, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', cubin, 0x3a)
    secs = [struct.unpack_from('<IIQQQQIIQQ', cubin, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    for s in secs:
        name = cubin[stroff + s[0]:cubin.index(b'\0', stroff + s[0])].decode()
        if name == '.text.' + kernel:
            return cubin[s[4]:s[4] + s[5]]
    raise ValueError('no .text.' + kernel)


def sass_lines(sass):
    """offset -> instruction text, from nvdisasm -hex"""
    out = {}
    for m in re.finditer(r'/\*([0-9a-f]{4,})\*/\s+(.*?)\s*;\s*/\* 0x', sass):
        out[int(m.group(1), 16)] = m.group(2)
    return out


def rewrite(cubin, sass, nin, nout):
    code = text_section(cubin)
    text = sass_lines(sass)
    insts = [struct.unpack_from('<QQ', code, i) for i in range(0, len(code), 16)]   # (lo, hi)
    regs = [int(r) for t in text.values() for r in re.findall(r'\bR(\d+)\b', t)]
    rb = max(regs + [0]) + 1
    if rb >= 255:
        raise ValueError('no free register')
    w = nin + nout
    out, notes, seen_in, seen_out = [], [], set(), set()
    # The driver's JIT starts every kernel with a one-time convergence barrier set-up: ISETP P0 on UR2; @P0 BRA past it; BMOV B0..B15. Offline-compiled
    # shaders (the games') have none, so it goes, up to the branch's target
    skip = 0
    if text.get(0, '').startswith('ISETP.NE.U32.AND P0, PT, RZ, UR2') and text.get(16, '').startswith('@P0 BRA'):
        rel = (insts[1][0] >> 32) & 0xffffffff   # BRA's offset from the next instruction, in bytes
        skip = (32 + rel) // 16
        notes.append(f'nop 0x0000..{skip * 16:#06x}: the JIT\'s barrier set-up')
    dead = address_only(insts, text, skip)
    for n in sorted(dead):
        notes.append(f'nop {n * 16:#06x} (feeds only the removed addressing): {text.get(n * 16, "")}')
    out.append((0x0000000000007919 | rb << 16, 0x000e280000002100))                                  # S2R Rb, SR_TID.X (write barrier 0)
    out.append((0x0000000000007824 | rb << 16 | rb << 24 | (w * 4) << 32, 0x000fe200078e00ff | WAIT_ALL))   # IMAD Rb, Rb, W*4, RZ
    for n, (lo, hi) in enumerate(insts):
        t = text.get(n * 16, '')
        opc = lo & 0xfff
        guard = lo & 0xf000
        off = (lo >> 40) & 0xffffff
        if n < skip:
            out.append((0x7918, hi & CTRL))
        elif opc == 0x981:   # LDG
            if not re.match(r'(@!?U?P\w+ )?LDG\.E(\.STRONG\.\w+)? R\d+, \[R\d+\.64(\+0x[0-9a-f]+)?\]', t):
                raise ValueError(f'unexpected load: {t}')
            k = off // 4
            if off % 4 or not 0 <= k < nin:
                raise ValueError(f'load offset {off:#x}: {t}')
            seen_in.add(k)
            rd = (lo >> 16) & 0xff
            out.append((0x984 | guard | rd << 16 | rb << 24 | (k * 4) << 40, 0x0800 | (hi & CTRL)))
        elif opc == 0x986:   # STG
            if not re.match(r'(@!?U?P\w+ )?STG\.E(\.STRONG\.\w+)? \[R\d+\.64(\+0x[0-9a-f]+)?\], R\d+', t):
                raise ValueError(f'unexpected store: {t}')
            k = off // 4
            if off % 4 or not 0 <= k < nout:
                raise ValueError(f'store offset {off:#x}: {t}')
            seen_out.add(k)
            rs = (lo >> 32) & 0xff
            out.append((0x388 | guard | rb << 24 | rs << 32 | ((nin + k) * 4) << 40, 0x0800 | (hi & CTRL)))
        elif n in dead:
            out.append((0x7918, hi & CTRL))
        else:
            if re.search(r'c\[0x[1-9a-f]\]', t):
                notes.append(f'constant bank read kept {n * 16:#06x}: {t}')
            out.append((lo, hi))
    if seen_in != set(range(nin)) or seen_out != set(range(nout)):
        raise ValueError(f'loads {sorted(seen_in)} of {nin}, stores {sorted(seen_out)} of {nout}')
    return b''.join(struct.pack('<QQ', lo, hi) for lo, hi in out), rb, notes


NO_DEST = re.compile(r'^(ST[GSL]|RED|BAR|BRA|BRX|EXIT|RET|CALL|JMP|NOP|BSSY|BSYNC|WARPSYNC|MEMBAR|ERRBAR|DEPBAR|BMOV|YIELD|ATOM)')
FLOW = re.compile(r'^(BRA|BRX|CALL|RET|JMP|EXIT|BSSY|BSYNC|WARPSYNC)')


def regs_of(op):
    """the general registers an operand names (R4.64 names R4 and R5); RZ is none"""
    out = set()
    for r, wide in re.findall(r'\bR(\d+)(\.64)?\b', op):
        out.add(int(r))
        if wide:
            out.add(int(r) + 1)
    return out


def parse(t):
    """(mnemonic, guarded, written registers, read registers) of an instruction's text"""
    guarded = t.startswith('@')
    body = t.split(' ', 1)[1] if guarded else t
    mn, _, rest = body.partition(' ')
    ops = [o.strip() for o in re.split(r',(?![^\[]*\])', rest)] if rest else []
    if not ops or NO_DEST.match(mn) or not re.match(r'^-?R\d+', ops[0]):
        return mn, guarded, set(), set().union(*map(regs_of, ops)) if ops else set()
    w = regs_of(ops[0])
    if '.WIDE' in mn or mn.startswith(('LDG.E.64', 'LDS.64')):
        w |= {min(w) + 1}
    return mn, guarded, w, set().union(*map(regs_of, ops[1:])) if ops[1:] else set()


def address_only(insts, text, skip):
    """the instructions to drop: those that read c[0x0] (the CUDA parameters), then, to a fixed point, those whose every result is read only by dropped
    instructions or as an LDG / STG address (which the rewrite replaces), so the translator sees no more than the op itself"""
    n_all = len(insts)
    info = []
    for n in range(n_all):
        t = text.get(n * 16, '')
        mn, guarded, w, r = parse(t)
        if mn.startswith(('LDG', 'STG')):
            # an LDG's or STG's address is not read once it is a shared memory access; an STG still reads its data
            ops = [o.strip() for o in re.split(r',(?![^\[]*\])', t.split(' ', 2)[-1])]
            r = set().union(*[regs_of(o) for o in ops if not o.startswith('[')]) if mn.startswith('STG') else set()
            w = regs_of(ops[0]) if mn.startswith('LDG') else set()
        info.append((t, mn, guarded, w, r))
    dead = {n for n in range(skip, n_all) if 'c[0x0]' in info[n][0]}
    changed = True
    while changed:
        changed = False
        for i in range(skip, n_all):
            t, mn, guarded, w, r = info[i]
            if i in dead or not w or mn.startswith(('LDG', 'S2R', 'SHFL', 'VOTE', 'MATCH', 'REDUX', 'HMMA', 'IMMA')):
                continue
            live = False
            for reg in w:
                for j in range(i + 1, n_all):
                    tj, mnj, gj, wj, rj = info[j]
                    if j not in dead and reg in rj:
                        live = True
                        break
                    if mnj == 'EXIT' and not gj:   # the thread ends: nothing after reads it
                        break
                    if FLOW.match(mnj):   # past a branch the order of the code is not the order it runs in: the register stays
                        live = True
                        break
                    if reg in wj and not gj and j not in dead:
                        break
                if live:
                    break
            if not live:
                dead.add(i)
                changed = True
    return dead
