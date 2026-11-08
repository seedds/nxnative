# dm/mac/pack_app.py itself, with the steps that need Apple's tools answered from the kit: the launcher comes prebuilt and signed, the icon is written with Pillow, and nothing is re-signed
import json, os, shutil, sys
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); KIT = os.environ.get('DM_WIN_KIT') or os.path.join(HERE, 'kit'); kit = json.load(open(os.path.join(KIT, 'kit.json')))
sys.path.insert(0, os.path.join(HERE, 'repo', 'dm', 'mac')); import pack_app as pa
def run(cmd):
    c = [str(x) for x in cmd]
    if c[:2] == ['xcrun', '--find']: return sys.executable
    if c[:3] == ['xcrun', '--sdk', 'macosx']: return os.path.dirname(sys.executable) if c[3] == '--show-sdk-path' else '27.2'
    if c[-1] == '--version': return 'kit'
    raise SystemExit(f'windows test: no stand-in for {c[:3]}')
pa.run = run
_which = shutil.which
pa.shutil.which = lambda cmd, *a, **k: sys.executable if cmd == 'codesign' else _which(cmd, *a, **k)
pa.min_os = lambda host: kit['minos']
pa.build_launcher = lambda out, *a: shutil.copyfile(os.path.join(KIT, 'nxnative'), out)
pa.can_clone = lambda *a: False
pa.source_commit = lambda: kit['commit']
pa.sign = lambda *a: None   # the kit's programs are signed on their own; the test shows whether the Mac runs the app without a seal over the bundle
def link_or_copy(a, b):
    try: os.link(a, b)
    except OSError: shutil.copyfile(a, b)
def copy(srcs, dst, clone):   # as cp -R
    for src in srcs:
        src = str(src); to = os.path.join(dst, os.path.basename(src)) if os.path.isdir(dst) else str(dst)
        if os.path.isdir(src): shutil.copytree(src, to, copy_function=link_or_copy)
        else: link_or_copy(src, to)
pa.copy = copy
def make_icns(jpg, out):
    from PIL import Image
    Image.open(jpg).convert('RGBA').resize((1024, 1024), Image.LANCZOS).save(out, format='ICNS')
pa.make_icns = make_icns
pa.pack(pa.parse(sys.argv[1:]))
