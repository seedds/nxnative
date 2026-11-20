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
- [Distributing branch hops between patch areas](../../../docs/reference/rewriter.md#distributing-branch-hops-between-patch-areas).

## Bone-index layout

Resolved on main build `84306c1f3543e39becef959079766644` (1.26.62). All 133 affected shaders have a scalar signed `a_indices` input, mapped by their BGFX attribute IDs to bone semantic 8. The legacy layout builder (`main+0xe4299a0`) constructs one non-normalized component for that field. Its type selector (`+0xe513bfc`) uses a cached capability initialized from context byte `+0x28`, which the constructor (`+0xe51551c`) sets to 1. The singleton's direct and GOT users contain no subsequent capability writer or setter-call escape; the guarded cache has one initialization store. Engine type 4 maps to backend type 3 in `+0xe815260`, then to `VK_FORMAT_R16_UINT` in both Vulkan pipeline format tables.

The other layout paths were traced before replacing the Metal variant. Three deferred callbacks copy the legacy builder's layout unchanged. The remaining debug callback uses the fixed `Debug/ImGui` material, whose inputs contain no bone index. Chunk layouts come from `+0x54d7590` through `+0xe8f7680`: their nine packed descriptors have semantics 0, 4, 10, 11, 12, 1, 2, 14 and 13, and their eight chunk materials have no bone-index inputs in any of 260 Vulkan vertex records. Mask enumeration, descriptor merging and the final six-byte layout copy preserve those semantics. Neither native path introduces an affected bone-index binding.

The [shader repair](../../../docs/reference/rendering.md#renderdragons-unsigned-bone-indices) therefore corrects the unsigned input before translation and keeps the original signed consumers. No per-build addresses enter the repair. SPIR-V validation covers all 133 repaired modules; synthetic cases cover shared signed pointer types, repeated loads and existing unsigned declarations. The Mac GPU test checks unsigned widening at `0x8000` and `0xffff`. Gameplay was not repeated for this change at the user's request.

## Open

Online services and graphics modes beyond the default renderer remain unverified. Clear rectangles smaller than an attachment still report unsupported operations.

The complete shader-table load reports 198 ForwardPBR shader rows with more than the host's 24 resource entries. The rows are unchanged by the bone-index repair; this existing limit remains separate from the default-renderer gameplay result.
