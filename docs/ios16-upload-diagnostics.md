# Packaged texture upload diagnostics

`DM_NVN_UPLOAD_DIAG_FRAMES=4` captures the next four frames that reach the packaged upload path. It is disabled by default and may be enabled in a running `DM_CTL` session with `env DM_NVN_UPLOAD_DIAG_FRAMES=4`. The request is one-shot. Change its token to rearm, for example `4:second`; setting it to `0` stops it at the next upload frame.

The diagnostic never skips an upload or changes a dirty stamp. It records both initial uploads and dirty reuploads, including serial, storage/pool range, dimensions, format, selected CPU/GPU path and source storage size. Each record reports upload encoding time separately from snapshot/hash overhead. Normal frame timings still include the real diagnostic overhead and must not be treated as a performance benchmark.

Snapshots are bounded to 16 requested frames, 128 records, 32 serials, 32 MiB retained across serials and 8 MiB per texture. A replacement snapshot temporarily needs up to one additional texture allocation. Oversized or excess records report the applicable limit and leave uploads unchanged. Snapshots are freed when the session reaches its frame/record limit or its request changes.

For a repeated serial with the same storage identity and size, the helper compares every source byte with the preceding snapshot, grouped by host pages. `exact=unchanged` is an exact `memcmp` result, not a hash match. The SHA256 digest attributes the captured bytes in a log without exposing their contents. Dirty generations are checked around the copy; a covered write reports `racy`, and an uncertain prior snapshot reports `prior-racy`. Such records cannot establish redundant uploads. `baseline` has no prior comparable snapshot. `changed` includes the number and first offset of differing pages, which can distinguish localized edits from broad source changes.

This establishes whether successive captured source snapshots were equal. It does not prove what an earlier GPU upload contained, nor that the source stayed unchanged between captures. Use an unchanged result only alongside successful upload records and the timing/source identity to design a subsequent correctness-preserving optimization.

## Publication ordering

A packaged upload protects its source pages and takes an upload generation before deswizzling or encoding a copy. A CPU write can fault after those source bytes were read and before the upload returns. Its page generation then exceeds the upload stamp. Publishing `seen = nvn_dirty_now()` at the end incorrectly treats that new generation as already inspected: the next lookup can take `now == seen` and return the older texture without checking its dirty pages.

Both initial and repeated uploads now publish `seen = upload_stamp`. A generation that advances during the upload must pass through the page check on the next lookup. A clean lookup may subsequently advance `seen`, because it actually inspected all writes through that generation. Lost-upload retries still run through the existing completion tracking and are unaffected.

The graphics tests exercise the real dirty-page tracking, shared-page pool handling and write-preparation races. `t_upload_diag` separately checks disabled operation, exact equality/change classification, concurrent-write attribution and size/record/frame bounds. It does not claim an end-to-end Metal regression for the publication ordering; that requires the real `NvPkg` resolution and GPU upload path.

## Exact CPU decode cache

The v58 city trace captured two dirty 512x512 textures over four frames. All six comparable source snapshots were byte-identical. The BC7 sRGB texture (NVN format 0x4e, ten mips, 351,744 source bytes) spent about 4.0 to 4.1 ms per upload in CPU decoding/encoding. The BC5 texture used the GPU decoder and needed only 0.07 to 0.12 ms of CPU encoding. This supports testing a CPU decode cache; it does not support skipping every dirty upload.

`DM_NVN_CPU_DECODE_CACHE=1` enables a bounded exact cache on CPU-BC reuploads. It is off by default pending device verification. The cache retains the compressed source snapshot and its decoded pixels, with a 32 MiB total limit, 32 LRU entries, an 8 MiB source limit and a 16 MiB decoded limit per entry. It compares every source byte, with no digest-based reuse. Storage identity, layout, dimensions, format, mip count and other descriptor fields must all match. A changed source is copied to owned storage and decoded from that same copy, so a guest write during the decode cannot make the baseline describe different input from the decoded pixels. The pre-comparison upload stamp remains the `seen` generation.

Every dirty candidate still receives an ordinary upload into the ordinary target texture. A cache hit only replaces CPU decompression with a copy of cached decoded pixels into independent Metal staging storage. This preserves the behavior of image-capable textures and lost-upload retries; it never assumes the current GPU texture still holds unmodified CPU pixels. LRU eviction cannot alter an in-flight staging buffer. Failed CPU decodes are not admitted. Setting the flag to 0 releases the cached byte buffers on the next CPU-BC reupload.

`[CPU-DECODE-CACHE]` reports hits, misses, evictions, retained bytes and comparison/source-copy costs. The upload diagnostic labels hits `cpu-bc-cached`; upload byte and reupload counters intentionally remain unchanged because the copies still execute. `t_cpu_decode_cache` uses the actual BC7 CPU decoder to check output parity, a write after snapshot capture, content/layout/pool changes, failed decodes, independent staging and byte/slot/size bounds. Device measurements must establish the actual frame-time improvement before enabling this by default.
