#!/usr/bin/env python3
"""Compress an existing build's shader compile cache, or restore raw SQLite. No game build is run."""
import argparse, sqlite3, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
import shader_cache


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('build_folder', type=Path, help='existing <title ID>_build folder')
    ap.add_argument('--decompress', action='store_true', help='replace the compressed cache with raw SQLite')
    args = ap.parse_args(argv)
    try: shader_cache.maintain(args.build_folder.expanduser() / 'shader/cache/cas.db', compress=not args.decompress, decompress=args.decompress)
    except (OSError, RuntimeError, ValueError, sqlite3.Error) as e:
        print(f'[RED] shader compile cache: {e}', file=sys.stderr); return 1
    return 0


if __name__ == '__main__': sys.exit(main())
