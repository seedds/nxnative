// The Yaz0 decoder of shader_split.py's yaz0(), in C: loaded through ctypes (dm/tools/native_c.py builds it at first use), with yaz0()'s Python decoder as the fallback.
// It reads and writes only inside the buffers it is given and decides nothing the Python decoder does not: it makes the Python decoder's reads and checks, in the same
// order, and on any failure returns a code without a message, and yaz0() runs the Python decoder on the stream to give that message.
// The format: from offset 16, groups of a header byte and 8 items, its bits from the highest; a 1 bit is one literal byte, a 0 bit a back-reference of 2 bytes
// (length nibble, 12-bit distance - 1), or 3 when the nibble is 0 (the third byte + 0x12 is the length). A copy longer than its distance repeats its period.
#include <stddef.h>
#include <string.h>

// s, len: the stream with its 16-byte header; out: n bytes, the size its header gives. Returns 0 with out filled, 1 when the stream ends before n bytes are decoded,
// 2 when a back-reference reaches before the start. A copy that passes n is cut at n (the Python decoder drops what it decoded past n).
int dm_yaz0(const unsigned char *s, size_t len, unsigned char *out, size_t n)
{
    size_t si = 16, o = 0;
    while (o < n) {
        if (si >= len) return 1;
        unsigned h = s[si++];
        for (int i = 0; i < 8 && o < n; i++, h <<= 1) {
            if (h & 0x80) {
                if (si >= len) return 1;
                out[o++] = s[si++];
                continue;
            }
            if (len - si < 2) return 1;
            size_t b1 = s[si], dist = ((b1 & 15) << 8 | s[si + 1]) + 1, ln = b1 >> 4;
            si += 2;
            if (ln) ln += 2;
            else {
                if (si >= len) return 1;
                ln = (size_t)s[si++] + 0x12;
            }
            if (dist > o) return 2;
            if (ln > n - o) ln = n - o;
            unsigned char *d = out + o;
            const unsigned char *p = d - dist;
            o += ln;
            if (dist >= ln) memcpy(d, p, ln);
            else while (ln--) *d++ = *p++;
        }
    }
    return 0;
}
