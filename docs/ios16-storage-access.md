# Compiled storage-pointer access on the legacy Metal backend

The runtime supplements storage-buffer write declarations from the actual
compiled function's argument-buffer reflection. It runs once when loading a
shader, after default function-constant specialization and the existing raw
argument-layout check. It only changes an in-memory `nvn_rbind.rw` from false to
true. Game shader source, library/reflection files, manifests, shader-chain
identity, parameter IDs, pipeline identity, barriers, and fences are untouched.

Resource declarations also retain their complete access union for each replay
render/compute encoder. A pool buffer used as both a writable SBUF and a
read-only UBO keeps `Read|Write` regardless of declaration order. Turning off
`DM_NVN_USE_DEDUP` only changes redundant-call elimination; every emitted usage
still contains the complete union. An explicit creation epoch resets the set
for each new encoder, including when Objective-C reuses its address. The
512-slot/16-probe fast table has a retained overflow vector rather than evicting
or forgetting previous Write flags. Allocation failure stops before issuing an
incomplete GPU declaration. No unrelated texture gains Write, and existing
fence/barrier calls are unchanged.

## Why supplement the existing metadata

The bundled translator exports `BufferUsageFlags.Write = 1 << 0`, while the
current offline `reflect()` uses `flags & 2`. Actual write flag 1 therefore
becomes explicit `rw:false`; the runtime parser trusts that explicit value.
For the audited `ec5ebd6d9902a9a1_b` CS, original MSL writes/atomics prove these
four writable bindings:

| Member | Argument ID | NVN storage slot |
| --- | ---: | ---: |
| `cp_s1` | 5 | 1 |
| `cp_s2` | 6 | 4 |
| `cp_s6` | 10 | 3 |
| `cp_s7` | 11 | 5 |

The runtime enhancement contains no game key or fixed binding list. This is a
conservative resource-use correction. It does not establish that wrong metadata
caused a particular visual fault: this backend already explicitly orders normal
compute/render/blit encoders with a global fence, and compute storage-buffer
pending-write range tracking does not depend on the erroneous `rw` bit.

## Public API and validation

`MTLFunction.newArgumentEncoderWithBufferIndex:reflection:` returns the same
encoder plus `MTLArgument` reflection. This public overload and nested pointer
reflection are available from iOS 11. The overload is deprecated on iOS 16 but
remains available; warnings are suppressed only around these compatibility API
uses. If it fails to create an encoder, the existing nonreflection overload is
retried and the metadata remains unchanged.

The outer argument must describe the requested buffer index and a struct.
Only resolved route-B `NVN_RB_SBUFP` storage pointers are eligible. Uniform,
driver, support, image, and nested argument-buffer pointers are excluded.
Members are matched by `MTLStructMember.argumentIndex`, never enumeration order
or `offset / 8`. The member must have pointer reflection and the exact byte
offset `aid * 8`, consistent with this backend's raw 8-byte argument-slot ABI;
its slot must fit the encoded storage. Duplicate or overlapping metadata IDs,
duplicate reflected IDs, unknown types/access, absent members, and unavailable
root reflection cannot promote a binding.

`MTLPointerType.access` of ReadWrite or WriteOnly adds Write. Existing true flags
are never cleared, including when reflection is missing or says ReadOnly.
The outer argument's access is deliberately ignored: a constant/read-only
argument block can hold writable device pointers. Compiled declaration access
can be conservative, so all eight device pointers in the example CS may be
promoted even though only four writes were proven from source.

## Diagnostics and verification

`[storage-access]` records include shader key/stage, metadata kind/storage slot,
argument ID, reflected offset/access, before/after `rw`, promotion result, and
rejection reason. Access 0/1/2 means ReadOnly/ReadWrite/WriteOnly; -1 means no
usable reflected access. At most 128 ordinary records are emitted per process.
`DM_NVN_STORAGE_ACCESS_DIAG=<shader key prefix>` selects an additional independent
128-record budget; `1` selects all keys. Both counters saturate. The filter only
changes logging, not the promotion policy.

Run:

```sh
make -C dm/gfx/tests GAME=0100F43008C44000 storage-access
```

The pure policy test verifies shuffled sparse IDs, both write modes, original
true preservation, nonstorage exclusion, metadata overlap, pointer/offset/range
validation, duplicate and omitted members, and unavailable reflection.
The Objective-C adapter test uses synthetic reflection objects through the
actual adapter, proves outer-vs-pointee access handling, checks actual log fields
and the 128-record cap, and tests encoder creation fallback. Neither creates a
GPU device.

The independent resource-use oracle checks both UBO/SBUF alias declaration
orders, separate read-only inputs, dedup-disabled unions, new-encoder epochs
and rollover, 2,048 colliding resource identities, capacity overflow, and initial
and growth allocation failures. Previous Write history survives every failure.

`t_storage_access_metal.m` contains only synthetic MSL. With a Metal device it
checks real sparse-ID reflection, a separate contiguous route-B ABI entry,
promotion of actual output/atomic pointers, and separate/aliased GPU readbacks.
Without a Metal device it explicitly prints **SKIP**. On the current local
virtual machine it compiled but skipped execution: real MSL compilation,
reflection, and GPU readbacks have not been validated locally.

For the device acceptance run, set the diagnostic prefix to the audited CS key.
Verify actual compiled reflection promotes at least IDs 5, 6, 10, and 11, with
valid offsets and access. Keep GPU validation enabled for the scene/camera run,
and verify the previously restored geometry and Hi-Z MIN constraints. An absent
promotion log is not evidence that a buffer is read-only. Do not broaden or
disable the existing fences based on these metadata changes.

Public API references: [argument encoder](https://developer.apple.com/documentation/metal/mtlfunction/makeargumentencoder(bufferindex:reflection:)),
[struct member](https://developer.apple.com/documentation/metal/mtlstructmember),
[pointer access](https://developer.apple.com/documentation/metal/mtlpointertype/access).
