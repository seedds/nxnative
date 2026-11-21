"""Run the production bounded ZIP reader on original synthetic calendars."""
from pathlib import Path
import io
import struct
import subprocess
import sys
import tempfile
import zipfile
import warnings

names = ('coopdata/CoopSetting.byaml', 'vsdata/VSSetting_0.byaml', 'vsdata/VSSetting_1.byaml',
         'fesdata/Festival.byaml', 'fesdata/HapTexture.bfres', 'fesdata/IconTexture.bfres', 'fesdata/PanelTexture.bfres')
byaml = b'YB\x02\0' + struct.pack('<III', 16, 0, 0) + bytes(16)
fres = b'FRES' + bytes(1024 * 1024)
entries = {name: byaml if name.endswith('.byaml') else fres for name in names}

def archive(values, method=zipfile.ZIP_DEFLATED, stream=False):
    class Output(io.BytesIO):
        def seekable(self): return False
        def seek(self, *args): raise io.UnsupportedOperation
    output = Output() if stream else io.BytesIO()
    with zipfile.ZipFile(output, 'w', compression=method) as bundle:
        for name, data in values: bundle.writestr(name, data)
    return output.getvalue()

def main():
    cases = [('valid-deflate-high-ratio', archive(entries.items()), True),
             ('valid-stored', archive(entries.items(), zipfile.ZIP_STORED), True),
             ('valid-data-descriptors', archive(entries.items(), stream=True), True),
             ('empty', archive([]), False), ('truncated', archive(entries.items())[:-1], False),
             ('trailing-bytes', archive(entries.items())+b'junk', False),
             ('missing-required', archive(list(entries.items())[1:]), False),
             ('bzip2', archive(entries.items(), zipfile.ZIP_BZIP2), False)]
    for bad in ('../outside.byaml', '/outside.byaml', 'vsdata/../../outside.byaml', 'vsdata\\outside.byaml',
                'vsdata//outside.byaml', 'vsdata/./outside.byaml', 'vsdata/unexpected.exe', 'other/setting.byaml'):
        cases.append((bad, archive([*entries.items(), (bad, byaml)]), False))
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', UserWarning)
        cases.append(('duplicate', archive([*entries.items(), (names[0], byaml)]), False))
    cases.append(('case-alias', archive([*entries.items(), (names[0].upper(), byaml)]), False))
    invalid = dict(entries); invalid[names[0]] = b'YB\x02\0' + struct.pack('<III', 0xfffffffc, 0, 0) + bytes(16)
    cases.append(('byaml-offset', archive(invalid.items()), False))
    invalid = dict(entries); invalid[names[-1]] = b'not a BFRES' + bytes(64)
    cases.append(('wrong-resource-format', archive(invalid.items()), False))
    cases.append(('unknown-text-szs', archive([*entries.items(), ('fesdata/Unknown.szs', b'not a compressed archive')]), False))
    raw = bytearray(archive(entries.items(), zipfile.ZIP_STORED)); raw[raw.index(b'FRES')+8] ^= 1
    cases.append(('crc-mismatch', bytes(raw), False))
    raw = bytearray(archive(entries.items())); central = raw.index(b'PK\x01\x02'); struct.pack_into('<I', raw, central+24, 0x40000000)
    cases.append(('oversize-output', bytes(raw), False))
    raw = bytearray(archive(entries.items())); central = raw.index(b'PK\x01\x02'); struct.pack_into('<I', raw, central+38, 0o120777<<16)
    cases.append(('unix-symlink', bytes(raw), False))
    raw = bytearray(archive(entries.items())); central = raw.index(b'PK\x01\x02'); raw[6] |= 1; raw[central+8] |= 1
    cases.append(('encrypted', bytes(raw), False))
    raw = bytearray(archive(entries.items())); raw[30] ^= 1
    cases.append(('local-central-name-mismatch', bytes(raw), False))
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'calendar.zip'
        for name, data, accepted in cases:
            path.write_bytes(data)
            result = subprocess.run([sys.argv[1], str(path), str(int(accepted))], capture_output=True)
            if result.returncode: raise AssertionError((name, result.returncode, result.stderr.decode(errors='replace')))
    print(f'BCAT production ZIP reader: {len(cases)} cases passed; valid high-ratio resources accepted, unsafe archives rejected')

if __name__ == '__main__': main()
