#!/usr/bin/env python3
# Writes the words of top_lift.c and bg_cover.c into patches.py (_TOP_LIFT, _BG_COVER, after its last marker line), so the C files are the source of truth.
#   python3 gen_native.py           compile both and rewrite the tail of patches.py
#   python3 gen_native.py --check   compile both and compare with patches.py (exit 1 if they differ); nothing is written
# Each file is compiled by clang to position-independent code without data, and `otool -r` must list no relocation (no calls out, no data, no jump tables: keep the helpers
# always_inline, no recursion). The words are those of its __text, as `otool -t` prints them. The generated words depend on the compiler version (Apple clang 21.0.0 made these)
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PATCHES = os.path.join(HERE, 'patches.py')
MARKER = '# ---- the C functions\' words: written by gen_native.py from top_lift.c and bg_cover.c, do not edit ----\n'
CFLAGS = ['-target', 'arm64-apple-macos', '-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-jump-tables', '-fno-asynchronous-unwind-tables']
FILES = (('_TOP_LIFT', 'top_lift.c'), ('_BG_COVER', 'bg_cover.c'))


def words(c, tmp):
    obj = os.path.join(tmp, c + '.o')
    subprocess.run(['clang', *CFLAGS, '-c', os.path.join(HERE, c), '-o', obj], check=True)
    rel = subprocess.run(['otool', '-r', obj], check=True, capture_output=True, text=True).stdout.splitlines()
    if len(rel) > 1: sys.exit(f'{c}: relocations (it is not position independent):\n' + '\n'.join(rel))
    text = subprocess.run(['otool', '-t', obj], check=True, capture_output=True, text=True).stdout.splitlines()[2:]   # after the file and section headers
    return ''.join(w for l in text for w in l.split()[1:])


def region():
    out = [MARKER]
    with tempfile.TemporaryDirectory() as tmp:
        for name, c in FILES:
            w = words(c, tmp)
            out.append(f'{name} = (   # {c}: {len(w) // 8} words\n' + ''.join(f"    '{w[i:i + 64]}'\n" for i in range(0, len(w), 64)) + ')\n')
    return ''.join(out)


src = open(PATCHES).read()
head = src[:src.index(MARKER)]
new = head + region()
if '--check' in sys.argv:
    print('patches.py matches the C files' if new == src else 'patches.py differs from the C files (run gen_native.py)')
    sys.exit(new != src)
open(PATCHES, 'w').write(new)
print('patches.py written')
