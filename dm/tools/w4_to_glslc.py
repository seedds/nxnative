# Extracts the shaders of Unrailed 2 (W4 Games Godot PCK) into extract/<version>/glslc_capture/ so the NSP script reuses them.
# Usage: python3 dm/tools/w4_to_glslc.py <path of the <id>_build folder> [version, default v327680]  (needs: pip install zstandard)
# Run it after a first build has created extract/<version>/RomFS.img.
import os, sys, mmap, struct, hashlib, shutil, subprocess
try:
    import zstandard
except ImportError:
    subprocess.run([sys.executable, '-m', 'pip', 'install', '--user', '-q', 'zstandard'])
    import zstandard
B = sys.argv[1]
ver = sys.argv[2] if len(sys.argv) > 2 else 'v327680'
V = os.path.join(B, 'extract', ver)
fh = open(os.path.join(V, 'RomFS.img'), 'rb')
m = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
g = m.find(b'GDPC')
assert g >= 0, 'no GDPC header: RomFS.img is not a Godot PCK'
base = g + struct.unpack_from('<Q', m, g + 24)[0]
def entry(o):
    n = struct.unpack_from('<I', m, o)[0]
    path = bytes(m[o+4:o+4+n]).rstrip(b'\0'); q = o + 4 + n
    off, size = struct.unpack_from('<QQ', m, q)
    return path, off, size, bytes(m[q+16:q+32]), q + 36
o = m.find(b'.godot/w4_shaders/') - 4
w4 = []
while True:
    try: e = entry(o)
    except Exception: break
    if not e[0].startswith(b'.godot/'): break
    if e[0].endswith(b'.w4_shader'): w4.append(e)
    o = e[4]
assert w4, 'no .w4_shader entries found'
first = w4[0]
assert hashlib.md5(bytes(m[base+first[1]: base+first[1]+first[2]])).digest() == first[3], 'PCK data base check failed'
ZM = bytes.fromhex('28b52ffd'); GL = bytes.fromhex('91688619'); CTL = bytes.fromhex('34127698')
out = os.path.join(V, 'glslc_capture')
shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
seen = set()
for e in w4:
    d = bytes(m[base+e[1]: base+e[1]+e[2]])
    pos = d.find(ZM)
    while pos >= 0:
        try:
            ob = zstandard.ZstdDecompressor().decompressobj()
            raw = ob.decompress(d[pos:]); used = len(d) - pos - len(ob.unused_data)
            if raw[:4] == GL and CTL in raw:
                h = hashlib.sha1(raw).hexdigest()[:16]
                if h not in seen:
                    seen.add(h); open(os.path.join(out, h + '.glslc'), 'wb').write(raw)
            pos = d.find(ZM, pos + used)
        except Exception:
            pos = d.find(ZM, pos + 1)
print('w4 files %d, unique GLSLC outputs written: %d -> %s' % (len(w4), len(seen), out))
