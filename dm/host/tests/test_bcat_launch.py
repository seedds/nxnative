"""Run the real launch selector/downloader with original synthetic HTTPS data.

No provider connection, account, Nintendo resource or app is used. The private
runtime copy differs only in its test signing pin and synthetic XML digest;
the fixture also supplies its own temporary home and synthetic HTTP responses.
Each invocation is a NEW process, so a cached launch cannot skip revalidation.
"""
from pathlib import Path
import base64
import hashlib
import io
import json
import os
import shutil
import shlex
import subprocess
import tempfile
import time
import zipfile
from datetime import datetime, timezone

REPO = Path(__file__).resolve().parents[3]
SOURCE = REPO / 'dm/host/hle_bcat_apple.m'
PIN = 'xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4='
XML = b'<setting><original-fixture/></setting>'


def run():
    with tempfile.TemporaryDirectory(prefix='bcat-launch-') as temp:
        root = Path(temp).resolve()
        crypto = os.environ.get('DM_OPENSSL_ROOT') or subprocess.check_output([os.environ.get('PYTHON', 'python3'), str(REPO / 'dm/tools/apple_crypto.py'), 'macos'], text=True).strip()
        openssl = os.environ.get('OPENSSL_TOOL', '/opt/homebrew/opt/openssl@4/bin/openssl')
        secret = bytes.fromhex('302e020100300506032b657004220420') + bytes([7]) + bytes(31)
        (root / 'key.der').write_bytes(secret)
        public = subprocess.check_output([openssl, 'pkey', '-inform', 'DER', '-in', str(root / 'key.der'), '-pubout', '-outform', 'DER'])[-32:]
        def signed(enabled=True):
            now = int(time.time())
            def fmt(value):
                return datetime.fromtimestamp(value, timezone.utc).isoformat().replace('+00:00', 'Z')
            payload = json.dumps({'document_type': 'game_catalog', 'schema_version': 1, 'revision': 4,
                                 'issued_at': fmt(now - 60), 'expires_at': fmt(now + 3600),
                                 'games': [{'title_ids': ['0100f8f0000a2000'], 'enabled': enabled,
                                            'required_capabilities': ['versions.exact.v1'], 'versions': {'policy': 'exact', 'accepted': ['5.5.2']}}]}, separators=(',', ':')).encode()
            (root / 'payload').write_bytes(payload)
            sig = subprocess.check_output([openssl, 'pkeyutl', '-sign', '-rawin', '-keyform', 'DER', '-inkey', str(root / 'key.der'), '-in', str(root / 'payload')])
            return json.dumps({'key_id': 'catalog-key-1', 'payload': base64.b64encode(payload).decode(), 'signature': base64.b64encode(sig).decode()}).encode()
        (root / 'catalog.json').write_bytes(signed())
        (root / 'invalid.json').write_bytes(signed(False))
        (root / 'static.xml').write_bytes(XML)
        (root / 'dummy.txt').write_bytes(b'0')
        names = ['coopdata/CoopSetting.byaml', 'vsdata/VSSetting_0.byaml', 'vsdata/VSSetting_1.byaml', 'fesdata/Festival.byaml', 'fesdata/HapTexture.bfres', 'fesdata/IconTexture.bfres', 'fesdata/PanelTexture.bfres']
        for marker in [1, 2]:
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                for name in names:
                    data = (b'YB\x02\0' + bytes(27) if name.endswith('.byaml') else b'FRES' + bytes(27)) + bytes([marker])
                    archive.writestr(name, data)
            (root / f'v{marker}.zip').write_bytes(buf.getvalue())
        (root / 'corrupt.zip').write_bytes(b'not a ZIP')
        source = SOURCE.read_text()
        old_xml = '8d8d21a93dce2f8eab1659a3bf5cdca10f61df83d50bc0a34784b1629dd215fa'
        assert source.count(PIN) == source.count(old_xml) == 1
        source = source.replace(PIN, base64.b64encode(public).decode()).replace(old_xml, hashlib.sha256(XML).hexdigest())
        runtime = root / 'fixture-runtime.inc'
        runtime.write_text(source)
        binary = root / 'launch'
        flags = shlex.split(os.environ.get('SAN', ''))
        if os.environ.get('DM_BCAT_TEST_SANITIZE') == '1' and not flags:
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        print('BCAT launch compiler sanitizer flags:', flags or 'none', flush=True)
        command = ['clang', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-deprecated-declarations', '-fobjc-arc', *flags,
                        '-I' + str(REPO / 'dm/host'), '-I' + str(Path(crypto) / 'include'), '-DDM_OPENSSL_X509', '-DDM_BCAT_RUNTIME_INCLUDE="' + str(runtime) + '"', '-DDM_BCAT_TITLES=@"0100f8f0000a2000",@"01003bc0000a0000",@"01003c700009c800"', '-DDM_BCAT_PLAIN_TITLE=@"01003c700009c800"',
                        str(REPO / 'dm/host/tests/t_bcat_launch.m'), str(Path(crypto) / 'lib/libcrypto.a'), '-lz', '-framework', 'Foundation', '-o', str(binary)]
        subprocess.run(command, check=True)
        cache = root / 'cache'
        def launch(mode, expected, *, use_cache=cache):
            child_env = os.environ.copy()
            if any('-fsanitize=' in flag for flag in flags):
                child_env['ASAN_OPTIONS'] = child_env.get('ASAN_OPTIONS', '') + ':halt_on_error=1'
                child_env['UBSAN_OPTIONS'] = child_env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
            completed = subprocess.run([str(binary), str(root), mode, str(use_cache)], capture_output=True, text=True, timeout=20, env=child_env)
            if completed.returncode:
                raise RuntimeError(f'BCAT scenario {mode} exited {completed.returncode}:\n{completed.stderr}')
            value = json.loads(completed.stdout)
            assert value['firstCalendarMarker'] == expected, (mode, value, completed.stderr)
            if mode != 'offline' and not mode.startswith('gate-'):
                assert value['catalogRequests'] == (2 if mode == 'runtime-refresh' else 1), (mode, value)
            elif mode == 'offline':
                assert value['requests'] == 0 and not value['freshAttempt'], value
                assert not value['syncAccepted'], value
            print(mode, json.dumps(value, sort_keys=True), flush=True)
            return value
        first = launch('v1', 1)
        assert first['result'] == 0 and first['archiveRequests'] == 1
        again = launch('304', 1)
        assert again['result'] == 0 and again['conditionalRequests'] == 1
        changed = launch('v2', 2)
        assert changed['result'] == 0 and changed['conditionalRequests'] == 1
        runtime = launch('runtime-refresh', 1, use_cache=root / 'runtime-cache')
        assert (runtime['runtimeDeliveryMarker'], runtime['runtimeOverlayMarker'], runtime['retainedFDMarker']) == (2, 1, 1), runtime
        plain_cache = root / 'cache-without-etag'
        for _ in range(2):
            plain = launch('noetag', 1, use_cache=plain_cache)
            assert plain['result'] == 0 and plain['archiveRequests'] == 1 and plain['conditionalRequests'] == 0
        pointer = (cache / 'current.json').read_bytes()
        inventory = {str(p.relative_to(cache)): hashlib.sha256(p.read_bytes()).hexdigest() for p in cache.rglob('*') if p.is_file()}
        for mode in ['error', 'corrupt', 'invalid', 'timeout']:
            result = launch(mode, 2)
            assert result['result'] != 0
            if mode == 'timeout':
                assert 11 <= result['elapsed'] <= 14, result
                assert result['archiveRequests'] == 1, result
            assert (cache / 'current.json').read_bytes() == pointer
            assert {str(p.relative_to(cache)): hashlib.sha256(p.read_bytes()).hexdigest() for p in cache.rglob('*') if p.is_file()} == inventory
        launch('offline', 2)
        launch('offline', 0, use_cache=root / 'empty-cache')
        profile = root / 'valid-profile'
        shutil.copytree(cache, profile)
        setup = {'formatVersion': 1, 'titleID': '0100f8f0000a2000', 'version': '5.5.2'}
        (profile / 'setup.json').write_text(json.dumps(setup))
        invalid = root / 'invalid-profile'
        invalid.mkdir()
        (invalid / 'setup.json').write_text(json.dumps(dict(setup, titleID='01003bc0000a0000')))
        for mode in ['gate-missing', 'gate-invalid', 'gate-unrelated']:
            value = launch(mode, 0)
            assert not value['requests'] and not value['freshAttempt'] and not value['syncAccepted'], value
        offline = launch('gate-no-provider', 2)
        assert not offline['requests'] and not offline['freshAttempt'] and not offline['syncAccepted'], offline
        online = launch('gate-online', 1)
        assert online['catalogRequests'] == online['archiveRequests'] == 1 and online['result'] == 0, online
        # A normal build without the optional verifier must retain the offline
        # behavior, even if another caller incorrectly requests online mode.
        no_crypto = [value for value in command if value not in ('-DDM_OPENSSL_X509', str(Path(crypto) / 'lib/libcrypto.a'))]
        subprocess.run(no_crypto, check=True)
        unavailable = launch('gate-no-verifier', 0)
        assert not unavailable['requests'] and not unavailable['freshAttempt'] and not unavailable['syncAccepted'], unavailable
        provider = root / 'fixture-provider.c'
        provider.write_text('int dm_bcat_online_enabled(void) { return 1; }\n')
        subprocess.run([*command, str(provider)], check=True)
        bridged = launch('gate-provider-online', 1)
        assert bridged['catalogRequests'] == bridged['archiveRequests'] == 1 and bridged['result'] == 0, bridged
        process = subprocess.Popen([str(binary), str(root), 'interrupt', str(cache)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not (root / 'interrupt-ready').exists():
                assert process.poll() is None and time.monotonic() < deadline
                time.sleep(0.01)
            process.terminate()
            process.communicate(timeout=3)
            assert process.returncode != 0
            assert (cache / 'current.json').read_bytes() == pointer
            assert {str(p.relative_to(cache)): hashlib.sha256(p.read_bytes()).hexdigest() for p in cache.rglob('*') if p.is_file()} == inventory
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=3)
        print('BCAT launch refresh: 20 new-process scenarios passed; first open/fallback/304/no-ETag/shared deadline/interruption/profile/provider/verifier gates and immutable runtime overlay validated')


if __name__ == '__main__':
    run()
