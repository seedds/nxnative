# NVN objects

NVN object layouts, memory pools, command memory, queues, syncs and entry points (`dm/gfx/nvn/hle_nvn.c`). Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Object sizes

### Host records must fit the game's SDK object sizes

The game allocates NVN objects with the sizes of *its* SDK's headers, which shrank in newer SDKs. A host record larger than the game's allocation overwrites neighbouring memory (memory corruption):

- **Symptom:** heap corruption; a stack smash where a function returned to `0x100000000`; a crash in SHA-256 on the code address 0x16300.
- **Cause:**
  - `NVNtexture` is 0x80 bytes in SDK 16 (heap corruption: the old 0x88 record overwrote the next heap block's links).
  - `NVNtextureView` is 16 bytes in SDK 16 (stack smash: a 16-byte local next to the saved frame pointer; the function returned to `0x100000000`).
  - `NVNprogram` is smaller in Mario than the 0xC0 bytes in `nvn.h`: growing the host record from 112 to 168 bytes corrupted the next object (a crash in SHA-256 on the code address 0x16300).
- **Fix:** keep `prog_t` at 112 bytes; per-program host data goes in a side table (`prog_key`).
- **Found on:** SDK 16 games (`NVNtexture`, `NVNtextureView`); [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (`NVNprogram`).
- **Check:** check every host object against the new SDK: `_Static_assert(sizeof(host) <= N)` next to the struct, and when a crash returns to a nonsense address, look at the 0x100 bytes below `sp` in the crash report for a zeroed frame.

### Inlined SDK code writes into `NVNcommandBuffer` +0x10

The game may also write into an NVN object directly, without an NVN call, through code inlined from the SDK headers. Host records stored in place must leave those bytes alone (the same holds for recorded command memory: [Recorded commands patched in place by the game (`OP_BIND_TEX1`)](#recorded-commands-patched-in-place-by-the-game-op_bind_tex1)).

- **Symptom:** out-of-range vertices, NaN, a black band and flicker; seen on the iPhone, while Mac runs of the same scene looked clean.
- **Cause:** Mario writes the byte 1 at `NVNcommandBuffer` +0x10 (`main+0x4da5c`, `main+0xb8dec`). In the host record it was the low byte of `end`. Each write shrank the game's 512-byte material setup blocks, so their recordings overflowed into host segments; `GetCommandMemoryUsed` (then `cur − begin`) returned 0, the game marked the setups empty and skipped calling them, and the draws ran with the previous draw's vertex buffers ([What `GetCommandMemoryUsed` counts](#what-getcommandmemoryused-counts)).
- **Fix:** `cmd_t` reserves the byte (`nvn_flag10` in `nvn_obj.h`).
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md).
- **Check:** to find such writes, put the field you suspect behind a guard value and log when it changes outside the host's own code, or force overflows on the Mac by shrinking the command memory the game gets.

### Guest ring allocators write `NVNbuffer` offset and size directly

- **Symptom:** a running game with sound draws a black screen; every uniform buffer returns the same initialized address, with its allocation offset reported as its size.
- **Cause:** an engine can retarget an initialized buffer with direct 64-bit stores to `NVNbuffer` +0x18 (offset) and +0x20 (size), then bind `nvnBufferGetAddress` and `nvnBufferGetSize`. The host previously stored offset at +0x10 and size at +0x18: the guest's offset overwrote the host's size while its address stayed unchanged.
- **Fix:** `buf_t` reserves the bytes before +0x18 and puts offset and size at the guest-written offsets, with compile-time assertions. Initialization and every buffer query use that record; CPU mapping still respects the pool's CPU-access flags. The record fits the existing 0x30-byte `NVNbuffer`; the builder layout and recorded command layouts are unchanged. No meaning is assigned to the reserved driver bytes.
- **Found on:** [LEGO Star Wars: The Skywalker Saga, issue #723](https://github.com/seedds/nxnative/issues/723), 1.10.0, SDK 12.3.6, main build `ec593a5f9552100a9fe95c4ca9e34873` (reporter's trace). This is a general object ABI correction; no game profile is needed.
- **Check:** `t_nvn.c` initializes a buffer through the public API, retargets it repeatedly with raw byte writes, and checks address, size, mapping, pool and offset queries, builder independence and neighbouring-object guards. It also retargets a CPU-inaccessible buffer. Validation uses synthetic memory only; the reported game's rendering has not been retested.

### Sizes the hardware never reports as 0 (ZCull save area)

- **Symptom:** sprites broke apart in some runs only, depending on the allocation order; buffers overlapped.
- **Cause:** host answers that size the game's own allocations count too. A size the hardware never reports as 0 must not be 0 on the host, even where the host needs no memory: Cuphead (Unity 2017.4) allocates each depth target's ZCull save area from its NVN pool allocator, which gives a zero-size request the offset of a live block. Its map of allocations then lost that block, and freeing it later released a garbage-sized range.
- **Fix:** the host never reports a ZCull storage size of 0 (`hle_nvn.c` `zcull_size`).
- **Found on:** [Cuphead](../../dm/games/0100A5C00D162000/README.md). Found by tracking the allocator from observation gates (`OBS` in its `patches.py`) on its free-list push and after its map insert, checking at every event that each block allocated so far was still in the game's map.

## Memory pools

### Heaps cut from a pool at alignment relative to its GPU address

- **Symptom:** a null write as soon as the main thread starts recording a command buffer (13 Sentinels: a constructor at `main+0x2fa9a0` runs on `this = 0` when the prologue enters its first battle).
- **Cause:** the object came from one of the engine's ring heaps (`main+0x2d83ec`), and that heap had base 0. At startup the engine takes one GPU memory pool, sized as exactly the sum of three ring heaps, and cuts the heaps out at 64 KB alignment *relative to the pool's GPU address* (`main+0x30c118`). On the Switch a pool's GPU address is big-page aligned; the host's GPU address is the CPU address, and the storage came from the game's `malloc` rounded only to 4 KB. Up to 60 KB was lost at the front, the third heap (16 MB) did not fit, and the sub-allocator returned 0.
- **Fix:** the game's `malloc` is `nn::mem::StandardAllocator::Allocate(size)`; on this build the host first routed allocations of 64 KB or more to `Allocate(size, 0x10000)` (`hle_os.c`). The routing is now the profile key `alloc_big_align` (`hle_os.c` `h_mem_SAAllocate`), which [Unicorn Overlord](../../dm/games/010069401ADB8000/README.md) (the same engine) sets too.
- **Found on:** [13 Sentinels: Aegis Rim](../../dm/games/01003FC01670C000/README.md). Probing the heap object at the allocation (`DM_PROBE=main:2bec60@x0+8`) showed the zero base directly.

### `newBufferWithBytesNoCopy` retried under memory pressure

- **Symptom:** `[RED] memory pool … created at retry N`.
- **Cause:** a memory pool created while the game maps and unmaps a large region was sometimes refused once by `newBufferWithBytesNoCopy`, then accepted.
- **Fix:** `mtl_pool_add` retries up to 12 times with a short back-off and prints the red line; on a final failure it logs the VM regions around the pool.
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md) (its 4 GB region).

### Virtual memory pools mapped onto one physical pool

- **Symptom:** a buffer is created with a null pool right after a scene change.
- **Cause:** NVN *virtual* memory pools (flag 0x200) have no storage until `nvnMemoryPoolMapVirtual` maps physical pool pages in; if the host returns failure, the game treats the pool as unusable.
- **Fix:** a virtual pool is aliased onto the physical pool named by `MapVirtual` when all requests share one base (`hle_nvn.c`): Mario maps its single 128 MB virtual pool onto one physical pool in one request, so the host aliases it. Other mappings (true sparse) were then unimplemented; they are now [sparse virtual pools](#sparse-virtual-pools-built-from-separate-physical-pools) (next entry).
- **Note:** a null-pool buffer even with virtual pools working means the game's own heap ran out ([A null-pool buffer from a game heap that ran out](memory.md#a-null-pool-buffer-from-a-game-heap-that-ran-out)).
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (all requests share one base).

### Sparse virtual pools built from separate physical pools

- **Symptom:** `[RED] MapVirtual: non-contiguous mapping (true sparse) not implemented`.
- **Cause:** RE Engine's GPU heap is one 64 GB virtual pool built from separately allocated 4 MB physical pools, each mapped at a virtual offset of its own (45 at boot, over the first 180 MB). A virtual pool larger than the application pool cannot be one alias of a physical pool.
- **Fix:** such a pool gets an address range of its own at `MemoryPoolInitialize` (reserved, inaccessible: 64 GB on the Mac, at most 1 GB on iOS), and each mapping request `vm_remap`s the physical pages over its part of the range, so virtual addresses read and write the same memory, as the GPU's page table does. Each run of mapped pages is registered with the replay like a pool of its own and rebuilt when a request joins or splits runs (`vpool_*` in `hle_nvn.c`). Mario's 128 MB pool, mapped whole onto one physical pool, keeps the alias. The log reports the mapped high-water mark per 128 MB.
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md).

### Virtual pools of one texture each, mapped before and after the texture

- **Symptom:** `[RED] MapVirtual: non-contiguous mapping (true sparse) not implemented`, on small virtual pools; streamed textures black or missing (OCTOPATH TRAVELER's village ground and plants).
- **Cause:** Unreal Engine 4.18's NVN RHI gives each streamed texture a virtual pool of its own (0x20000 to 0x560000 bytes, flags 0x2a1), creates the texture in it before mapping anything, maps the mip tail from a 4 MB physical pool after writing its texels there on the CPU, and later maps the higher mips from other pages of that pool. Only pools larger than the application pool had [an address range of their own](#sparse-virtual-pools-built-from-separate-physical-pools); the others were aliased onto `physical − offset` at their first mapping, so the texture had no storage (its pool had none when it was created) and every later mapping at another base was refused.
- **Fix:** every virtual pool gets its reserved range at `MemoryPoolInitialize` (`vpool_reserve`; the sparse records are now one per pool, reached through `pool_t.vp`, without the old limit of 4). Its first mapping request decides: one that maps the whole pool from offset 0 keeps the alias (the range is given back; Mario's 128 MB pool), anything else makes the pool sparse, with the page table and the uploads through it ([next entry](#textures-read-through-a-4-kb-page-table-of-a-sparse-pool)).
- **Found on:** [OCTOPATH TRAVELER](../../dm/games/010057D006492000/README.md): 221 pools by the first village; the same route with the old alias left the ground, plants and props black.
- **Not checked:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (no game files on hand), whose pool should take the alias branch as before.

### The sparse tile layout of a texture

- **Symptom:** `[RED] NVN unimplemented function called: nvnTextureBuilderGetSparseTileLayout`, then an out-of-memory abort in Unreal's allocator (`Failed to allocate 1879113728 bytes from Standard Allocator`, on `DM_DBG=1`).
- **Cause:** the call fills `NVNtextureSparseTileLayout` {numTiledLevels, tile width, height and depth in texels, 16 reserved bytes}; Unreal Engine 4.18 sizes a sparse texture's mip tail as storage(all levels) − storage(numTiledLevels levels) and allocates it from its heap. Unimplemented, the count was stack garbage and the whole 1.75 GB texture counted as tail.
- **Fix:** `nvnTextureBuilderGetSparseTileLayout` and `nvnTextureGetSparseTileLayout` answer from the host's [sparse layout](rendering.md#sparse-textures-laid-out-in-64-kb-tiles) (`nvn_tex_sparse_tiles`: the levels at least one tile wide, high and deep).
- **Found on:** [OCTOPATH TRAVELER](../../dm/games/010057D006492000/README.md), at boot (`main+0x1f1a688`).

### Textures read through a 4 KB page table of a sparse pool

- **Symptom:** `CRASH sig=10` in `memmove` from `pkg_upload` → `nvn_deswizzle`, at an address 32 GB into a sparse virtual pool; textures with unaliased tails came out magenta.
- **Cause:** the game's streamed textures (flag 0x20, packaged layout) live 32 GB into its virtual pool; it writes their tiles on the CPU into 4 MB physical pools and then maps them, with no GPU copy. NVN maps at 4 KB and the host aliases at its 16 KB page, so a request whose offsets differ modulo 16 KB cannot be aliased: the ASTC 6x6 textures' mip tail starts 0x81000 into the texture.
- **Fix:** every request also goes into a 4 KB page table (`vpt_*` in `hle_nvn.c`: the physical page behind each virtual page and the generation of the request that set it). The replay reads a sparse pool's textures through it (`nvn_sparse_copy`, unmapped pages as 0) and uploads one again only when a mapping in its range changed or the CPU wrote a physical page behind it (`nvn_sparse_since`; the physical pools are dirty-tracked). Before that the untracked alias counted as written on every look: 32 textures, 18 MB, were uploaded again every frame, and those with unaliased tails came out magenta. Plain sampled textures in the pool go the same way: [Sampled textures in a sparse pool uploaded through the page table](#sampled-textures-in-a-sparse-pool-uploaded-through-the-page-table).
- **Note:** requests beyond the reserved range (iOS reserves 1 GB: the buffers stay under 0x20100000 through the opening) are kept in the table only, like unaligned ones: textures can read them, the GPU cannot address them.
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md).

### Large sparse textures as Metal placement sparse textures

- **Symptom:** `kIOGPUCommandBufferCallbackErrorOutOfMemory` on every command buffer from the first frames, no frame presented, "GPU allocated" near the Mac's memory; `large texture … in a sparse virtual pool` lines name textures of gigabytes.
- **Cause:** a sparse texture in a sparse virtual pool holds only what the game maps into it, but the host made each one a whole Metal texture and uploaded all of its storage through the page table ([Textures read through a 4 KB page table of a sparse pool](#textures-read-through-a-4-kb-page-table-of-a-sparse-pool)), again on any change. Six terrain arrays of 8192x8192x68 with 12 levels (two BC6H of 5,803 MB, four ASTC 8x8 of 1,451 MB) took more memory than a 16 GB Mac has.
- **Fix:** such a texture becomes a Metal placement sparse texture with 64 KB pages (`nvn_mtl.m` `sparse_make`, `sparse_sync`), costing no memory until a tile is mapped. NVN's 64 KB tile is the standard shape that Metal's is (256x256 texels of 16-byte blocks, 512x512 of ASTC 8x8, 128x128 of RGBA8, measured on an M6), and both put the mip tail at the first level smaller than a tile. A tile the game has mapped in full (`hle_nvn.c` `nvn_sparse_tile_state`) gets a 64 KB tile of a 256 MB placement heap and its own 64 KB decoded (`nvn_deswizzle_tile`: a tile is one block row of `128 >> block height` GOB columns, at the level's start + (row × tiles across + column) × 64 KB). A tile the game unmaps gives its heap tile back once the GPU is past the submission. The pool's mapping changes reach the texture through a log of changed ranges (`hle_nvn.c` `nvn_sparse_log_read`), so a frame with none costs one comparison, whatever the size of the pool. A tile whose physical pages the CPU writes is uploaded again: pages of sparse pools carry a flag (`nvn_dirty_protect_sparse`), a write to one counts (`nvn_dirty_sparse_writes`), and only then, at most once a second, are the texture's mapped tiles compared. A tile of an upload batch the host had to drop is uploaded again the same way. Render targets and images in a sparse pool are placement sparse too; they take their usages up front, since a whole copy made later would lose the tiles (`tex_upgrade` refuses one). Each layer's tail is one Metal tile, decoded from its own bytes (`nvn_deswizzle_from`). The mapping operations reach the queue through the record stream, ahead of the upload batch that fills the tiles (`nvn_rec.h` `rec_queue_block`). Only a 2D texture or 2D array that would take 64 MB or more made whole goes this way, whose tiles match Metal's: smaller ones keep the whole upload and [sparse_view's level clamp](rendering.md#sparse-textures-bound-from-their-first-mapped-level). `DM_NVN_SPARSE_TILES=0` turns it off.
- **Found on:** [Mario Kart World](../../dm/games/0400C3F00006E000/README.md)'s base game: by its flyover's first minute, the six arrays and a 6144x6144x2 RGBA8 one map 21,796 tiles (1.36 GB) where made whole they took 8.7 GB, and the flyover presents. A race then ran at a median of 96 to 117 ms: the whole-range walk of the page table each texture lookup took half the submit thread (the log above), and a whole copy of one texture took 389 MB; with both fixed and [large pools in chunks](#large-pools-wrapped-in-chunks), a median of 16.7 ms. Of 42,072 tiles mapped by the first race, none was rewritten in place.
- **Check:** `t_sparse_layout` decodes every tile and the tail of sparse BC6H and RGBA8 textures on their own and compares them with the whole level, in both GOB orders. `t_sparse_tiles` goes through the real NVN entry points: mapped tiles and the tail hold what their physical pages hold, an unmapped tile gives its heap tile back unless the GPU drew into it ([Physical pages mapped at several places of a placement sparse texture](#physical-pages-mapped-at-several-places-of-a-placement-sparse-texture)), a tile rewritten in place is uploaded again, more requests than the log holds still end right, and a GPU-written texture gets tiles and no uploads.
- **Open:** resolved for drawn tiles of consecutive physical pages ([Physical pages mapped at several places of a placement sparse texture](#physical-pages-mapped-at-several-places-of-a-placement-sparse-texture)): their content stays with that memory. A tile of scattered pages, a tile cut by the level's edge, or the tail, still reads its old heap tile's content when a GPU-written texture maps it again.

### Physical pages mapped at several places of a placement sparse texture

- **Symptom:** the placement heaps (`sparse tiles: placement heap N`) keep growing through a session, to several times the memory the game maps behind its sparse textures. On a 16 GB M1 Pro, Mario Kart World's race reached 11 heaps (2.75 GB) within three minutes, wired memory 8.7 GB, and the system swapped: frames went from 25 to 50 ms. A GPU write through one place of a page is also not seen at another.
- **Cause:** the host gave every mapped place its own heap tile, but a game maps one physical page at several places at once. In a Mario Kart World race, 2,959 MB of mapped virtual pages sat on 976 MB of distinct physical memory, almost every page at about three places, always in textures of one format: RGBA8 SNORM 3072² and 6144² arrays (which the GPU draws into) with each other, BC6H and ASTC 8x8 terrain arrays within themselves. On hardware every place is the same memory.
- **Fix:** `nvn_mtl.m` `sp_tile`. A tile whose 16 pages are consecutive physical memory (`hle_nvn.c` `nvn_sparse_tile_phys`) takes the heap tile of that memory and format (`sp_ph_find`), and every place mapping that memory maps the same heap tile. What a new place does depends on whether the GPU has drawn into the slot, set when a texture mapping it is an attachment, a written image, or a copy or clear destination (`sparse_gpu_wrote`; `s->gpu` cannot say this, since a packaged placement texture can be drawn into too):
  - **Not drawn:** the slot is a copy of the memory. Each new place uploads it again, because the memory may have changed in ways no fault shows (the GPU writing it as a buffer), and it goes back with its last place (`sp_release`). The memory saving is the sharing; the upload rate is what it was with one tile per place.
  - **Drawn:** the slot holds what the memory does not. No new place uploads it unless the CPU wrote that memory since (`nvn_dirty_since` against the slot's stamp), and it stays with no place mapping it while its physical pool lives, as the memory would. A lost upload batch does not overwrite it.

  A physical pool's `Finalize` takes its slots out of the table at once (`mtl_sp_phys_gone`), so a new pool over the same memory never finds one, even one that a texture not looked at since still holds; a place still mapping it moves off at its next look. The content is read from the physical address the slot is keyed by, not through the page table, as the game can remap the place on its own thread during a look. The unmaps of a look are issued before its maps, so a tile moving to another slot is unmapped first. Metal reads one heap tile exactly through any place of any texture of the format (every texel, at seven positions and in a second texture, for RGBA8, RGBA8 SNORM, RGBA16F, BC6H and R8 on an M1 Pro; `t_sparse_tiles` also passes on an M6) only without lossless compression, which no placement sparse texture has. The tail, tiles of scattered pages and tiles cut by the level's edge (only part of such a tile is uploaded, and a whole tile of the same page would read the rest as undefined) keep a slot of their own.
- **Check:** `t_sparse_tiles`: one physical page mapped at two places is one heap tile and both hold its texels; a clear through one place shows at the other and marks the slot drawn; a drawn slot keeps its content with no place and shows it at a new one with no upload, until the CPU writes that memory; an undrawn slot goes back with its last place; a packaged place behind a GPU-written one reads the CPU's fill; a new place reads what the GPU wrote to the memory as a buffer; a cut tile has a slot of its own; the physical pool's Finalize gives the kept slots back, and a new pool over the same memory reads its own content.
- **Open:** once a texture is drawn into, every slot it maps then counts as drawn, whichever tiles the draw touched. A page the CPU filled before a GPU-written place of a drawn texture mapped it can read undefined texels at a packaged place of the same memory, as the host cannot tell it from a drawn page. A dropped upload batch is redone only for the texture that issued it (`up_lost_take`), so another place sharing its tile waits until that texture is resolved again.
- **Found on:** [Mario Kart World](../../dm/games/0400C3F00006E000/README.md): over five minutes of the same race on a 16 GB M1 Pro, 4 placement heaps (1 GB; 13,041 of 13,449 tiles shared) where there had been 11 by three minutes, wired memory 6.9 to 7.1 GB with no swap growth where it had been 8.7 GB with the system swapping, and the frame median steady at 25 to 28 ms where it had risen to 50 ms. Two Grand Prix in one session (about 30 minutes) played with the heaps at 1 GB throughout, wired memory 7.0 to 8.6 GB and swap up about 1.2 GB, then steady.

### Large pools wrapped in chunks

- **Symptom:** frames of 100 ms and more every few frames on a Mac short of memory, with no failing command buffer; a Metal System Trace shows the driver wiring and unwiring all the time (3.2 s of a 6 s trace on the game below), and the `restop` line of `DM_CTL` shows gigabytes of shared buffers.
- **Cause:** Metal wires a zero-copy buffer whole while it is in the residency set, so one buffer over a large pool pins all of it, whatever the GPU reads. A pool's pages are guest memory, already resident only in part, and wiring the rest pushes other memory out.
- **Fix:** a pool of 512 MB or more has no buffer of its own (`nvn_mtl.m` `big_lookup`). A lookup gets a chunk: a zero-copy buffer over the 16 MB stride holding its address and on to the end of what it asked for, made when first asked for, so only what lookups reach is wired. The chunks overlap, as adjacent pools' buffers do at shared pages (`MTL4VisibilityOptionResourceAlias` is on while one exists). GPU addresses are not contiguous across chunks, so vertex, uniform and direct index lookups now pass their real lengths; one that does not know its length (an indirect draw's index buffer, a raw storage address) gets 64 MB. An upload reads from a buffer of its own held by the upload batch (`up_big_src`), so a one-time read leaves no chunk wired. Raw global memory, whose pool table the shader walks by itself, takes a whole-pool buffer (`gmem_map_gpu`). `DM_NVN_POOL_CHUNKS=0` turns it off.
- **Found on:** [Mario Kart World](../../dm/games/0400C3F00006E000/README.md)'s base game, on a 16 GB M6: the GPU has touched 1,111 MB of its 2,427 MB graphics pool by the first race. Shared buffers in the residency set went from 3,864 to about 1,500 MB, wired memory stayed at 9.3 to 9.8 GB (it swung to 12.7 GB), and the 95th percentile frame of a race went from 33-85 ms to 17-33 ms.
- **Check:** `t_pool_chunks`: a lookup's buffer says the same memory as the guest address, a read of unknown length has 64 MB past it, a chunk asked for more is lengthened, a texture's upload wires no chunk, raw global memory's table covers the whole pool, the pool's removal takes every entry, and a smaller pool stays one buffer.

### Finalized pools kept alive by residency and the sharing table

- **Symptom:** GPU memory grows with every scene change (Engage's idle title demo reached 5.4 GB of GPU memory and 3.6 GB resident in 850 s).
- **Cause:** a game that frees memory pools on scene changes and builds new ones (Unity does; Mario never did in its first course) hit two host structures that kept freed pools alive: the residency set still held each pool's no-copy `MTLBuffer` after `nvnMemoryPoolFinalize`, and the storage-sharing texture table (`g_alias`, keyed by pool object, offset and description) never evicted, so the old pools' render targets stayed, and linear textures built on a pool buffer kept the whole buffer alive.
- **Fix:** pool buffers are retired through deferred release, and finalizing a pool evicts its sharing-table entries (`mtl_alias_drop_pool`).
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md): it now stays at 1.2 to 1.4 GB.
- **Note:** the `memory up to frame` line also reports `serial objects` (host objects by serial), `freed but alive` (finalized pool buffers still alive) and `shared textures` (sharing table size and evictions); growth in GPU memory with those counts flat points to an allocation outside them.

### Finalized pool buffers kept by a guest thread's autorelease pool

- **Symptom:** the memory line's `freed but alive` count only grows, while no host table holds those buffers; "GPU allocated" is far above resident memory (Ghost Trick: Phantom Detective, 125 to 146 buffers and 6.6 to 8.7 GB of GPU allocations against 2.2 GB resident).
- **Cause:** `mtl_pool_del` runs on the guest thread that finalizes the pool, whose implicit autorelease pool never drains, and adding the buffer to the weak table `g_dead_pools` (the table behind `freed but alive`) left it autoreleased there: every finalized pool's zero-copy buffer stayed alive for good, wrapping freed guest memory. A temporary census of every host table that can hold a texture over a pool buffer (by serial, finalized serials, the sharing table, texture ID views, view caches, the deferred queue, the residency set) found no holder at all. `containsObject:` and `removeObject:` on a weak `NSHashTable` do the same (a standalone test: the object outlived its last strong reference until its thread ended), and `res_add` / `res_remove` call them on guest threads too.
- **Fix:** `mtl_pool_del`, `res_add_at` and `res_remove` (`nvn_mtl.m`) each have their own `@autoreleasepool`, as `defer_drain` and the memory line's count already did ([Deferred-release drain without an autorelease pool](memory.md#deferred-release-drain-without-an-autorelease-pool)).
- **Found on:** [Ghost Trick: Phantom Detective](../../dm/games/010029B018432000/README.md): 0 buffers freed but alive with the fix; GPU allocated 1.7 GB instead of 8.3 GB 300 frames into the boot and 2.9 GB instead of 9.9 to 11.7 GB in Chapter 1, resident memory, frame times and pictures unchanged (zero-copy buffers add no memory of their own). [Pokémon Sword](../../dm/games/0100ABF008968000/README.md): 8 to 0 at the title after 90 s. Earlier Mac logs show the count in most games, growing with play: Animal Crossing: New Horizons 3,113 after 27,900 frames, Pokémon Sword 2,050, Super Smash Bros. Ultimate 1,279, Pokémon Legends: Z-A 932 (small pools there: 100 to 300 MB).

### Finalized textures kept by registered descriptor views

- **Symptom:** GPU and resident memory keep growing during texture streaming while pool, residency and serial-object counts stay nearly flat, with no freed pool buffers alive.
- **Cause:** texture descriptors copy their NVN texture metadata, so they remain readable after the API object's `Finalize`. Their cached Metal views, however, were released only when the descriptor ID was registered again. A game that streams textures into new IDs left those uploads retained indefinitely after their serial and shared-view entries had been removed.
- **Fix:** index descriptors by texture serial and retire their cached base, compatible-type and image views at `mtl_tex_release`, through deferred release behind the GPU fence. Preserve the descriptor metadata: a late read resolves it through `tex_mtl` and caches its views for that submission, then retires them at commit. `t_cpu_tex` checks multiple descriptors, ID replacement and pixel-correct recorded replay after finalization and API-object reuse.
- **Found on:** [Xenoblade Chronicles: Definitive Edition](../../dm/games/0100FF500E34A000/README.md#memory).

### Texture ID views revalidated only for the serials that changed

- **Symptom:** the submit thread spends milliseconds a frame in `pool_gpu` → `tex_by_id` → `tex_mtl` / `tex_get` (a `sample` of the process), and the `[nvn-mtl] pool for handles traced to no slot … built in X ms` line shows a rebuild of the texture pool table every frame at about 2 ms. Fire Emblem Fortune's Weave's battle map: 1.9 ms a build, three builds a frame, about 14 % of the main thread.
- **Cause:** a texture ID's cached view was valid while `g_ser_gen` stood still, and every change to the serial table bumped it. A game that changes a few serials a frame (the per-stage program ring in `bind_stages` does) therefore sent every registered ID down the slow path at the next lookup, and `pool_gpu` walks all of them (about 5,000 in Fortune's Weave). Packaged (CPU-written) IDs took the slow path again on any guest write anywhere.
- **Fix:** in `nvn_mtl.m`, for every game. A change to one serial's object bumps `g_ser_gen` through `ser_note`, which records the serial; ID views are valid for an epoch (`treg_ep`), and `ser_sync` invalidates only the IDs registered with a noted serial (`g_treg_serial`). A bump without a note (the sharing table, upgrades, a dropped upload), more than 64 notes, or any sparse mapping (`nvn_sparse_gen`) starts a new epoch, which re-checks every ID as before. A packaged ID whose pages nobody wrote since its upload stamp keeps its view (the test `tex_mtl_` makes), and `tex_fast_rid` returns the cached resource ID of a view `tex_by_id` kept. Other users of `g_ser_gen` (argument blocks, `pool_gpu`'s own key) are unchanged.
- **Check:** `make -C dm/gfx/tests GAME=<id> run` (`t_cpu_tex` among them). Fortune's Weave's battle map (975 draws a frame, MacBook Pro M1 Pro): pool build 1.9 → 1.2 ms, submit 52.5k → 42.8k instructions per draw.
- **Found on:** [Fire Emblem Fortune's Weave](../../dm/games/04007EF01F8AE000/README.md).

### Memory-pool tracking table grows in blocks

- **Cause:** the memory-pool tracking table had a fixed 4,096 slots; a pool past the end would have its textures re-uploaded at every use.
- **Fix:** it grows in blocks of 1,024 (`nvn_dirty.c`, up to 262,144).
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md): the phone reached 4,006 pools (found while fixing GitHub issue #9: [Deferred-release drain without an autorelease pool](memory.md#deferred-release-drain-without-an-autorelease-pool)).

### Dead texture serials kept in a ghost table

- **Symptom:** the serial table grows by about 10 entries a frame (276,000 after 25,000 frames).
- **Cause:** a game that records transient render targets and finalizes them before it submits: replay put the dead serials back into `g_by_serial`.
- **Fix:** dead serials go to a 512-entry table (`tex_put`, `g_ghost`; the memory line's `finalized textures still referenced`).
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md) (the HUD captures, the 640×360 buffers; found while fixing GitHub issue #9).

### A replay's own objects take serials above a capture's

- **Symptom:** `nvn_replay` aborts on a game's capture with `-[NvProg width]: unrecognized selector` (from `rt_dims`) or `-[NvProg mipmapLevelCount]`: a render target resolves to a program.
- **Cause:** a capture's textures and programs keep the live run's serials, while the replay layer's own objects (the combined program `bind_stages` makes when a game binds its shader stages from separate programs) took theirs from the replayer's counter, which starts at 1. Textures and programs share the serial table (`tex_get`, `serial_put`), so a combined program could take an early texture's serial and replace it there.
- **Fix:** `nvn_replay` raises the counter to 2^61 before it loads a capture (`nvn_serial_floor`); a live run's serials stay far below that, and external textures take 2^62 and up. `check_t_capture_live` binds two programs to separate stages to cover it.
- **Found on:** [Pokemon Scarlet](../../dm/games/0100A3D008C5C000/README.md) (frame 3000 of its regression route).

### A captured texture ID whose memory pool was finalized

- **Symptom:** `DM_NVN_CAPTURE` reports `a record names … (0 bytes), outside every pool … texture registry entry …` for some texture IDs, with addresses that look like data; one capture stopped with `a sparse virtual pool (step 1 does not capture page tables): the capture is incomplete`, another with `straddles a region`.
- **Cause:** the game finalizes a texture's memory pool, frees the pool object and reuses its memory, and leaves the texture ID registered. The host marks such an ID `gone` (`treg_drop_pool_views`: nothing makes its texture until the ID is registered again), but the capture did not record that state. It walked the entry's texture fields and read the dead pool object through `tex_t.pool`: the bytes now there (Scarlet: a texture's data, such as `0x2492492492497f80`) became the pool's address, size and mode.
- **Fix:** a registry entry carries `gone` (`cap_treg`), and a gone entry keeps neither its pool nor its storage address and is not read back. `nvn_replay` registers it and then marks it gone (`mtl_reg_tex_gone`), as the live run left it. Captures made before keep working (the byte was padding, 0). `check_t_capture_live` registers a texture on a pool of its own and finalizes that pool, then fills the pool object with such bytes. Without the fix the capture is incomplete.
- **Found on:** [Pokemon Scarlet](../../dm/games/0100A3D008C5C000/README.md) (texture IDs 1726 to 1786 in the bedroom, each texture with a pool of its own).

### A texture read after its memory pool object was freed

- **Symptom:** `CRASH sig=11 … pc=nvn_sparse_protect+0x40`, an address that looks like data, `x8` 8 below it, after a long session; the backtrace `tex_by_id` ← `tex_mtl` ← `pkg_upload`. The pool need not be sparse.
- **Cause:** a texture ID keeps a copy of the texture, and with it `tex_t.pool`, the game's memory pool object. The game may finalize the pool, free the object and leave the ID registered: a descriptor costs nothing until a shader samples through it. `tex_by_id` still resolved such an ID, marked `gone` or not, and `tex_mtl_` read the object: garbage in its `vp` made the texture look sparse, and the upload's `vpool_at` read `v->base` from that value. The fault is on `pool->vp` itself, before any page table is read, so no check of a page table pointer can stop it. `tex_storage_live` guarded only `pool_gpu`.
- **Fix:** nothing is made or refreshed from a texture whose pool is gone, on two checks. An ID marked `gone` (`tex_by_id`, `img_fit_upgrade`, `image_prepare`) cannot race the finalize and stays right when a new pool takes the old object's address: `nvnMemoryPoolFinalize` now marks it under `g_rmu` before it tears anything down, not after. Every other route passes `tex_mtl_`, which checks the object's magic, the only sign left when a game overwrites the object with no `Finalize`. A texture the host already made keeps its host texture without a refresh (`tex_made`), as in-flight work may still read it; one it has not made reads as an empty slot. Each case logs its ID or serial. A hand-built `pool_t` in a test therefore needs its magic. `t_readback` (`stale_pools`) faulted at this instruction before the fix and covers each case, sparse pools included. Which route the game took is **open**: the report has no backtrace, so if a host bug put a dead pool's texture in a live slot, this stops the crash and that bug remains.
- **Found on:** [FINAL FANTASY RESONANCE, Asian release, issue #934](https://github.com/seedds/nxnative/issues/934), after 22 minutes of play (not reproduced on the game: the fault was reproduced by `t_readback`); once in [Mario Party Superstars](../../dm/games/01006FE013472000/README.md), in the first pool walk.

### A frame capture runs under the standard thread policy

- **Symptom:** `regress.py capture` stops with `frame N was not captured within … s` although the game reached the frame before it; a capture takes 5 to 20 minutes where an earlier one of the same frame took 2 to 3, and a sample of the host shows the submitting thread in `lzfseEncodeBase` under `nvn_cap_submit`.
- **Cause:** the capture's steps run on the thread that submits or presents, and that thread can hold a time-constraint (real-time) policy declared for a few milliseconds a frame ([time-constraint computation declared below the thread's peaks](frame-rate.md#time-constraint-computation-declared-below-the-threads-peaks)). A step that runs for minutes far exceeds the declaration, and the kernel lets the thread run only in short slices: Pokemon Scarlet's pool bytes took 1,175 s to compress where the same data takes 55 s outside the host. The harness also counted the capture step's 300 s without a frame as a hang.
- **Fix:** each capture step runs under the standard policy, and the thread gets its own policy back after it (`cap_policy_standard`, `cap_policy_restore` in `nvn_capture.inc`). Scarlet's frame 3000 now takes 53 s to capture (1,178 s before), Breath of the Wild's frame 5000 34 s (124 s). The log gives each step's time (`submit N: registries and their textures …, records …, pool bytes …`). `regress.py capture` bounds the step by `--capture-limit` instead of the hang time. The pool bytes are also copied last in the step, as the live replay of the submit reads them, and once.
- **Found on:** [Pokemon Scarlet](../../dm/games/0100A3D008C5C000/README.md) and [Breath of the Wild](../../dm/games/01007EF00011E000/README.md) (their regression route captures).

### A capture refused after a change to the reflection structs

- **Symptom:** `nvn_replay` stops with `program #N (a key …) stage S: the translated shader's bindings or inputs differ from the ones the records were captured against`, and `regress.py replay` reports every game's capture as one the candidate cannot replay, while the shaders' reflection (`<key>_b.json`) is the same in both chains apart from the chain field.
- **Cause:** the capture compares each program's interface with the replayer's through a hash (`cap_iface`), and that hash was of the reflection structs' bytes (`nvn_rbind`, the vertex inputs). 682fdd26 widened `nvn_rbind.n` to 16 bits and added `soff`, so every shader's hash changed although no binding had.
- **Fix:** the hash is of the values, field by field, and a field at 0 adds nothing, so a field widened or added later keeps the hash of every shader that does not use it; `t_capture_live` pins the hash of one reflection. A capture records its hash kind (header `iface 2`), and `nvn_replay` refuses one of another kind with `capture again`: the captures made before have to be recorded again once.
- **Found on:** the regression captures of [Breath of the Wild](../../dm/games/01007EF00011E000/README.md), [Pokemon Scarlet](../../dm/games/0100A3D008C5C000/README.md), [Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) and [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (2026-10-30, all four refused at f5aee5aa).

## Command memory

### Control memory capacity queries

- **Symptom:** `[RED] NVN unimplemented function called: nvnCommandBufferGetControlMemoryFree` (or `…GetControlMemorySize`) once, after the game gives a command buffer control memory.
- **Cause:** `AddControlMemory` was a no-op, and the size and free getters reached the unimplemented stub, which returns 0, so a game that checks the free space sees none left. The host writes its records only to command memory and uses no byte of the game's control memory, so in NVN's terms (size = used + free) the whole block stays free.
- **Fix:** `cmd_t` records the size of the last `AddControlMemory` block and returns it from both `GetControlMemoryFree` and `GetControlMemorySize`; `GetControlMemoryUsed` stays 0. `t_nvn` checks the values before any block, after one, after a recording and after a smaller block replaces it.
- **Found on:** South Park: The Fractured But Whole 1.05, whose port is [PR #662](https://github.com/seedds/nxnative/pull/662) (not yet in the games table). With the getters implemented it reached the title, menu, a save and character customization on the Mac; with them returning 0 it also progressed, but submitted commands at a different rate.

### Host-lent command memory segments

- **Symptom:** `FATAL NVN command memory: the game's pool is empty…`.
- **Cause:** our command records are larger than real NVN commands, so command buffers the game sizes for hardware overflow; when the game's out-of-memory callback declines to add memory, the host lends a segment. Many small command buffers each overflowing once per frame exhausted a fixed pool of 64 × 1 MB.
- **Fix** (`hle_nvn.c`): the pool now grows on demand (256 KB segments, abort only beyond 1 GB; `hostseg_take`). A segment is released only when the game re-grants memory overlapping the guest bytes of the recording that jumped into it (`hostseg_release` in `AddCommandMemory`): the game records setup blocks once and keeps `CallCommands`-ing them for many frames, so recycling segments after a fixed number of frames would overwrite live recordings. An empty-pool grant of at most 4 KB outside a recording gets a 4 KB segment, a record larger than 256 KB gets a segment that holds it, and the cap counts every segment's bytes (`hostseg_size`). Such a grant has no guest range, so its segment is never reclaimed. On hardware a null pool cannot be written to, so a game that grants one has run out of the command memory it measured for itself: the host's records were larger than the driver's ([Model heaps sized by the host's command record sizes](memory.md#model-heaps-sized-by-the-hosts-command-record-sizes)). That is fixed in the record sizes; the host does not try to recycle these segments by guessing which recording a grant replaces.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (the many small command buffers: its opening course, per the comment at `HOSTSEG_SZ` in `hle_nvn.c`), [Super Mario Odyssey](../../dm/games/0100000000010000/README.md) (it hands out many 512-byte grants before recording, and with no recording to reclaim them by, 256 KB each reached the cap entering the Wooded Kingdom: the 4 KB segments; 1.4.1 still reached it after 11 minutes of play, about 260,000 4 KB segments, because its display-list heap was full and each re-recording took a new one; fixed by the cause, [Model heaps sized by the host's command record sizes](memory.md#model-heaps-sized-by-the-hosts-command-record-sizes)).

### What `GetCommandMemoryUsed` counts

Games read this count as a position in their own command memory and size their heaps and slots from it, so it must be what NVN counts.

- **Symptom:** frames stop after about 15 s; a game thread spins in its command-memory callback (a ring allocator at `main+0x9cc120` waiting for fences that have all signalled).
- **Cause:**
  - NVN counts used bytes from the last `AddCommandMemory`; the host counted from `BeginRecording`. KTGL gives its main command buffer one 16 MB chunk of a ring and, in its callback (`main+0x9ca960`), takes chunk start + `GetCommandMemoryUsed` as the position the GPU has reached, so the position stayed at one frame's size and the ring never freed.
  - `GetCommandMemoryUsed` must count everything the recording used, including the parts that went into host segments (plus 16 bytes for the jump). Mario marks a recorded block as empty when the count is 0 and then skips calling it (how that happened: [Inlined SDK code writes into `NVNcommandBuffer` +0x10](#inlined-sdk-code-writes-into-nvncommandbuffer-0x10)).
  - Games also size their own heaps and slots from this count: [Model heaps sized by the host's command record sizes](memory.md#model-heaps-sized-by-the-hosts-command-record-sizes) (Breath of the Wild's model heaps) and [Prerecorded lists in slots of their measured size](#prerecorded-lists-in-slots-of-their-measured-size) (next entry).
- **Fix:** the count now resets at `AddCommandMemory` (`hle_nvn.c`).
- **Check:** Super Mario Bros. Wonder and Super Mario Odyssey checked unchanged after the reset.
- **Found on:** [Fire Emblem: Three Houses](../../dm/games/010055D009F78000/README.md) (the count from `AddCommandMemory`); [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (host segments counted).

### Prerecorded lists in slots of their measured size

- **Symptom:** `CRASH sig=10` in `mtl_exec` after minutes of play, the fault address equal to `x19` and shaped like `0x1000000003`.
- **Cause:** the fault is the record-header load at the top of `mtl_exec`'s loop, `x19` is the replay pointer, and the address is itself a record header (`{op 3 = OP_CALL, size 16}`): a JUMP's target had been overwritten by the next recording. Tears of the Kingdom gives each model's prerecorded lists fixed slots of the size a recording of that list measured (`GetCommandMemoryUsed`), slot k at base + size × k, and passes `AddCommandMemory` max(size, 512) (`main+0xbb120`); its memory callback is empty. The host kept 24 bytes free after every record for "a JUMP or an END", so a 496-byte list's END went to a host segment and the 16-byte JUMP at 488 reached 8 bytes into the next slot.
- **Fix:** the host now keeps 16 bytes (one JUMP), lets the END take a segment's last bytes, and links only recordings that have records, so a JUMP only stands where a record of 16 bytes or more was to go and never writes past what `GetCommandMemoryUsed` reported (`OP_RESERVE` in `hle_nvn.c`; `t_nvn` "slotted lists").
- **Note:** to read such a crash, `x8` holds the jump-table target of the previous record's case, and the registers a callee saved below `sp` give the previous replay pointer (`x19`) and payload (`x26`).
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) (1.4.3, GitHub issue #61).

### Recorded commands patched in place by the game (`OP_BIND_TEX1`)

- **Symptom:** the title showed one stretched 64×64 icon over the 3D scene, a new game a white screen; post-process passes with stale texture handles; `[RED] bind record … overwritten after recording`.
- **Cause:** the game may write into recorded command memory directly, and host records stored in place must leave those bytes alone. The engine records a command buffer, then rewrites texture handles in it before submitting, as the hardware lays the command out: before `nvnCommandBufferBindTexture` it takes the command memory base + `GetCommandMemoryUsed` (`main+0x7c8964`, the only reader of that NVN entry point), and every frame it stores a 32-bit handle at that address + 0x10 (`main+0xb7d0d4`, on a job worker). In a host bind record that word was the count, and the replay read 535 million handles from whatever followed, so the post-process passes kept stale texture handles in their slots.
- **Fix:** a single texture bind is recorded as `OP_BIND_TEX1`, its handle 16 bytes in, in the hardware's layout (`nvn_obj.h` `p_bind_tex1`, read through `bind_rec`); every game.
- **Found on:** [Luigi's Mansion 3](../../dm/games/0100DCA0064A6000/README.md). Found with a check that reports a bind record holding more than 32 handles (`[RED] bind record … overwritten after recording`), then `DM_PROBE_WATCH` on that word.
- **Note:** any other command a game patches this way needs the same: find the readers of the `GetCommandMemoryUsed` slot first.

## Queues and syncs

### Bindings belong to each queue

- **Symptom:** choosing X-2 after the collection launcher crashes in `keys_hash`, reading control address `0x8000000000`; the program object now contains command-buffer data.
- **Cause:** the replay kept one global binding state across queue destruction and creation. X-2 binds its fragment program first, so combining stages dereferenced the launcher's freed vertex program. Distinct live queues could also inherit each other's bindings.
- **Fix:** give every queue initialization a serial. Save and restore draw state and render-target state when submissions switch queues, resolve composite programs again, and invalidate encoder caches. Finalization discards that queue's saved state. Reusing the queue's storage starts with defaults and cannot recover old guest pointers.
- **Found on:** [FINAL FANTASY X/X-2 HD Remaster](../../dm/games/0100BC300CB48000/README.md), launcher-to-X-2 handoff.
- **Check:** `t_queue_origin` tests independent state, switching back, finalization and address reuse with a poisoned old program pointer, alongside its real GPU culling tests.

### Finalized programs leave every queue's bindings

- **Symptom:** about 23 s after launch, when the title screen hands over to the opening demo, a crash in `keys_hash` (`prog_key` ← `prog_get` ← `bind_stages` ← `prog_resolve` ← `mtl_queue_activate`), reading a control address made of float data; the program object now holds other data.
- **Cause:** NVN reads a program only when it is bound, so a game may finalize a program a queue still has bound and reuse its memory. The host kept the pointer in that queue's bindings (`S.bst`, `S.prog`, `S.cprog`); it went into the queue's saved state when another queue submitted, and switching back combined the stages from the reused memory.
- **Fix:** `nvnProgramFinalize` drops the program from the active bindings and from every saved queue state under the submission lock (`mtl_prog_forget`). When the active queue's effective program is a composite of its stage and another program's (stages bound apart), the composite is taken again from the stages left: it holds its own copy of each stage's shader, so clearing the stage alone kept drawing the finalized program's shader. A saved queue's composite is taken again when the queue is switched back to (`prog_resolve`). It logs `finalized program … dropped from queue bindings` the first time and at powers of two.
- **Found on:** [Fire Emblem Fortune's Weave](../../dm/games/04007EF01F8AE000/README.md), left on the title screen until the opening demo: 5 of 5 runs crashed before, 0 of 7 after, each logging one drop from the active queue.
- **Check:** `t_queue_origin` finalizes a program bound in the active queue and in a saved one, poisons its memory and switches back: both lose it; finalizing the fragment program of a composite leaves the vertex stage alone as the effective program.

### Fence syncs recorded in command buffers

- **Symptom:** `nvnCommandBufferFenceSync` unimplemented, every `SyncWait` timed out at once.
- **Cause:** Pokémon Legends: Z-A fences once per frame from a command buffer and waits on it; Unreal's RHI records the call in its command buffers too.
- **Fix:** a record (`OP_FENCE_SYNC`) fences the sync at replay with the fence value of the submission segment being encoded, so it signals once everything before it has completed (the replay fences the sync with the submission that reaches it).
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md) (19.3.11; one boot); [Octopath Traveler 0](../../dm/games/01005270232F2000/README.md) (new with Unreal).

### Submissions from two queues on two threads

- **Symptom:** `FATAL submit re-entered` in the first frames.
- **Cause:** the game initializes two NVN queues and submits on both from two threads, while the Metal replay keeps one command buffer for the submission in progress.
- **Fix:** `nvnQueueSubmitCommands` now holds a lock across the replay (`g_sub_mu` in `hle_nvn.c`). It takes the lock before the graphics gate ([Going to the background during the prepare step](#going-to-the-background-during-the-prepare-step)).
- **Found on:** [Fire Emblem: Three Houses](../../dm/games/010055D009F78000/README.md).

### Going to the background during the prepare step

- **Symptom:** going to the background (iOS `sceneDidEnterBackground`, minimizing the Mac window) while the "Preparing shaders" or "Preparing pipelines" screen is up blocks the main thread until the step ends: minutes on a first start, long enough for iOS to end the app. The log's `[nvn] graphics suspended` line comes only after the `pipeline prepare:` line.
- **Cause:** the prepare step (the prewarm and the pipeline prebuild, `mtl_prepare` in `nvn_mtl.m`) runs in the game's first `nvnQueueSubmitCommands`, which held the graphics gate across it. `dm_gfx_suspend(1)` closes the gate and waits for every submit and present inside it to leave. The step loads shaders and builds pipelines and submits no GPU work, so it never needed the gate.
- **Fix:** the submit entry runs the prepare step under the submit lock (`g_sub_mu`) but before it enters the gate, marked as host work for the guest scheduler (`dm_host_work`). If the app went to the background meanwhile, the submission waits at the gate for the foreground like any other. The lock now comes before the gate, taken with `dm_host_mutex_lock`: with the gate first, a second submission waiting for the lock counted as in flight, so the suspend waited for it while the first, done preparing, waited at the closed gate holding the lock. `mtl_sub_begin` still calls `mtl_prepare`, a no-op by then, for the replay tool (`nvn_replay`), which has no gate.
- **Check:** the CPU-only `t_gfx_suspend` runs the real submit entry on two threads with a prepare step held by semaphores. It checks that the step runs outside the gate as host work, that the queued second submission is not in flight, that the suspend returns while the step holds the lock, that nothing commits in the background, and that both submissions finish after resume. With the gate taken before the lock it fails before suspending.
- **Found on:** no game named in the source (general: every NVN game with a prewarm list; a first start without the Metal cache takes longest). **Open:** not yet run by going to the background during a real game's prepare step on an iPhone.

### A GPU command buffer that never completes

- **Symptom:** frames stop, no crash, the main thread sits in `nvnSyncWait(sync, -1)`, and the watchdog's wait-state line shows the main fence one short (`main fence submitted N / completed N-1`).
- **Cause:** a GPU command buffer never completes.
- **Check:** GPU load from outside: `ioreg -r -d 1 -c IOAccelerator | grep 'Device Utilization'` at 100 % means a shader is looping forever. Find the shaders first used in the last frames (`DM_NVN_PASSDUMP`, compare the `K` keys) and look at their `.metal` for an empty `do {} while (true);` with no `return` (one such case: [Exact program start for the decoder](shaders.md#exact-program-start-for-the-decoder)).
- **Found on:** no game named in the source (general).

### The game's SyncWait wakes on the GPU's own signal of its fence

- **Symptom:** the frame's blocking SyncWait on its newest fence wakes about 0.26–0.29 ms after the GPU end (M2 Max; the wait ledger, `DM_NVN_WAITS=1`), on the critical path of every frame.
- **Cause:** the waiter slept until the commit feedback handler of the fence's submission had run, which comes that long after the GPU end.
- **Fix:** the game's waits (newest fence, older fence, acquire sync) wait on `g_ev`, which the queue signals after each submission, and then make the CPU-side writes of every submission up to the fence that no handler has made yet (`post_early`: events, samples-passed and counter reports; a ring of `POST_RING` records, each run once by whichever comes first). A counter report of the fence's last segment takes the wake as that segment's end. `DM_NVN_FAST_WAKE=0` turns it off.
- **Note:** the commit feedback handler's other blocks (diagnostics such as `DM_NVN_FUSE_CHECK`) still run when the handler does, after a fast-woken wait has returned.
- **Resolved in review of #158:** claiming a record is not completion. The original early path skipped records a handler or another waiter owned and advanced `g_done` before their writes finished; a delayed callback also retained a recyclable slot pointer. `post_claim` now holds `g_post_mu` through `post_run`, and both contenders call `post_complete` with their fence to apply earlier records in order. A slot is reused under that lock only after its writes finish; an old fence cannot claim its new generation. `g_done` and `g_fence` are atomic.
- **Check:** `make -C dm/gfx/tests GAME=010015100B514000 check_t_perf_order` covers a paused completion owner, a competing waiter, recycled-slot generations, event order and exactly-once writes, with direct/deferred encoding and fast wake disabled. `t_perf_order --cpu` and `perf-order-tsan` run the completion cases without a GPU in CI. The original harness reproduced premature completion; the corrected CPU cases pass ThreadSanitizer. Current gameplay coverage and timing limits are recorded in [BotW](../../dm/games/01007EF00011E000/README.md#final-synchronization-and-pass-validation-2026-10-26-current) and [Metroid Dread](../../dm/games/010093801237C000/README.md#final-gameplay-validation-2026-10-26-current); [13 Sentinels](../../dm/games/01003FC01670C000/README.md#rendering) has additional menu/report traffic coverage.
- **Finite timeout budget:** when the event wait times out, the feedback wait uses only the remaining requested time. Previously an expired 20 ms event wait could be followed by another full 20 ms feedback wait. Elapsed time is taken from the monotonic wait clock independently of wait diagnostics. Zero/sub-millisecond polls, successful CPU completion, and the existing per-stage one-hour cap for `UINT64_MAX` are unchanged. This bounds the time assigned to the two waits, not scheduling delays or the CPU completion lock.
- **Budget regression:** the `--cpu` path also uses a synthetic event, clock and condition-wait deadline to exercise the actual `mtl_wait_at` function without a Metal device. Run with `DM_NVN_FAST_WAKE=0/1` and `DM_NVN_WAITS=0/1`; no gameplay or FPS improvement is inferred from these timeout cases.
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md): it reads 5 timestamp reports a frame right after the wait (its interval controller and dynamic resolution), which is why the waiter makes them itself. Wake after the GPU end 0.29 → 0.08 ms on an M2 Max, 0.147 ms on an iPhone 17 Pro Max.

### `nvnQueueFinish` waits for the GPU, not the display

- **Symptom:** loading screens many times slower on the iPhone than on the Mac (Three Houses' first loading screen 78 s on the iPhone, 5 s on the Mac).
- **Cause:** `mtl_finish` called `mtl_present_flush` first, and with a window that waits until the display link has taken every queued present: one to two display frames per call. A loader that follows each upload copy with `nvnQueueFinish` pays that hundreds of times per loading screen. The windowless Mac runs present at once, so they never showed it; `DM_WINDOW=1` does (one step took 20 s instead of 1.7 s).
- **Fix:** `nvnQueueFinish` waits only for the GPU work submitted so far, as on the Switch, where it does not wait for the display. A swapchain texture is still not handed back before its previous present is processed (`AcquireTexture` waits for that ticket), and `mtl_drain` keeps its flush.
- **Found on:** [Fire Emblem: Three Houses](../../dm/games/010055D009F78000/README.md): KTGL's loader (effect banks, face models). Windowed Mac run after the fix: first load 6.1 s, frame pacing unchanged.
- **Note:** how it was found on the device: `Documents/env.txt` with `DM_WATCH=50` (thread table at 50 s: only the yield-polling main thread used CPU) and `DM_FSLOG=<absolute path>` (the reads came in bursts, with stalls of 4 / 24 / 13 s right after the effect banks).

## CPU-written textures

### CPU deswizzle copies use slice and row offsets

- **Symptom:** CPU texture uploads spend time recalculating block-linear addresses and calling variable-length `memcpy` for every 16-byte segment.
- **Cause:** `move` called `nvn_bl_offset` inside its innermost loop, repeating slice/row calculations and division by runtime block dimensions.
- **Fix:** hoist slice and row offsets, express power-of-two block dimensions as shifts/masks, and use the four fixed x offsets and eight fixed y offsets within a GOB. Full segments use constant-size 16-byte copies; byte counts not divisible by 16 retain a tail copy. ASTC blocks are always 16 bytes irrespective of their texel dimensions. Both copy directions preserve the layout, sparse row padding and mip/layer offsets.
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md#found-on-this-game), reported in [PR #261](https://github.com/seedds/nxnative/pull/261).
- **Check:** `t_tex` compares both directions against the byte-address formula for 1,029 deterministic layouts, including 1D/2D/3D, arrays/cubes, packaged block dimensions, sparse mip tails, 1/2/3/4/6/8/12/16-byte formats, padded pitches and partial segments. All 5,334 checks pass, including under AddressSanitizer and UBSan. A local arm64 Mac comparison against the old implementation, Apple clang 21 at the host's `-O1`, measured 7.4–7.7 GB/s before and 35.1–37.6 GB/s after (4.6–5.1x) across 1024x1024 sparse ASTC 6x6, 2048x2048 BC7, 1920x1080 RGBA8 and 1280x720 RGBA16F. Each result is the median of seven runs copying about 128 MiB, with implementation order alternated. This is a CPU microbenchmark; no new gameplay run here.

### The Switch 2's order inside a GOB

- **Symptom:** a Switch 2 game's textures from its files keep their shapes but break up along every edge: misplaced 4×4 blocks (16-byte pieces) in a regular weave, invisible in flat areas. Normal maps make lit surfaces look woven; opacity masks punch holes.
- **Cause:** the GOB (64 bytes × 8 rows) keeps its size and place, but the Switch 2 orders its 16-byte pieces x5 y2 x4 y1 y0 from the GOB's start, where the Tegra X1 orders them x5 y2 y1 x4 y0: offset bits 5 and 6 exchanged, (x%64/32)·256 + (y%8/4)·128 + (x%32/16)·64 + (y%4/2)·32 + (y%2)·16 + x%16. Block heights, sizes and level offsets are unchanged (the BNTX headers' block heights and sizes match `nvn_tex_layout`).
- **Fix:** `nvn_gob_switch2` (`dm/gfx/tex/nvn_deswizzle.h`), set at `nvnDeviceInitialize` when the SDK targets the Switch 2 (`dm_sdk_ounce`): the CPU address formula, the CPU copy tables and both Metal untiling kernels take the Switch 2's order.
- **Check:** `t_tex` checks the Switch 2's offsets by hand (B2) and both orders where they differ; `t_deswz_gpu` runs every case in both orders, the GPU kernels against the CPU copy.
- **Found on:** [Donkey Kong Bananza](../../dm/games/040074A01BF12000/README.md#rendering): level 0 of its BNTX textures (BC7 `BaseRock000_Pnrm`, BC4 `ArenaLevelShadowMaskSunbeam00_Smsk`) decoded with every order of the five address bits; the X1's left the weave, this order alone gave clean images (mean neighbour difference 19.5 → 14.9 on the rock normal map, 21.0 → 15.7 on the BC4 mask).

### Packaged texture block dimensions and linear storage rows

- **Symptom:** driver comparisons report the wrong block dimensions for packaged 2D / 3D textures, or too little storage for linear block-compressed textures (64x64 BC1 with stride 128: 2048 bytes instead of 8192).
- **Cause:** `SetPackagedTextureLayout` byte 0 holds log2 block height in its low nibble and log2 block depth in its high nibble. The host treated the whole byte as a 2D block height and ignored it for 3D. Linear storage was sized by compressed block rows, while NVN counts pixel rows.
- **Fix:** `nvn_tex_layout` decodes both nibbles for 3D and only the height nibble for 2D; a present byte of zero overrides automatic dimensions too (`pkg_block_h` stores byte 0 + 1). Level offsets and CPU / GPU deswizzling use those shared dimensions, reduced for smaller mips. Both linear flags use stride times pixel height, preserving row alignment and explicit strides.
- **Check:** `t_tex_layout` covers nine driver-derived shapes; four fail before the fix. `t_tex` also decodes packaged 3D markers at independently calculated addresses and checks mip dimensions and offsets; `t_deswz_gpu` checks both packaged 3D layouts across six mips on Metal. For flag 0x200 see [Minimal texture layouts](#minimal-texture-layouts); linear compressed-texture uploads are outside these storage checks.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md): digitalhivehub's driver probes in [PR #11](https://github.com/seedds/nxnative/pull/11), ported through MacNativeGames `3c482a7` and [PR #201](https://github.com/seedds/nxnative/pull/201). Validated here with CPU and synthetic Mac GPU tests; packaged 3D / linear BC gameplay and the iPhone have not been rechecked.

### Minimal texture layouts

- **Symptom:** huge stretched terrain walls and ceilings, flickering, while characters and the HUD are right: `WriteTexels` into a texture overwrote the mesh data stored right after it.
- **Cause:** `nvn_tex_layout` ignored `MINIMAL_LAYOUT` (0x200) and laid every such texture out with the automatic block height. The driver keeps the automatic block height and depth only when the whole layer (every level and the layer alignment) is no larger than with 1-GOB blocks; otherwise it takes 1-GOB blocks. A game that packs resources by the driver's sizes then gets textures larger than their slots.
- **Fix:** `nvn_tex_layout` compares the two layer sizes (`layer_bytes`) and takes 1-GOB blocks when they differ, so storage size, level offsets and CPU / GPU swizzling agree. A packaged layout and a sparse texture's tiles keep their own blocks. This is the rule digitalhivehub measured on the game's own driver in [PR #11](https://github.com/seedds/nxnative/pull/11) (a sweep with no difference left outside `SPARSE`), which the port of that PR left out.
- **Check:** `t_tex_layout` has PR #11's six driver shapes with the flag (320×320 RGB565, 260×260 RG8 with 9 levels, 8×1920 R8 with 11, 3D 64×64×8 and ×9, 64×128 RGBA8 that keeps its block) and three from issue #183 (RGB5A1 320×320, a 2D array with mips, a packaged layout over the flag): 23 of 23 match, 17 before. `t_nvn` writes two adjacent 320×320 textures with `WriteTexels` and checks the bytes after each and the texels read back; `t_deswz_gpu` deswizzles the Depths shape and the array on Metal (19 cases pass).
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md#rendering) (GitHub issue #183, the Depths): two 320×320 RGB5A1 / RGB565 textures before mesh data in one resource take 204,800 bytes each; the host laid them out in 245,760. Found by the issue's reporters (M1 with 1.2.1, replaying the wrong writes reproduced every changed byte of the resource; checked on M4 with 1.4.3).

### Linear textures in CPU-visible pools

- **Symptom:** menus and dialogue boxes draw but contain no text, while images and pictogram digits are fine; the sky is black (CPU-written linear colour ramps).
- **Cause:** CPU writes into a linear texture in a CPU-visible pool must be visible to the GPU, and a private Metal texture never sees them. Mario rasterizes glyphs on the CPU into a *linear* texture in a CPU-visible pool (2048×8192 R8, flags IMAGE|LINEAR), and its sky reads four 64×1 RGBA16F colour ramps written the same way.
- **Fix:** a texture with `NVN_TEXTURE_FLAGS_LINEAR` or `LINEAR_RENDER_TARGET` in a CPU-visible pool is created on the pool's `MTLBuffer` (`tex_linear` in `nvn_mtl.m`), so CPU writes (Mario's glyph cache) are visible to the GPU. Since Rhythm Heaven Groove a linear texture is made on its pool's buffer in any pool ([A pool's storage at creation is its textures' content](#a-pools-storage-at-creation-is-its-textures-content)).
- **Note:** block-linear CPU-written textures still need the packaged-texture re-upload path (next entry).
- **Check:** `DM_NVN_TEXCPU=1` lists non-packaged textures in CPU-visible pools.
- **Found on:** [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md): its glyph cache (the text texture) and its four 64×1 RGBA16F sky colour ramps.

### Linear render-target storage without the LINEAR flag

- **Symptom:** a trap during graphics initialization in `SetupSurfaceInfoMemoryInside`, asserting `ctx.memorySize <= localmemsetupinfo.memorySize`.
- **Cause:** `LINEAR_RENDER_TARGET` specifies linear storage independently of `LINEAR`. The Metal backing path recognized either flag, but the storage-size and deswizzle helpers tested only `LINEAR`, underallocating some render targets and treating their rows as block-linear.
- **Fix:** `nvn_tex.c` recognizes either flag for linear sizing and excludes both from block-linear level offsets, GOB columns and sparse layout. `nvn_deswizzle_geom` rejects both. `tex_linear` uses the same calculated row pitch, including implicit alignment, as `GetStorageSize`. Render-target rows align to 128 bytes; ordinary linear rows align to 32. Explicit strides are preserved. `t_tex` covers the layout and `t_linear_readback` checks a GPU clear against padded CPU-visible rows.
- **Found on:** [Persona 5 Royal](../../dm/games/01005CA01580E000/README.md#rendering): a 16x16 RGBA8 target with flags `0x44` and stride 128 needs 2 KB; block-linear sizing returned 1 KB.

### Textures written with `nvnTextureWriteTexels`

- **Symptom:** video frames, or a game's built-in default textures, are all zero.
- **Cause:** the texture is block-linear, has no packaged layout and is written from the CPU with `nvnTextureWriteTexels`, so its content lives in guest memory, not in a GPU texture (Engage's video frames, Unity's movie frames: Y plane `R8` + UV plane `RG8`, every frame; Mario's and Unity's 4×4 fallback textures, which hold black, white, transparent and red texels). The host uploaded only packaged textures from guest memory and made everything else a GPU-only texture.
- **Fix:** `WriteTexels` now records the texture, and it resolves like a packaged one: uploaded from guest memory, re-uploaded when its pages are dirtied (`mtl_tex_cpu_written`).
- **Note:** Mario's frames got slightly brighter where materials sample the default white texture.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md) (video frames and default textures), [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (fallback textures), Unity games (fallback textures; the source names no other).

### `WriteTexels` regions written in place by block-linear address

- **Symptom:** after a few minutes of play the main thread spends whole frames in `WriteTexels` (Metroid Dread's frames stopped at about 175 s).
- **Cause:** a game that writes a 1×1 region per call (Metroid Dread's set-pixel helper `main+0x160e80`, probably the map / minimap mask); the host decoded the whole level, patched it and encoded it again for every call.
- **Fix:** non-3D regions are written in place by block-linear address (`write_region_bl` in `hle_nvn.c`).
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md).
- **Check:** `t_nvn` checks 15 regions in RGBA8, BC1 and a two-layer RGBA16F array, from 1×1 to the whole level, byte for byte against the old whole-level path.

### Packaged texture data copied at `TextureInitialize`

- **Cause:** a texture created with `TextureBuilderSetPackagedTextureData` gets that data copied into its storage by NVN at `TextureInitialize` (a compressible pool cannot be written by the CPU).
- **Fix:** the host copies it and marks the texture CPU-written, so it is de-laid-out and uploaded like a packaged texture. Linear textures are excluded: in a CPU-visible pool they are read straight from the pool buffer.
- **Found on:** [13 Sentinels: Aegis Rim](../../dm/games/01003FC01670C000/README.md) (every texture is created this way; the linear ones are the save thumbnails).

### A pool's storage at creation is its textures' content

- **Symptom:** black screen with every geometry right (`DM_NVN_HILITE` shows each sprite and glyph in place); the uniform buffer right (a `CBDUMP`), so every texture read 0.
- **Cause:** NVN keeps what a pool's storage held at `nvnMemoryPoolInitialize` as its content, but the host uploaded from guest memory only packaged, `WriteTexels` or flushed textures, and made a linear texture on its pool's buffer only in a pool the CPU can see. Rhythm Heaven Groove reads each texture file into memory and makes a `CPU_NO_ACCESS | GPU_CACHED` pool (flags 0x21) on it, so its textures sit in `CPU_NO_ACCESS` pools made on memory the files were read into, whose initial content the host did not take; its sprite shader discards a fragment whose alpha is 0.
- **Fix:** a new texture in a `CPU_NO_ACCESS` pool whose storage holds data starts with that content, uploaded once (`tex_init_fill` in `nvn_mtl.m`); it stays a GPU texture that can be rendered into, with no re-upload on page writes. A linear one is made on the pool's buffer in any pool (`tex_linear`). Left out: storage of zeros, `COMPRESSIBLE` textures in a `CPU_NO_ACCESS` pool (render targets in Rhythm Heaven Groove, in their own 0xa1 pools; their memory means nothing without the compression tags — [a pool the CPU writes is different](#compressible-in-a-pool-the-cpu-writes-is-only-a-hint)), multisample, depth / stencil and sparse ones.
- **Found on:** [Rhythm Heaven Groove](../../dm/games/0100D9F01D474000/README.md) (SDK 21.4.0): the sprite shader (`40adb3021c79b155`) computes texture × colour multiply + colour add and discards a fragment whose alpha is 0 (the `CBDUMP`: multiply 1, add 0); 245 pools by the first screens, each holding one texture at offset 0, 17 block-linear BC7 (flag `IMAGE`, no packaged layout) and 228 linear R8 (`IMAGE | LINEAR`).
- **Check:** found by a census of every texture's pool flags, flags, format and whether its storage held data at creation (a temporary line under `DM_NVN_LOG`).

### `COMPRESSIBLE` in a pool the CPU writes is only a hint

- **Symptom:** a game draws its UI and text but every image is black; `packaged textures: first uploads 0` while `TextureInitialize` counts the textures the game created (SD GUNDAM G GENERATION CROSS RAYS: 74, of which the 10 BC3 ones hold data and none is uploaded). After the fill works, one overlay still comes up with its text over nothing: the panel's background is missing and the text of the next block sits on top of it.
- **Cause:** the game sets `NVN_TEXTURE_FLAGS_COMPRESSIBLE` (0x8) on every texture it creates. That flag is a Switch memory-compression hint, not a data format: in a pool the CPU writes (`CPU_UNCACHED`, flags 0xa2) the texels are plain. Two host paths skipped such a texture — `tex_init_fill` (its content at creation, [the pool's storage at creation](#a-pools-storage-at-creation-is-its-textures-content)) and `ft_add` (`hle_nvn.c`, the CPU-write tracking that `tex_flush_upload` and `tex_dirty_upload` rely on). The second one is why an overlay generated after creation — zero at `nvnTextureInitialize`, written by the CPU a frame later — never appeared: the texture was never registered, so no page fault reported the write, and `write-protection faults` stayed 0.
- **Fix:** the `COMPRESSIBLE` exclusion applies only in a `CPU_NO_ACCESS` pool, where the game never writes the storage and the tags may really be there (Rhythm Heaven Groove's render targets, flags 0xa1). Both `tex_init_fill` and `ft_add` make that check after the pool is known; a texture in a pool the CPU writes is filled and registered as any other.
- **Found on:** SD GUNDAM G GENERATION CROSS RAYS (`010055700CEA8000`, #1052, no profile on `main` yet) (SDK 9.4.1): 74 textures in 0xa2 pools, all with `COMPRESSIBLE`; the 10 that hold data at first use are block-linear BC3 (`0x44`). With both paths fixed, `first uploads` goes 0 → 10, `write-protection faults` 0 → 94, the title, the menus and the battle screens draw, and the stage-start overlay shows its panel with the tutorial text on it.
- **Check:** `DM_NVN_TEXINFO=WxH` prints the pool flags and the non-zero byte count at first use, and `initial content:` in the log names the pool kind each fill came from. `write-protection faults` staying at 0 while a texture's `any=` is 0 at creation means its CPU writes are not being tracked — the `ft_add` path.

### First-use fill of storage the game flushed

- **Symptom:** a texture the game filled with `memcpy` and flushed reads as zeros (BotW's sky brownish grey, the distant haze brown).
- **Cause:** a game that makes a CPU-cached pool, creates the textures, then copies data into the pool and flushes it: the host created the texture as a GPU texture of zeros at its first use.
- **Fix:** every game now fills a new GPU texture from its storage once when the game flushed that storage before the texture's first use, as it already did for a pool the CPU cannot access (`nvn_flushed` in `hle_nvn.c`, `tex_init_fill` in `nvn_mtl.m`; [previous entry](#a-pools-storage-at-creation-is-its-textures-content)): the GPU has not written such a texture yet, so its content is what the CPU put there. Unlike the profile's `tex_flush_upload` ([next entry](#mapped-range-flushes-taken-as-uploads-tex_flush_upload)) it never re-uploads, so a texture the game renders into afterwards stays a GPU texture. `DM_NVN_FLUSH_FILL=0` turns it off.
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md), GitHub issue #52 (Mac and iPhone). The sky is precomputed atmospheric scattering: each frame a pass (`08fe25061d583637`, `uking_pass_shader.sharcb`) turns a 256×32×16 RGBA16F 3D table of in-scattered light into a 256×256 sky map, which the sky draw (`8110c74078548172`) and the materials' aerial perspective sample. The table and two 2D ones (transmittance 256×64, irradiance 64×64) come from `System/KSys/sky.skybin` in `Pack/Bootup_Graphics.pack` (1,212,416 bytes, the three textures' storage back to back). The game makes a `CPU_CACHED | GPU_CACHED` pool for each texture, creates the textures, copies the file's data in with `memcpy` and flushes (`main+0x1257d50`, once, at the sky's setup; the flush goes through `main+0xebf4c4`, which flushes only CPU-cached pools). With zeros the sky had no scattered light: only the clouds and the sun's colour were left. In the Great Plateau save the fill covers that one texture; the flushes (about 15 a frame) merge into about 530 ranges, and frame times did not change. DRAGON QUEST XI S (title, free play, menus; 300 s) filled none.
- **Note:** `DM_NVN_TEXRAW=212,214` (all zero) and `DM_NVN_TEXINFO=256x32` (`storage 1048576 bytes non-zero 781567` at first use, `swizzled=0`) showed it.

### Storage of a CPU-uncached pool at first use

- **Symptom:** magenta (ASTC's error colour) where UI images belong: a title logo and menu panels, while the 3D scene and the text draw; `DM_NVN_ZBLOCK=1` reports nothing (the textures are not uploaded on the CPU at all); `DM_NVN_TEXINFO=WxH` shows storage holding data at first use (`non-zero …`) in a pool with flags 0x22 (`CPU_UNCACHED | GPU_CACHED`).
- **Cause:** in a `CPU_UNCACHED` pool the CPU's writes reach memory with no flush, and the GPU reads that memory: what the storage holds is the texture's content. The host took a CPU-visible pool's storage only when the game had flushed it ([previous entry](#first-use-fill-of-storage-the-game-flushed)), and a game that writes an uncached pool never needs to. With `tex_flush_upload` and `tex_dirty_upload` the logo stayed magenta too, so its texels were apparently in the storage before the texture was registered, where no later write marks it.
- **Fix:** `tex_init_fill` (`nvn_mtl.m`) also fills a new texture in a `CPU_UNCACHED` pool from its storage once, at first use, when the storage holds data; packaged layouts, multisample, depth / stencil and sparse ones stay out. The log says `initial content: … a CPU_UNCACHED pool's`. A texture the CPU writes again after its first use still needs the profile's keys.
- **Found on:** [Donkey Kong Country: Tropical Freeze](../../dm/games/0100C1F0051B6000/README.md): the 912×512 ASTC 5×5 title logo (242,374 of 376,832 storage bytes non-zero at first use, no packaged layout) and the menus' ASTC panels; with the fill and neither profile key, the title and every menu draw.
- **Also found on:** [NieR:Automata US 1.0.3](../../dm/games/010056B015FE8000/README.md): 9S's 512×512 ASTC 6×6 sRGB face texture (ten mip levels) uses pool flags `0x22`; the GPU has zero blocks while 151,736 of 244,224 guest bytes are nonzero and decode to the correct face. His hair uses the same pool flags. GPU fixtures (`dm/gfx/tests/t_initial_uncached.m`) sample three independently colored mip levels in linear and sRGB ASTC.
- **Superseded for block-compressed formats:** such a texture is now made CPU-written at first use instead of being filled once ([next entry](#block-compressed-storage-the-cpu-wrote-before-first-use)), so a later CPU write reaches it too. The one-time fill is left for uncompressed formats.

### Block-compressed storage the CPU wrote before first use

- **Symptom:** a picture that should change with a selection is black, or shows the previous item's picture while the item's name and description are right. `DM_NVN_TEXINFO=WxH` shows `swizzled=0` at first use with storage holding data, no packaged layout, in a `CPU_CACHED` or `CPU_UNCACHED` pool, and `DM_NVN_DIRTYCHECK=1` reports no missed write.
- **Cause:** two gaps, met one after the other. (1) The CPU wrote the texels before `nvnTextureInitialize` and never flushed them. `tex_dirty_upload` stamps a texture only when it is registered, so it never sees those writes, and `tex_init_fill` took a `CPU_CACHED` pool's storage only when the game had flushed it: the texture started as zeros. (2) A one-time fill (`tex_init_fill`, any pool) makes a GPU texture, which goes into the sharing table `g_alias` (pool, offset, description). When the game writes the next image into the same storage and builds a new texture object with the same description over it, the table hands back the earlier GPU texture with the earlier picture, and nothing tracks writes to it.
- **Fix:** every game. At first use (submit thread), a texture is made CPU-written (`tex_cpu_content` in `nvn_mtl.m`, then `mtl_tex_cpu_written`) when all of these hold:
  - a block-compressed format, or any format with target 3D, or an uncompressed 2D texture in a pool the GPU has not written ([next entry](#uncompressed-storage-the-cpu-wrote-in-a-pool-the-gpu-has-not-written))
  - no packaged layout
  - not `COMPRESSIBLE`, `LINEAR`, `LINEAR_RENDER_TARGET` or `SPARSE`, not `IMAGE` unless block-compressed (shaders cannot write a block-compressed texture), and not a buffer or multisample texture
  - in a pool the CPU can write (not `CPU_NO_ACCESS`, not virtual)
  - its storage holds data

  It is uploaded from the storage, re-uploaded when its pages change, and never enters the sharing table. Until the GPU first uses a texture it has not written it. A block-compressed one it can never render into, and GPU copies into it are written into its storage too ([GPU copies into textures uploaded from their storage](#gpu-copies-into-textures-uploaded-from-their-storage)). So what its storage holds is what the CPU wrote, flushed or not, before or after `nvnTextureInitialize`. Uncompressed 2D textures cannot be told from render targets (NVN has no flag for that, and `tex_desc` gives them render-target use), so they keep the flushed-range and uncached-pool fill (entries above). An uncompressed 3D texture with neither `IMAGE` nor `COMPRESSIBLE` is taken from its storage as well: the 3D textures Xenoblade Chronicles 3 renders or stores into carry one of them. That is what the games seen so far do, not an NVN rule. In Breath of the Wild and Tears of the Kingdom no 3D texture the rule takes is drawn into, cleared or read back; their flag-0 3D textures the GPU draws into are empty at first use or already CPU-written by an earlier path, and Breath of the Wild's CPU-filled 256x32x16 RGBA16F table moves from the flushed-range fill to this path. A 3D render target with neither flag and data in its storage at first use would still be drawn into (`attach` gives it render-target use), but a `ReadTexels` of it, or a second texture object over the same storage, would read the storage rather than what the GPU drew. The log says `texture written by the CPU before its first use …`. `DM_NVN_STORAGE_FIRST_USE=0` turns it off, to measure it. GPU fixtures: `dm/gfx/tests/t_initial_uncached.m` (an unflushed `CPU_CACHED` pool, a later CPU write, and a new object over rewritten storage).
- **Found on:** [The Great Ace Attorney Chronicles](../../dm/games/010036E00FB20000/README.md), Japanese release `0100D7F00FB1A000` (GitHub issue #568). Its court-record evidence and profile pictures are 256x256, 512x512 and 2048x2048 BC1 (format `0x42`) with no packaged layout, in its `CPU_CACHED | GPU_CACHED | COMPRESSIBLE` pool (flags `0xa4`). At first use `TEXINFO` showed 31,800 of 32,768 bytes non-zero. They were black, and once filled once, the autopsy report showed the school badge's picture. The 2048x2048 BC1 and BC7 chapter title cards (`UI_opdemo00_jpn.arc`, `opTitle0_BM_NOMIP_HQ`) are written the same way (GitHub PR #167).
- **Also:** Pokémon Pokopia gives nearly every texture `IMAGE` (flags 0x4), its icons BC1 / BC7 in a `CPU_UNCACHED` pool: with `IMAGE` excluding them, a new icon over storage another icon had used got that icon's picture (in the store and the Pokédex, once storage was reused). `IMAGE` now keeps only uncompressed textures out. Found and fixed by xFractals/pokopia-native (issue #1).
- **Also found on:** [Xenoblade Chronicles 3](../../dm/games/010074F013262000/README.md) (GitHub issue #1011): its particle steering fields are 10x10x10 RGBA8 and RGBA16F 3D textures, flags 0, that the CPU writes before first use. As GPU textures of zeros they steered every send-off particle along (-1, -1, -1), so the particles sank diagonally into the ground instead of rising. `t_initial_uncached.m` also covers a 4x4x4 RGBA8 3D texture (each slice holds the CPU's texels at first use) and a 3D `IMAGE` texture (left alone).

### Mapped-range flushes taken as uploads (`tex_flush_upload`)

- **Symptom:** a movie plays black (Age of Calamity: its intro movie under the "+ Skip" prompt).
- **Cause:** the decoder's NV12 output is copied by the game into textures in a CPU-visible pool and published with `nvnMemoryPoolFlushMappedRange`, which the host did not take as an upload.
- **Fix:** the profile key `tex_flush_upload`. Buffer textures stay out of that path: they are views onto the pool buffer, and an upload would have sent them past Metal's 2D size limit.
- **Found on:** [Hyrule Warriors: Age of Calamity](../../dm/games/01002B00111A2000/README.md): a 1280×720 R8 and a 640×360 RG8 texture; the buffer texture kept out is a 1,048,576-texel R32F one.
- **Also found on:** [NieR:Automata The End of YoRHa Edition](../../dm/games/010056B015FE8000/README.md): the title background; the game note records the controlled comparison.

### Uncompressed storage the CPU wrote, in a pool the GPU has not written

- **Symptom:** open-world ground is one flat colour on a flat plane with blocky seams, or water and sand are missing, while course meshes, rocks and the kart look right. `DM_NVN_TEXINFO=WxH` shows uncompressed textures with `swizzled=0`, no packaged layout and storage holding data in a `CPU_CACHED` pool.
- **Cause:** `tex_cpu_content` took a texture's storage at first use only for block-compressed formats and 3D targets, because an uncompressed 2D texture cannot be told from a render target by its flags ([Block-compressed storage the CPU wrote before first use](#block-compressed-storage-the-cpu-wrote-before-first-use)). `tex_init_fill` takes a `CPU_CACHED` pool's storage only when the game flushed it. So an uncompressed texture the CPU wrote with no flush became a GPU texture of zeros.
- **Fix:** every game. A pool the GPU has not written holds only what the CPU wrote, whatever the texture's flags say, so `tex_cpu_content` also takes an uncompressed 2D texture's storage there. A pool counts as written once a render target, a texture clear, a copy or downsample into a texture, or a shader image has been recorded in it (`pool_t.gpu_written`, `mtl_pool_gpu_written`); copies count, so a game that fills textures by staging copies behaves as before. At the pool's first GPU write its textures taken this way become GPU textures with the content they have, and CPU writes to them are no longer uploaded (`cpu_unc_drop`). In a pool the GPU already writes, the earlier rule stays. Display, video-decode, depth, `IMAGE`, `COMPRESSIBLE`, `LINEAR`, buffer and multisample textures stay out, and so does storage of zeros. `DM_NVN_STORAGE_FIRST_USE=0` turns it off. The log says `(uncompressed, in a pool the GPU has not written)` and `a pool's first GPU write`.
- **Found on:** [Mario Kart World](../../dm/games/0400C3F00006E000/README.md): terrain tiles (260x260 R16 height, RG8 with 9 levels, RGBA8, R11G11B10F, and 68x68 RGBA16) in a `CPU_CACHED | GPU_CACHED` pool (flags 0x24), 50 to 90% of their bytes non-zero at first use and all zero on Metal, so every tile was a flat plane at its base height. Pull request #1109 found this on update 1.8.0.
- **Check:** `make -C dm/gfx/tests GAME=<id> check_t_cpu_storage`.

### CPU writes with no flush, caught by page protection (`tex_dirty_upload`)

- **Symptom:** magenta surfaces (Metal decodes an all-zero ASTC block as the error colour (1, 0, 1)); a zero BC block decodes black, which looks like missing light; black screen while the UI (or a dialog box) draws in the right places under `HILITE` or in the pass dump; `packaged textures: first uploads 0`.
- **Cause:** the game writes texels into a texture's storage after `nvnTextureInitialize` with no `nvnMemoryPoolFlushMappedRange` or `FlushTexels` over them, so neither the flush path (`tex_flush_upload` never marks them) nor `tex_init_fill` (which only takes `CPU_NO_ACCESS` pools) sees them, and they stay GPU-only with content 0. Tears of the Kingdom creates textures on zeroed memory and then fills them from the CPU (its cache maintenance runs natively). Writes to an uncached pool need no flush (FINAL FANTASY TACTICS); a game with no cache maintenance instruction at all gives the host no flush to take as the signal (The Great Ace Attorney Chronicles).
- **Fix:** profile key `tex_dirty_upload`: `ft_add` write-protects a registered texture's pages, and on first use (submit thread) a write since then makes it CPU-written, so a texture whose pages the CPU writes after creation is uploaded at its first use. A `COMPRESSIBLE` texture in a pool the CPU writes is registered too ([a pool the CPU writes](#compressible-in-a-pool-the-cpu-writes-is-only-a-hint)). FINAL FANTASY TACTICS and The Great Ace Attorney Chronicles set it together with `tex_flush_upload`; with `tex_flush_upload` alone The Great Ace Attorney Chronicles stayed black. Textures written after their first use and `ReadTexels` before any draw: next entry.
- **Check:** `DM_NVN_TEXINFO=WxH` tells the cases apart: it prints how many bytes of the texture's guest memory are non-zero at first use.
- **Found on:**
  - [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md): the cave was magenta; the terrain draws sample two 121-layer ASTC 8×8 arrays (42 MB each) filled this way.
  - [FINAL FANTASY TACTICS - The Ivalice Chronicles](../../dm/games/010038B015560000/README.md): the UI reads its textures from `.tex` files the engine loads straight into a `CPU_UNCACHED | GPU_CACHED | COMPRESSIBLE` pool (flags 0xa2): 231 textures by the title screen, no packaged layout, no `WriteTexels`, no copies.
  - [The Great Ace Attorney Chronicles](../../dm/games/010036E00FB20000/README.md): a dialog box sampling 256x256 and 1024x256 textures of format 0x4d; `DM_NVN_TEXINFO=1024x256` showed storage that already held data at first use (226,555 of 262,144 bytes non-zero): no packaged layout, block-linear, in a 1000 MB `CPU_CACHED | GPU_CACHED | COMPRESSIBLE` pool (flags 0xa4). A log of the pool flushes, texture creations and file reads (temporary lines under `DM_NVN_LOG`) showed every `.tex` read into staging buffers (512 KB `ReadFile`s, none into the pool) and the texels copied by the CPU into the texture's storage; only the four linear 1024x64 text textures are flushed, every frame. `main` has no cache maintenance instruction at all.
  - [Kirby Air Riders](../../dm/games/040077201BE50000/README.md): magenta props and panels in races, one of them a 512x512 ASTC texture all zeros when dumped; with both keys none of the 378 ASTC textures bound on one frame had a zero block.

### `ReadTexels` from guest memory and textures written after first use

- **Symptom:** a texture the game builds from CPU-filled textures read back with `nvnTextureReadTexels` comes out zero.
- **Cause:** `tex_dirty_upload` only marked such a texture CPU-written when the submit thread first resolved it, so `ReadTexels` on another thread before any draw returned the never-uploaded host texture: zeros. A texture filled after it was first bound was never marked at all.
- **Fix:** `mtl_read_texels` reads any texture whose content is in guest memory there (a packaged layout, a CPU-written texture, or a `tex_dirty_upload` texture written since registration), on any thread, without an upload; and `nvn_ft_poll` (`hle_nvn.c`, at each submission after guest memory changed, at most 128 MB of textures per look, going on where it stopped) marks registered textures written after their first use. Textures the GPU renders into or writes (`mtl_tex_gpu_owned`) leave that list without an upload.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) (Mac and iPhone): the stone blocks and steps of the opening cave got no torch light (on the Switch they are grey stone lit warm on the side facing the torch; here they kept their texture and the ambient light but stayed black). Their fragment shader `54e8c08998050e79` takes its roughness from a 5120×5120 BC4 atlas (roughness = the square root of its red, packed into the upper six bits of blue in colour target 3, which the tile-light shaders read); the atlas was all zero, and roughness 0 gives no point light. TOTK fills 1024×1024 BC4 material textures from the CPU with no NVN flush, reads them back with `nvnTextureReadTexels` on its own threads before any draw uses them, and copies the blocks into the atlas, which itself is filled after it was first bound.
- **Note:** found by running MacNativeGames' build of the same game on the same save: their atlas held data and their blocks were lit (their `ReadTexels` reads guest memory). Traced with `DM_NVN_TEXDUMP` (the blocks' roughness bits in colour target 3), the shader's MSL, `DM_NVN_TEXRAW` on the atlas and `DM_NVN_TEXINFO` (guest bytes), then lldb reading the atlas memory inside their process. Fast loop: the in-cave autosave (`run/save`, keys `f2500:A,f2800:A,f3100:A,f3600:A`, shot at frame 4500); `DM_DETCLOCK` cannot be used (TOTK stops at frame 1 waiting for guest time).

### `ReadTexels` on a texture of a sparse pool

- **Symptom:** `[RED] upload batches belong to the submit thread only: up_begin called on thread '…'`, then an abort, with `pkg_upload` ← `tex_mtl` ← `mtl_read_texels` on the stack; or `CRASH sig=10` in `nvn_deswizzle` ← `mtl_read_texels`, at an address in a sparse virtual pool's reserved range.
- **Cause:** `mtl_read_texels` read a texture from its storage only when it had a packaged layout or was CPU-written, while `tex_mtl_` also takes a sampled texture of a [sparse virtual pool](#sampled-textures-in-a-sparse-pool-uploaded-through-the-page-table) from its storage. Such a texture went through `tex_mtl`, which uploaded it (first use, or pages written or mapped since) on the game's thread, where the upload batch must not be used. And a texture of a sparse pool that did take the storage path was read at its virtual address directly, not through the page table: unmapped pages, and pages mapped at offsets the host cannot alias, are inaccessible there.
- **Fix:** one predicate, `tex_from_storage`, decides for both `tex_mtl_` and `mtl_read_texels` which textures the host takes from their storage, and `mtl_read_texels` reads a sparse pool's texture through a copy gathered from its page table (`nvn_sparse_copy`, unmapped pages as 0), as `pkg_upload_` uploads it. Neither touches the host texture, so the read works on any thread. The `up_owner_check` abort stays: a texture upload on another thread is still a bug to find.
- **Check:** `t_readback` reads a sparse texture over mapped and over unmapped pages, first on another thread and then on the submit thread. Before the fix, a profile without `tex_dirty_upload` aborted in `up_begin`, and TOTK's profile crashed with SIGBUS on the unmapped texture.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) on an iPad Pro (M5), GitHub issue #471: an abort a few seconds after the app came back from the background (switching between Stage Manager and full screen triggers it), on thread `bake::ModuleMgr`. The reporter traced it to `mtl_read_texels`.

### GPU copies into textures uploaded from their storage

- **Symptom:** a streamed texture's small mip levels hold another texture or zeros while its large levels are right (`texraw <serial>.<level>`), so surfaces sampled with a mip bias or at a distance show bright speckles, dark blotches and sharp detail where the Switch shows soft texture (Yoshi's felt), from the moment finer levels stream in (the opening going dark).
- **Cause:** the game fills a texture partly from the CPU and partly with GPU copies. Unreal's streamer re-creates a texture with more levels, copies the resident levels from the old texture on the GPU (`RHIAsyncReallocateTexture2D`: `nvnCommandBufferCopyTextureToTexture`, a level at a time) and writes the new, finer levels from the CPU. The host uploads a CPU-written texture whole from its storage, where the copied levels were never written: they held what the pool had there before.
- **Fix:** a copy into a texture whose storage the host uploads from (a packaged layout, CPU-written, or registered by `tex_flush_upload` / `tex_dirty_upload` and not resolved yet) is written into the storage as well, as the hardware's copy would (`nvn_tex_mirror_t2t` / `nvn_tex_mirror_b2t` in `hle_nvn.c`, from `copy_t2t` / `copy_b2t` in `nvn_mtl.m`), when its source is a buffer or a texture whose storage holds its content; the write marks the pages, so the next upload carries the levels. When the source is the GPU's (a texture whose host texture, or the one its packaged record holds, can be rendered into or written by shaders, `tex_gpu_touched`; a buffer a copy, fill or update queued in the submission, a shader or a copy of a submission still running wrote, `pend_hits` / `sbw_hits` / `gw_live`), when a readback into the source or the destination is still pending (`mrb_pending`: the storage lacks those bytes until it lands, and the later copy must land after it), or when the CPU mirror cannot do the copy (a format change, a scaled or flipped copy drawn by `copy_t2t_draw`), the destination region is read back after the copy on the GPU into a shared buffer and written into the storage in the submission's completion handler, before its fence is published (`mirror_readback` / `mirror_rb_land`, a kind 4 post-op). Depth and stencil formats, multisample textures and 3D destinations are left out and reported red.
- **Check:** `t_nvn` 6c3 checks region and whole-level copies between textures of different block heights (RGBA8, BC1) and from a strided buffer, byte for byte against a level round trip; `t_readback` copies a cleared render target into a packaged texture and reads the clear colour back from its storage. In the log, `texture→texture copy into ser=… written into the storage as well` or `… read back into the storage when the GPU completes`.
- **Found on:** [Yoshi's Crafted World](../../dm/games/01006000040C2000/README.md): the Yoshis' 1024² DXT1 base colour and BC5 normal textures, 11 levels; level 4 of one held a pink felt texture, of the others zeros; the speckles began when the opening went dark.

### Render passes into textures uploaded from their storage

- **Symptom:** what the GPU drew into a texture is lost later: a texture re-created from the same memory, or one the game moved with `memcpy`, comes out zero; on the Switch it holds the drawing. Tears of the Kingdom: the grass of a meadow is drawn with no length and the ground under it goes dark, a few seconds after loading, tile by tile around Link.
- **Cause:** a texture the host uploads from its storage (a packaged layout, CPU-written: a flush over its range is enough) can also be a render target. The hardware's render pass writes the storage; the host's wrote only the host texture, and the storage kept what it held before. Tears of the Kingdom creates the normal maps of its terrain tiles (260×260 RG8, 9 levels, `MINIMAL_LAYOUT`, in a `CPU_CACHED | GPU_CACHED` pool) on zeroed memory, flushes a range covering them (which made them CPU-written), then draws each level. Its resource defragmentation (`main+0x5bc988`, terrain resources with the magic `TERA`) later moves each tile with `memcpy` and re-creates the texture at the new address: uploaded from there, the tile was zero. The grass vertex shader scales each blade by how flat the ground is, from that normal map: a zero normal is horizontal, so every blade got length 0, and the terrain's lighting lost its normals too. The 226 tiles drawn at loading were moved within the first half minute; the 410 the defragmentation created afterwards were never drawn again. Such a texture was also attached for rendering without Metal's render-target usage, which Metal's validation stops at.
- **Fix:** `attach` (`nvn_mtl.m`) gives a texture made without render-target use that use (`tex_upgrade`, as `clear_tex` does for a clear). A colour attachment whose texture the host uploads from its storage (`tex_storage_dst`) is read back into the storage when its pass ends (`rt_mirror_add` / `rt_mirror_flush`, through `mirror_readback`, as for [GPU copies](#gpu-copies-into-textures-uploaded-from-their-storage)): its whole level, and in a layered or slice-merged pass every layer the pass draws (`rt_mirror_layers`, from `renderTargetArrayLength`); the copy encoder the readbacks open is closed again before `rp_end` returns, since its callers may open an encoder of their own next (left open, the opening of a new game stopped presenting at frame 1983). That write is not a change to upload: the completion handler stamps and protects the storage again and keeps the texture's bytes in its edge pages (`pkg_absorb_note`); at the next resolution the texture keeps its host texture when that is still the one read back, no page wholly inside it was written since and its edge bytes are unchanged (`pkg_absorb_take`), and while a readback is pending it is not uploaded at all (`pkg_absorb_pending`). Without that, the four CPU-written targets Tears of the Kingdom draws into every frame were uploaded again after every readback (31,249 uploads in 200 s instead of 19). A re-upload of such a texture (a CPU write into it) keeps the render-target use its last texture was upgraded to (`pkg_upload`'s `keep`); made without it, every re-upload was followed by another upgrade, a second texture and a copy of every level, at its next attachment.
- **Open:** not read back: 3D textures (as for GPU copies; `nvn_tex_mirror_b2t` writes 2D layers only), block-compressed targets drawn through a shadow (`bcrt_attach`; the shadow is copied back into the host texture only) and depth targets. A CPU write into the texture made after its pass is replayed and before the readback lands gets a page generation below the readback's stamp, so `pkg_absorb_take` keeps the host texture without it (the storage holds it; the next CPU write to that page uploads it). Telling those writes from the readback's own needs a dirty-tracker query for a generation range; no game is known to write a texture while the GPU draws into it.
- **Check:** `t_readback` clears a packaged texture through a render pass, reads the clear colour back from its storage, checks that resolving it does not upload it again, then copies the storage to another address with `memcpy` and reads the colour from a texture created there; without the readback both reads fail. In the log, `render→texture copy into ser=… read back into the storage when the GPU completes`.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md) on the Mac: meadows lost their grass and went dark within half a minute of loading; with the fix the grass stays through three minutes of standing and walking. Uploads of textures that changed in the pool, same script: 40 before, 449 after, almost all while loading; frame times the same (p50 about 18 ms).

### Readback buffers kept for reuse

- **Symptom:** the `[nvn] residency set up to frame …` line shows hundreds of additions from `mirror_readback`'s line in `nvn_mtl.m` every 300 frames, and as many removals, with the residency commits' time up. Tears of the Kingdom: about 1,500 every 300 frames on an iPhone 17 (1.4.2, [seedds/nxnative#931](https://github.com/seedds/nxnative/issues/931)), about 370 on a Mac.
- **Cause:** each [readback into a texture's storage](#render-passes-into-textures-uploaded-from-their-storage) made a shared buffer of its size, put it into the residency set, and released it after its submission, which took it out again: a residency commit each way, for about five render passes a frame in Tears of the Kingdom.
- **Fix:** `mrbuf_get` / `mrbuf_put` in `nvn_mtl.m`, for every game: a readback takes a kept buffer of its power-of-two size (4 KB and up) and gives it back once it has landed (`mirror_rb_land`, after the GPU is done with it); at most 16 MB of them is kept, and a buffer past that leaves the residency set and is released.
- **Check:** `make -C dm/gfx/tests GAME=<id> check_t_readback`: a landed buffer serves the next readback of its size, a larger one takes the next size, no more than 16 MB is kept, and a landed buffer is held only by the kept set ([Completion readbacks survive a guest waiter's pool](memory.md#completion-readbacks-survive-a-guest-waiters-pool)).
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md#found-on-this-game): its four CPU-written render targets (256×256, 192×192, 96×96, 500×400) are read back every frame. On the Mac (Apple M4 Pro, 2026-11-03, the regression route) 390-418 buffers every 300 frames on `main` at `12915a31`, none with the fix; the residency numbers of both fixes together are under [Snapshot textures taken by format and size](rendering.md#snapshot-textures-taken-by-format-and-size).

### Sampled textures in a sparse pool uploaded through the page table

- **Symptom:** textures that should hold CPU-written data come out magenta (ASTC's error colour) or black.
- **Cause:** textures in the sparse virtual pool hold what the CPU wrote into physical pages before mapping them: the streamed textures (flag 0x20, packaged layout) and plain sampled ones with no packaged layout. The replay took the plain ones for GPU textures, so their Metal copies stayed 0.
- **Fix:** a sampled texture in a sparse pool (not a render target, image, linear or buffer texture) is now uploaded like a packaged one, through the pool's page table ([Textures read through a 4 KB page table of a sparse pool](#textures-read-through-a-4-kb-page-table-of-a-sparse-pool)).
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md): the hunter's innerwear (ASTC 6x6) came out magenta and the face black.

### A sparse texture's CPU-write check at every lookup

- **Symptom:** the submit thread spends most of its time in `nvn_dirty_since` under `nvn_sparse_since`, called from `tex_mtl` (render-target and texture lookups of textures in a sparse pool).
- **Cause:** a sparse texture is checked for CPU writes by walking its range of the pool's page table and asking the dirty tracker about each run of physical pages. The check was skipped only when nothing had been written anywhere and nothing remapped since the last one; a game that streams pages remaps every frame and writes some page between nearly every two draws, so every lookup walked again. Each run then searched every registered range (`find_span`; about 4,300 in a Mario Kart World race).
- **Fix:** a texture is checked once per submission: a submission sees the CPU writes and the mappings made before it, as NVN's (a map is a CPU call, not a command), so a second check in the same submission can only find writes the GPU work need not see. A skipped check leaves the write and mapping generations of the first, so what another thread writes or maps during the replay is found at the next submission's check. The walk keeps the last range it found (`nvn_dirty_since_cached`).
- **Found on:** Mario Kart World 1.0.0 (#900; its terrain render targets are sparse, up to 16384x16384): in a race on an M4 Pro, translation 9.7 → 6.25 ms a frame, frame time p50 18.2 → 16.66 ms and p95 23.4 → 17.0 ms. Measured on its fork, where those targets are Metal placement-sparse textures; the check and the walk are the same here.

### Dirty-page ranges of pools that share a host page

- **Symptom:** some textures are re-uploaded every frame (several MB per frame).
- **Cause:** the dirty-page tracker registers each memory pool rounded to 16 KB host pages, but NVN pools are only 4 KB aligned, so neighbouring pools share their first / last host page and one address can lie in several registered ranges. Taking the first range that contains an address judged textures in the second pool untracked (always dirty), released only one range on a write fault, and could unregister the neighbour when a pool was destroyed.
- **Fix:** `nvn_dirty.c` now uses the range that holds the whole span, releases every range covering a faulting page, and removes a range by the pool's exact start.
- **Found on:** the source names no game.
- **Check:** `t_dirty_slots` covers it.

### Writes beside a packaged texture in a shared host page

- **Symptom:** packaged textures re-uploaded at every use with unchanged content; upload staging and frame times rising.
- **Cause:** the game keeps each texture of its models in a memory pool of its own, the pool's storage 4 KB after its own object, in the same 16 KB host page, and writes there every frame. The dirty-page tracker marks whole host pages, so every such texture counted as changed at each use; hashing showed every re-uploaded content unchanged.
- **Fix:** a packaged texture keeps the bytes of its storage that lie in its first and last host pages when they are shared (`pkg_edges_take`); when only those pages were written and the bytes are as uploaded, it is protected again instead of re-uploaded (`pkg_edges_clean`, counted in the `packaged textures` line as `writes beside a texture only`).
- **Found on:** [Pokémon Sword](../../dm/games/0100ABF008968000/README.md): 73,232 re-uploads in 70 s from a save, 12 GB of upload staging in 8 minutes, frame times rising to 75 ms; the same 70 s with the fix: 0 re-uploads and 72,584 such checks.
- **Check:** `t_nvn` 6c1 covers it.

### Dirty-page tracker race and early stalls

- **Symptom:** about one run in four stalled early (around the opening movie or the title), with no log output, not even the time-limit report.
- **Cause:** two candidates. First, the stalls happened before the real thread pause for IL2CPP's GC ([Pausing threads and reading their context for garbage collectors](threads.md#pausing-threads-and-reading-their-context-for-garbage-collectors)): without the pause the collector ran while other threads kept changing the heap. Second, fixed later: a race in the dirty-page tracker (`nvn_dirty.c`, ported from d2r-mobile) could leave a page read-only while marked writable, so a game thread writing it spun forever in the fault handler; Engage's movie decoder writes frames into texture storage from a host thread while the replay thread protects pages.
- **Fix:** the thread pause for the GC, and the tracker race fixed; `tests/t_dirty_race.c` reproduces the race.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md): probably fixed; since the pause, 12 runs (eight of them 200 s runs through the movie and title, four of 330 to 850 s) have not stalled once, where one in four used to.
- **Note:** if a stall comes back, sample the process when its log stops growing (`sample <pid> 3`) to see where every thread waits. The crash handler writes one line with `write()` and exits if the stderr lock stays taken for 1 s, so a stall that holds that lock shows up as such.

### Dirty-page writes missed during upload

- **Symptom:** a CPU-written texture stays stale after later writes; letters can be missing from a glyph cache.
- **Cause:** a protect skipped a page another thread was unprotecting, leaving it writable with a generation already covered by the upload stamp. Taking the stamp after protecting, or setting the cached last-check generation after the upload, could also cover a fault whose bytes arrived after the read. Separately, an unprotect advanced the global clock before publishing the page generation: a checker could cache a clean result in between and never scan again once the page became writable. Both the page-fault path and batched host pre-write path had that publication window.
- **Fix:** take the stamp before protecting and reading, and start the last-check generation at that stamp. Finish any pending protection run before waiting for a BUSY page, then retry that page. Both unprotect paths use `stamp_page` to publish the page generation and advance the global clock once more afterward, invalidating checks that ran before publication. These are general tracker and cache rules, with no game-specific gate.
- **Found on:** [Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md), reported in MacNativeGames commit `a166602` and ported through [PR #197](https://github.com/seedds/nxnative/pull/197). The missing-letter symptom has not been reproduced in nxnative; the tracker failures are reproduced locally by the regression tests.
- **Check:** `t_dirty_race` covers the BUSY-page retry, stamp ordering, and a check between clock allocation and generation publication in both unprotect paths. The publication cases fail on main; the batched case also fails with the original PR applied.

### A retired pool remains read-only

- **Symptom:** a host system call writing into storage after its NVN pool was finalized fails with `EFAULT`; a direct write can fault after the tracker stopped owning the page.
- **Cause:** `nvn_dirty_protect` found a live range without holding the slot mutex. Pool retirement could restore its pages to writable and mark the range dead before protect changed the page state to BUSY. Protect then set the retired storage read-only, with no live range left to release it. In the same window a new pool could reuse the slot, and protect would then compute its pages from the new pool's base and state array.
- **Fix:** hold the pool-slot mutex across protect's lookup, page-state transitions and final `mprotect`. Pool creation and retirement already hold that mutex. The page-fault and host pre-write paths still take no slot mutex, so they can finish a BUSY transition while protect waits.
- **Found on:** code review of the shared tracker, from a fork's compatibility commit `587d5206` ported through [PR #686](https://github.com/seedds/nxnative/pull/686). No game is known to hit it: a game normally finalizes a pool only after the GPU is done with it.
- **Check:** `t_dirty_retire_race` retires the pool between protect's lookup and its first page-state exchange, then reads 16 bytes into the pool with `read()`. Without the fix, retirement completes first and the read fails with `EFAULT`; with it, retirement waits for protection, unregisters the pool and the read returns 16. `t_dirty_race` and `t_dirty_slots` cover page-state publication and shared / reused pool slots.

## NVN entry points

### Integer sampler border colours

- **Symptom:** `[RED] NVN unimplemented function called: nvnSamplerBuilderSetBorderColorui` during renderer initialization.
- **Cause:** only the floating-point setter was implemented.
- **Fix:** implement signed and unsigned integer setters with numeric conversion into the sampler's border-colour state. Metal's existing selection among transparent black, opaque black and opaque white then sees integer zero and one correctly; bit-casting integer one to float would select the wrong colour.
- **Found on:** [FINAL FANTASY X/X-2 HD Remaster](../../dm/games/0100BC300CB48000/README.md), launcher and both games.
- **Check:** `t_nvn` invokes both setters through the real entry-point lookup and checks initialized sampler state.

### Bound handles are the driver's 32-bit words

- **Symptom:** `[RED] sampler ID 4096 not registered` (or another ID past the sampler pool), once, from the first frames on.
- **Cause:** NVN's texture, separate texture and sampler handles are 64-bit, but the driver writes each bound one into the c2 handle table as a 32-bit word (texture ID in bits 0-19, sampler ID in bits 20-31), so the GPU never sees the upper half. The host kept the whole 64 bits of the handles of `nvnCommandBufferBindTextures`, `BindSeparateTextures`, `BindSeparateSamplers` and `BindImages`, and took the sampler ID as the handle shifted right by 20. EA SPORTS FC 27 binds separate textures with bit 32 set (`0x1000001d1`, texture 0x1d1): a shader that takes its sampler from that combined word got ID 4096, where the GPU reads sampler 0. The single `nvnCommandBufferBindTexture` record already read its handle as 32 bits.
- **Fix:** `bind_handles` (`hle_nvn.c`) keeps the low 32 bits of each handle before it moves the IDs to the part of the host's table of the pool the command buffer bound (which may then pass 12 bits).
- **Found on:** [EA SPORTS FC 27](../../dm/games/0100C49025D3E000/README.md), at the title and in the menus.

### Version and capability checks that fail quietly

- **Symptom:** device / subsystem setup returns quietly and the next stage never starts.
- **Cause:** a version or capability check failed (NVN API version, a feature query).
- **Check:** compare what the game queries with what the host answers. Two cases follow: [The device's API version](#the-devices-api-version) and [Newest GPU code version reported by the driver](#newest-gpu-code-version-reported-by-the-driver).
- **Found on:** every game (general).

### The device's API version

- **Symptom:** a null-pointer crash early in `nnMain`, right after NVN setup should have happened (Super Mario 3D World + Bowser's Fury, `main+0x759e08`: the device singleton `main+0x1a28ea0` is still null).
- **Cause:** the game checks `nvnDeviceGetInteger(API_MAJOR / API_MINOR)` for major == 53 and minor ≥ 313 (`main+0x711c88`) and quietly skips creating its device otherwise.
- **Fix:** the reported version follows the SDK (`hle_nvn.c` `nvn_api`: 53.313 for SDK 10 and older, 55.15 for later ones, Mario's requirement); the profile key `device_api` overrides it. NVN 53 also brings `nvnQueueAcquireTexture` (the older window acquire: the queue waits instead of a sync object), `nvnSamplerCompare` and the debug group calls (no-ops).
- **Found on:** [Super Mario 3D World + Bowser's Fury](../../dm/games/010028600EBDA000/README.md) (SDK 10); [Super Mario Bros. Wonder](../../dm/games/010015100B514000/README.md) (needs 55.15).

### Newest GPU code version reported by the driver

- **Symptom:** null read at `main+0xb8462c` right after the first two textures, called from `main+0x670eb0` (Bayonetta 3), or cleanup crashes at `main+0x9c0d1c` after shader initialization fails (ASTRAL CHAIN).
- **Cause:** not the fault itself: `nnMain`'s start-up (`main+0x6705c0`) returns false when any of its 30 steps fails, the game then skips its main loop and goes straight to shutdown, and the shutdown walks a pool (`main+0x4efce70`) whose initialization (`main+0xb840d0`) came after the failed step. `DM_PROBE` on each step's entry (lldb was unavailable: developer mode off) showed the last one entered, `main+0x69c698`, the built-in shaders of `shader2.dat` (`hwfillcolor`, `hwshaderpf`, …), and probes inside it the step that failed: creating a shader from its object (`main+0xd0d998`). The engine checks each shader object's GPU code version (byte 6 major, byte 7 minor of the object) against the driver's range (`nvnDeviceGetInteger` 0x2E–0x31: newest major, oldest major, newest minor, oldest minor) and refuses newer code; the host reported 1.14, the table measured on Super Mario Odyssey's SDK 7 driver, and `hwfillcolor.vso` is 1.15 (the game's objects run from 1.9 to 1.16).
- **Fix:** report the translator's supported GPU-code range, through 1.16, independently of the linked SDK. Precompiled shaders can come from a newer compiler: ASTRAL CHAIN links SDK 6.4 but ships 1.15 objects, defeating the earlier SDK-based floor. Keep the additional floor from a GLSLC the game carries (`hle_nvn.c`; that GLSLC check: [GLSLC GPU code version against the driver's range](shaders.md#glslc-gpu-code-version-against-the-drivers-range)). `t_nvn.c` checks the advertised range even without a loaded SDK.
- **Note:** a wider range only lets more shaders pass such checks.
- **Found on:** [Bayonetta 3](../../dm/games/01004A4010FEA000/README.md) (SDK 13.3.2, PlatinumGames' engine); [Catherine: Full Body](../../dm/games/0100BF00112C0000/README.md) (its GLSLC 8.2.1 set the earlier SDK 8 value); [Super Mario Odyssey](../../dm/games/0100000000010000/README.md) (the table's 1.14); [ASTRAL CHAIN](../../dm/games/01007300020FA000/README.md) (SDK 6.4, precompiled GPU code 1.15).

### The Switch 2's device values

- **Symptom:** none seen: a Switch 2 game asking `nvnDeviceGetInteger` got the Tegra X1's values (measured on a Switch 1), and items past 0x5F answered 0 with a red line.
- **Cause:** NVN2 is compiled into the game's own `sdk`, and its answers differ: the procedure table's `DeviceGetInteger` calls a switch over the items (Donkey Kong Bananza's sdk 20.5.19: `sdk+0x5d9960`, items 0 to 0xA9, the default 231) that falls back on a table lookup (`sdk+0x5c0a20`). The item numbers are NVN's (API 231.30 at 0 / 1, GPU code 201.0 to 201.17 at 0x2E to 0x31, 201 = 0xc9, the version of its shaders' control sections). Ampere's differ from the X1's in 32 vertex bindings and attributes, 1,024 compute threads, an unbounded dispatch width, 16 texture levels, 16 KB scratch granularity, 2 KB shader code padding and 32 separate sampler bindings, and items 0x60 to 0xA9 are new. Seventeen items come from the device's own GPU description at run time (scratch scale factors, L2 size, texture layers and sizes, linear stride alignment, pool page size and others), which the binary cannot give.
- **Fix:** `g_devinfo2` (`hle_nvn.c`), taken for an SDK with the Switch 2's spec (`dm_sdk_ounce`): NVN2's constants, read statically, except vertex bindings and attributes, which stay 16, all the host's tables hold. The run-time items keep the X1 table's value below 0x60 and answer 0 with a red line above it.
- **Found on:** [Donkey Kong Bananza](../../dm/games/040074A01BF12000/README.md): plays as before with the new values (a 6,900-frame run at 60 fps, no red line).
- **Note:** the switch's own constants move with the SDK (API 231.30 and GPU code minor 17 in 20.5.19; 231.39 and 20 in 21.4.4), and games check them: [Splatoon Raiders](../../dm/games/0400380020056000/README.md) requires minor 39 or more and otherwise creates no graphics system (`main+0x31b86b0`). So the host reads them from the game's own `sdk` (`dm_sdk_nvn2_item`, `loader.c`), and `g_devinfo2` holds the newest seen for what it cannot read.

### Extensions fetched by name and called unchecked

- **Symptom:** a jump to pc=0 right after the NVN bootstrap loader (`lr=main+0x37fe930`, `[RED] NVN address lookup of an unknown name: nvnSetExternalDebuggerEnabledNVX`).
- **Cause:** the game fetches an NVX extension of the SDK 8 driver by name and calls it without checking the pointer. Unknown names return null, as the real driver does for a name it lacks; this one the driver has.
- **Fix:** it joins `dm/gfx/nvn/names.txt` (thunks regenerated with `gen_protos.py`; no prototype in `nvn.h`, so `NVN_NOPROTO`) as a no-op.
- **Found on:** [Super Smash Bros. Ultimate](../../dm/games/01006A800016E000/README.md) (13.0.5).

### NVN entry points on the big host stack

- **Symptom:** SIGBUS in `AGCDeserializedReply`, address in the guest stack region.
- **Cause:** the render thread's stack is small, and Metal's driver compiles internal programs (a fast-clear blit) on the calling thread while the host ends a render pass inside `nvnQueueSubmitCommands`, which overflowed the guest stack.
- **Fix:** all implemented NVN functions except the `nvnCommandBuffer*` recording calls run on the per-thread 512 KB host stack (`nvn_big_entry` in `nvn_entry.S`). The general mechanism (`dm_bigstack_enter`, and `HLE_FORCE_BIG` for the file system hooks): [Host calls on small guest stacks](memory.md#host-calls-on-small-guest-stacks).
- **Found on:** [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md).

### State read back through NVN getters

- **Symptom:** red `NVN unimplemented function called` lines for `nvnColorStateGetBlendEnable`, `nvnBlendStateGetBlendFunc`, `nvnPolygonStateGetCullFace`, `nvnDepthStencilStateGetDepthTestEnable`, `nvnBufferBuilderGetSize`, `nvnTextureBuilderGetWidth`, ….
- **Cause:** Trinity builds its pipeline states from state objects it reads back; the red-reporting stub returned whatever was in x0, the object's address, so a blend enable or depth test read as on and a texture builder's width as an address.
- **Fix:** the getters read the packed state objects (`hle_nvn.c`); the multisample setters the host lacked (alpha-to-coverage dither, raster samples, coverage modulation and coverage-to-colour) are recorded in fields `ms_st` already had.
- **Found on:** [Pokémon Legends: Z-A](../../dm/games/0100F43008C44000/README.md) (19.3.11, Game Freak's Trinity engine; one boot).

### Device getters that were red stubs

- **Fix:** `nvnDeviceGetWindowOriginMode` and `nvnDeviceGetDepthMode` were red stubs: the game sets neither mode and reads both. `nvnDeviceGetTexelFetchHandle` returns the texture ID with sampler 0.
- **Found on:** [Pokémon Sword](../../dm/games/0100ABF008968000/README.md) (SDK 7: the window origin and depth modes); [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md) (SDK 11: `nvnDeviceGetTexelFetchHandle`).

### Pool size getters, texture address and command memory size

- **Symptom:** an abort in device setup when `nvnTexturePoolGetSize` / `nvnSamplerPoolGetSize` returned 0.
- **Fix:** implemented: `nvnTexturePoolGetSize` / `nvnSamplerPoolGetSize` and their memory pool / offset getters; `nvnCommandBufferFenceSync`, which Unreal's RHI records in its command buffers ([Fence syncs recorded in command buffers](#fence-syncs-recorded-in-command-buffers)); `nvnTextureGetTextureAddress` (pool address + offset, as `nvnMemoryPoolGetBufferAddress`); `nvnCommandBufferGetCommandMemorySize` (used + free, as in NVN; `GetCommandMemoryFree` is what is left in the current segment).
- **Found on:** [Octopath Traveler 0](../../dm/games/01005270232F2000/README.md) (the pool getters and `nvnCommandBufferFenceSync`, new with Unreal); [DRAGON QUEST XI S](../../dm/games/01006C300E9F0000/README.md) (Unreal Engine 4.18: `nvnTextureGetTextureAddress`, `nvnCommandBufferGetCommandMemorySize`).

### Builder storage getters

- **Symptom:** `[RED] NVN unimplemented function called: nvnTextureBuilderGetMemoryOffset`, then `nvnTextureBuilderGetRawStorageClass`.
- **Cause:** the renderer reads a texture builder's storage back before it initializes the texture; the host had the texture's getters (`nvnTextureGetMemoryPool` / `Offset` / `RawStorageClass`) but not the builders'.
- **Fix:** the texture, buffer and event builders return the pool and offset `SetStorage` stored (the pool getters return its address, as `nvnTextureGetMemoryPool` does; the header the prototypes come from declares the builders' by value, so they are `NVN_NOPROTO`), and the texture builder's raw storage class is the texture's (1): it only names the page kind a virtual pool maps (`NVNmappingRequest.storageClass`), which the host's virtual pools do not distinguish.
- **Found on:** [EA SPORTS FC 27](../../dm/games/0100C49025D3E000/README.md) (SDK 22, Frostbite's NVN renderer NRO).

### `Map` of a pool without CPU access returns NULL

- **Symptom:** `[nvn-mtl] FATAL buffer copy address is in no memory pool src=0x70d08a8b00 dst=0 n=0x29a00` as a match loads: a recorded `CopyBufferToBuffer` whose destination is 0.
- **Cause:** NVN maps a buffer or pool created with `NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS` to NULL, and the game depends on it: Frostbite's renderer (`OpcodeCopySubresource` in `Engine.Render.Core2.PlatformNvn.nro`) takes a buffer's GPU address as `nvnBufferGetAddress + (its data pointer − nvnBufferMap)`, with a null data pointer for a GPU-only buffer. The host's storage is CPU memory, and its `Map` returned the same pointer `GetAddress` does, so the sum came to 0 (traced to a `BufferMap` of a pool with flags `0x21`, CPU_NO_ACCESS | GPU_CACHED, right before the copy).
- **Fix:** `nvnBufferMap` and `nvnMemoryPoolMap` return NULL when the pool has no CPU access, as in NVN (`hle_nvn.c`). The host's own accesses to the storage go through the pool, not through `Map`.
- **Found on:** [EA SPORTS FC 27](../../dm/games/0100C49025D3E000/README.md) (SDK 22): the first match load.

### NVN calls that were red stubs

- **Symptom:** red stub lines for NVN calls.
- **Fix** (`hle_nvn.c`):
  - `nvnDeviceSetIntermediateShaderCache`.
  - `nvnCommandBufferDiscardColor`: a discarded target is undefined, so keeping its content is correct.
  - `nvnWindowGetCrop`: read back every few frames; before any `SetCrop` it is the whole first window texture.
  - `nvnSamplerBuilderGetLodClamp`.
- **Found on:** [The Great Ace Attorney Chronicles](../../dm/games/010036E00FB20000/README.md) (SDK 10.4.1: the first two), [Bayonetta 3](../../dm/games/01004A4010FEA000/README.md) (SDK 13.3.2: the two getters).

### `nvnDeviceRegisterFastClearDepth` is a hint

- **Fix:** `nvnDeviceRegisterFastClearDepth`, a hardware hint, returns success instead of reaching the red stub.
- **Found on:** [The Legend of Zelda: Tears of the Kingdom](../../dm/games/0100F2C0115B6000/README.md).

### SDK 8 window and copy-stride entry points

- **Symptom:** `nvnWindowGetPresentInterval`, `nvnCommandBufferSetCopyImageStride` missing.
- **Fix:** implemented (the latter is the layer stride of texture ↔ buffer copies).
- **Found on:** [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md) (SDK 8.2.0).

### Images registered on their own

- `nvnTexturePoolRegisterImage` writes the same table as `RegisterTexture` (image handles are pool indices).
- **Found on:** [Hyrule Warriors: Age of Calamity](../../dm/games/01002B00111A2000/README.md).

### Image bindings supply shader-write intent

- **Symptom:** `[RED] texture bound to an image slot lacks the shader-write usage`, and compute image writes go to a dummy texture.
- **Cause:** Metal usage came only from the texture builder's `IMAGE` flag. A render texture may instead have only `COMPRESSIBLE`, then its descriptor be used in a shader image slot. Snowdrop writes these descriptors with `RegisterTexture`, not `RegisterImage`. Its depth-conversion compute shader `cf5ba107e29b51f4` gathers from a depth texture and unconditionally writes four pixels per invocation to a 256×256 R16F texture whose flags are `0x8`. Its Metal texture had read / render-target usage (`0x5`), so the image binding was rejected.
- **Fix:** before a draw or dispatch uses a writable image, `image_prepare` resolves it with image intent and ensures its backing allows shader writes. `RegisterImage` also keeps this intent in its host snapshot; neither path changes the guest texture. A texture already rendered into is upgraded through `tex_upgrade`: end its render pass first, copy every mip / slice, replace all shared serials and invalidate cached sampled views. Load-only image slots accept read-only storage without upgrading, and ordinary render targets retain their original usage. `t_image_usage.m` exercises both registration entry points, a late registration while a render pass is open, the clear and other mip surviving the upgrade, and a GPU image write read through a sampled alias.
- **Found on:** [Mario + Rabbids Sparks of Hope](../../dm/games/0100317013770000/README.md) (SDK 13.3.5, Snowdrop).

### Plural texture binds, builder-registered samplers, strided texel writes, device item 0x67

- `nvnCommandBufferBindTextures` (the plural of `BindTexture`), `nvnSamplerPoolRegisterSamplerBuilder` (a sampler registered from its builder), `nvnTextureWriteTexelsStrided` (`WriteTexels` with row and image strides) and device item 0x67, which the game reads before every program it loads and uses as the alignment of its copies of the shader (answered 256, not measured).
- **Found on:** [FINAL FANTASY TACTICS - The Ivalice Chronicles](../../dm/games/010038B015560000/README.md).

### `nvnPrivate0`, an inlined command-buffer fast path flag

- `nvnPrivate0`, a new SDK 20 import, needs nothing: it is a flag in `sdk`'s bss that enables an inlined command-buffer fast path when the NVN driver sets it (it stays 0). Found on [The Legend of Zelda: Breath of the Wild](../../dm/games/01007EF00011E000/README.md) (1.9.0, SDK 20.5.22).

### `nvnTextureBuilderGetViewOffset`, `nvnTextureViewCompare`, `nvnBlendStateSetAdvancedNormalizedDst`

- **Symptom:** `[RED] NVN unimplemented function called` for each.
- **Fix:** the builder's offset is the texture's (`view_offset`). The compare is true when the two views set the same fields to the same values: that true means equal is taken from the name, as no caller in `main` was found to confirm it. The blend flag is kept for its getter; the fixed-function factors the replay uses read a normalized target as it is.
- **Found on:** [Kirby Air Riders](../../dm/games/040077201BE50000/README.md).

### Texture IDs of one storage share one packaged texture

- **Symptom:** a pool walk makes gigabytes of textures from far fewer images: Ocarina of Time's 32,768 IDs name 3,607 storages, and one texture per ID came to 2 GB.
- **Cause:** a packaged texture belongs to its serial, and the engine registers the same storage under many texture objects.
- **Fix:** general (`nvn_mtl.m` `g_pkg_share`, `tex_alias_key`, `pkg_shared`). Packaged textures are shared by storage, layout and format through a weak table, so a new serial of the same storage takes the existing one. A pool's removal evicts its keys (`mtl_alias_drop_pool`), and the walk's budget does not count a texture that is already shared.
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md).

### Finalizing a copy of a texture object

- **Symptom:** thousands of first uploads a frame in a scene whose textures do not change: Ocarina of Time, about 15,000 a frame.
- **Cause:** the engine copies an `NVNtexture` by value and finalizes the copy, while the original stays in use. The host keyed the texture by serial, so finalizing the copy deleted the host texture of the original, and its next use uploaded it again.
- **Fix:** general (`nvn_mtl.m` `mtl_tex_release`). The registry keeps the address of the object that was initialized, and a finalize is ignored when that owner still holds the texture's magic and serial (read with `vm_read_overwrite`, as the owner's memory may be gone). `t_cpu_tex` checks both orders.
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md).
