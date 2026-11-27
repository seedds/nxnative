# NARUTO SHIPPUDEN: Ultimate Ninja STORM 3 Full Burst (`01006BB00800A000`)

Switch update 1.0.1 (v65536), SDK 4.4.0, `main` build `1f3bde8d5e55742767b332bc9308074f`. CyberConnect2's engine renders through NVN and uses CRIWARE.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 5 GiB reservation, 2.25 GiB alias region, 32 MiB system resource | [`game.tsv`](game.tsv) | The NPDM enables virtual address memory. The Mac heap check measured a 2.15 GiB request, which fits in the 2.25 GiB heap region. |
| NVN shader library | `shader_map.tsv`, `shader_pairs.tsv` | The standard splitter found 460 stages, 458 in RomFS and two in `main`. All 460 built on macOS. |

## Found on this game

- [A shorter mip chain lost the uploaded levels](../../../docs/reference/rendering.md#a-shorter-mip-chain-lost-the-uploaded-levels). The fix restores the ground and buildings in Hidden Leaf Village and Naruto's eyes in character select.

## Frame rate

The game passes 30 fps to CyberConnect2's native rate setter at startup. The setter divides 60 by the requested rate and uses that result for both the number of vertical blanks to wait and the frame duration. The engine still waited for two blanks with the host set to present on every 60 Hz refresh.

[`patches.py`](patches.py) passes 60 fps at that startup call. The same complete function matches on both Switch builds and locates the instruction at `main+0x778af8` in 1.0.0 and `main+0x778e90` in 1.0.1. The rewriter checks the original instruction and skips the change if the function cannot be identified uniquely.

On update 1.0.1 an Apple M6 Mac mini running macOS 27.2.1 averaged 58.9 fps in a 1600 × 900 VS Battle in Hidden Leaf Village, up from 29.7 fps before the patch. Median frame time fell from 33.33 ms to 16.67 ms. In the sampled battle windows, process CPU rose from 18% to 38% of one core, and reported GPU time rose from 215 to 339 ms per second. The battle timer counted down five units in 10.44 seconds at 60 fps and six in 10.72 seconds at 30 fps, within one displayed unit of the baseline.

## Mac gameplay

The 60 fps check covered the startup logos, character and stage selection, opponent attacks and pausing. From Game Mode Select, use the left stick (W/S on the Mac keyboard) to select **Free Battle**. Choose **VS Battle**, **1P VS COM**, and **Team Match**, then select both teams and Hidden Leaf Village. J confirms and K goes back.

The host still logs a skipped buffer-to-texture copy at mip level 6 on a texture with six levels during startup. No visual effect has been identified for that warning.

The Mac check used the development host and update 1.0.1. The final Mac and iOS packages were skipped. Base-game gameplay, other modes and physical iOS devices remain unverified at 60 fps.
