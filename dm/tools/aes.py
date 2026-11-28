"""Block-aligned AES decryption through macOS CommonCrypto, shared by archive readers."""
import ctypes

_cc = None


def decrypt(key, data, iv=None):
    """AES ECB without an IV, CBC with a 16-byte IV; neither mode removes padding."""
    if len(key) not in (16, 24, 32) or len(data) % 16 or (iv is not None and len(iv) != 16):
        raise ValueError('invalid AES key, data or IV length')
    global _cc
    if _cc is None:
        _cc = ctypes.CDLL('/usr/lib/libSystem.B.dylib').CCCrypt
        _cc.restype = ctypes.c_int32
        _cc.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t,
                       ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                       ctypes.POINTER(ctypes.c_size_t)]
    out, moved = ctypes.create_string_buffer(len(data)), ctypes.c_size_t()
    if _cc(1, 0, 2 if iv is None else 0, key, len(key), iv, data, len(data), out, len(data), ctypes.byref(moved)):
        raise ValueError('CCCrypt failed')
    return out.raw[:moved.value]
