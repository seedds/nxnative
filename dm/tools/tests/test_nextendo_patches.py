"""Synthetic signed-catalog policy/IPS32/NSO tests; no console or game files."""
import base64
import copy
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nextendo_patches', REPO / 'dm/tools/nextendo_patches.py')
assert spec is not None and spec.loader is not None
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
TITLE = '0100000000001000'; BUILD = '12' * 32

def catalog(offset=0x104, original=b'abcd', changed=b'WXYZ', rle=False):
    record = struct.pack('>IH', offset, 0 if rle else len(changed))
    record += struct.pack('>H', len(changed)) + changed[:1] if rle else changed
    data = b'IPS32' + record + b'EEOF'
    return {'revision':1,'patches':[{'id':'synthetic','title_id':TITLE,'build_id':BUILD,'module':'main',
        'patch_version':1,'offset_basis':'nso_header_included','enabled':True,'required_capabilities':['npln.ips32.v1'],
        'sha256':hashlib.sha256(data).hexdigest(),'ips32_base64':base64.b64encode(data).decode(),
        'guards':[{'offset':offset,'original_hex':original.hex(),'patched_hex':changed.hex()}]}]}

class NextendoPatchTests(unittest.TestCase):
    def test_exact_full_identity_preimages_and_original_preservation(self):
        image = bytearray(b'0000abcd0000'); saved = bytes(image)
        changed, ids = p.apply_image(image,TITLE,BUILD,catalog())
        self.assertEqual(changed, b'0000WXYZ0000'); self.assertEqual(ids,['synthetic']); self.assertEqual(image,saved)
        self.assertEqual(p.apply_image(image,TITLE,'12'*16+'34'*16,catalog()),(None,[]))
        self.assertEqual(p.apply_image(image,'0100000000002000',BUILD,catalog()),(None,[]))
        with self.assertRaises(ValueError): p.apply_image(b'0000abce0000',TITLE,BUILD,catalog())
    def test_disabled_patch_does_not_modify_image(self):
        doc=catalog(); doc['patches'][0]['enabled']=False
        self.assertEqual(p.apply_image(b'0000abcd0000',TITLE,BUILD,doc),(None,[]))
    def test_unknown_policy_digest_and_incomplete_guard_fail(self):
        for field, value in [('sha256','0'*64),('offset_basis','unknown'),('module','sdk'),('required_capabilities',[]),
                             ('enabled',1),('guards',[]),('build_id','12'*16)]:
            with self.subTest(field=field):
                doc=catalog(); doc['patches'][0][field]=value
                with self.assertRaises(ValueError): p.validate_catalog(doc)
        doc=catalog(); doc['patches'][0]['guards'][0]['patched_hex']='00'*4
        with self.assertRaises(ValueError): p.validate_catalog(doc)
    def test_overlapping_guard_and_conflicting_patches_rejected(self):
        doc=catalog(); doc['patches'][0]['guards']*=2
        with self.assertRaises(ValueError): p.validate_catalog(doc)
        doc=catalog(); other=copy.deepcopy(doc['patches'][0]); other['id']='conflicting'; doc['patches'].append(other)
        with self.assertRaises(ValueError): p.apply_image(b'0000abcd0000',TITLE,BUILD,doc)
    def test_rle_and_bounds_and_trailing_bytes(self):
        doc=catalog(changed=b'WWWW',rle=True)
        self.assertEqual(p.apply_image(b'0000abcd',TITLE,BUILD,doc)[0],b'0000WWWW')
        with self.assertRaises(ValueError): p.apply_image(b'0000abc',TITLE,BUILD,doc)
        for data in [b'IPS32EEOFextra',b'IPS32'+struct.pack('>IH',0,4)+b'WXYZEEOF',b'IPS32'+struct.pack('>IH',0x104,4)+b'WXY']:
            bad=catalog(); bad['patches'][0].update(ips32_base64=base64.b64encode(data).decode(),sha256=hashlib.sha256(data).hexdigest())
            with self.assertRaises(ValueError): p.validate_catalog(bad)
    def test_nso_copy_preserves_build_and_updates_segment_integrity(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); source=root/'main'; out=root/'online/main'
            header=bytearray(0x100); header[:4]=b'NSO0'; header[0x40:0x60]=bytes.fromhex(BUILD)
            chunks=[b'0000abcd',b'ro00',b'data']
            for i,(memory,chunk) in enumerate(zip([0,0x10,0x20],chunks)):
                struct.pack_into('<III',header,0x10+0x10*i,0x100+sum(map(len,chunks[:i])),memory,len(chunk))
                struct.pack_into('<I',header,0x60+4*i,len(chunk))
            original=header+b''.join(chunks); source.write_bytes(original)
            self.assertEqual(p.prepare_nso(source,out,TITLE,catalog()),['synthetic'])
            new=out.read_bytes(); self.assertEqual(source.read_bytes(),original)
            self.assertEqual(new[0x40:0x60],bytes.fromhex(BUILD)); self.assertEqual(new[0x104:0x108],b'WXYZ')
            self.assertEqual(new[0xa0:0xc0],hashlib.sha256(b'0000WXYZ').digest())
            with self.assertRaises(ValueError): p.prepare_nso(source,out,TITLE,catalog(offset=0x108,original=b'\0'*4))
    def test_signed_document_rejects_duplicate_fields(self):
        with self.assertRaises(ValueError): p.strict_json('{"revision":1,"revision":2}')

if __name__ == '__main__': unittest.main()
