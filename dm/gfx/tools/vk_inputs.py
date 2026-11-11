"""Keep Vulkan integer vertex fetch conversions when Metal requires matching signs.

Fetch raw unsigned components, then sign extend only signed source formats. The
host specializes each location's shift from its VkVertexInputAttributeDescription.
This preserves unsigned high bits even when the SPIR-V destination is signed.
"""
import re

PREFIX = 'dm_vk_input_shift_'


def integer_inputs(src):
    stage = re.search(r'\bvertex\s+\w+\s+\w+\(\s*(\w+)\s+(\w+)\s*\[\[stage_in\]\]', src)
    if not stage:
        return src
    struct = re.search(r'\bstruct\s+' + re.escape(stage[1]) + r'\s*\{(.*?)\};', src, re.S)
    if not struct:
        raise ValueError('vertex stage input struct not found')
    fields = list(re.finditer(r'\b(u?int)([234]?)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', struct[1]))
    if not fields:
        return src
    indices = [int(v) for v in re.findall(r'\[\[function_constant\((\d+)\)\]\]', src)]
    first = max(indices, default=-1) + 1
    if first + len(fields) > 65536:
        raise ValueError('no Metal function constant indices left for vertex fetch')
    body = struct[1]
    declarations = []
    for i, field in reversed(list(enumerate(fields))):
        original, lanes, name, loc = field.groups()
        body = body[:field.start(1)] + 'uint' + body[field.end(1):]
        constant = PREFIX + loc
        declarations.append(f'constant uint {constant} [[function_constant({first + i})]];')
        access = stage[2] + '.' + name
        # Arithmetic right shift in signed 32 bits implements sign extension;
        # shift zero preserves the 32-bit pattern for unsigned fetches.
        value = f'{original}{lanes}(as_type<int{lanes}>({access} << {constant}) >> {constant})'
        src = re.sub(r'\b' + re.escape(access) + r'\b', '(' + value + ')', src)
    src = src[:struct.start(1)] + body + src[struct.end(1):]
    # metal_stdlib and the using directive must precede the declarations.
    at = src.index('struct ' + stage[1])
    return src[:at] + '\n'.join(reversed(declarations)) + '\n' + src[at:]
