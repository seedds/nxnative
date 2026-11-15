"""The capstone versions the rewriter's analysis is known to give the same result on (docs/reference/rewriter.md, "The capstone version"). patch_nso.py, nso_code.py and
nso_match.py decode with capstone, and what they decide depends on what it decodes, so an older capstone does not stop the rewrite: it rewrites differently.
  capstone 4.0.2 (Ubuntu 24.04's python3-capstone) decodes no udf #imm16 (the words 0x0000xxxx, among them the zero padding between functions), so nso_code.data_blocks counts
    those words as data: in dm/tools/aot/corpus's excl-clang.nro the code from +0x1f80 on (svc stubs without unwind entries, then zero padding) became a data block and its ten
    svc sites were left unrewritten (and the recompiler's manifest got a data row there)
  capstone 5.0.1 drops operands of some instructions: patch_nso.py stops with an IndexError on excl-clang.nro and excl-gcc.nro, and the synthetic module (tests/aot_synth.py)
    loses its spin wait
  capstone 5.0.0 and 5.0.2 to 5.0.9 rewrite the corpus and the synthetic module byte for byte the same, and pass dm/tools/tests: 5.0.0 is left out only because
    the version after it is broken, so the range starts where every release on is known good
  capstone 6 (pre-releases only) has no CS_ARCH_ARM64: patch_nso.py does not start
So the rewriter takes MIN <= version < BELOW, and the packer and patch_nso.py refuse any other with a [RED] line. The version is the installed distribution's (pip's or the
system package's metadata): capstone.__version__ says 5.0.7 in 5.0.8 and 5.0.9"""
import re

MIN, BELOW = (5, 0, 2), (6,)
WANT = 'capstone 5.0.2 or later 5.0.x (pip install "capstone>=5.0.2,<6")'


def installed():
    """The version string of the capstone this Python imports (None when it imports none): its distribution's when the metadata found is that module's, else capstone.__version__"""
    try: import capstone
    except ImportError: return None
    from pathlib import Path
    try:
        from importlib import metadata
        here = Path(capstone.__file__).resolve().parent
        for dist in metadata.distributions(name='capstone'):
            try:
                if Path(dist.locate_file('capstone')).resolve() == here: return dist.version
            except (OSError, TypeError): continue
    except Exception: pass   # metadata unreadable: the module's own string below
    return getattr(capstone, '__version__', '0')


def parse(v):
    """(major, minor, patch) of a version string ('5.0.0.post1' -> (5, 0, 0); '6.0.0a11' -> (6, 0, 0))"""
    m = re.match(r'(\d+)(?:\.(\d+))?(?:\.(\d+))?', v or '')
    return tuple(int(x or 0) for x in m.groups()) if m else (0, 0, 0)


def problem():
    """'' when this Python's capstone is one the rewriter takes, else what is wrong: the version found and the one needed (the callers print it as a [RED] line)"""
    v = installed()
    if v is None: return ''   # no capstone at all: the callers' own missing-module message says so
    if MIN <= parse(v) < BELOW: return ''
    import capstone
    return (f'capstone {v} ({getattr(capstone, "__file__", "?")}): the rewriter needs {WANT}. Other versions decode differently and rewrite a module differently '
            f'(dm/tools/capstone_version.py: 4.0.2 leaves code with zero padding unrewritten as data, 5.0.1 drops operands, 6 has no CS_ARCH_ARM64)')


def unusable():
    """Why the rewriter's analysis cannot be tested with this Python's capstone ('' when it can): none installed, or problem(). The tests that decode skip with it"""
    try: import capstone  # noqa: F401
    except ImportError: return 'needs the capstone Python module (pip install "capstone>=5.0.2,<6"), which this Python cannot import'
    return problem()


def require():
    """Stop the program with problem()'s [RED] line, if there is one"""
    p = problem()
    if p: raise SystemExit('[RED] ' + p)
