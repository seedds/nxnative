#!/usr/bin/env python3
"""Exercise the crash-safe APFS RomFS patch materializer with owned fixtures."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile


GAME = '0100F43008C44000'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fixture(root):
    romfs = root / 'romfs'; arc = romfs / 'arc'; arc.mkdir(parents=True)
    bundle = root / 'bundle'; bundle.mkdir()
    archive = bytearray((i * 37 + i // 251) & 255 for i in range(1_100_003))
    descriptor = bytearray((i * 19 + 7) & 255 for i in range(37_009))
    append = bytes((i * 11 + 3) & 255 for i in range(123_457))
    root_offset = 900_000
    archive[:8] = b'ONEPACK\0'
    archive[8:16] = root_offset.to_bytes(8, 'little')
    final_root_offset = root_offset + len(append)
    virtual_root_offset = root_offset
    virtual_archive_bytes = len(archive)
    header_source = bytes(archive[8:16])
    header_replacement = virtual_root_offset.to_bytes(8, 'little')
    header_edits = [{
        'source_offset': 8, 'output_offset': 8, 'bytes': 8,
        'source_hex': header_source.hex(),
        'replacement_hex': header_replacement.hex(),
    }]
    index_offset = root_offset + 1
    archive[index_offset:index_offset + 16] = bytes(range(16))
    replacement = bytes(range(16, 32))
    source = bytes(archive[index_offset:index_offset + len(replacement)])
    archive_edits = [{'source_offset': index_offset,
                      'output_offset': index_offset + len(append),
                      'bytes': len(replacement), 'pack_count': 2,
                      'source_hex': source.hex(),
                      'replacement_hex': replacement.hex()}]
    descriptor_edits = []
    for offset, replacement in ((17_777, b'PACKSIZE'), (31_001, b'NEWSIZE!')):
        source = bytes(descriptor[offset:offset + len(replacement)])
        descriptor_edits.append({'offset': offset, 'bytes': len(replacement),
                                 'source_hex': source.hex(),
                                 'replacement_hex': replacement.hex()})
    final_root = bytearray(archive[root_offset:])
    final_descriptor = bytearray(descriptor)
    for row in archive_edits:
        data = bytes.fromhex(row['replacement_hex']); at = row['source_offset'] - root_offset
        final_root[at:at + len(data)] = data
    final_archive = bytearray(archive[:root_offset])
    final_archive[8:16] = header_replacement
    final_archive.extend(append)
    final_archive.extend(final_root)
    for row in descriptor_edits:
        data = bytes.fromhex(row['replacement_hex']); at = row['offset']
        final_descriptor[at:at + len(data)] = data
    redirects = (f'DM_ROMFS_REDIRECTS_V1\tarc/data.trpfs\t2\t{virtual_archive_bytes}'
                 f'\t{len(final_archive)}\n'
                 f'0\t{virtual_root_offset}\t0\t{virtual_root_offset}\n'
                 f'{virtual_root_offset}\t{len(archive) - root_offset}\t{final_root_offset}'
                 f'\t{len(archive) - root_offset}\n').encode()
    manifest = {
        'schema': 4, 'mode': 'virtual-pack-layout-before-root', 'game': GAME,
        'ready_to_apply_to_new_output_directory': True,
        'archive_name': 'data.trpfs', 'archive_bytes': len(archive),
        'archive_sha256': sha(archive), 'descriptor_name': 'data.trpfd',
        'descriptor_bytes': len(descriptor), 'descriptor_sha256': sha(descriptor),
        'archive_root_offset': root_offset,
        'archive_root_bytes': len(archive) - root_offset,
        'archive_root_sha256': sha(archive[root_offset:]),
        'final_archive_root_offset': final_root_offset,
        'virtual_archive_root_offset': virtual_root_offset,
        'virtual_archive_bytes': virtual_archive_bytes,
        'final_archive_root_sha256': sha(final_root),
        'archive_header_edits': header_edits,
        'archive_index_edits': archive_edits,
        'descriptor_size_edits': descriptor_edits,
        'append': {'file': 'append.bin', 'bytes': len(append), 'sha256': sha(append),
                   'output_offset': root_offset},
        'redirects': {'file': 'redirects.tsv', 'bytes': len(redirects),
                      'sha256': sha(redirects), 'records': 2,
                      'target': 'arc/data.trpfs',
                      'virtual_file_bytes': virtual_archive_bytes,
                      'physical_file_bytes': len(final_archive)},
        'final_archive_bytes': len(final_archive),
        'final_archive_sha256': sha(final_archive),
        'final_descriptor_bytes': len(final_descriptor),
        'final_descriptor_sha256': sha(final_descriptor),
    }
    (arc / 'data.trpfs').write_bytes(archive)
    (arc / 'data.trpfd').write_bytes(descriptor)
    (bundle / 'append.bin').write_bytes(append)
    (bundle / 'redirects.tsv').write_bytes(redirects)
    (bundle / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return romfs, bundle, bytes(archive), bytes(descriptor), bytes(final_archive), bytes(final_descriptor)


def run(exe, romfs, writable, bundle, game=GAME, stage=None):
    writable.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if stage: env['DM_TEST_ROMFS_PATCH_EXIT'] = stage
    return subprocess.run([str(exe), str(romfs), str(writable), str(bundle), game],
                          capture_output=True, text=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--real-romfs', type=Path)
    parser.add_argument('--real-bundle', type=Path)
    args = parser.parse_args()
    if bool(args.real_romfs) != bool(args.real_bundle):
        parser.error('--real-romfs and --real-bundle must be supplied together')
    tv = Path(__file__).resolve().parents[1]
    main_source = (tv / 'Sources/main_tv.m').read_text()
    prepare_source = (tv / 'tools/prepare_project.py').read_text()
    assert 'getenv("DM_ROMFS_PATCH")' in main_source
    materialize_at = main_source.index('tv_materialize_romfs_patch(')
    activate_at = main_source.index('setenv("DM_ROMFS_OVERLAY"')
    preload_at = main_source.index('dm_host_preload_start(2, host_argv);', materialize_at)
    assert materialize_at < activate_at < preload_at
    assert 'sources:\n      - path: Sources' in prepare_source
    strip_ios_env_at = prepare_source.index("(replacement / 'env.txt').unlink(missing_ok=True)")
    publish_inputs_at = prepare_source.index('replacement.rename(dest)')
    assert strip_ios_env_at < publish_inputs_at
    print('explicit opt-in, fail-closed startup wiring: PASS')
    with tempfile.TemporaryDirectory(prefix='nxnative-romfs-patch-') as temporary:
        temp = Path(temporary)
        runner = temp / 'runner.m'
        runner.write_text('''#import <Foundation/Foundation.h>
#include <pthread.h>
#import "romfs_patch.h"
struct job { char **argv; int result; };
static void *materialize(void *opaque) { @autoreleasepool {
 struct job *job = opaque; char **argv = job->argv;
 NSString *overlay = nil, *failure = nil;
 BOOL ok = tv_materialize_romfs_patch(@(argv[1]), @(argv[2]), @(argv[3]),
                                      @(argv[4]), &overlay, &failure);
 if (overlay) printf("OVERLAY=%s\\n", overlay.fileSystemRepresentation);
 if (failure) fprintf(stderr, "FAILURE=%s\\n", failure.UTF8String);
 job->result = ok ? 0 : 2; return NULL;
} }
int main(int argc, char **argv) { @autoreleasepool {
 if (argc != 5) return 64;
 struct job job = {.argv = argv, .result = 70};
 pthread_attr_t attr; pthread_attr_init(&attr);
 pthread_attr_setstacksize(&attr, 512u << 10);
 pthread_t thread;
 if (pthread_create(&thread, &attr, materialize, &job)) return 71;
 pthread_attr_destroy(&attr);
 if (pthread_join(thread, NULL)) return 72;
 return job.result;
} }
''')
        exe = temp / 'materializer'
        subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-fobjc-arc',
            '-Wno-deprecated-declarations', '-framework', 'Foundation',
            '-I', str(tv / 'Sources'), str(tv / 'Sources/romfs_patch.m'),
            str(runner), '-o', str(exe)], check=True)
        romfs, bundle, source_a, source_d, final_a, final_d = fixture(temp / 'fixture')

        writable = temp / 'success'
        result = run(exe, romfs, writable, bundle)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        overlay = Path(next(line.split('=', 1)[1] for line in result.stdout.splitlines()
                            if line.startswith('OVERLAY=')))
        assert (overlay / 'arc/data.trpfs').read_bytes() == final_a
        assert (overlay / 'arc/data.trpfd').read_bytes() == final_d
        assert (overlay / 'redirects.tsv').read_bytes() == (bundle / 'redirects.tsv').read_bytes()
        assert (overlay / 'READY.json').is_file()
        assert not list((writable / 'romfs-patch-overlays').glob('*.partial'))
        assert (romfs / 'arc/data.trpfs').read_bytes() == source_a
        assert (romfs / 'arc/data.trpfd').read_bytes() == source_d
        before = (overlay / 'READY.json').stat().st_mtime_ns
        again = run(exe, romfs, writable, bundle)
        assert again.returncode == 0 and 'verified existing overlay' in again.stderr
        assert (overlay / 'READY.json').stat().st_mtime_ns == before
        print('materialize + fast READY reuse: PASS')

        wrong_game = run(exe, romfs, temp / 'wrong-game', bundle, game='bad')
        assert wrong_game.returncode == 2 and 'OVERLAY=' not in wrong_game.stdout
        assert not list((temp / 'wrong-game').rglob('READY.json'))
        print('wrong game fails closed without overlay: PASS')

        legacy_root = temp / 'legacy-schema'; legacy_romfs, legacy_bundle, *_ = fixture(legacy_root)
        legacy_manifest_path = legacy_bundle / 'manifest.json'
        legacy_manifest = json.loads(legacy_manifest_path.read_text())
        legacy_manifest['schema'] = 2
        legacy_manifest['mode'] = 'append-relocated-packs'
        legacy_manifest_path.write_text(json.dumps(legacy_manifest, indent=2) + '\n')
        legacy = run(exe, legacy_romfs, temp / 'legacy-writable', legacy_bundle)
        assert legacy.returncode == 2 and 'OVERLAY=' not in legacy.stdout
        assert 'incompatible with this game' in legacy.stderr
        assert not list((temp / 'legacy-writable').rglob('READY.json'))
        print('schema-2 append-after-EOF bundle fails closed: PASS')

        renamed_root = temp / 'renamed-redirect'; renamed_romfs, renamed_bundle, *_ = fixture(renamed_root)
        renamed_manifest_path = renamed_bundle / 'manifest.json'
        renamed_manifest = json.loads(renamed_manifest_path.read_text())
        (renamed_bundle / 'redirects.tsv').rename(renamed_bundle / 'other.tsv')
        renamed_manifest['redirects']['file'] = 'other.tsv'
        renamed_manifest_path.write_text(json.dumps(renamed_manifest, indent=2) + '\n')
        renamed = run(exe, renamed_romfs, temp / 'renamed-redirect-writable', renamed_bundle)
        assert renamed.returncode == 2 and 'OVERLAY=' not in renamed.stdout
        assert 'payload names are invalid' in renamed.stderr
        assert not list((temp / 'renamed-redirect-writable').rglob('READY.json'))
        print('noncanonical redirect sidecar name fails closed: PASS')

        bad_root_hash = temp / 'bad-root-hash'; root_romfs, root_bundle, *_ = fixture(bad_root_hash)
        root_manifest_path = root_bundle / 'manifest.json'
        root_manifest = json.loads(root_manifest_path.read_text())
        root_manifest['archive_root_sha256'] = '0' * 64
        root_manifest_path.write_text(json.dumps(root_manifest, indent=2) + '\n')
        root_failed = run(exe, root_romfs, temp / 'bad-root-writable', root_bundle)
        assert root_failed.returncode == 2 and 'OVERLAY=' not in root_failed.stdout
        assert 'source or append SHA-256 differs' in root_failed.stderr
        assert not list((temp / 'bad-root-writable').rglob('READY.json'))
        print('source-root hash mismatch fails closed: PASS')

        bad_root = temp / 'bad-source'; bad_romfs, bad_bundle, *_ = fixture(bad_root)
        bad = bytearray((bad_romfs / 'arc/data.trpfs').read_bytes()); bad[200_000] ^= 1
        (bad_romfs / 'arc/data.trpfs').write_bytes(bad)
        failed = run(exe, bad_romfs, temp / 'bad-writable', bad_bundle)
        assert failed.returncode == 2 and 'OVERLAY=' not in failed.stdout
        assert not list((temp / 'bad-writable').rglob('READY.json'))
        assert (bad_romfs / 'arc/data.trpfs').read_bytes() == bad
        print('source mismatch fails closed and preserves source: PASS')

        for stage in ('cloned', 'written', 'ready', 'published'):
            interrupted = temp / ('interrupt-' + stage)
            first = run(exe, romfs, interrupted, bundle, stage=stage)
            assert first.returncode == 93, (stage, first.stderr)
            recovered = run(exe, romfs, interrupted, bundle)
            assert recovered.returncode == 0, (stage, recovered.stderr)
            roots = [p for p in (interrupted / 'romfs-patch-overlays').iterdir()
                     if p.is_dir() and not p.name.endswith('.partial')]
            assert len(roots) == 1 and (roots[0] / 'READY.json').is_file()
            assert not list((interrupted / 'romfs-patch-overlays').glob('*.partial'))
        print('interrupted clone/write/READY/publish recovery: PASS')

        concurrent = temp / 'concurrent'; concurrent.mkdir()
        commands = [[str(exe), str(romfs), str(concurrent), str(bundle), GAME] for _ in range(3)]
        processes = [subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True) for command in commands]
        results = [process.communicate(timeout=30) + (process.returncode,) for process in processes]
        assert all(code == 0 for _, _, code in results), results
        roots = [p for p in (concurrent / 'romfs-patch-overlays').iterdir()
                 if p.is_dir() and not p.name.endswith('.partial')]
        assert len(roots) == 1 and (roots[0] / 'arc/data.trpfs').read_bytes() == final_a
        print('concurrent materializers serialize to one overlay: PASS')

        if args.real_romfs:
            real = run(exe, args.real_romfs.resolve(), temp / 'real-owned',
                       args.real_bundle.resolve())
            assert real.returncode == 0, (real.returncode, real.stdout, real.stderr)
            real_overlay = Path(next(line.split('=', 1)[1] for line in real.stdout.splitlines()
                                     if line.startswith('OVERLAY=')))
            assert (real_overlay / 'READY.json').is_file()
            print(real.stderr.strip())
            print('complete owned RomFS append-relocated bundle: PASS')


if __name__ == '__main__':
    main()
