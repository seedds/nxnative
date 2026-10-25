#!/usr/bin/env python3
"""Set an unsigned arm64 Mach-O executable's __TEXT maximum protection to RWX."""
import struct
import sys


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <unsigned Mach-O>")
    path = sys.argv[1]
    with open(path, "r+b") as f:
        header = f.read(32)
        if len(header) != 32 or struct.unpack_from("<I", header)[0] != 0xFEEDFACF:
            raise SystemExit(f"{path}: not a little-endian 64-bit Mach-O")
        ncmds = struct.unpack_from("<I", header, 16)[0]
        off = 32
        for _ in range(ncmds):
            f.seek(off)
            command = f.read(72)
            if len(command) < 8:
                raise SystemExit(f"{path}: truncated load commands")
            cmd, cmdsize = struct.unpack_from("<II", command)
            if cmdsize < 8:
                raise SystemExit(f"{path}: invalid load command size {cmdsize}")
            if cmd == 0x19 and command[8:24].rstrip(b"\0") == b"__TEXT":
                maxprot, initprot = struct.unpack_from("<II", command, 56)
                if initprot != 5 or maxprot not in (5, 7):
                    raise SystemExit(
                        f"{path}: unexpected __TEXT protections max={maxprot:#x} init={initprot:#x}"
                    )
                f.seek(off + 56)
                f.write(struct.pack("<I", 7))
                print(f"{path}: __TEXT maxprot {maxprot:#x} -> 0x7, initprot stays 0x5")
                return
            off += cmdsize
    raise SystemExit(f"{path}: __TEXT segment not found")


if __name__ == "__main__":
    main()
