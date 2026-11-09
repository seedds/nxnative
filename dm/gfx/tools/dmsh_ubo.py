"""Carry each fragment uniform bank's bound length into the Metal argument buffer."""
import re

import dmsh_abi as abi

BANK_READ = re.compile(r'\(\*spvDescriptorSet0\.(fp_c(\d+))\)\._m0\[')
MEMBER = re.compile(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', re.M)

# Keep the full-vector path a single read. Only the last partial vector needs byte loads.
HELPER = '''
static inline float4 dm_ubo_load(constant float4 *bank, uint index, uint bytes) {
    if (index < bytes / 16) return bank[index];
    float4 result = 0;
    if (index == bytes / 16 && (bytes & 15u)) {
        constant uchar *src = reinterpret_cast<constant uchar *>(bank) + index * 16;
        thread uchar *dst = reinterpret_cast<thread uchar *>(&result);
        for (uint b = 0; b < (bytes & 15u); ++b) dst[b] = src[b];
    }
    return result;
}
'''

def bounded_fragment(src):
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', src, re.S)
    if not body:
        return src
    members = list(MEMBER.finditer(body[1]))
    banks = sorted({m[1] for m in BANK_READ.finditer(src) if 3 <= int(m[2]) <= 16})
    if not banks:
        return src
    # IDs count array elements, even when only the array's first ID is explicit.
    def end(m):
        array = re.fullmatch(r'array<.*,\s*(\d+)>', m[1].strip())
        return int(m[3]) + (int(array[1]) if array else 1)
    first = max(map(end, members))
    lengths = {name: abi.CBUF_BYTES_PREFIX + name for name in banks}
    add = ''.join(f'    ulong {lengths[name]} [[id({first+i})]];\n' for i,name in enumerate(banks))
    src = src[:body.end(1)] + add + src[body.end(1):]
    # Work from the inside out for an index computed from another bank read.
    for m in reversed(list(BANK_READ.finditer(src))):
        name = m[1]
        if name not in lengths:
            continue
        pos = m.end()
        depth = 1
        while depth:
            depth += (src[pos] == '[') - (src[pos] == ']')
            pos += 1
        index = src[m.end():pos-1]
        bank = '(*spvDescriptorSet0.' + name + ')._m0'
        read = f'dm_ubo_load({bank}, uint({index}), spvDescriptorSet0.{lengths[name]})'
        src = src[:m.start()] + read + src[pos:]
    return src.replace('using namespace metal;\n', 'using namespace metal;\n'+HELPER, 1)
