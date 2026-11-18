# Survey: resident evil 0 (010097000BC10000)

- display version: 1.0.0
- base: `Resident Evil 0.xci` (13.89 GB)

## Code modules

| module | what | build ID | text / ro / data (MB) | bss (MB) |
|---|---|---|---|---|
| main | the game | `fb4239aa962b429b3d1776af92d93e79` | 10.0 / 8.0 / 0.5 | 9.2 |
| rtld | rtld | `6574c7e3a1069ff7c8f9a2523cd3566b` | 0.0 / 0.0 / 0.0 | 0.0 |
| sdk | nnSdk | `26ffb122262ca25e4c016c0af2f92a29` | 4.9 / 5.8 / 0.4 | 1.0 |
| subsdk0 | NintendoSDK_movie | `41097c438a36afd2e879fa6f5e0f0850` | 3.3 / 2.4 / 0.2 | 1.1 |

- main.npdm program ID: `010097000BC10000`
- system resource: 0 MB
- SDK: 5_4_0 (Mario: 16_2_0 / 16_2_12)
- SDK components: NintendoSDK_NVN-5_4_0, NintendoSDK_libz-5_4_0, NintendoSDK_movie-5_4_0, NintendoSdk_nnSdk-5_4_0
- engine and API markers: NVN (main, sdk, subsdk0)

## RomFS: 11903 files, 13.48 GB

| top level | files | GB |
|---|---|---|
| nativeNXx64/scene | 4636 | 0.03 |
| nativeNXx64/effect | 2778 | 0.03 |
| nativeNXx64/arc | 2670 | 9.98 |
| nativeNXx64/ui | 670 | 0.24 |
| nativeNXx64/sound | 550 | 0.27 |
| nativeNXx64/scheduler | 208 | 0.00 |
| nativeNXx64/sa | 175 | 0.26 |
| nativeNXx64/event | 152 | 0.00 |
| nativeNXx64/movie | 44 | 2.68 |
| nativeNXx64/motion | 9 | 0.00 |
| nativeNXx64/system | 6 | 0.00 |
| nativeNXx64 | 3 | 0.00 |
| nativeNXx64/shader | 1 | 0.00 |
| nativeNXx64/shortcut | 1 | 0.00 |

| header magic | files | GB |
|---|---|---|
| MT Framework ARC | 2845 | 10.24 |

extensions: `.arc` 2845, `.mrl` 1716, `.mod` 1715, `.efl` 1263, `.tex` 834, `.evc2` 550, `.epv` 464, `.stef` 460, `.opus` 432, `.sbc` 329, `.package` 238, `.sdl` 228, `.bscn` 220, `.bes2` 152, `.stqr` 118, `.efcc` 114, `.msd` 84, `.gui` 54, `.mp4` 43, `.ean` 18

## Rewrite-site census (insn_census.py)

```
extract/ExeFS/main: 2627443 instructions decoded (119 undecodable words)
  x18 use                4184
extract/ExeFS/sdk: 1285450 instructions decoded (270 undecodable words)
  x18 use                10354
  svc                    60
  mrs tpidrro_el0        54
  mrs cntpct_el0         34
  msr tpidr_el0          4
  mrs tpidr_el0          2
  svc numbers: #1 x1, #2 x1, #3 x1, #4 x1, #5 x1, #6 x1, #7 x1, #8 x1, #9 x1, #0xa x1, #0xb x1, #0xc x1, #0xd x1, #0xe x1, #0xf x1, #0x10 x1, #0x11 x1, #0x12 x1, #0x13 x1, #0x14 x1, #0x15 x1, #0x16 x1, #0x17 x1, #0x18 x1, #0x19 x1, #0x1a x1, #0x1b x1, #0x1c x1, #0x1d x1, #0x1f x1, #0x21 x1, #0x22 x1, #0x23 x1, #0x25 x1, #0x26 x1, #0x27 x1, #0x28 x1, #0x29 x1, #0x2a x1, #0x2c x1, #0x2d x1, #0x2f x1, #0x32 x1, #0x33 x1, #0x34 x1, #0x35 x1, #0x40 x1, #0x41 x1, #0x43 x1, #0x44 x1, #0x45 x1, #0x48 x1, #0x49 x1, #0x4b x1, #0x50 x1, #0x51 x1, #0x52 x1, #0x53 x1, #0x70 x1, #0x72 x1
extract/ExeFS/subsdk0: 853238 instructions decoded (316 undecodable words)
  x18 use                8453
```

## Imports vs host hooks (nso_syms.py)

