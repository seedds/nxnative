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

## Open

Online services and graphics modes beyond the default renderer remain unverified. Clear rectangles smaller than an attachment still report unsupported operations.
