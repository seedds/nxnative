"""Default iOS memory capabilities for a profile's guest reservation (bytes).

Usage: gen_entitlements.py <mem_reserve> <output.plist>
"""
import plistlib
import sys
from pathlib import Path


def memory_entitlements(reserve):
    result = {'com.apple.developer.kernel.increased-memory-limit': True}
    # Without Extended Virtual Addressing, a device test allowed a single 6 GiB
    # reservation but refused 7 GiB. Larger guest blocks need this independently
    # of the physical-memory limit; shrinking them changes the game's heap.
    if reserve > 6 * 1024**3:
        result['com.apple.developer.kernel.extended-virtual-addressing'] = True
    return result


if __name__ == '__main__':
    Path(sys.argv[2]).write_bytes(plistlib.dumps(memory_entitlements(int(sys.argv[1], 0))))
