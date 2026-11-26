# NARUTO SHIPPUDEN: Ultimate Ninja STORM 3 Full Burst (`01006BB00800A000`)

Switch update 1.0.1 (v65536), SDK 4.4.0, `main` build `1f3bde8d5e55742767b332bc9308074f`. CyberConnect2's engine renders through NVN and uses CRIWARE.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GiB reservation, 2.25 GiB alias region, 32 MiB system resource | [`game.tsv`](game.tsv) | The NPDM enables virtual address memory. The Mac heap check measured a 2.15 GiB request, which fits in the 2.25 GiB heap region. |
| NVN shader library | [`shader_map.tsv`](shader_map.tsv), [`shader_pairs.tsv`](shader_pairs.tsv) | The standard splitter found 460 stages, 458 in RomFS and two in `main`. All 460 built on macOS. |

## Found on this game

- [A shorter mip chain lost the uploaded levels](../../../docs/reference/rendering.md#a-shorter-mip-chain-lost-the-uploaded-levels). The fix restores the ground and buildings in Hidden Leaf Village and Naruto's eyes in character select.

## Mac gameplay

An Apple M6 Mac mini running macOS 27.2.1 reached an offline team battle at 1600 × 900 and about 30 fps. The standing-still check covered opponent attacks and pausing. From Game Mode Select, use the left stick (W/S on the Mac keyboard) to select **Free Battle**. Choose **VS Battle**, **1P VS COM**, and **Team Match**, then select both teams and Hidden Leaf Village. J confirms and K goes back.

The host still logs a skipped buffer-to-texture copy at mip level 6 on a texture with six levels during startup. No visual effect has been identified for that warning.

The Mac check used the development host. The final Mac and iOS packages were skipped. Other modes and physical iOS devices remain unverified.
