#!/usr/bin/env python3
# Fetch the compact stand-in for the standard shared font (standard_small.ttf): Noto Sans Regular, 0.6 MB, 3,884 glyphs (Latin, Greek, Cyrillic). The host gives it to a game's font engine
# only when that engine fails to load the standard stand-in (hle_sdk16.c h_font_LoadFont): Mario Tennis Aces loads the shared font into engines with 50 KB work buffers, where Arial Unicode
# (23 MB) fails with error 201 and the game then crashes looking up the "?" glyph. The file comes from the Noto project at a fixed commit and is checked against its SHA-256; it is kept in a
# cache (DM_FONT_CACHE, else ~/Library/Caches/nxnative/fonts), so only the first build downloads it. The font carries its licence (SIL Open Font License 1.1) in its name table (IDs 0, 13, 14).
# Usage: compact_font.py out.ttf; exit status 1 when it cannot be had (offline and not cached): the host then falls back to macOS's Arial
import hashlib, os, shutil, sys, tempfile, urllib.request
from pathlib import Path

URL = ('https://raw.githubusercontent.com/notofonts/notofonts.github.io/412a7edb2c8c535d1066157e5b17e661fd5591fe/'
       'fonts/NotoSans/hinted/ttf/NotoSans-Regular.ttf')
SHA256 = '478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823'
TIMEOUT = 30   # seconds without data before the download gives up


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def cached():
    """The verified font in the cache, downloaded there first when it is not; raises OSError (or ValueError on a wrong digest) when it cannot be had"""
    d = Path(os.environ.get('DM_FONT_CACHE') or Path.home() / 'Library/Caches/nxnative/fonts')
    f = d / f'NotoSans-Regular-{SHA256[:16]}.ttf'
    if f.is_file() and digest(f) == SHA256: return f
    d.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, suffix='.part')
    try:
        with os.fdopen(fd, 'wb') as out, urllib.request.urlopen(URL, timeout=TIMEOUT) as r: shutil.copyfileobj(r, out)
        if digest(Path(tmp)) != SHA256: raise ValueError(f'{URL}: SHA-256 is not {SHA256}')
        os.replace(tmp, f)   # only a verified file takes the cache name
    finally:
        if os.path.exists(tmp): os.unlink(tmp)
    return f


def main():
    if len(sys.argv) != 2: sys.exit('usage: compact_font.py out.ttf')
    try: src = cached()
    except (OSError, ValueError) as e:
        print(f'[YELLOW] compact shared font not fetched ({e}); the host falls back to macOS\'s Arial', file=sys.stderr); return 1
    out = Path(sys.argv[1]); out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, out)
    print(f'{out}: Noto Sans Regular, {out.stat().st_size} bytes (from {src})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
