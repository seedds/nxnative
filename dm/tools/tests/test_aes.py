"""Archive AES modes against the independent NIST SP 800-38A known-answer vectors."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aes import decrypt


@unittest.skipUnless(sys.platform == 'darwin', 'CommonCrypto')
class AES(unittest.TestCase):
    def test_aes256_ecb(self):
        key = bytes.fromhex('603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4')
        ciphertext = bytes.fromhex('f3eed1bdb5d2a03c064b5a7e3db181f8')
        self.assertEqual(decrypt(key, ciphertext), bytes.fromhex('6bc1bee22e409f96e93d7e117393172a'))

    def test_aes128_cbc(self):
        key = bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c')
        iv = bytes(range(16))
        ciphertext = bytes.fromhex('7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2')
        plaintext = bytes.fromhex('6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51')
        self.assertEqual(decrypt(key, ciphertext, iv), plaintext)

    def test_bad_lengths(self):
        for key, data, iv in ((bytes(15), bytes(16), None), (bytes(16), bytes(15), None), (bytes(16), bytes(16), bytes(15))):
            with self.subTest(lengths=(len(key), len(data), len(iv) if iv else None)), self.assertRaises(ValueError):
                decrypt(key, data, iv)


if __name__ == '__main__': unittest.main()
