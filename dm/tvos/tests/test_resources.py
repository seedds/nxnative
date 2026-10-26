#!/usr/bin/env python3
"""Exercise the actual streaming importer with owned synthetic archives."""
from pathlib import Path
import gzip
import hashlib
import io
import subprocess
import tarfile
import tempfile
import argparse
import os
import shutil


def archive(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for name, payload, kind in entries:
            info = tarfile.TarInfo(name); info.type = kind
            info.size = len(payload) if kind == tarfile.REGTYPE else 0
            if kind == tarfile.SYMTYPE: info.linkname = '/tmp/forbidden'
            tar.addfile(info, io.BytesIO(payload))
    return gzip.compress(stream.getvalue(), mtime=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--real-archive', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='nxnative-tv-import-') as temporary:
        temp = Path(temporary)
        runner = temp / 'runner.m'
        runner.write_text('''#import <Foundation/Foundation.h>
BOOL tv_unpack_shaders(NSString *, NSString *, NSString **);
int main(int argc, char **argv) { @autoreleasepool {
 NSString *failure = nil;
 BOOL ok = tv_unpack_shaders(@(argv[1]), @(argv[2]), &failure);
 if (!ok) fprintf(stderr, "%s\\n", failure.UTF8String);
 return ok ? 0 : 2;
} }
''')
        exe = temp / 'importer'
        subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-fobjc-arc',
            '-DDM_TV_RESOURCE_TESTS=1',
            '-Wno-deprecated-declarations', '-framework', 'Foundation', '-lz',
            str(root / 'Sources/resources_tv.m'), str(runner), '-o', str(exe)], check=True)
        valid = archive([('msl', b'', tarfile.DIRTYPE),
            ('msl/b-example/library.metallib', b'owned fixture', tarfile.REGTYPE),
            ('msl/b-example/' + 'a' * 90 + '.json', b'{}', tarfile.REGTYPE)])
        corrupted = valid[:-8] + bytes([valid[-8] ^ 1]) + valid[-7:]
        cases = [
            ('valid', valid, True, None),
            ('update', valid, True, None),
            ('wrong-sha', valid, False, '0' * 64),
            ('bad-gzip-crc', corrupted, False, None),
            ('traversal', archive([('msl/../escape', b'bad', tarfile.REGTYPE)]), False, None),
            ('absolute', archive([('/msl/file', b'bad', tarfile.REGTYPE)]), False, None),
            ('symlink', archive([('msl/link', b'', tarfile.SYMTYPE)]), False, None),
        ]
        for name, data, expected, override in cases:
            folder = temp / name; folder.mkdir()
            if name == 'update':
                (folder / 'msl').mkdir(); (folder / 'msl/old').write_text('previous owned resource')
            source = folder / 'msl.tar.gz'; source.write_bytes(data)
            result = subprocess.run([str(exe), str(folder), override or hashlib.sha256(data).hexdigest()], capture_output=True, text=True)
            assert (result.returncode == 0) == expected, (name, result.stderr)
            if expected:
                assert (folder / 'msl/b-example/library.metallib').read_bytes() == b'owned fixture'
                assert (folder / 'msl/.archive-sha256').read_text() == hashlib.sha256(data).hexdigest()
                assert not source.exists()
                if name == 'update': assert (folder / '.msl-before/old').is_file()
            else:
                assert source.exists() and not (folder / 'msl').exists()
            assert not (folder / 'escape').exists()
            print(f'{name}: PASS')
        folder = temp / 'marker-failure'; folder.mkdir()
        (folder / 'msl').mkdir(); (folder / 'msl/old').write_text('previous owned resource')
        source = folder / 'msl.tar.gz'; source.write_bytes(valid)
        environment = dict(os.environ); environment['DM_TEST_TV_RESOURCE_MARKER_FAIL'] = '1'
        result = subprocess.run([str(exe), str(folder), hashlib.sha256(valid).hexdigest()],
            capture_output=True, text=True, env=environment)
        assert result.returncode == 2, result.stderr
        assert source.exists(), 'failed marker publication must keep the transfer archive'
        assert (folder / 'msl/old').read_text() == 'previous owned resource'
        assert not (folder / 'msl/b-example').exists()
        assert not (folder / 'msl/.archive-sha256').exists()
        print('marker-failure-rollback: PASS')
        if args.real_archive:
            folder = temp / 'real-archive'; folder.mkdir()
            source = folder / 'msl.tar.gz'
            try: os.link(args.real_archive, source)
            except OSError: shutil.copyfile(args.real_archive, source)
            with source.open('rb') as f: digest = hashlib.file_digest(f, 'sha256').hexdigest()
            result = subprocess.run([str(exe), str(folder), digest], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            assert not source.exists() and (folder / 'msl').is_dir()
            assert (folder / 'msl/.archive-sha256').read_text() == digest
            print(result.stderr.strip())
            print('complete owned shader archive: PASS')


if __name__ == '__main__':
    main()
