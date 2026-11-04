#!/usr/bin/env python3
# The game name, display version and languages from the NSP's control data (control.nacp, saved in the extraction folder by nx_extract): the iOS app name and version come from them (shared by the packing script and build_ipa.sh),
#   and the packing script checks the game language against the languages the game supports.
# Usage: nacp.py <control.nacp> (prints the game name), nacp.py --version <control.nacp> (prints the display version), nacp.py --languages <control.nacp> (prints the supported language codes);
#   each prints an empty line and exits 1 when none can be read. From Python: import nacp; nacp.name(path), nacp.version(path), nacp.languages(path)
import re, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import romfs

# The NACP's languages in its own order (the bit order of SupportedLanguageFlag and the order of the title entries): the language code nn::oe::GetDesiredLanguage returns for each, and its name
LANGUAGES = [('en-US', 'American English'), ('en-GB', 'British English'), ('ja', 'Japanese'), ('fr', 'French'), ('de', 'German'), ('es-419', 'Latin American Spanish'), ('es', 'Spanish'), ('it', 'Italian'),
             ('nl', 'Dutch'), ('fr-CA', 'Canadian French'), ('pt', 'Portuguese'), ('ru', 'Russian'), ('ko', 'Korean'), ('zh-Hant', 'Traditional Chinese'), ('zh-Hans', 'Simplified Chinese'), ('pt-BR', 'Brazilian Portuguese')]

# Nintendo's region-language tags (System/RegionLangMask.txt, Mals/<tag>.Product.*) mapped to LANGUAGES codes, for a dump or NUP without control.nacp
MALS_LANGUAGES = {'USen': 'en-US', 'EUen': 'en-GB', 'JPja': 'ja', 'EUfr': 'fr', 'EUde': 'de', 'USes': 'es-419', 'EUes': 'es', 'EUit': 'it',
                  'EUnl': 'nl', 'USfr': 'fr-CA', 'EUpt': 'pt', 'EUru': 'ru', 'KRko': 'ko', 'TWzh': 'zh-Hant', 'CNzh': 'zh-Hans', 'USpt': 'pt-BR'}


def name(path):
    """The title area holds 0x300 bytes per language (name 0x200 + publisher 0x100); take the first non-empty one (0 = American English); a missing file or no sensible name returns an empty string"""
    p = Path(path)
    if not p.is_file(): return ''
    b = p.read_bytes(); t = b[:0x3000]
    if len(b) >= 2:
        try:   # newer SDKs compress the title area: a u16 size + raw deflate, inflating to 32 languages × 0x300 (Mario 1.2.1 does); inflate no further than that (a crafted size could inflate about 1,000 times)
            d = zlib.decompressobj(-15); z = d.decompress(b[2:2 + struct.unpack_from('<H', b)[0]], 0x6000 + 1)
            if d.eof and len(z) == 0x6000: t = z
        except zlib.error: pass   # not compressed
    for i in range(len(t) // 0x300):
        n = t[i * 0x300:i * 0x300 + 0x200].split(b'\0', 1)[0]
        if not n: continue
        try: n = n.decode('utf-8')
        except UnicodeDecodeError: return ''
        return n if n.isprintable() else ''
    return ''


def version(path):
    """The display version (0x10 bytes at 0x3060, e.g. 1.2.1), cut to what iOS accepts as a bundle version (up to three dot-separated integers); a missing file or no leading number returns an empty string"""
    p = Path(path)
    if not p.is_file(): return ''
    m = re.match(rb'\d+(\.\d+){0,2}', p.read_bytes()[0x3060:0x3070])
    return m.group().decode() if m else ''


def romfs_languages(folder):
    """The language codes a dump's or NUP's RomFS lists when there is no control.nacp: System/RegionLangMask.txt's first line, else the Mals/<tag>.* archives, in LANGUAGES order"""
    r = romfs.source(folder)
    mask, mals = r / 'System/RegionLangMask.txt', r / 'Mals'
    try:
        if mask.is_file(): tags = set((mask.read_text(encoding='utf-8', errors='replace').splitlines() or [''])[0].split())
        elif mals.is_dir(): tags = {e.name.partition('.')[0] for e in mals.iterdir() if not e.name.startswith('.')}
        else: return []
    except (OSError, ValueError, IndexError): return []
    have = {MALS_LANGUAGES[t] for t in tags if t in MALS_LANGUAGES}
    return [c for c, _ in LANGUAGES if c in have]


def languages(path):
    """The language codes the game supports (the u32 SupportedLanguageFlag at 0x302C, one bit per language in LANGUAGES order; Horizon picks the game language among them), in that order;
    without control.nacp (a dump folder, or a NUP without one), those its sibling RomFS lists; a missing file or no flag returns an empty list"""
    p = Path(path)
    if p.is_file():
        b = p.read_bytes()
        if len(b) >= 0x3030:
            f = struct.unpack_from('<I', b, 0x302C)[0]
            if f: return [c for i, (c, _) in enumerate(LANGUAGES) if f >> i & 1]
    return romfs_languages(p.parent)


if __name__ == '__main__':
    a = sys.argv[1:]; f = {'--version': version, '--languages': lambda p: ' '.join(languages(p))}.get(a[0] if a else '', name)
    if f is not name: a = a[1:]
    if len(a) != 1: sys.exit('usage: nacp.py [--version | --languages] <control.nacp>')
    n = f(a[0]); print(n); sys.exit(0 if n else 1)
