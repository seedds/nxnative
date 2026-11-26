"""Lower a Maxwell indexed fragment input to individually decorated SPIR-V inputs.

The translated program still indexes the same vector array. Only initialization of
that array moves behind the raster interface. This pass does not modify IPA
arithmetic or depend on the spelling of generated Metal source.
"""
import struct

# SPIR-V opcodes and enumerants used by this lowering.
ENTRY, NAME, DECORATE = 15, 5, 71
TYPE_ARRAY, TYPE_POINTER, CONSTANT = 28, 32, 43
FUNCTION, FUNCTION_END, VARIABLE, LOAD, STORE = 54, 56, 59, 61, 62
ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN, COMPOSITE = 65, 66, 80
LABEL, LINE, NO_LINE = 248, 8, 317
INPUT, PRIVATE, LOCAL = 1, 6, 7
LOCATION, FLAT, NO_PERSPECTIVE = 30, 14, 13
FRAGMENT = 4


class UnsupportedInterface(Exception):
    """An interface outside this pass's supported SPIR-V layouts."""


def lower_or_keep(binary, sph):
    """Keep the input bytes for known unsupported interfaces only."""
    try:
        module, changed = lower_indexed_fragment_inputs(binary, sph)
    except UnsupportedInterface as exc:
        return binary, (), str(exc)
    return module, changed, None


def instruction(op, *args):
    return ((len(args) + 1) << 16 | op, *args)


def interpolation_modes(sph, size):
    if size > 32 or len(sph) < 24 + size:
        raise ValueError('invalid Maxwell interpolation map')
    modes = []
    for value in sph[24:24 + size]:
        used = {value >> shift & 3 for shift in (0, 2, 4, 6)} - {0}
        if len(used) > 1:
            raise UnsupportedInterface(f'mixed interpolation within location {len(modes)} (SPH {value:#04x})')
        modes.append(next(iter(used), 0))
    return modes


def lower_indexed_fragment_inputs(binary, sph):
    """Return (new module, corrected locations); raise for unsupported interfaces."""
    words = struct.unpack(f'<{len(binary) // 4}I', binary)
    ops = []
    offset = 5
    while offset < len(words):
        size, opcode = divmod(words[offset], 65536)
        if not size or offset + size > len(words):
            raise ValueError('invalid SPIR-V instruction')
        ops.append((opcode, words[offset + 1:offset + size]))
        offset += size

    arrays, pointers, constants, locations = {}, {}, {}, {}
    variables, decorations, entries = {}, {}, []
    for op, a in ops:
        if op == TYPE_ARRAY:
            arrays[a[0]] = a[1:]
        elif op == TYPE_POINTER:
            pointers[a[0]] = a[1:]
        elif op == CONSTANT and len(a) == 3:
            constants[a[1]] = a[2]
        elif op == VARIABLE and a[2] == INPUT:
            variables[a[1]] = a[0]
        elif op == DECORATE:
            decorations.setdefault(a[0], []).append(a[1:])
            if a[1] == LOCATION:
                locations[a[0]] = a[2]
        elif op == ENTRY and a[0] == FRAGMENT:
            entries.append(a)

    targets = [v for v, p in variables.items()
               if locations.get(v) == 0 and pointers[p][1] in arrays]
    if not targets:
        return binary, ()
    if len(targets) != 1 or len(entries) != 1:
        raise UnsupportedInterface('multiple indexed fragment arrays or entry points')
    target, = targets
    entry, = entries
    _, array = pointers[variables[target]]
    vector, length = arrays[array]
    count = constants[length]
    modes = interpolation_modes(sph, count)
    old_flags = {a[0] for a in decorations[target]}
    if old_flags - {LOCATION, FLAT, NO_PERSPECTIVE}:
        raise UnsupportedInterface('unsupported decoration on indexed fragment input')
    old_mode = 1 if FLAT in old_flags else 3 if NO_PERSPECTIVE in old_flags else 2
    changed = tuple(i for i, mode in enumerate(modes) if mode and mode != old_mode)
    if not changed:
        return binary, ()
    if any(v != target and 0 <= loc < count for v, loc in locations.items() if v in variables):
        raise UnsupportedInterface('overlapping fragment input locations')

    bound = words[3]

    def fresh():
        nonlocal bound
        result = bound
        bound += 1
        return result

    ids = [fresh() for _ in modes]
    input_vector_ptr, private_array_ptr = fresh(), fresh()
    accesses = {a[1]: a[0] for op, a in ops
                if op in (ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN) and a[2] == target}
    access_ptrs = {p: fresh() for p in sorted(set(accesses.values()))}
    # The generated array is used directly by access chains and loads. Refuse
    # derived pointers so an unrelated address-space use cannot be rewritten.
    for op, a in ops:
        if op in (ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN) and a[2] in accesses:
            raise UnsupportedInterface('nested indexed input pointer')

    annotation = []
    for location, (variable, mode) in enumerate(zip(ids, modes)):
        annotation.append(instruction(DECORATE, variable, LOCATION, location))
        qualifier = {1: FLAT, 3: NO_PERSPECTIVE}.get(mode or old_mode)
        if qualifier is not None:
            annotation.append(instruction(DECORATE, variable, qualifier))
    types_seen = set()
    result = list(words[:5])
    in_entry = waiting_for_label = initialize = False
    initialized = False
    for op, a in ops:
        if op == ENTRY and a == entry:
            string_end = next(i + 1 for i in range(2, len(a)) if 0 in a[i].to_bytes(4, 'little'))
            interface = a[string_end:]
            if target not in interface:
                raise ValueError('indexed fragment input missing from entry point')
            a = (*a[:string_end], *(v for v in interface if v != target), *ids)
            if words[1] >= 0x10400:
                a += (target,)  # SPIR-V 1.4+ lists statically used private globals.
        elif op == DECORATE:
            if a[0] == target:
                if annotation:
                    for add in annotation:
                        result.extend(add)
                    annotation.clear()
                continue
        elif op == FUNCTION:
            in_entry = a[1] == entry[1]
            waiting_for_label = in_entry
        elif op == LABEL and waiting_for_label:
            waiting_for_label = False
            initialize = True
        elif initialize and op not in (VARIABLE, LINE, NO_LINE):
            values = [fresh() for _ in ids]
            for value, var in zip(values, ids):
                result.extend(instruction(LOAD, vector, value, var))
            composite = fresh()
            result.extend(instruction(COMPOSITE, array, composite, *values))
            result.extend(instruction(STORE, target, composite))
            initialize = False
            initialized = True

        if op == VARIABLE and a[1] == target:
            result.extend(instruction(TYPE_POINTER, input_vector_ptr, INPUT, vector))
            result.extend(instruction(TYPE_POINTER, private_array_ptr, PRIVATE, array))
            for variable in ids:
                result.extend(instruction(VARIABLE, input_vector_ptr, variable, INPUT))
            a = (private_array_ptr, target, PRIVATE)
        elif op == TYPE_POINTER and a[0] in access_ptrs:
            result.extend(instruction(TYPE_POINTER, access_ptrs[a[0]], PRIVATE, a[2]))
            types_seen.add(a[0])
        elif op in (ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN) and a[1] in accesses:
            a = (access_ptrs[a[0]], *a[1:])
        result.extend(instruction(op, *a))

    if annotation or not initialized or types_seen != set(access_ptrs):
        raise ValueError('incomplete indexed fragment input lowering')
    result[3] = bound
    return struct.pack(f'<{len(result)}I', *result), changed
