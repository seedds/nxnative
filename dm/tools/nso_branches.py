"""Register-preserving branch fallback beyond AArch64 B/BL's signed 128 MiB range.

The host resumes a marked BRK at its recorded target. This is slower than a direct
branch, but needs no scratch register or writable guest code at runtime.
"""
import bisect
import struct
from nso_asm import B, BL, BRK

MARKER = BRK(0x6E78)
REACH = 1 << 27


def _signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def padding_slots(text, ranges, image, excluded=()):
    """Unreferenced zero alignment after a terminal FDE, before its successor.

    Never use holes of unknown size, instruction NOPs, function contents or
    literal data. Conservatively reject PC-relative references, entire code
    pages addressed by ADRP, and aligned module-relative pointers in the image.
    """
    slots = set()
    def terminal(word):
        return word in (0xd65f03c0, 0xd65f0bff, 0xd65f0fff) or word & 0xfffffc1f == 0xd61f0000 or word >> 26 == 5
    end = 0
    for start, stop in sorted(ranges):
        if start >= len(text): break
        if end >= 4 and 0 < start - end <= 28 and start % 16 == end % 4 == 0 and not any(text[end:start]):
            word = struct.unpack_from('<I', text, end - 4)[0]
            if terminal(word) and terminal(struct.unpack_from('<I', image, end - 4)[0]): slots.update(range(end, start, 4))
        end = max(end, stop)
    if not slots: return []
    blocked, pages = set(excluded), set()
    def reject(address):
        if address in slots: blocked.add(address)
    for i, (word,) in enumerate(struct.iter_unpack('<I', text)):
        pc = i * 4
        if word & 0x7c000000 == 0x14000000:
            reject(pc + _signed(word & 0x3ffffff, 26) * 4)
        elif word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000 or word & 0x3b000000 == 0x18000000:
            reject(pc + _signed((word >> 5) & 0x7ffff, 19) * 4)
        elif word & 0x7e000000 == 0x36000000:
            reject(pc + _signed((word >> 5) & 0x3fff, 14) * 4)
        elif word & 0x9f000000 in (0x10000000, 0x90000000):
            delta = _signed(((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2), 21)
            if word & 0x80000000:
                page = (pc & ~0xfff) + delta * 4096
                if 0 <= page < len(text): pages.add(page)
            else: reject(pc + delta)
    # RELR addends, symbol values, absolute pointers and RELA addends are all
    # aligned words in the decompressed image. False positives only lose slots.
    view = memoryview(image)
    for alignment in (0, 4):
        stop = alignment + (len(view) - alignment) // 8 * 8
        for (value,) in struct.iter_unpack('<Q', view[alignment:stop]):
            if value in slots: blocked.add(value)
    return sorted(a for a in slots if a not in blocked and a & ~0xfff not in pages and image[a:a + 4] == text[a:a + 4])


class Branches:
    def __init__(self):
        self.far = {}
        self.targets = set()

    def encode(self, source, target, link=False):
        assert source % 4 == target % 4 == 0
        self.targets.add(target)
        if -(1 << 27) <= target - source < (1 << 27):
            self.far.pop(source, None)
            return (BL if link else B)(source, target)
        self.far[source] = (target, int(link))
        return MARKER

    def resolve(self, image, text, ranges, write):
        """Replace marked edges with two register/flag-preserving direct branches.

        A BL sets LR at its original site; the relay always uses B. If no proven
        slot reaches both endpoints, retain the exact signal-return fallback.
        write handles sources in the image and both external patch areas.
        """
        if not self.far: return []
        slots = padding_slots(text, ranges, image, self.targets)
        parent = list(range(len(slots) + 1))
        def free(index):
            root = index
            while parent[root] != root: root = parent[root]
            while parent[index] != index:
                next_index = parent[index]; parent[index] = root; index = next_index
            return root
        resolved, targets = [], {}
        for source, (target, link) in sorted(self.far.items(), key=lambda item: (-abs(item[0] - item[1][0]), item[0])):
            lo, hi = max(source - REACH, target - REACH + 4), min(source + REACH - 4, target + REACH)
            relay = next((a for a in targets.get(target, ()) if lo <= a <= hi), None)
            if relay is None:
                index = free(bisect.bisect_left(slots, lo))
                if index == len(slots) or slots[index] > hi: continue
                relay = slots[index]; parent[index] = free(index + 1)
                struct.pack_into('<I', image, relay, B(relay, target))
                targets.setdefault(target, []).append(relay)
            write(source, (BL if link else B)(source, relay))
            resolved.append((source, relay, target, link))
        for source, _, _, _ in resolved: del self.far[source]
        return resolved
