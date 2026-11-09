# Baten Kaitos I & II HD Remaster (`0100C07018CA6000`)

Update 1.0.3 (v196608). Unity 2021.3.27f1 IL2CPP on NVN, SDK 16.2.0. The base and update NSPs each carry three programs in one application.

| Program | Contents | Merged RomFS | Shaders |
|---|---|---:|---:|
| 0 | Collection launcher | 62 files, 139.8 MiB | 595 |
| 1 | [Baten Kaitos I](../0100C07018CA6001/README.md) | 3,961 files, 3.32 GiB | 27,254 |
| 2 | [Baten Kaitos II](../0100C07018CA6002/README.md) | 4,233 files, 2.85 GiB | 23,764 |

The launcher's `programs` row makes the NSP script pack all three in one app, with separate modules, RomFS images and shader libraries. The launcher calls `nn::oe::ExecuteProgram` to enter a game. The Mac host switches to that program's profile and resources using [the program table](../../../docs/reference/services.md#multi-program-applications-oeexecuteprogram). A provisional build of program 0 alone packaged the menu in about 265 MB and compiled only its 595 shaders.

## What it needs

The NPDM declares 16 MiB of system resource. Each program gets a 5 GiB reservation and a 4.38 GiB alias region. The games use a 1 GiB application pool so their own allocator has room in the alias region. All three set NVN's depth mode to zero at startup, which the profile records for offline pipeline builds. The three programs use the same SDK and movie modules and have separate main build IDs.

## Mac build

Give the NSP script the base and update NSPs and select `--platform mac-app`. Use the collection title ID `0100C07018CA6000` if the folder holds other games. For bring-up, `--program 1` or `--program 2` builds that game separately.

Tested on an Apple silicon Mac with Xcode 27.2. The 9.0 GiB app contains all three RomFS images and all 51,613 shader entries. A headless run of the signed bundle used `DM_CTL` to switch from the launcher into each game and back after the first presented frame. Each program mounted its own RomFS and loaded its own shader map. Both games were tested from the title screen through New Game and their first room.

For a headless New Game test, set `DM_KBD=Player` for the name prompt. Choose Yes with Left, then press A. The ordinary Mac app presents the software keyboard. On first launch, the collection menu prompts the player to review its terms.
