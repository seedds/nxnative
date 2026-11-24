"""Preserve integer wraparound before lowering SPIR-V to runtime Metal source."""
import struct


TYPE_INT, TYPE_VECTOR, CONSTANT_NULL = 21, 23, 46
FUNCTION, BITCAST, NEGATE = 54, 124, 126
ADD, SUBTRACT, MULTIPLY, SHIFT_LEFT = 128, 130, 132, 196
WRAPPING = {ADD, SUBTRACT, MULTIPLY, SHIFT_LEFT, NEGATE}


def instruction(opcode, *operands):
    return [(len(operands) + 1) << 16 | opcode, *operands]


def wrap_signed_integers(data):
    """Run signed arithmetic on unsigned bit patterns, then restore the result type.

    Metal's runtime compiler cannot take the offline compiler's -fwrapv flag.
    The shift count retains its original type. SPIR-V arithmetic is already
    modulo the result width, so this does not change its value.
    """
    words = struct.unpack(f'<{len(data) // 4}I', data)
    header = list(words[:5])
    bound = header[3]
    module = []
    signed, unsigned, vectors = {}, {}, {}
    at = 5
    while at < len(words):
        size, opcode = words[at] >> 16, words[at] & 0xffff
        if not size or at + size > len(words):
            raise ValueError('invalid SPIR-V instruction span')
        operands = words[at + 1:at + size]
        module.append((opcode, operands))
        if opcode == TYPE_INT:
            identifier, width, sign = operands
            (signed if sign else unsigned)[width] = identifier
        if opcode == TYPE_VECTOR:
            identifier, component, count = operands
            vectors[component, count] = identifier
        at += size
    signed_types = {identifier: (width, 1) for width, identifier in signed.items()}
    signed_types.update({identifier: (width, count) for width, component in signed.items()
                         for (element, count), identifier in vectors.items() if element == component})
    additions, code, zeroes = [], [], {}

    def fresh():
        nonlocal bound
        identifier = bound
        bound += 1
        return identifier

    def unsigned_type(width, count):
        if width not in unsigned:
            unsigned[width] = fresh()
            additions.extend(instruction(TYPE_INT, unsigned[width], width, 0))
        scalar = unsigned[width]
        if count == 1:
            return scalar
        if (scalar, count) not in vectors:
            vectors[scalar, count] = fresh()
            additions.extend(instruction(TYPE_VECTOR, vectors[scalar, count], scalar, count))
        return vectors[scalar, count]

    first_function = None
    for opcode, operands in module:
        if opcode == FUNCTION and first_function is None:
            first_function = len(code)
        if opcode not in WRAPPING or operands[0] not in signed_types:
            code.extend(instruction(opcode, *operands))
            continue
        signed_type, result, lhs, *rhs = operands
        bits = unsigned_type(*signed_types[signed_type])
        left, value = fresh(), fresh()
        code.extend(instruction(BITCAST, bits, left, lhs))
        if opcode == NEGATE:
            if bits not in zeroes:
                zeroes[bits] = fresh()
                additions.extend(instruction(CONSTANT_NULL, bits, zeroes[bits]))
            code.extend(instruction(SUBTRACT, bits, value, zeroes[bits], left))
        elif opcode == SHIFT_LEFT:
            code.extend(instruction(opcode, bits, value, left, rhs[0]))
        else:
            right = fresh()
            code.extend(instruction(BITCAST, bits, right, rhs[0]))
            code.extend(instruction(opcode, bits, value, left, right))
        code.extend(instruction(BITCAST, signed_type, result, value))
    if bound == header[3]:
        return data
    if first_function is None:
        raise ValueError('SPIR-V arithmetic outside a function')
    header[3] = bound
    result = header + code[:first_function] + additions + code[first_function:]
    return struct.pack(f'<{len(result)}I', *result)
