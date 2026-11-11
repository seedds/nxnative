"""Length-prefixed SPIR-V in RenderDragon CompiledMaterialDefinition containers."""
import struct

MAGIC = b'\x03\x02\x23\x07'
HEADER = b'\x1a\xda\x11\x0a\0\0\0\0\x27\0\0\0RenderDragon.CompiledMaterialDefinition'


def spirv_blobs(data):
    if not data.startswith(HEADER):
        return
    end = len(HEADER)
    while (at := data.find(MAGIC, end)) >= 0:
        if at < 4:
            end = at + 4
            continue
        size = struct.unpack_from('<I', data, at - 4)[0]
        stop = at + size
        if size < 20 or size % 4 or stop > len(data):
            raise ValueError(f'RenderDragon: invalid SPIR-V size at {at:#x}')
        pos = at + 20
        while pos < stop:
            count = struct.unpack_from('<I', data, pos)[0] >> 16
            if not count or pos + count * 4 > stop:
                raise ValueError(f'RenderDragon: invalid SPIR-V instruction at {pos:#x}')
            pos += count * 4
        yield data[at:stop]
        end = stop