```
HLE hooks in host: 1291 | present in new game sdk exports: 758
main imports: old game 0, new game 545, shared 0, new-only 545

new game main imports by area (total / not imported by old game):
  nn::audio                 93 /   93
  nn::hid                   71 /   71
  C / libc                  66 /   66
  nn::os                    64 /   64
  nn::fs                    30 /   30
  other C++                 27 /   27
  nn::nfp                   26 /   26
  nn::oe                    21 /   21
  nn::web                   21 /   21
  nn::account               20 /   20
  nn::socket                16 /   16
  nn::irsensor              15 /   15
  nn::friends               10 /   10
  nn::vi                     9 /    9
  nn::ngc                    8 /    8
  nn::err                    7 /    7
  nn::mem                    7 /    7
  nn::nifm                   7 /    7
  nn::swkbd                  7 /    7
  nn::time                   6 /    6
  nn::codec                  3 /    3
  nn::ldn                    3 /    3
  nn::settings               2 /    2
  nn::cstd                   2 /    2
  nn::aoc                    2 /    2
  nvn                        1 /    1
  nn::util                   1 /    1

examples of new-only imports per area:
  nn::audio: _ZN2nn5audio12IsVoiceValidEPKNS0_9VoiceTypeE, _ZN2nn5audio13AcquireSubMixEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeEii, _ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc, _ZN2nn5audio13ReleaseSubMixEPNS0_19AudioRendererConfigEPNS0_10SubMixTypeE
  nn::hid: _ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj, _ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj, _ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj, _ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj
  C / libc: __cxa_guard_acquire, __cxa_guard_release, __cxa_pure_virtual, __errno_location
  nn::os: _ZN2nn2os10ClearEventEPNS0_9EventTypeE, _ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE, _ZN2nn2os11GetHostArgcEv, _ZN2nn2os11GetHostArgvEv
  nn::fs: _ZN2nn2fs10CreateFileEPKcl, _ZN2nn2fs10DeleteFileEPKc, _ZN2nn2fs10RenameFileEPKcS2_, _ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE
  other C++: _ZN2nv18InitializeGraphicsEPvm, _ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_, _ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_, _ZN5movie11SetCoreMaskEm
  nn::nfp: _ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE, _ZN2nn3nfp10InitializeEv, _ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii, _ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE
  nn::oe: _ZN2nn2oe10InitializeEv, _ZN2nn2oe16EnableScreenShotEv, _ZN2nn2oe16GetOperationModeEv, _ZN2nn2oe17DisableScreenShotEv
  nn::web: _ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE, _ZN2nn3web14ShowWebPageArg14SetCallbackUrlEPKc, _ZN2nn3web14ShowWebPageArg16SetLeftStickModeENS0_16WebLeftStickModeE, _ZN2nn3web14ShowWebPageArg17SetBackgroundKindENS0_17WebBackgroundKindE
  nn::account: _ZN2nn7account10InitializeEv, _ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE, _ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE, _ZN2nn7account12AsyncContext9GetResultEv
  nn::socket: _ZN2nn6socket10InitializeEPvmmi, _ZN2nn6socket10SetSockOptEiiiPKvj, _ZN2nn6socket11GetSockNameEiP8sockaddrPj, _ZN2nn6socket12GetLastErrnoEv
  nn::irsensor: _ZN2nn8irsensor10InitializeERKNS0_14IrCameraHandleE, _ZN2nn8irsensor17GetIrCameraHandleERKj, _ZN2nn8irsensor17GetIrCameraStatusERKNS0_14IrCameraHandleE, _ZN2nn8irsensor18RunMomentProcessorERKNS0_14IrCameraHandleERKNS0_21MomentProcessorConfigE
  nn::friends: _ZN2nn7friends10InitializeEv, _ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE, _ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE, _ZN2nn7friends14ShowFriendListERKNS_7account3UidE
  nn::vi: _ZN2nn2vi10InitializeEv, _ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE, _ZN2nn2vi12CloseDisplayEPNS0_7DisplayE, _ZN2nn2vi12DestroyLayerEPNS0_5LayerE
```

## What it means for a port

- modules besides main / sdk: subsdk0 (NintendoSDK_movie); the provisional profile loads main subsdk0 sdk (load order main, subsdk*, sdk)
- MT Framework ARC archives (Capcom, The Great Ace Attorney Chronicles): the shaders are in a shader package (SPK) inside one of them, which shader_split.py reads (mtarc_members, mtspk_shaders)
- SDK 5, not Mario's 16: NVN object sizes, API version and service layouts differ by SDK (the host was converted from D2R's SDK 12 to SDK 16 sizes); check them one by one per docs/reference/nvn-objects.md

## Provisional profile keys (dm/games/profile_guess.py)

What the game files decide; a game with virtual address memory that sets a heap gets a heap check on the Mac (scripts/build_ipa_from_nsp.py heap_check).

```
modules	main subsdk0 sdk	# every module main imports from
mem_reserve	0x100000000	# main sets a heap of at most the pool − 64 MB: a 3328 MB heap region
mem_alias	0x10000000
mem_stack	0x10000000
app_pool	0xCD500000
sys_resource	0x0	# the NPDM declares 0 MB of system resource: IsVirtualAddressMemoryEnabled is false, as on Horizon (Big Brain Academy: Brain vs. Brain sets a heap only then)
```

Next steps: docs/agents (02 survey, 03 bring-up, symptoms.md for each failure).
