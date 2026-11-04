#!/usr/bin/env python3
"""Build the Atmosphère cheat file for Hyrule Warriors: Age of Imprisonment v196608 (main build 659CB288B5C5B2B4): two code patches into main.

The HP-delta routine main+0x11c5ce0 (unit entry x19: HP +0x4c, previous HP +0x50, max HP +0x54, flags +0x5c; w2 = the attacker's unit id;
x22 = the game's global object; s0 = the delta, negative for damage; s1 = old HP, s2 = max HP, s3 = 1.0, w8 = flags) computes new = old + delta
at 0x11c5da0 (fadd s0, s1, s0) and clamps it at 0x11c5dc0 (fcmp s0, s3 ...). Each patch replaces one of those words with a branch to a stub in
the zero padding after the text (the cave, 0x1ed71c0), which does the original work plus the cheat and branches back.
- One hit kill: when the attacker's entry has flag bit 2 (a playable unit: the KO credit and the invincibility mode key on it) and the delta is
  negative, the delta becomes -max HP, so the victim's HP clamps to 0.
- Infinite health: when the victim has flag bit 2 and the new HP is below the old one, the old HP is kept.
The stubs are assembled with clang; branches between the cave and the code are written as absolute distances.
Usage: mkcheat.py [output folder]   (default: beside this script); the .txt is then imported with scripts/cheat_anchor.py into HWAoI.cheats.toml."""
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
CAVE = 0x1ed71c0
SITE_OHK, SITE_INF = 0x11c5da0, 0x11c5dc0
STUB_OHK, STUB_INF = CAVE, CAVE + 0x60
def asm(text, at):
    """Assemble text (arm64) as if placed at main+at; returns the words. Branch expressions use '.' so the file offset is the only base."""
    with tempfile.TemporaryDirectory() as d:
        s = os.path.join(d, 'a.s'); o = os.path.join(d, 'a.o')
        open(s, 'w').write('.text\n' + text + '\n')
        subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-c', s, '-o', o], check=True)
        out = subprocess.run(['xcrun', 'otool', '-t', o], capture_output=True, text=True, check=True).stdout
    words = []
    for l in out.splitlines()[1:]:
        p = l.split()
        if p and re.match(r'^[0-9a-f]{8,16}$', p[0]): words += [int(w, 16) for w in p[1:]]
    return words
def back(frm, to): return f'b . + {to - frm:#x}' if to >= frm else f'b . - {frm - to:#x}'
ohk = f"""
    cmp   w2, #0xc8
    b.hs  1f
    ldr   x9, [x22]
    mov   w10, #0x4b18
    movk  w10, #0x1a2, lsl #16
    ldr   x9, [x9, x10]
    add   x9, x9, #0x374, lsl #12
    add   x9, x9, #0x430
    mov   w10, #0x74
    umaddl x9, w2, w10, x9
    ldrh  w9, [x9, #0x5c]
    tbz   w9, #2, 1f
    fcmp  s0, #0.0
    b.pl  1f
    fneg  s0, s2
1:  fadd  s0, s1, s0
    fcmp  s1, s3
    {back(STUB_OHK + 0x44, SITE_OHK + 4)}
"""
inf = f"""
    tbz   w8, #2, 1f
    fcmp  s0, s1
    b.pl  1f
    fmov  s0, s1
1:  fcmp  s0, s3
    {back(STUB_INF + 0x14, SITE_INF + 4)}
"""
w_ohk, w_inf = asm(ohk, STUB_OHK), asm(inf, STUB_INF)
assert len(w_ohk) == 18 and len(w_inf) == 6, (len(w_ohk), len(w_inf))
j_ohk, j_inf = asm(back(SITE_OHK, STUB_OHK), SITE_OHK)[0], asm(back(SITE_INF, STUB_INF), SITE_INF)[0]
def entry(name, site, jump, stub_at, stub):
    lines = [f'[{name}]'] + [f'04000000 {stub_at + 4 * i:08X} {w:08X}' for i, w in enumerate(stub)] + [f'04000000 {site:08X} {jump:08X}']
    return '\n'.join(lines)
txt = '\n\n'.join([
    entry('One hit kill', SITE_OHK, j_ohk, STUB_OHK, w_ohk),
    entry('Infinite health', SITE_INF, j_inf, STUB_INF, w_inf)]) + '\n'
out = os.path.join(sys.argv[1] if len(sys.argv) > 1 else HERE, '659CB288B5C5B2B4.txt')
open(out, 'w').write(txt); print(txt); print('wrote', os.path.abspath(out))
