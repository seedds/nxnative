# South Park: The Fractured But Whole (`01008F2005154000`)

Update **1.05** (`v327680`) over the base game, Ubisoft Snowdrop on NVN, SDK **3.5.1**. Modules: `main`, `subsdk0`, `sdk`. This profile uses a 5 GiB guest reservation and a separate RomFS image for the iPhone package. Its 4,615 extracted shader keys are recorded in the committed tables as keys, stages, pairs and source locations; these tables contain no shader binaries or game assets.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Seven Snowdrop allocator cuts | [`patches.py`](patches.py), `main` build `df15edaaf603e00cd7d8a01bd05a15c2` | Snowdrop requests 12 GiB of virtual address space. That fails inside the 4.375 GiB alias region, and the unchecked failure leads to an invalid write. The cuts reserve 4 GiB and rebalance its three classes. |
| Three SDK node and alignment cuts | [`sdk_patches.py`](../../tools/sdk_patches.py), `sdk` build `8bb65519bc6399ab432e0ba7cdb16e2b` | A 62 MiB node area and 32 MiB alignment for large regions allow the compact reservation to fit. Both node-area instructions must change together. |
| `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000`, `sys_resource 0x1000000` | [`game.tsv`](game.tsv) | The phone-sized guest layout. Reserved virtual address space is not committed RAM. |
| `shader_groups /camel/=camel` | `game.tsv` | Finds Snowdrop GLSLC outputs in the game's `camel` data. The committed shader tables are the split's v1.05 outputs; the required conflict-rulings table has no data rows for this game. |

The 4 GiB Snowdrop reservation assigns **1.5 GiB** to large allocations, **512 MiB** to 512 KiB slabs and **2 GiB** to 64 KiB slabs. The large allocator's separate 64 KiB page capacity is reduced from 65,536 to **24,576** to keep it inside its 1.5 GiB class. The first boundary requires a companion `movk` cut at `main+0x17fb304`; changing only the preceding instruction clears its high bits and overlaps the large and slab ranges. A prior 512 MiB 64 KiB class filled at 8,191 of 8,192 pages. The larger class addresses that measured failure, while the original 4 GiB growth fallbacks remain unchanged.

The game and SDK cuts assert their original instruction words and apply only to the build IDs above. Other game updates need their own opcode and memory-layout checks.

## Validation

- **Mac:** The latest Mac build was played extensively without issues. A separate recorded run of the compact 5 GiB layout saved and progressed through the opening without a crash or red NVN error.
- **iPhone:** The current separate-RomFS 1.05 IPA passed gameplay testing, including combat, save/reload and all six DLC packages. It defaults to the game's requested 30 fps. The same IPA also ran well with `DM_PRESENT_INTERVAL=display` at 60 fps; the phone log confirms regular 60 Hz presentation without dropped frames after startup.

## Running and frame rate

Build with the registered v1.05 profile and its matching base and update packages. For a separate-RomFS IPA, transfer the matching `romfs.img` into the app's Documents folder before launch. The game requests interval 2 and therefore defaults to about 30 fps. To try 60 fps, put `DM_PRESENT_INTERVAL=display` in `Documents/env.txt` and relaunch. This is a presentation override, not a launcher Frame rate option or an engine timing patch.

## Open

- Sustained per-class allocator high-water during later gameplay remains unmeasured. The original 4 GiB growth fallbacks are unchanged and unsafe if a compact class fills; the recorded 15,000-frame run showed address headroom only for that route.
