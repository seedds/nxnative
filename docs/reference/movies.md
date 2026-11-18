# Movies

The SDK's movie library (`movie::Decoder`, `movie::Extractor`), which the host takes over in `dm/host/hle_movie.c`, and the games' movie players. Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Decoder lifecycle

### The SDK's hardware decoder opens nvdrv

- **Symptom:** abort in NVIDIA's video path (`NvRmChannelOpen` → `NvOsDrvOpen`).
- **Cause:** the SDK movie library's hardware decoder opens nvdrv directly.
- **Fix:** the host takes over `movie::Decoder` (`hle_movie.c`, [`movie::Decoder` taken over as a MediaCodec-style protocol](#moviedecoder-taken-over-as-a-mediacodec-style-protocol)); nvdrv opens fail (`hle_nvdrv.c`).

### `movie::Decoder` taken over as a MediaCodec-style protocol

- **Symptom:** the game waits forever at a movie (Engage's 94 s opening `S01F.webm`; frames keep presenting, nothing happens).
- **Cause:** Unity demuxes the WebM itself and feeds VP9 packets to `movie::Decoder`; with no working decoder the movie never ends.
- **Fix:** `movie::Decoder` is a thin wrapper over Android's MediaCodec, and its vtable entries are symbol relocations, so `HLE_FORCE` on each method also catches Unity's virtual calls. The host implements the protocol: four `nn::os` events (`DecoderEvents` = input available, output ready, format changed, error), input indices (`AcquireInputBufferIndex`, none = 0x1011; `GetInputBuffer` fills the client's `movie::Buffer`; `SendInputBufferForDecode(index, offset, size, µs, flags)`), output indices (`AcquireOutputBufferIndex`, none = 0x100e; one NV12 frame per packet copied into the buffer Unity allocated, w × h × 1.5; `ReleaseOutputBufferIndex`). `Buffer`, `MediaData` and events are handled through the SDK's own exported functions (`SetDataAndCapacity`, `SetRange`, `SetInt32`, `SetRect`, `SignalEvent`). Unity reads `width`, `height`, `crop`, `nv12-y-stride`, `nv12-y-offset`, `nv12-uv-offset` and `slice-height`. The guest thread that sends a packet or returns an output index waits for the decode and signals the events itself, because `SignalEvent` is guest code and cannot run on a host thread.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md): when the movie ends the game continues to its title screen.
- **Note:** output slots, VP9 decoding and the output format keys are in their own entries ([Packets wait while every output slot is taken](#packets-wait-while-every-output-slot-is-taken), [VP9 on host threads with libvpx](#vp9-on-host-threads-with-libvpx), [Output format keys (`md_format`)](#output-format-keys-md_format)).

### Output format keys (`md_format`)

- **Symptom:** a movie in grey with vertical bands of colour, its luma sharp; or a read far outside guest memory at `main+0x344f74` on the first movie picture (CRI Sofdec2's NV12 → RGB conversion).
- **Cause:** the host never set `nv12-uv-stride` in the output format (Unity reads `nv12-uv-offset` only). CRIWARE's Sofdec hands its VP9 video to `movie::Decoder` in mode 1, NV12 into its own buffers, and draws the planes with its shaders; it reads `nv12-uv-stride` from the output format, and the bands were the top row's chroma: every chroma row was read from the first one. Catherine's Sofdec2 sends its H.264 through `movie::Decoder`, mode 1, output format 4; the player reads `nv12-y-stride`, `nv12-y-offset`, `nv12-uv-stride` and `nv12-uv-offset` from the output format and checks none of the results (`main+0x34e694`), so the chroma stride was stack garbage.
- **Fix:** the host now sets everything the SDK's `DecoderImpl::GetOutputBufferFormatFromDecoder` (SDK 19 `subsdk0+0x121f50`) does: also `nv12-uv-stride`, the luma (Y) stride as for any NV12 picture, `nv12-width` and `nv12-height` (`md_format` in `hle_movie.c`).
- **Found on:** [DRAGON QUEST VII Reimagined](../../dm/games/0100A9D01C446000/README.md) (the grey picture); [Catherine: Full Body](../../dm/games/0100BF00112C0000/README.md) (SDK 8.2.1, CRI middleware; one boot: the crash).

### SDK 4's `movie::Decoder` constructor without a priority

- **Symptom:** a call through a null pointer in the host's `movie::Decoder::Configure` right after `nnMain` starts the logos.
- **Cause:** SDK 4's `Decoder` constructor has no priority argument (`_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE`). The host took over only the four-argument one, so the SDK built its own decoder and the host's `Configure` read that object as its own.
- **Fix:** both forms are taken over now (`hle_movie.c`).
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md) (SDK 4.5.0; one boot).

### The complete-object destructor

- **Cause:** the game calls the complete-object destructor `movie::Decoder::~Decoder` (`D1`); the SDK's own one destroyed the host object as a `DecoderImpl`.
- **Fix:** the host now takes it over too.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md).

### Client events read through the registered pointer

- **Symptom:** the first deferred signal crashed in `InternalCriticalSectionImplByHorizon::Enter` (DRAGON QUEST XI S).
- **Cause:** `RegisterClientEvents` copied the four event pointers, because Dread's struct had been overwritten by its second movie. DRAGON QUEST XI S (Unreal's Switch movie player, VP9 with 6-channel Vorbis) does the opposite: it registers its struct right after constructing the decoder (`main+0x39edab0`, through the decoder's vtable) and fills it later, so the copy held leftovers (`0xe00000010`). The SDK keeps only the pointer (`DecoderImpl::RegisterClientEvents` stores it at +0xd0) and reads the entry each time it signals, skipping a null one (`SendOutputBufferEventAvailableToClient` and the others, `subsdk0+0x1feb0`).
- **Fix:** the host now does the same (`md_event` in `hle_movie.c`). When the client's entry is not a readable, 8-byte aligned pointer, it falls back to the entry's value at registration if that was one (Dread's case), and otherwise logs a red line.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md); [DRAGON QUEST XI S](../../dm/games/01006C300E9F0000/README.md).

### `Flush`, `Stop` and `Start`

- **Fix:** as with MediaCodec, `Flush` announces nothing, and no input or output index is handed out while the decoder is stopped. The "input available" after `Start` is signalled about 2 ms later from a guest thread, as the SDK's codec thread would. Pending signals are dropped at `Stop`, `Flush` and destruction.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md): the game ends a movie with `Flush`, `Flush`, `Stop`, destroy, and calls `Start` after a `Flush` when it loops.

### Input indices returned by a flush while a client holds them

- **Symptom:** the process dies when an activity starts, 2 runs in 5, with no `CRASH` line: the run ends with exit 137 (SIGKILL), and `~/Library/Logs/DiagnosticReports/dmhost-*.ips` has `EXC_BREAKPOINT` in `_os_unfair_lock_corruption_abort` ← `pthread_mutex_destroy` ← `md_free` (the movie decoder's destructor).
- **Cause:** each activity's preview is an H.264 movie (`movie/Movie/*.mp4`) the game loops with Flush and Start, from four threads at once (`DM_MOVIE_TRACE=1` logs every decoder call with its thread). Its event thread acquired an input index while the control thread's Flush waited for the packet being decoded; the flush then returned that index to the free list, it was handed out a second time, two packets shared one buffer (VideoToolbox -12909 on the overwritten one, in exactly the runs that died), and a fifth pending packet wrote past `pend[]` into the decoder's `npend` and `dmu`.
- **Fix** (`hle_movie.c`): a send now claims an index a flush returned, and `pend[]` cannot overflow. Handing such an index out last was not enough (with three hosts running at once one run still sent an index twice): an index the client held across a flush is now withheld for 250 ms unless the client sends it first, and an acquire refused only for that announces input when the first one comes back, so a client that abandons its indices, as MediaCodec's rules expect, loses none.
- **Found on:** [Big Brain Academy: Brain vs. Brain](../../dm/games/0100620012D6E000/README.md) (SDK 11.4.3, its activity previews).

### End of stream is the last input until `Flush` or `Stop`

- **Symptom:** when the prologue movie was skipped, 2.25 GB were mapped within seconds, past the application pool, and the game crashed in its allocator (DRAGON QUEST XI S).
- **Cause:** DRAGON QUEST XI S's movie player sends an end-of-stream packet (length 0, time 0) for every input it gets once its extractor is done, until the game stops the movie. Each one became an end-of-stream output that the player turned into an audio sample.
- **Fix:** also as with MediaCodec, an input marked end-of-stream is the last until `Flush` / `Stop`: no input index is handed out or announced after it (`in_eos` in `hle_movie.c`).
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md) (movie sub-list); [DRAGON QUEST XI S](../../dm/games/01006C300E9F0000/README.md).

### End-of-stream packets with no data, and deleted decoders

- **Symptom:** decoding the end-of-stream packet gave VideoToolbox −12909 and AudioToolbox −50.
- **Cause:** the game ends a stream with time −1 and the whole 4 MB input buffer as its length: that packet carries no data. It also deletes its decoders (`D0`).
- **Fix:** for `D0` the host frees the object with the game's `operator delete` after its own state.
- **Found on:** [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md).

### H.264 drain at the end of a stream goes on with released slots

- **Symptom:** the game never leaves a movie that has played to its end; the last picture stays on screen.
- **Cause:** `DM_MOVIE_TRACE=1`: the end-of-stream packet came with H.264 pictures still in the reorder queue, its output went out as a black picture without the end-of-stream flag, and the queued pictures were to follow one per free output slot (the drain). Unity held all 16 video slots by then. `ReleaseOutputBufferIndex` decoded again only when packets were queued, and none were, so the drain never went on: no flag, and the game waited for the movie's end for good.
- **Fix:** a released slot now goes on with a drain too, and the end-of-stream packet's own output takes the first queued picture (`jflush`), so no black picture shows before a movie's last frames (`hle_movie.c`).
- **Found on:** [AI: THE SOMNIUM FILES](../../dm/games/010089B00D09C000/README.md) (Unity 2017.4: the 6 s Spike Chunsoft logo `StreamingAssets/Movie/al_titel_cl_sc_1k_30.mp4`, H.264 High, before the title).
- **Note:** the audio decoder was never affected: AAC has no reorder queue.

## Output slots

### Output slot counts per decoder kind

A decoder hands out as many outputs as it has slots, and while every slot is taken it takes no input ([Packets wait while every output slot is taken](#packets-wait-while-every-output-slot-is-taken)). The counts now (`hle_movie.c`): `MD_NOUT` 64 for a Vorbis decoder (the most any decoder has), `MD_NOUT_AAC` 32 for AAC and `MD_NOUT_VIDEO` 16 for video; a slot is allocated when first used. Every decoder had 8 at first. The entries below raised them one game at a time, and each gives the counts of its time: audio 32 ([Audio decoders keep more outputs than a pre-decoding player fills](#audio-decoders-keep-more-outputs-than-a-pre-decoding-player-fills)), audio 64 ([Audio output slots for players that hold every output](#audio-output-slots-for-players-that-hold-every-output)), video 16 ([Video output slots: how far the extractor gets ahead](#video-output-slots-how-far-the-extractor-gets-ahead)) and AAC 32 ([AAC decoders offer at most 32 outputs](#aac-decoders-offer-at-most-32-outputs)).

### Packets wait while every output slot is taken

- **Fix:** while every output slot is taken, packets wait and their input indices are not returned, as with MediaCodec; dropping them instead lost frames.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md).

### Audio decoders keep more outputs than a pre-decoding player fills

- **Symptom:** the opening movie never starts, in about half the boots (DRAGON QUEST XI S: a black screen and no sound from boot on).
- **Cause:** Square Enix's `UMediaPlayer` has two natives of its own, `StartPreDecoding` and `IsPreDecodingCompleted`. The opening's Blueprint polls the second once a frame and only then plays its Matinee, whose event key calls `Play`. Its Switch player (vtable `main+0x77921b8`, `IsPreDecodingCompleted` at `main+0x42d4d20`) is complete when every selected track has at least one decoded output queued. The player starts its clock and its decoders on the first `TickInput` after `StartPreDecoding`. The render thread's first fetch (`main+0x39f07d0`, the `+0x5c7` flag) takes every decoded picture and shows the newest. The host decoded all eight video outputs (to 236 ms) within 36 ms. Each Vorbis decoder's 8 outputs were full by then, because nothing consumes audio before `Play`, so the extractor waited for an audio input and no picture followed the ones the render thread took. The movie started only when the game thread's poll came between the first decoded picture and that fetch.
- **Fix:** audio decoders then kept 32 outputs (`MD_NOUT`) and video decoders 8, so video keeps flowing after the first fetch. Since then audio decoders have 64 (Monster Train 2), AAC ones 32 (`MD_NOUT_AAC`, Mario vs. Donkey Kong) and video ones 16 (`MD_NOUT_VIDEO`, Animal Crossing: New Horizons); see [Output slot counts per decoder kind](#output-slot-counts-per-decoder-kind).
- **Found on:** [DRAGON QUEST XI S](../../dm/games/01006C300E9F0000/README.md): 8 boots out of 8 start; [Monster Train 2](../../dm/games/010051701FB46000/README.md) (64).
- **Note:** to find such a gate: probe the player's `SetRate` (`DM_PROBE=main:<off>`) to see whether the game ever asks to play, then follow the Blueprint native that calls it (the `FNameNativePtrPair` tables give the names).

### Audio output slots for players that hold every output

- **Symptom:** a movie stays black after its first picture, the sound never starts, and the game keeps presenting frames.
- **Cause:** the player takes every output a decoder offers and starts playing only once it holds about half a second of audio. The host then gave each decoder 8 output slots (before DRAGON QUEST XI S's 32 for audio) and takes input only while one is free (as MediaCodec does). `DM_MOVIE_TRACE=1` (each line now names the decoder): Unity held all 8 audio outputs, the audio decoder kept the next 4 packets and handed out no input index, the video packets behind them in the file never came, and the video decoder had given out one picture (the host releases H.264 pictures once more than 4 are queued).
- **Fix:** slot counts per decoder kind, measured: 8 audio / 8 video and 8 audio / 32 video hang; 64 audio / 8 video plays, starting with 27 audio and 8 video outputs held (peak 30 audio). 32 audio slots hold that peak with two to spare, so audio decoders got 64 (`MD_NOUT`; a PCM slot is 128 KB, allocated when first used), and video kept 8 (`MD_NOUT_VIDEO`: a 1080p picture is 3 MB, 8 MB as RGBA).
- **Found on:** [Monster Train 2](../../dm/games/010051701FB46000/README.md) (its 90 s intro, `StreamingAssets/intro_fmv.mp4`: H.264 High with 2 reorder frames and AAC interleaved packet by packet, played by Unity 2022.3's movie player: `movie::Extractor` in `subsdk0` feeds both decoders from one thread in file order). The game's A, B and + do not skip this intro.
- **Note:** later changes: video decoders have 16 slots ([Video output slots: how far the extractor gets ahead](#video-output-slots-how-far-the-extractor-gets-ahead)) and AAC decoders 32 ([AAC decoders offer at most 32 outputs](#aac-decoders-offer-at-most-32-outputs)); Vorbis keeps 64. `hle_movie.c` now has `MD_NOUT` 64, `MD_NOUT_AAC` 32, `MD_NOUT_VIDEO` 16.

### Video output slots: how far the extractor gets ahead

- **Symptom:** a movie shows as a black screen with the loading icon, one picture every few seconds, while its sound decoder keeps up.
- **Cause:** this player also holds every output it is offered (16 of 16 video, 59 of 64 audio), so how far its extractor can get ahead of the picture on screen is the video slots plus the 4 queued packets. With 8 video slots that was 0.4 s, and the audio behind the next video packet, whose clock the player follows, ran dry: 86 pictures in 25 s. With 16 or 32 slots all 1,960 play at 30 fps.
- **Fix:** a video decoder now has 16 (`MD_NOUT_VIDEO`); a slot is allocated when first used, so the 8 more cost 24 MB of NV12 at 1080p only in a player that holds them all.
- **Found on:** [Animal Crossing: New Horizons](../../dm/games/01006F8002326000/README.md) (the in-flight movie `Movie/OpeningMovie.webm`, VP9 1080p and Vorbis through `movie::Extractor`).

### AAC decoders offer at most 32 outputs

- **Symptom:** a crash in the game's heap free (`sead::ExpHeap`) on a movie thread, a second into the first movie, at the same address every boot (`pc=main+0x42f980`, a free-list unlink through a link of 0x3f, `addr=0x47`).
- **Cause:** the game's movie player keeps the outputs it holds in a ring of 32 entries {u32 output index, u32 flags, u64 time} per track (`main+0xe93d4c`: 0x200 bytes) and pushes with no bounds check (`main+0xe94140`), and the host's audio decoder offered 64 outputs, so the 33rd outstanding one, index 63, went past the ring. The console's AAC decoder cannot offer more than 32, or the game would overflow there too.
- **Fix:** AAC decoders now have 32 outputs (`MD_NOUT_AAC`; Monster Train 2 peaked at 30 held), Vorbis keeps 64.
- **Found on:** [Mario vs. Donkey Kong](../../dm/games/0100B99019412000/README.md) (1.0.1, SDK 15.3.0: its opening movie `Movie/Cutscene01.mp4`, H.264 and AAC through `movie::Extractor`).
- **Check:** `DM_PROBE_WATCH` on the link broke the movie's file reads (the page also held a 64 KB read buffer, and `read()` into a protected page fails with EFAULT), so the movie stopped and nothing crashed; `DM_PROBE_POLL=<address>+8=3f` (a snapshot only at the change to that value) showed it being written right after the block was relinked, and the values around it were the ring's pairs.

### AAC output buffer size is one frame

- **Symptom:** the movie then froze on its first second, its sound decoder full and the game taking no more audio.
- **Cause:** the player opens an AudioOut of its own (`main+0xe92c2c`), sizes its buffers by the audio decoder's `GetOutputBufferSize`, decodes straight into them (`SetDataAndCapacity` with that size) and appends each whole, advancing its audio clock by size / 4 samples (`main+0xe92a74`). The host answered 128 KB, a bound for any Vorbis packet, so every 4 KB AAC frame played as 0.68 s and the clock crawled (a temporary log of `AppendAudioOutBuffer` showed 131,072-byte buffers).
- **Fix:** an AAC decoder now reports one frame: 1024 samples per channel, 2048 with SBR (object type 5 or 29 in the AudioSpecificConfig), which is what each output holds; Vorbis keeps the bound, its packets vary in length.
- **Found on:** [Mario vs. Donkey Kong](../../dm/games/0100B99019412000/README.md) (1.0.1, the same movie as in [AAC decoders offer at most 32 outputs](#aac-decoders-offer-at-most-32-outputs)).

## Native-texture output

### Decoding into the client's NVN texture

- **Cause:** the decoder is created with output format 3 and mode 2 (`DecoderMode_NativeTexture`). The client's `movie::Buffer` then carries the `NVNtexture` object itself as its data, 8 bytes of capacity (an `RGBX8_SRGB` texture embedded in the game's texture pool entry, `main+0x226c98`), plus `SetNvnDev` / `SetNvnSync`.
- **Fix:** the host converts the VP9 picture to RGBA (BT.709 / BT.601 and range from the stream) and writes it into the texture's storage like `WriteTexels`, then marks the sync as signalled (`nvn_host_write_texture` / `nvn_host_signal_sync` in `hle_nvn.c`). The host writes the texture's block-linear storage on the CPU and the replay layer re-uploads the dirtied pages, as for `WriteTexels` ([nvn-objects.md](nvn-objects.md)).
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md).

### SDK 4's `movie::Buffer` has no sync object

- **Symptom:** `FATAL movie: missing SDK symbol _ZNK5movie6Buffer10GetNvnSyncEv` at the first frame of the native-texture movie (H.264 into the game's RGBA texture, as in Monster Hunter Rise).
- **Cause:** SDK 4's `movie::Buffer` carries the texture object but no sync object.
- **Fix:** the lookup is optional now; the game reads the texture once `GetOutputBuffer` returns, which the CPU write satisfies.
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md) (SDK 4.5.0; one boot).

### Parameter sets in `Configure` made the H.264 decoder without RGBA

- **Symptom:** a native-texture movie shows as a strip of small grey copies at the top of the screen.
- **Cause:** the texture held NV12 bytes. In RGBA8, 405 rows of 1920 pixels are exactly a 1920×1080 NV12 frame, rows 0–270 the luma at video black (16) and rows 270–405 the chroma (128). This game's MP4 demuxer hands the parameter sets to `Configure` (`csd-0` / `csd-1`), which made the H.264 decoder there without switching it to RGBA, and the first packet then found it made.
- **Fix:** both paths now go through `md_avc_open` (`hle_movie.c`).
- **Found on:** [Absolum](../../dm/games/0100A83022A22000/README.md) (H.264 logos, FNA3D's `nx_video_player.cpp`: type 1, output 3, mode 2). Monster Hunter Rise's movies never brought parameter sets to `Configure`.

### Last picture kept across a flush and at end of stream

- **Symptom:** looping movie previews flash black at the end of every loop.
- **Cause:** the game reads the buffers of the end-of-stream output (no picture) and of an output it acquired before the flush, and the host painted its texture black for both.
- **Fix** (`hle_movie.c`): a flush now keeps each slot's last picture, and in native-texture mode an output with no picture leaves the texture as it was, as the SDK's GPU consumer has nothing to draw.
- **Found on:** [Big Brain Academy: Brain vs. Brain](../../dm/games/0100620012D6E000/README.md) (the same looped activity previews, Flush and Start).

## Demuxers and codecs

### `movie::Extractor` runs natively

- **Symptom:** the game crashed on a null right after the failed open.
- **Cause:** `movie::Extractor` (libwebm in `subsdk0`) runs natively. The host used to force `SetDataSource` to fail because Engage's Unity demuxes by itself.
- **Fix:** the host no longer forces `SetDataSource` to fail.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md).

### VP9 on host threads with libvpx

- **Fix:** VP9 packets are decoded with libvpx (`dm/third_party/libvpx`) on one host thread per decoder: guest threads run on the stacks the SDK gives them, too small for libvpx. Packets are decoded in arrival order from one queue.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md).

### VP8 uses the same libvpx worker and output paths

- **Symptom:** VP8 WebM movies play black; `movie::Decoder` is created with decoder type 2.
- **Cause:** the host offered output slots for VP8 without decoding the packets, and its vendored libvpx included only the VP9 decoder.
- **Fix:** the same libvpx revision now includes its VP8 decoder. Type 2 selects `vpx_codec_vp8_dx`, type 3 selects VP9; both use the existing worker, NV12 conversion and native RGBA output. VP8 keyframe dimensions come from its ten-byte uncompressed header. Only decoded pictures and end-of-stream packets make an output ready.
- **Found on:** [Pokémon Brilliant Diamond](../../dm/games/0100000011D90000/README.md): logos and opening WebM movies. Decoding 3,182 packets over 106 seconds averaged about 1 ms per packet; making the picture visible also needed [zero initialization of depth textures](rendering.md#new-textures-start-as-zero). [Silver Pines](../../dm/games/0100D5B026844000/README.md): the Team17 logo and the title screen's animated background, 1920x1080.
- **Check:** `t_movie_vpx` decodes synthetic VP8 and VP9 keyframes through the real decoder, checking NV12 and RGBA pixels and a truncated VP8 header.

### H.264 into an RGBA texture and AAC through AudioToolbox

- H.264 (type 1) into the client's NVN texture (output 3 = RGBA, mode 2), with AAC 5.1 sound (type 4, PCM16). VideoToolbox decodes to BGRA for that mode (`mavc_set_rgba`), and AAC goes through AudioToolbox's converter with an MPEG-4 ES descriptor built from the AudioSpecificConfig in `csd-0` (as FFmpeg's AudioToolbox decoder does). Found on [Monster Hunter Rise](../../dm/games/0100B04011742000/README.md); its end-of-stream packets and decoder deletes are in [End-of-stream packets with no data, and deleted decoders](#end-of-stream-packets-with-no-data-and-deleted-decoders).

### H.264 movies without a decoder

- **Symptom:** on Linux or Windows the game presents frames until a movie starts, then none: `[RED] movie: no H.264 decoder on this platform yet (Phase 1: the video of H.264 movies is not shown)`, a stall watchdog (`DM_WATCH`) finds no present for 15 s, the main thread waits on an address under the game's movie player, and the decoder's last line says `3283 packets in, 4 frames out`.
- **Cause:** the port has no H.264 decoder yet (a decoder library is a dependency the user decides on; Apple's host decodes with VideoToolbox), and its stopgap took every access unit and handed out no picture, on the assumption that a player runs on without its video. A player that waits for its pictures, to show them at their times or to know where the movie is, waits for ever.
- **Fix:** the stopgap (`hle_movie_avc.c`, the part outside `__APPLE__`) gives a black picture for each access unit at the unit's presentation time, as the AAC stopgap gives silence: NV12 black (Y 16, UV 128), or opaque black for the native-texture mode (`mavc_set_rgba`); pictures leave in presentation order through a queue as deep as VideoToolbox's path keeps (4, or none for a Baseline SPS from `Configure`'s parameter sets, the `avcC` record or the stream), and the end of the stream flushes the rest through the drain of `hle_movie.c`. It remains a stopgap: the movie's video is not shown. Apple's decoder is unchanged.
- **Then:** the native-texture output wrote each black picture into the client's texture: the same bytes every frame, 7.9 MB a frame for Mario vs. Donkey Kong's two 960x1080 textures, 1.7 ms of CPU a frame at its title and a re-upload of both textures. The stopgap says its pictures are all the same (`mavc_constant`), and `hle_movie.c` `md_out_tex` writes a texture once, keyed by its Initialize serial (`nvn_host_texture_serial`: a texture made again at the same address is written); a size change at Configure clears the list. VideoToolbox's decoder answers 0, so Apple writes every picture as before.
- **Check:** `t_movie_avc` (Linux, Windows): times in decode order come out sorted, one black picture per access unit, 4 held until the end of the stream for Main, none held for Baseline, RGBA opaque black, a flush drops the queue, its pictures all the same; the stopgap before gives no picture (7 of its 21 checks fail).
- **Found on:** [Mario vs. Donkey Kong](../../dm/games/0100B99019412000/README.md#on-linux-the-x86-64-engine) (its second movie at boot, two 960x1080 H.264 streams into textures: before, no frame after the first 160; after, 3,000 frames in 150 s on lavapipe).
- **Since:** Windows decodes H.264 and AAC with Media Foundation ([H.264 and AAC on Windows through Media Foundation](#h264-and-aac-on-windows-through-media-foundation)). The stopgap is Linux's, where bundling a decoder is a dependency the owner has not decided on, and Windows' where Media Foundation is missing; `t_movie_avc` runs on Windows against that fallback.

### AAC without a decoder gives each packet's frame as silence

- **Symptom:** on Linux (no AAC decoder yet), while a movie with AAC sound plays, the movie's own audio output gets no buffer: every callback of that output is counted as an underrun (Mario vs. Donkey Kong's story movie: 1,982 and 2,000 of 2,000 callbacks of `audio #2` in the log's windows), although the log said `[RED] movie: no AAC decoder on this platform yet (Phase 1), outputting silence`.
- **Cause:** the stopgap's `md_aac` returned no output for a packet, so the player never got an audio buffer to append: it sizes its AudioOut buffers by the decoder's frame, appends each output whole and advances its audio clock by it ([AAC output buffer size is one frame](#aac-output-buffer-size-is-one-frame)). The H.264 stopgap had the same defect once and gives a black picture per access unit since ([H.264 movies without a decoder](#h264-movies-without-a-decoder)).
- **Fix:** without a decoder each packet gives one frame of silence, 1024 samples a channel (2048 with SBR), PCM16 interleaved (`hle_movie.c` `md_aac_silence`, `aac_frame_samples`, which `GetOutputBufferSize` uses too): on Linux, and on Windows where Media Foundation has no AAC decoder (an N edition without the Media Feature Pack). Apple's path is AudioToolbox's and unchanged.
- **Found on:** [Mario vs. Donkey Kong](../../dm/games/0100B99019412000/README.md), on Linux, in the audio statistics of its story movie. Measured on its opening movie (54.7 s in its header: 3,280 pictures, 2,565 AAC packets; headless, the silent audio device): before, the AAC decoder gave 4 outputs for 2,565 packets and the movie's output had 1,982 to 2,000 underruns in every 2,000 callbacks; after, 2,565 outputs, no underrun and 4.0 blocks queued at each callback. The movie then follows its audio output's clock, as on the console: with SDL's dummy audio device, which took about 5.5 ms for a 5 ms callback in these runs, it lasted 61 s where it lasted 54 s before; a real device was not run here. Checked on the story movie against the wall clock (34.43 s in its header: 38.47 s, 2,308 presented frames, its output's callbacks 5.61 to 5.63 ms apart, no underrun): the dummy driver sleeps a buffer's 5 ms after each buffer's work (`SDL_Delay` in its `WaitDevice`) instead of keeping a deadline, so its period grows with the load (6.0 to 6.3 ms with a software rasteriser on every CPU), and a headless movie's length says nothing about a real device's. The trace's end-of-stream line (`AcquireOutputBufferIndex ... flags 0x4`) is no longer in such a log: `MD_TRACE` keeps a call's first 12 lines and then the powers of two, and the audio decoder now has an output per packet; a script waits for `[movie] Stop`.
- **Check:** `t_movie_vpx` (the frame's size for AAC-LC and SBR, all zero in a slot used again, nothing for an empty packet, the channels from the configuration).

### H.264 and AAC on Windows through Media Foundation

- **Symptom:** on Windows the movies played black and silent: `[RED] movie: no H.264 decoder on this platform yet (Phase 1)` and `[RED] movie: no AAC decoder on this platform yet (Phase 1), outputting silence` ([H.264 movies without a decoder](#h264-movies-without-a-decoder)).
- **Cause:** the port had no H.264 or AAC decoder outside Apple, where VideoToolbox and AudioToolbox decode them. Every Windows 10 and 11 has both in Media Foundation (the N editions get them with the Media Feature Pack), so Windows needs no library of its own.
- **Fix:** `dm/host/hle_movie_win32.c`, behind the interfaces Apple's backends use: `mavc_*` (`hle_movie_avc.h`) for the video and `maac_*` (`hle_movie_aac.h`, new; Apple's AudioToolbox converter stays in `hle_movie.c`) for the sound, so another backend (a bundled decoder for Linux) can sit beside it.
  - Loading: `mfplat.dll` is loaded at the first movie and its four functions taken by name, so the programs start without it; `ole32` and `mfuuid` (a static library of GUIDs) are linked, on Windows only. The decoders are the transforms by class ID, `CLSID_MSH264DecoderMFT` and `CLSID_CMSAACDecMFT`, software and synchronous, with no Direct3D device. COM is initialised on the movie decoder's own host thread, which makes, uses and frees them (`hle_movie.c` `md_worker`); the parameter sets and a flush come from guest threads and touch only the backend's own state, the flush reaching the transform at its next call.
  - Video: the access units in Annex B (or 4-byte lengths, as the demuxer gives them) with the SPS and PPS put before every IDR unit; the input type carries the SPS's coded size. The transform reorders B-frames itself and hands pictures out in presentation order with the samples' times, so no queue depth is kept (VideoToolbox decodes in decode order and keeps 4); the end of the stream drains it (`MFT_MESSAGE_COMMAND_DRAIN`), a flush or seek flushes it. NV12 out, read with the output buffer's pitch and the chroma plane after the frame size's padded height (1088 for 1080 lines), cut to the minimum display aperture; `MF_E_TRANSFORM_STREAM_CHANGE` picks NV12 again with the new size. NV12 goes out in video range as VideoToolbox's does (a full-range stream is scaled); the native texture mode's RGBA is converted here with the matrix and range of the SPS's VUI, and when it declares none BT.709 from 720 lines up, else BT.601, as the VP9 path does (VideoToolbox converts with the stream's matrix; its choice for an undeclared one is not known here).
  - Sound: raw AAC with payload type 0 and, as `MF_MT_USER_DATA`, the part of `HEAACWAVEINFO` after its `WAVEFORMATEX` followed by the AudioSpecificConfig (`csd-0`; made from the rate and channels without one, as for AudioToolbox); out 16-bit PCM at the stream's rate and channel count, interleaved, which the game's AudioOut takes. Multichannel PCM comes in Windows' channel order (front left, right, centre, LFE, back left, right); what AudioToolbox gives a 5.1 stream on Apple was not compared.
  - Missing: no `mfplat.dll`, a class not registered or a transform that refuses the stream gives one `[RED]` line with the reason and `on a Windows N edition, install the Media Feature Pack`, then the stopgap's black pictures (`mavc_constant` 1) and silence. The bundle's `README.txt` says so for Windows ([docs/development.md](../development.md#the-ports-bundles)).
  - `DM_MOVIE_TRACE` also logs every call into a transform with its result.
- **Check:** `t_movie_mf` (Windows only) on streams that `dm/host/tests/movie_streams.py` writes without an encoder at build time: Baseline 96x60 (cropped from 64 lines, an avcC record and length-prefixed units) of I_PCM macroblocks and P pictures of skipped macroblocks, Main 64x48 in decode order I0 I4 B2 I8 B6 whose B pictures are skipped macroblocks (the rounded average of their references, so B-frame reordering and bi-prediction are checked to the byte), and silent AAC-LC frames (stereo 48 kHz, mono 44.1 kHz: `max_sfb` 0, as a frame with a signal needs the spectral Huffman codebooks, which are not written by hand); the pictures byte for byte in NV12 and within 1 of a floating-point BT.709 / BT.601 conversion in RGBA, in presentation order with their times, a flush in the middle of a stream, a size change on one decoder, the drain, 1024 samples a frame from the configuration's rate and channels, and Media Foundation taken as absent (black pictures, no AAC decoder). Where a transform cannot be made it says why and skips. `t_movie_avc` runs the stopgap's checks on Windows against the fallback.
- **Under Wine 11:** Wine makes both transforms from GStreamer elements (`winegstreamer`, `wg_transform.c`): `avdec_h264` and `avdec_aac` (gstreamer1.0-libav), `videoconvert` and `audioconvert` (gstreamer1.0-plugins-base), and for H.264 also the `h264parse` parser (gstreamer1.0-plugins-bad's `videoparsersbad`): without a parser its avdec_h264 branch fails to make the input caps writable (`gst_caps_set_simple: assertion 'IS_WRITABLE (caps)' failed`), the capsfilter lacks `alignment=au`, and no picture comes out. `docker/Dockerfile.wine` installs them in a layer of its own (the two packages and, out of plugins-bad, only the parser's plugin and its library: about 128 MB): in an image without that layer `t_movie_mf` skips (`GStreamer doesn't support H.264 decoding, please install appropriate plugins`, `CoCreateInstance 0xd0000001`), with it it passes, 94 checks; the silent AAC frames come out as samples of -1 to 1 there, the float decoder's output dithered to 16 bits by `audioconvert`, which the test allows under Wine only. Wine's H.264 transform hands a picture out about 9 access units after its own (GStreamer's frame-threaded decoder; about 4, VideoToolbox's depth, with `MF_LOW_LATENCY` on the transform, which the backend does not set). A result under Wine is not Windows: the backend has not run on Windows.
- **Open, under Wine:** Mario vs. Donkey Kong's opening movie decodes there (both transforms, its pictures with the movie's content, its sound in the game's AudioOut) and stays black on screen: the game's movie player acquires the video outputs and releases nearly all of them without reading them (2 of 3,282 read, the first one black), as a player drops pictures it finds late; its frames under Wine and lavapipe took 36 to 76 ms (p50) against the movie's 60 pictures a second, and with the transform's lag cut to VideoToolbox's 4 units it dropped them all the same. Whether a PC at 60 fps shows the movie is not known here ([the game's note](../../dm/games/0100B99019412000/README.md#movies)).

### Vorbis movie sound

- Movie sound in [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md) (SDK 8.2.0): the decoder is asked for type 5 (Vorbis) with output format 1 (PCM16); see the other entries in this file and `hle_movie.c`.

## Movie players

### Unity's video planes and checking the movie path

- Unity's `Hidden/VideoDecode` samples luma as `.w` of an `R8` texture with the swizzle `(0, 0, 0, R)` (Unity's Alpha8) and chroma from an `RG8` texture at half size; both are CPU-written block-linear textures ([nvn-objects.md](nvn-objects.md)).
- **Check:** to check the movie path apart from the decoder, fill the frames with a gradient in `hle_movie.c`; it should appear on screen.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md).
