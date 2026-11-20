# Minecraft for Nintendo Switch (`0100D71004694000`)

Update 1.26.62 (`v10747904`), Nintendo SDK 20.5.14, RenderDragon on Vulkan. The Mac host loads `main` and `sdk` and supplies Vulkan in place of `subsdk0`. The base game and iOS are untested.

## What it needs

| Setting | Reason |
|---|---|
| `modules main sdk` | RenderDragon finds `vkGetInstanceProcAddr` through `nn::ro::LookupSymbol`, then asks the host for Vulkan functions by name. |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `sys_resource 0x1000000` | The NPDM declares 16 MiB of system resource and the game maps memory in the alias region. |

## Mac

The Mac brings up the title panorama, the first-run dialog and a local world. Press **K** (Switch B) to close the first autosave notice, then **J** (Switch A) to select menu entries. Use **WASD** for movement and **Shift+arrow keys** for the right stick, or use a controller.

## Found on this game

- [Vulkan functions looked up by name](../../../docs/reference/rendering.md#vulkan-functions-looked-up-by-name) and [RenderDragon material shaders](../../../docs/reference/shaders.md#renderdragon-material-shaders).
- [RenderDragon's unsigned bone indices](../../../docs/reference/rendering.md#renderdragons-unsigned-bone-indices).
- [Vulkan shader resource tables truncated](../../../docs/reference/rendering.md#vulkan-shader-resource-tables-truncated).
- [Distributing branch hops between patch areas](../../../docs/reference/rewriter.md#distributing-branch-hops-between-patch-areas).

## Bone-index layout

Resolved on main build `84306c1f3543e39becef959079766644` (1.26.62). All 133 affected shaders have a scalar signed `a_indices` input, mapped by their BGFX attribute IDs to bone semantic 8. The legacy layout builder (`main+0xe4299a0`) constructs one non-normalized component for that field. Its type selector (`+0xe513bfc`) uses a cached capability initialized from context byte `+0x28`, which the constructor (`+0xe51551c`) sets to 1. The singleton's direct and GOT users contain no subsequent capability writer or setter-call escape; the guarded cache has one initialization store. Engine type 4 maps to backend type 3 in `+0xe815260`, then to `VK_FORMAT_R16_UINT` in both Vulkan pipeline format tables.

The other layout paths were traced before replacing the Metal variant. Three deferred callbacks copy the legacy builder's layout unchanged. The remaining debug callback uses the fixed `Debug/ImGui` material, whose inputs contain no bone index. Chunk layouts come from `+0x54d7590` through `+0xe8f7680`: their nine packed descriptors have semantics 0, 4, 10, 11, 12, 1, 2, 14 and 13, and their eight chunk materials have no bone-index inputs in any of 260 Vulkan vertex records. Mask enumeration, descriptor merging and the final six-byte layout copy preserve those semantics. Neither native path introduces an affected bone-index binding.

The [shader repair](../../../docs/reference/rendering.md#renderdragons-unsigned-bone-indices) therefore corrects the unsigned input before translation and keeps the original signed consumers. No per-build addresses enter the repair. SPIR-V validation covers all 133 repaired modules; synthetic cases cover shared signed pointer types, repeated loads and existing unsigned declarations. The Mac GPU test checks unsigned widening at `0x8000` and `0xffff`. Gameplay was not repeated for this change at the user's request.

## Resource bindings

Resolved: 198 ForwardPBR fragment shaders have 25–29 resource entries, and the host's old 24-entry storage dropped 522 samplers. All omitted samplers have shader-body uses: previous-frame luminance, caustics, scattering, shadow cascades and specular IBL. A representative 29-entry shader uses 5 buffers, 12 textures and 12 samplers, within the separate Metal argument-table limits. The [general loader fix](../../../docs/reference/rendering.md#vulkan-shader-resource-tables-truncated) retains the full list without changing shader assets. A synthetic 25-resource GPU readback verifies the binding failure and its correction.

On main build `84306c1f3543e39becef959079766644`, these affected actor ForwardPBR materials are unreachable through the normal shipped settings and material paths. The actor selector (`main+0xe1cbb70`, called at `+0xe438568` and `+0xe43efe8`) chooses them only for blended materials in backend mode 1, the ray-traced path. The hardware constructor and client refresh unconditionally clear `RayTracingHardwareOptions` byte `+0x19` at `+0x5e34d5c` and `+0x427acf4`. The client renderer update (`+0x422e5a0`) checks that capability and replaces a requested mode 1 with mode 0 before passing it to the framebuilder. A saved mode-1 setting cannot bypass this check. Deferred/Vibrant Visuals uses separate backend mode 2, which does not enter this selector branch. This is a static binary trace, not a physical Switch runtime observation.

No additional Minecraft gameplay validation is pending for this loader fix on this build: normal world play cannot exercise the rejected path. Complete-table, sanitizer and synthetic GPU checks cover the loader contract, and the full Hades regression passed. The general host fix remains valid independently of this build's unused shader assets.

## Open

Online services remain unverified. Availability and rendering of the separate deferred/Vibrant Visuals mode have not been established for this build. Clear rectangles smaller than an attachment still report unsupported operations.
