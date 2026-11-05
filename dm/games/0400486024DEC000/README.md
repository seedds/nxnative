# The Legend of Zelda: Ocarina of Time (`0400486024DEC000`)

Switch 2, update v131072 over base v65536. Nintendo EPD's engine (`sead` / `agl` / `gsys`, Phive, Havok and CRIWARE), SDK components at 22.2.8, on NVN2. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`. **The Mac host reaches the frame loop and renders the AV1 opening through the selected Metal shaders. Gameplay has not been reached or validated.**

The ordered rebuild and validation checklist is in [`PLAN.md`](PLAN.md). The NUP and every extracted or compiled game artifact stay in the private external work folder.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `pad_layout full` | [`game.tsv`](game.tsv) | a 3D game with camera control on the right stick |
| `render_cond on` | [`game.tsv`](game.tsv) | OOT's agl passes use conditional rendering; PR [#1299](https://github.com/seedds/nxnative/pull/1299) found that running the main opaque pass unconditionally left its `EQUAL` depth test failing across the scene |
| `mem_used 0x6800000` | `game.tsv` | the rewritten `main`, `subsdk0` and `sdk` need 104 MB, above the 64 MB default |
| Provisional `mem_reserve 0x650000000`, `mem_alias 0x400000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | the NPDM declares 48 MB of system resource, so virtual address memory is on, and `main` imports `SetMemoryHeapSize`; the first heap and address-region checks must replace this starting layout with measurements |

The selected modules are 64-bit ARM. Their registered build IDs are `main` `e0f3ccfa…`, `subsdk0` `b0110087…` and `sdk` `1643a838…`.

## Files

- One private NUP container of 24 GB: a base record at v65536 and update v131072, no DLC. The update has its own ExeFS and a whole RomFS of 26,236,422,768 bytes and 186,583 files. The NUP carries a name record but no NACP or icon, so the app takes the profile's name and has no source icon.
- RomFS: 102,185 `.zs` files (16.46 GB), 46,324 `.extm` files (5.71 GB), 26,378 `.bwav` files (2.03 GB) and 26 WebM movies (1.64 GB). The largest file is the 404 MB staff-credit movie.
- The largest trees are `TerrainArc`, `PreComputedCulling`, `Tex`, `Sound`, `Bake`, `Phive`, `Voice`, `Pack` and `Model`. `Pack/ZsDic.pack.zs` is the profile's marker and holds the zstd dictionaries.

## Static survey

- `main`: 63,201,744-byte text, build `e0f3ccfaf50065ae4ea01eb5aeb13b1a`, 1,183 imports. The instruction census found 50,390 uses of `x18` and 1,264 reads of `tpidr_el0`.
- `subsdk0`: build `b0110087a7b339ef74588d3bd0b4dd5d`; the census found 3,369 uses of `x18`.
- `sdk`: build `1643a838814f9c10c696f930ed681e7f`; the census found 25,435 uses of `x18`, 441 reads of `tpidrro_el0`, 57 SVC stubs, 45 counter reads and five thread-pointer reads/writes.
- The SDK components name NVN, G3d2, UI, FreeType, HarfBuzz, NPLN, libcurl and the movie library at 22.2.8. The code names `sead`, `agl`, `gsys`, Havok and CRIWARE.

## Rewrite

- All three selected modules rewrite successfully. The rewriter accounts exactly for every instruction-census site: `main`'s 50,390 `x18` uses, `subsdk0`'s 3,369 and `sdk`'s 25,435, plus the thread-pointer, timer and SVC sites above.
- The SDK-wide function matcher found the SDK 22.2.8 `AddressRegionManager` constructor and cut its node area from 1 GB to 62 MB at `sdk+0x16ba48` and `sdk+0x16bad4`. Its observation gates for address regions and `StandardAllocator` also matched.
- The matcher for SDK 20-style 2 MB alignment of address regions found no implementation in this SDK. Nothing was patched for it; the address-region check will show whether this version needs a new general implementation.
- The rewritten modules require `mem_used 0x6800000` (104 MB).

## Shaders

- The named archives are `Shader/*.Nin_Ounce_NVN2.bfsha.zs`, `Shader/ApplicationPackage.Nin_Ounce_NVN2.release.sarc.zs` and `Layout2Shader/MergeShader.Product.100.Nin_Ounce_NVN2.bfsha.zs`. They are BNSH inputs with Ampere code, compressed through the RomFS's zstd dictionaries.
- The v131072 RomFS produces 42,851 shader stages: 4,123 compute, 500 mesh, 22,356 pixel, 20 task and 15,852 vertex. Their ordinary BNSH source groups are joined by 22 compute stages from `Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs`.
- The checked-in discovery tables contain all 42,851 stages, 4,123 compute-shader facts, 26,894 variation pairs and 6,296 candidate pipeline pairs. The split binary package in the private work folder is an intermediate input, not a compiled Metal library.
- [`shader_seen.tsv`](shader_seen.tsv) records boot coverage only. Eleven program keys first dumped at run time are compute shaders in the movie's `NDVE` container, whose embedded `NDSR` block holds 22 NVN2 shaders. `shader_split.py` now parses that format, and all eleven keys resolve through the regenerated map.
- The selected Mac build produced 16,885 Metal libraries in the current chain directory. Its selected pass reported 3,493 successful shaders (3,471 cache hits and 22 rebuilt), 697 generated variants and 134 compiled variants, with no translation failure or undecoded binding slot. It is deliberately not stamped as a full 42,343-stage build; the host therefore rejects `c1_full.tsv` and `cb_extent.tsv` until a full `dmsh_build.py` run completes.

## Open

- Run and stamp the full Metal shader build, then expand coverage from gameplay.
- Reach the first controllable scene and make a reproducible title/gameplay route.
- Measure the heap request and address regions, then replace the provisional memory layout.
- Validate services, input, audio, movies, saves, rendering, shader coverage and performance through gameplay.

## First Mac boot

- The first diagnostic host build loaded all three rewritten modules, completed `nnosInitialize` / `nninitStartup`, entered `nnMain`, mounted all 187,182 packed RomFS entries and initialized an NVN 231.40 Switch 2 device on Metal. It requested a `0x18a600000`-byte heap (6.166 GiB).
- With an empty Metal cache it discovered 2,324 shader keys before the run stopped; missing libraries were expected in this discovery run and are the input to the first selected shader build.
- The first real abort was Result `0xa99` in SDK 22.2.8's `audio::detail::ListVirtualInputDevices` (`sdk+0x1f61ac`), after it tried to open the Switch 2 `auddmg:u` service. The host now answers the public virtual-input list as empty, consistent with its existing no-microphone AudioIn behavior.
- Resolved startup regression after integrating the shared input-stream implementation: both it and the original port registered `ListVirtualInputDevices`, so the loader aborted with `duplicate entry in the forced takeover list` before SDK initialization. The port now uses the single shared input-stream handler with the same empty-device result.
- The next abort came from the title's bundled gRPC initialization. It probes `nn::socket::EventFd(0, 0)` before selecting its `poll` engine; the offline host returned `ENETDOWN`, disabled that engine and then had no poll strategy. The host now provides the local event counter, read/write/close and poll semantics without enabling network sockets. The next run created the `npln-grpc` timer and executor threads normally.
- A five-second discovery run passed the service boundary, decoded the 1920x1080 AV1 opening, created the resource/scene/decompression/presentation/background workers, and reached frame 86. The watchdog stopped a live process; there was no game abort.
- The title calls `nvnCommandBufferSetComputeWorkloadPriority`, a GPU scheduling hint new to this NVN2 surface. The host has one ordered Metal queue, so it accepts the hint as an empty state operation. An empty provisional `pso_manifest.tsv` also keeps device prewarm state readable until real first-use rows can be recorded.
- With the selected library, all eleven NDVE compute keys load. A cold pipeline archive spent about 27 seconds compiling the first four movie compute pipelines; the warm archive has no compilation stall and renders the opening at about 35 ms GPU time per frame on this Mac.
- Early captures were black because the movie itself encodes black through about timestamp 1.68 s. A decoder trace showed two one-picture pools registered at image index 0, each decoded image delivered to and read from the matching pool, and changing luma fingerprints after the encoded black lead-in. Captures at presented frames 240 and 300 show the night landscape and moon, confirming AV1 decode, NV12 upload, all NDVE compute passes and final presentation together.
- The NDVE raw-pointer stores originally overflowed the 1,024-page write log on every dispatch, leaving all GPU writes conservative. The general log now holds 8,192 exact pages: it learns the movie output's one pool over 11 dispatches with no overflow. On a warm archive this lowers the opening's GPU time from about 35 ms to 15 ms per frame (frames 300–600: p50 15.30 ms, p95 15.64 ms); presentation stays at about 16.67 ms. The chain change's first compute-pipeline build took 42.3 seconds once.
- Decoder input reaches ordinary protocol back-pressure at the 12-picture host queue during startup. The game handles the returned busy result, retries, and continues releasing and reading pictures; this is no longer reported as a red fault. The remaining red diagnostics are the intentionally unstamped partial shader library and its missing generated constant-buffer tables.

## Found on this game

- [No audio input devices](../../../docs/reference/audio.md#no-audio-input-devices): SDK 22's virtual-input list opened `auddmg:u` and aborted when no microphone service existed.
- [Local `eventfd` while network sockets are offline](../../../docs/reference/services.md#local-eventfd-while-network-sockets-are-offline): the bundled gRPC runtime disabled its only poll engine when the offline host rejected its local event counter.
- [Global memory at an address loaded from memory](../../../docs/reference/shaders.md#global-memory-at-an-address-loaded-from-memory): seven NDVE compute shaders write image surfaces through loaded pointers, exceeding the original 1,024-page write log on every dispatch.
