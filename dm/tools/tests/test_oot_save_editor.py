"""Synthetic files only. No game assets required."""
import copy
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import re
import urllib.request
import urllib.error
import base64
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from save_editor.oot_items import BOTTLES, REVISION, Slot

ROOT = Path(__file__).resolve().parents[3]


def fixture():
    rows = [{'Hash': key, 'SaveFileIndex': 0, 'RawValues': ['Empty', 'Fairy'],
             'Values': [17, 29]} for key in BOTTLES.values()]
    schema = {'Data': {'Enum': rows}, 'MetaData': {'SaveDataOffsetPos': [96, 0, 56],
                                                   'SaveTypeHash': [100, 0, 200]}}
    p = bytearray(104)
    struct.pack_into('<III', p, 0, 0x01020304, REVISION, 96)
    struct.pack_into('<II', p, 40, 0, 5)
    for i, key in enumerate(BOTTLES.values()):
        struct.pack_into('<II', p, 48 + i * 8, key, 17)
    struct.pack_into('<II', p, 88, 100, 96)
    c = bytearray(56)
    struct.pack_into('<III', c, 0, 0x01020304, REVISION, 56)
    struct.pack_into('<II', c, 48, 200, 0)
    return bytes(p), bytes(c), schema


class EditorTests(unittest.TestCase):
    def setUp(self):
        self.p, self.c, self.schema = fixture()
        self.slot = Slot(self.p, self.c, self.schema)

    def test_noop_is_identical(self):
        self.assertEqual(self.slot.edit({'BottleA': 'Empty'}), (self.p, []))
        self.assertEqual(self.slot.edit({}), (self.p, []))

    def test_only_selected_four_byte_value_changes(self):
        data, changes = self.slot.edit({'BottleB': 'Fairy'})
        offset = changes[0]['offset']
        self.assertEqual(data[:offset], self.p[:offset])
        self.assertEqual(data[offset+4:], self.p[offset+4:])
        self.assertEqual(struct.unpack_from('<I', data, offset)[0], 29)
        self.assertEqual(Slot(data, self.c, self.schema).catalog()['BottleB']['value'], 'Fairy')
        self.assertEqual(self.slot.progress, self.p)

    def test_archive_pairs_unchanged_caption_and_manifest(self):
        with zipfile.ZipFile(io.BytesIO(self.slot.archive({'BottleE': 'Fairy'}))) as z:
            self.assertEqual(set(z.namelist()), {'slot/progress.sav', 'slot/caption.sav', 'edit-manifest.json'})
            self.assertEqual(z.read('slot/caption.sav'), self.c)
            m = json.loads(z.read('edit-manifest.json'))
            self.assertEqual(m['changes'][0]['field'], 'BottleE')
            self.assertNotEqual(m['source_progress_sha256'], m['export_progress_sha256'])

    def test_invalid_edits(self):
        for edits in (None, [], {'Sword': 'Fairy'}, {'BottleA': 'Sword'}, {'BottleA': []}):
            with self.subTest(edits=edits), self.assertRaises(ValueError):
                self.slot.edit(edits)

    def test_rejects_revision_bounds_duplicates_unknown_values(self):
        for offset, value in ((0, 0), (4, 123), (8, 100000), (60, 999),
                              (56, next(iter(BOTTLES.values()))), (92, 500)):
            data = bytearray(self.p)
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                Slot(bytes(data), self.c, self.schema)
        for length in (0, 12, 39, 60, 103):
            with self.subTest(length=length), self.assertRaises(ValueError):
                Slot(self.p[:length], self.c, self.schema)

    def test_schema_mismatch(self):
        s = copy.deepcopy(self.schema)
        s['Data']['Enum'][0]['SaveFileIndex'] = 2
        with self.assertRaises(ValueError):
            Slot(self.p, self.c, s)

    def test_payload_bounds_and_unexplained_bytes(self):
        s = copy.deepcopy(self.schema)
        row = s['Data']['Enum'].pop()
        s['Data']['Binary'] = [row]
        data = bytearray(self.p)
        struct.pack_into('<I', data, 84, 100000)
        with self.assertRaisesRegex(ValueError, 'Payload outside'):
            Slot(bytes(data), self.c, s)
        with self.assertRaises(ValueError):
            Slot(self.p, self.c + b'x', self.schema)

    def test_cli_export_never_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p/'progress.sav').write_bytes(self.p)
            (p/'caption.sav').write_bytes(self.c)
            (p/'schema.json').write_text(json.dumps(self.schema))
            args = [sys.executable, str(ROOT/'scripts/oot_save_editor.py'), '--slot', str(p),
                    '--schema', str(p/'schema.json'), '--set', 'BottleA=Fairy', '--out', str(p/'copy.zip')]
            first = subprocess.run(args, capture_output=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            before = (p/'copy.zip').read_bytes()
            self.assertNotEqual(subprocess.run(args, capture_output=True).returncode, 0)
            self.assertEqual((p/'copy.zip').read_bytes(), before)
            self.assertEqual((p/'progress.sav').read_bytes(), self.p)
            self.assertEqual((p/'caption.sav').read_bytes(), self.c)


class HttpTests(unittest.TestCase):
    def test_http_export_import_and_guards(self):
        progress, caption, schema = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p/'progress.sav').write_bytes(progress)
            (p/'caption.sav').write_bytes(caption)
            (p/'schema.json').write_text(json.dumps(schema))
            proc = subprocess.Popen([sys.executable, str(ROOT/'scripts/oot_save_editor.py'),
                    '--schema', str(p/'schema.json'), '--slot', str(p), '--serve'],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            try:
                url = proc.stdout.readline().split()[1].rstrip('/')
                page = urllib.request.urlopen(url, timeout=5).read().decode()
                token = re.search("const token='([^']+)'", page)[1]
                catalog = json.load(urllib.request.urlopen(url+'/items', timeout=5))
                def post(path, obj, headers=None):
                    req = urllib.request.Request(url+path, data=json.dumps(obj).encode(),
                          headers=headers or {'X-Editor-Token': token, 'Content-Type': 'application/json'})
                    return urllib.request.urlopen(req, timeout=5)
                request = {'source': catalog['source'], 'edits': {'BottleA': 'Fairy'}}
                with post('/export', request) as response:
                    with zipfile.ZipFile(io.BytesIO(response.read())) as z:
                        decoded = Slot(z.read('slot/progress.sav'), z.read('slot/caption.sav'), schema)
                        self.assertEqual(decoded.catalog()['BottleA']['value'], 'Fairy')
                for headers in ({'X-Editor-Token': 'wrong'}, {'X-Editor-Token': token, 'Origin': 'https://example.com'}):
                    with self.assertRaises(urllib.error.HTTPError) as caught:
                        post('/export', request, headers)
                    self.assertEqual(caught.exception.code, 403)
                with self.assertRaises(urllib.error.HTTPError) as caught:
                    post('/export', {'source': 'stale', 'edits': {}})
                self.assertEqual(caught.exception.code, 400)
                with self.assertRaises(urllib.error.HTTPError):
                    post('/open', {'progress': 'bad-base64', 'caption': ''})
                with post('/open', {'progress': base64.b64encode(progress).decode(),
                                    'caption': base64.b64encode(caption).decode()}) as response:
                    self.assertEqual(json.load(response)['source'], catalog['source'])
                self.assertEqual((p/'progress.sav').read_bytes(), progress)
            finally:
                proc.terminate()
                proc.wait(timeout=5)
                proc.stdout.close()


if __name__ == '__main__':
    unittest.main()
