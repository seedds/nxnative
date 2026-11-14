# Movies

The SDK's movie library (`movie::Decoder`, `movie::Extractor`), which the host takes over in `dm/host/hle_movie.c`, and the games' movie players. Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Decoder lifecycle

### The SDK's hardware decoder opens nvdrv

- **Symptom:** abort in NVIDIA's video path (`NvRmChannelOpen` → `NvOsDrvOpen`).
- **Cause:** the SDK movie library's hardware decoder opens nvdrv directly.
- **Fix:** the host takes over `movie::Decoder` (`hle_movie.c`, [`movie::Decoder` taken over as a MediaCodec-style protocol](#moviedecoder-taken-over-as-a-mediacodec-style-protocol)); nvdrv opens fail (`hle_nvdrv.c`).

### SDK 1 movie buffer getters

- **Symptom:** constructing a movie decoder aborts with `FATAL movie: missing SDK symbol _ZNK5movie6Buffer4DataEv`.
- **Cause:** SDK 1.3.1 exports `movie::Buffer::Data()`, `Capacity()` and `Size()` as non-const methods. Looking up only the const overloads misses the symbols in `subsdk0`.
- **Fix:** resolve each getter by the const name first, then by its non-const name. The decoder still requires all three; it uses the guest's methods to read the buffer without assuming its layout.
- **Found on:** [Ultra Street Fighter II: The Final Challengers (#960)](https://github.com/seedds/nxnative/issues/960).

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

### `nv12-colorspace` picks the game's YUV matrix

- **Symptom:** a movie decodes and its sound plays, but the picture is black: `DM_MOVIE_TRACE=1` shows pictures coming out, the pass dump shows the two planes uploaded and one quad drawn, and the uniform block of the conversion shader (`DM_NVN_CBDUMP`) is all zeros.
- **Cause:** the host's output format had no `nv12-colorspace`. The SDK's `DecoderImpl::GetOutputBufferFormatFromDecoder` sets it (SDK 4.4.1 `subsdk0+0x2d8c60`) to the hardware decoder's `hw-buf-ColorSpace` / 32 + 1, from 1 to 5. A player can index its matrices with it: Kirby Star Allies' (`main+0x1529f8`) copies a 64-byte block chosen by the value, 1 and 5 BT.601 limited range, 2 BT.601 full, 3 BT.709 limited, 4 BT.709 full, and for anything else, an unset key's 0 too, an all-zero block.
- **Fix:** `md_format` sets the key from the stream, by the rule native-texture output already uses for its own conversion: the matrix the stream declares (the H.264 format description's `YCbCrMatrix`, the VP9 colour config), undeclared BT.709 from 720 lines up. H.264 pictures are reported as limited range, since VideoToolbox gives video range NV12 whatever the stream's range; VP8 and VP9 report the stream's range.
- **Found on:** [Kirby Star Allies](../../dm/games/01007E3006DDA000/README.md): the opening movie and the movie after the first file start.

### SDK 4's `movie::Decoder` constructor without a priority

- **Symptom:** a call through a null pointer in the host's `movie::Decoder::Configure` right after `nnMain` starts the logos.
- **Cause:** SDK 4's `Decoder` constructor has no priority argument (`_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE`). The host took over only the four-argument one, so the SDK built its own decoder and the host's `Configure` read that object as its own.
- **Fix:** both forms are taken over now (`hle_movie.c`).
- **Found on:** [DARK SOULS: REMASTERED](../../dm/games/01004AB00A260000/README.md) (SDK 4.5.0; one boot).

### SDK 3's `movie::Decoder` constructor takes a type and a format only

- **Symptom:** a call through a null pointer in the host's `movie::Decoder::Configure` (`pc=0 lr=h_movie_Configure+…`) when the first movie starts; with only the constructor taken over, `[RED] movie: decoder type 1 output 3 mode 1 not supported by the host`.
- **Cause:** SDK 3.5.1's constructor is `Decoder(DecoderType, DecoderOutputFormat)` (`_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatE`, `subsdk1+0x2de630`: it builds `DecoderImpl(type, format)`), a third form after [SDK 4's](#sdk-4s-moviedecoder-constructor-without-a-priority): the SDK built its own decoder, as on SDK 4. That SDK has no decoder mode: its decoders work on the CPU. And its output formats are numbered one lower than later SDKs': `DecoderImpl` keeps the format at +0xdc and the type at +0xe0, `ConvertDecoderOutputFormat` (`subsdk1+0x2dd16c`) converts format 2 to ARGB and copies format 3 out as NV12 with stride = width, the UV rows right after the Y rows, and `GetOutputBufferSize` (`subsdk1+0x2dcde8`) gives w × h × 4 for 2 and w × h × 1.5 for 3; format 1 is PCM16 as later. Its `GetOutputBufferSize` also puts 0x20, an alignment, in the second output, where SDK 4 and later put the decoder mode (Metroid Dread `subsdk0+0x11f308`: `str w8,[x2]` with the mode, 1 for the CPU).
- **Fix:** the two-argument constructor is taken over as a CPU-mode decoder (mode 1) with format 3 as NV12 and 2 as ABGR (`h_movie_Decoder3` in `hle_movie.c`), and its decoders report 0x20 as `GetOutputBufferSize`'s second output (`out_align`). The host's NV12 already has the SDK 3 layout (`md_nv12`).
- **Found on:** [Hyrule Warriors: Definitive Edition](../../dm/games/0100AE00096EA000/README.md) (SDK 3.5.1, H.264 MP4 movies through its own player): the title's background and the opening movie play.

### The base-object constructors of `movie::Decoder`

- **Symptom:** `CRASH sig=11 addr=0 pc=0` with `lr=h_movie_Configure+…`, as the first movie starts.
- **Cause:** the host took over only `movie::Decoder`'s complete-object constructors (`C1`). A game that reaches the base-object constructor (`C2`) ran the SDK's: Mario Tennis Aces' SDK 4.5.0 has a `C1` of two instructions (`mov w4,#0; b C2`). The object then held a guest `DecoderImpl`, whose vtable pointer `Configure` read as the host's decoder.
- **Fix:** `hle_movie.c` takes over the three `C2` forms as well. The class has no virtual bases, so both constructors do the same.
- **Found on:** Mario Tennis Aces (`0100BDE00862A000`, no profile; [issue #1131](https://github.com/seedds/nxnative/issues/1131), found and tested by its reporter).

### The complete-object destructor

- **Cause:** the game calls the complete-object destructor `movie::Decoder::~Decoder` (`D1`); the SDK's own one destroyed the host object as a `DecoderImpl`.
- **Fix:** the host now takes it over too.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md).

### Client events read through the registered pointer

- **Symptom:** the first deferred signal crashed in `InternalCriticalSectionImplByHorizon::Enter` (DRAGON QUEST XI S).
- **Cause:** `RegisterClientEvents` copied the four event pointers, because Dread's struct had been overwritten by its second movie. DRAGON QUEST XI S (Unreal's Switch movie player, VP9 with 6-channel Vorbis) does the opposite: it registers its struct right after constructing the decoder (`main+0x39edab0`, through the decoder's vtable) and fills it later, so the copy held leftovers (`0xe00000010`). The SDK keeps only the pointer (`DecoderImpl::RegisterClientEvents` stores it at +0xd0) and reads the entry each time it signals, skipping a null one (`SendOutputBufferEventAvailableToClient` and the others, `subsdk0+0x1feb0`).
- **Fix:** the host now does the same (`md_event` in `hle_movie.c`). When the client's entry is not a readable, 8-byte aligned pointer, it falls back to the entry's value at registration if that was one (Dread's case), and otherwise logs a red line.
- **Found on:** [Metroid Dread](../../dm/games/010093801237C000/README.md); [DRAGON QUEST XI S](../../dm/games/01006C300E9F0000/README.md).

### Video decoded off the caller's thread

- **Symptom:** a crash reading address 0 at `main+0xab4e18` right after `[movie] Stop` and `decoder … destroyed`, at the first skip of the intro movie or the first menu change (Mario Strikers: Battle League; `MoviePlayerWorker` in the thread dump).
- **Cause:** the SDK queues a video packet and returns; its codec thread decodes it and signals each result once the hardware has finished it. The host decoded inside the `SendInputBufferForDecode` or `ReleaseOutputBufferIndex` that started the decode, for the whole decode (3–8 ms a packet, longer in a burst). Strikers' movie worker feeds its decoder while the owner thread stops the movie, destroys the player and clears its pointer (`main+0xab55a4`); the worker, still inside the call, then read the cleared player (`main+0xab4e18`, `x8 = [[x21+0x6c0]+0x20] = 0`). `DM_PROBE_POLL` on the player's slot caught the null write: the owner was in the SDK decoder's destructor, the worker inside the player's update loop (`main+0xab4db8`).
- **Fix:** each video decoder has its own thread (`md_pump_thread` in `hle_movie.c`). The send and release calls queue the work and return, as the SDK's do, and the decode runs there. Its results are signalled from the deferred thread, due at once (`md_pump_ev`): the pump thread is not a guest thread and `SignalEvent` is guest code. `Stop`, `Flush` and the destructor drop one still pending there, and `Stop` takes the deferred queue's lock, so none is delivered after `Stop`. The destructor stops the pump thread before it drops the pending signals and frees anything: a pump still running would otherwise queue a signal for a freed decoder. Audio decoders decode on the caller and signal at once: their players feed one packet per signal, and a packet decodes quickly.
- **Open:** with the decoder's thread, 2 of 12 runs still crashed the same way on one Mac (0 of 30 on another), both right after `GetOutputBuffer`, `Stop`, destroy. In texture mode the host's `GetOutputBuffer` still writes the whole frame into the client's texture on the caller (`md_out_tex`, with a byte swap for BGRA textures), where the SDK starts a GPU copy and returns a sync object: the same kind of hold as the decode, suspected and not checked. Signalling every video result 16 ms late also gave 0 of 12 there, by letting `Stop` drop the signals of the movie's last frame, which the SDK would have delivered; it delayed every movie's signals and was left out as a workaround.
- **Found on:** Mario Strikers: Battle League 1.3.2 (no profile), with a frame-stamped `DM_KEYS` script that skips the intro and changes menus (`f150:A~f5,f210:A~f7,f270:L-0.2/-0.9~f3,f280:L-0.2/-0.9~f3,f300:A~f5,f350:A~f5`), on two Macs: 3 of 4 runs crashed before; with the decoder's thread, 0 of 30 on one and 2 of 12 on the other ([seedds/nxnative#762](https://github.com/seedds/nxnative/pull/762)). Its provisional profile had `core_preempt on`; that key alone did not stop the crash.
- **Check:** `t_movie_avc`: after a decode, the output event is queued for the deferred thread, due at once, not signalled on the decoding thread.

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

### Input buffers already held when end of stream arrives

- **Symptom:** the intro finishes, then the game crashes while calling its movie-completion handlers.
- **Cause:** stopping new input acquisitions at end of stream does not cover buffers the client already holds. Tied Together's Unity player acquires all four inputs and sends each with the end-of-stream flag. The host queued all four, producing four completion notifications; the first handler freed the event list that the later notifications still used.
- **Fix:** `h_movie_SendIn` accepts the first end-of-stream packet and returns subsequent client-held inputs once, without decoding or signalling them, whether or not they carry the flag. A queued or decoding input cannot be returned this way. An input returned by `Flush` is already on the free list and is not added again. Duplicate and full-queue checks precede ownership changes, and only an accepted packet commits `in_eos`. `Flush` / `Stop` still allow a new stream.
- **Found on:** Tied Together (`0100B6D00C2DE000`) 1.0.2, with a provisional profile on the Mac. The intro reaches the title menu after the fix.
- **Check:** `t_movie_avc` feeds four held inputs through `SendInputBufferForDecode`, drains the reordered pictures and checks that only one output carries end of stream. It also covers late ordinary data, duplicate and invalid submissions, an input being decoded, and held inputs sent after `Flush`. The four-input check fails on the previous host.

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

### H.264 loops retain presentation order when timestamps restart

- **Symptom:** a looping tutorial loses B-frame presentation order after the first loop, without a `Flush` or end-of-stream packet.
- **Cause:** the reorder queue sorted every picture by PTS alone. When the next loop restarted PTS at its IDR, the previous loop's high-PTS tail stayed behind the new pictures and occupied their reorder lookahead.
- **Fix:** `hle_movie_avc.c` identifies an IDR once per access unit and tags the synchronous decode callback with its coded-sequence number. The queue sorts by sequence, then PTS; pictures from a completed sequence can leave without waiting for more lookahead. The current sequence retains its existing depth, EOS drains all pictures, and `Flush` discards the queue and resets sequence tracking. PTS values are unchanged.
- **Found on:** [Super Mario Odyssey](../../dm/games/0100000000010000/README.md), reported with source by zatteogit in [issue #889](https://github.com/seedds/nxnative/issues/889). The synthetic `t_movie_avc` regression feeds both Annex B and length-prefixed packets through the production parser and callback wiring, checking distinct picture identities over repeated loops, short sequences, Flush and EOS. It substitutes decompression. Separately, x264 B-frame clips (a single IDR with 3 or 1 B-frames, and 12-frame GOPs) looped three times through real VideoToolbox had 123 of 180 pictures in the wrong position before the fix and none after. Gameplay and playback rate were not checked on this code; the reporter's replay of the game's tracks and gameplay test ran on a modified local checkout. The existing fixed reorder depth and IDR `no_output_of_prior_pics_flag` handling are unchanged.

### No picture carries the end-of-stream flag

- **Symptom:** an anime or story movie ends on a black screen; the pause menu still works and skipping lets the game continue.
- **Cause:** at the end of an H.264 stream the host put the end-of-stream flag on the last picture of its reorder queue. CRI Sofdec2 drops an output flagged end-of-stream without showing it and waits for the picture it counted on: the movie never completes. Catherine's anime intro had 1,140 of its 1,141 pictures delivered, the last only with flag 4. The queue held pictures there for no reason too: `hle_movie_avc.c` held four for every non-Baseline stream, assuming B-frames, but Catherine's High-profile streams use SPS `pic_order_cnt_type = 2`, whose output order is the decode order (H.264 8.2.1.3).
- **Fix:** the drain hands out every queued picture without the flag, then one output without a picture carries it, with the end-of-stream packet's time (`md_pump` in `hle_movie.c`), as VP9 and Baseline streams whose end-of-stream packet has no data already ended. This holds for every H.264 stream, with or without B-frames. Separately, `avc_reorder_depth` reads the SPS through `pic_order_cnt_type` (emulation-prevention bytes removed, the High-profile chroma fields and scaling lists skipped): POC type 2 needs no reorder queue in any profile; other POC types keep a depth of 4 (0 for Baseline), as does an SPS cut short. No timeout or movie skip is involved; the intro delivers all 1,141 pictures and goes on to Golden Playhouse by itself.
- **Found on:** [Catherine: Full Body](../../dm/games/0100BF00112C0000/README.md) (1.0.1a, CRI Sofdec2, H.264 High, NV12).
- **Check:** `t_movie_avc` (`dm/host/tests`) runs the drain through `md_pump` with synthetic pictures, with all output slots free and with two, and checks the SPS reader (POC types, scaling lists, chroma formats, truncated input). The game's `smoke.tsv` lets its intro finish without a skip press. On the Mac from a fresh save, the host before this fix never closed the intro's decoder in 180 s; the drain alone (with the old reorder depth of 4) and the drain with POC type 2's depth of 0 both had all 1,141 pictures out and the decoder closed, and the latter's screenshot shows Golden Playhouse.
- **Open:** CRI hands the decoder zero timestamps, so the reorder queue, sorted by time, cannot reorder a CRI stream that has B-frames; none has been found yet. Catherine's later story movies and its movies on the iPhone have not been checked one by one.

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
- **Fix:** the host converts the VP9 picture to RGBA (BT.709 / BT.601 and range from the stream) and writes it into the texture's storage like `WriteTexels`, then marks the sync as signalled (`nvn_host_write_texture` / `nvn_host_signal_sync` in `hle_nvn.c`). The host writes the texture's storage on the CPU in the texture's own layout, and the replay layer re-uploads the dirtied pages, as for `WriteTexels` ([nvn-objects.md](nvn-objects.md)). The layout is block-linear by address, or rows of the stride for a `LINEAR` output texture ([`WriteTexels` into a linear texture dropped the write](nvn-objects.md#writetexels-into-a-linear-texture-dropped-the-write)).
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

### `GetOutputBuffer` returns the output index in native-texture mode

- **Symptom:** a native-texture movie freezes on its 16th picture while the game keeps running; `DM_MOVIE_TRACE=1` shows 16 `AcquireOutputBufferIndex` / `GetOutputBuffer` pairs and no `ReleaseOutputBufferIndex`.
- **Cause:** in native-texture mode (output 3, mode 2) the SDK's `DecoderImpl::GetOutputBuffer` calls `CopyOutputBufferToClientTextureAndRelease`, which renders the picture into the client's texture with MediaCodec's `renderOutputBufferAndRelease`: the index goes back to the decoder there, and a client need not release it. The host kept the slot until a release, so a client that never releases held all 16 video slots and the decoder stopped. A client that releases anyway gets success from the SDK, whose texture-mode `ReleaseOutputBufferIndex` turns MediaCodec's "not owned" (-13) into 0 (SDK 17.5.9 `subsdk0+0x123a0c`).
- **Fix** (`hle_movie.c`): the host tracks which output slots the client holds (taken by `AcquireOutputBufferIndex`, returned by a release or a flush). A texture-mode `GetOutputBuffer` returns its slot after writing the texture, and a release of a slot the client does not hold changes nothing and returns 0. Before, such a second release freed a slot the decoder might have filled again.
- **Found on:** [Super Mario Party Jamboree](../../dm/games/0100965017338000/README.md) (SDK 17.5.9: the opening movie and the mode previews). Metroid Dread (SDK 11.4.3) has the same copy-and-release and still releases each index itself, the case the second rule covers.

### A native-texture movie shows a picture 16 frames ahead, then goes back

- **Symptom:** a native-texture movie jumps ahead for one picture and returns: every 17 pictures, each exactly 16 ahead (one cycle of the video output slots), the picture before it never shown. Text burnt into the movie shows the line being typed half a second later for one frame.
- **Cause:** the host returned a native-texture slot inside `GetOutputBuffer` (the entry above), and returning a slot decodes the next queued packet into it within the call and signals the output event. The player's decoder thread acquired the slot again before its output thread's own `ReleaseOutputBufferIndex` for the old picture came, and that release then freed the new picture; the slot was filled once more, a cycle ahead. The SDK returns the index there too, but its hardware decoder fills it later, so the client's release always comes first.
- **Fix** (`hle_movie.c`, `md_rendered`): a slot written into the client's texture is kept until the client releases it, or, for a client that has never released a rendered slot itself (Super Mario Party Jamboree, Metroid Prime 4: Beyond), until its next `GetOutputBuffer`. A decoder learns which kind its client is at the client's first release of a rendered slot; the earlier slots are returned under the same lock that checks them.
- **Found on:** TSUKIHIME -A piece of blue glass moon- (010057A01E4D4000, no profile here; [GitHub issue #700](https://github.com/seedds/nxnative/issues/700): its prologue, a 1080p H.264 movie, `type 1 output 3 mode 2`) and [Metroid Dread](../../dm/games/010093801237C000/README.md#movies), whose movies jumped the same way (15 times in 75 s on the Mac before, none after).

### A BGRA8 client texture gets its movie with red and blue swapped

- **Symptom:** `[RED] movie: client texture format 0x9a is not RGBA8 / RGBX8 (sRGB included), writing in RGBA byte order`; a native-texture movie plays in the wrong colours, warm lamps blue.
- **Cause:** the SDK's consumer draws the decoded picture into the client's texture on the GPU, so it comes out right in any colour format. The host writes the bytes on the CPU, and wrote them in RGBA order whatever the texture's format was.
- **Fix** (`hle_movie.c`, `md_out_tex`): for BGRX8, BGRA8 and their sRGB forms (NVN formats 0x99 to 0x9C), the picture goes through a per-decoder copy with red and blue swapped. The slot's own picture stays RGBA, as a slot can be read twice.
- **Found on:** [TRIANGLE STRATEGY](../../dm/games/0100CC80140F8000/README.md) (the title screen's background movies; BGRA8).

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

### H.264 packets are Annex B, not NAL lengths

- **Symptom:** an H.264 movie is black or frozen and the log has `[RED] movie: AVC packet without an Annex B start code (… bytes), not decoded` (or `AVC parameter sets without an Annex B start code`).
- **Cause:** the SDK's demuxer converts the MP4's length-prefixed samples to Annex B start codes before they reach the decoder, also when it hands over the `avcc` record, and its `csd-0` / `csd-1` are Annex B too. `hle_movie_avc.c` nevertheless read packets without a leading start code as 4-byte NAL lengths, a fallback no game was seen to need. The two framings cannot be told apart from the bytes: a 4-byte length from 256 to 511 begins `00 00 01`, so such a NAL was taken as Annex B with its last length byte as the NAL header. [Issue #629](https://github.com/seedds/nxnative/issues/629) reported this ambiguity from another runtime's decoder (MGS3 clips with 269- and 407-byte NALs).
- **Fix:** `each_nal` reads only Annex B (3- or 4-byte start codes, leading zero bytes allowed); a packet or parameter set that does not begin with a start code is refused with the red line above, and `mavc_decode` returns -1. A game found to send NAL lengths would need its framing known for the stream, not guessed per packet. `t_movie_avc` runs its loops with both start-code lengths and checks the refusal.
- **Found on:** [Kirby Star Allies](../../dm/games/01007E3006DDA000/README.md) as the check. Its `ImageMovie.mp4` has 167 of 1,386 samples whose first NAL is 256 to 511 bytes (the first at decode index 2). Through real VideoToolbox, main's parser decoded 3 of its pictures from the length-prefixed samples and all 1,386 from Annex B, so the game's movie playing in full on main shows the SDK hands over Annex B. With the change, a boot from an empty save decoded that movie with "1387 packets in, 1387 frames out" and no red line (2026-11-09, Mac, on the chain its libraries were built with).

### Vorbis movie sound

- Movie sound in [The Legend of Zelda: Link's Awakening](../../dm/games/01006BB00C6F0000/README.md) (SDK 8.2.0): the decoder is asked for type 5 (Vorbis) with output format 1 (PCM16); see the other entries in this file and `hle_movie.c`.

### Vorbis output lengths at a window change

- **Symptom:** a movie with a Vorbis track starts clean, then crackles and stutters. The log has no underrun and no red line, and with `DM_AUDIO_DUMP` the game's mix has exact zeros in whole 512-sample blocks (mostly one or two at a time, 11 to 21 ms).
- **Cause:** at a change of window size `stb_vorbis` splits the samples differently from the specification: it ends a packet's samples at its window's `right_start`, which depends on the next window, not at the window's centre. With blocksizes 256 / 2048, a long window before a short one gave 1,472 samples instead of 1,024, and the short one 128 instead of 576. The total is the same, but those outputs did not match the distance to the next packet's time.
- **Fix:** `md_vorbis` keeps the decoded samples and each packet leaves with (previous blocksize + its blocksize) / 4 of them, the rest going with the next. The blocksizes come from the packets' mode numbers. Samples still kept after the last packet lie past the centre of its window, where the specification has none, so they are dropped. Handing out longer merged outputs instead was tried and made the sound much worse: the game's buffers are 8,192 bytes, and the outputs no longer matched the packets' times.
- **Found on:** [Super Smash Bros. Ultimate](../../dm/games/01006A800016E000/README.md) (13.0.5, Mac: [#768](https://github.com/seedds/nxnative/issues/768)).

### Vorbis sound stops for good after a Flush

- **Symptom:** a movie with a Vorbis track plays its first seconds with sound; after the game flushes and restarts it (a seek, a loop, or the app losing and regaining focus) the picture goes on but the sound does not come back, for the rest of the movie. The log shows `[movie] Flush`, `Flush`, `Start`, `Start` and no more `GetOutputBuffer … → PCM` lines. With `DM_AUDIO_DUMP`, the game's mix is real audio up to the flush and exact zeros from then on, so the AudioUnit is not at fault (0 underruns).
- **Cause:** `h_movie_Flush` called `stb_vorbis_flush_pushdata` on the Vorbis decoder, which puts it into its page search mode. The packets `md_vorbis` hands it afterwards (each wrapped as one whole Ogg page) gave no sample: in a test on a generated stream, none of the next 100 packets did, whether the stream went on or started again from its first packet.
- **Fix:** a Flush closes the Vorbis decoder, and the next packet opens it again from the headers (`md_vorbis_open`), as at the start of a stream. `t_movie_vpx` builds a small Vorbis stream, flushes twice and checks that the same samples come again.
- **Found on:** [Super Smash Bros. Ultimate](../../dm/games/01006A800016E000/README.md) (13.0.5, iPhone: the opening movie, after the app lost focus once. [#505](https://github.com/seedds/nxnative/issues/505) reported the same silence and was closed after a reinstall, with the app having been backgrounded once in the silent session).

### AV1 movies stop an M1 iPad with SIGILL in dav1d

- **Symptom:** on an iPad with an M1 (the one supported chip without I8MM), the app quits at the first AV1 movie (a title or opening movie): `[dm] CRASH sig=4 addr=put_8tap_neon_i8mm+0x… lr=dav1d_recon_b_inter_8bpc+0x…`, with a `dav1d_worker_task` thread beside it. The same build plays on an M2 or newer iPad, an A17 Pro or newer iPhone, and on the Mac.
- **Cause:** the iOS project compiled everything with `-mcpu=apple-a18`, and dav1d's `src/cpu.h` adds every feature the compiler's target promises (`__ARM_FEATURE_MATMUL_INT8`, `__ARM_FEATURE_DOTPROD`) to the features it detects at run time (`sysctl hw.optional.arm.FEAT_I8MM`, which says no on an M1). Its I8MM motion compensation then ran on a CPU without those instructions. The A18 target had been chosen only for the LSE atomics of the host's assembly.
- **Fix:** `dm/ios/project.yml` and `dm/ios/tools/compile_check.py` build for `-mcpu=apple-m1`, the oldest chip the app supports (the games need BC textures: an M1 iPad, an iPhone from the A17 Pro) and the Mac build's default, which has LSE; dav1d picks its I8MM paths from the device at run time. `dm/ios/tests/test_cpu_target.py` asks clang what each build's target defines: LSE, and none of I8MM, BF16 or SME.
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md) (its title movie, on an iPad Pro 11-inch 3rd generation, M1, iPadOS 27.0.1; 2026-11-08). Every M1 iPad that plays an AV1 movie was affected.

## The Switch 2's decoder API

### `nn::codec` video decoders taken over

- **Symptom:** a Switch 2 game crashes when a movie starts: `nn::codec::video::Initialize` faults in the SDK (Donkey Kong Bananza: `sdk+0x2de99c`, after its first punch, when the guide movie starts), and before that `nv::InitCheck` aborts ([`hle_nv.c`](../../dm/gfx/hle_nv.c)). Later, when a second movie starts: `game called nn::diag abort: Result 0` in `nn::os::ClearSystemEvent` or `TryWaitSystemEvent` on the decoder's event.
- **Cause:** the Switch 2's SDK decodes video through `nn::codec` (H.264, AV1, VP8, VP9 decoders over a decoder service, decoder image pools, NV12 / RGBA readback), not `movie::Decoder`; every call goes to a service the host does not serve.
- **Fix:** `dm/host/hle_codec_video.c` takes the API over: VP8 and VP9 decoded with libvpx (the YUV conversions of `hle_movie.c`, `hle_movie_yuv.h`), H.264 with VideoToolbox (`hle_movie_avc.c`, its NV12 pictures kept as I420, the parameter sets an Initialize overload passes given to it first). The four decoder types (`Vp8`, `Vp9`, `H264`, `Av1DecoderType`) have one layout, their event at +8 in the SDK, so one host decoder serves them (`cv_dec_*`, each family's entry points by their SDK names), `Unregister…DecoderBuffer` included. The protocol as the SDK implements it (sdk 20.5.19 Ounce): one image pool per output picture, registered with the decoder under an index; registering puts the index in an 8-deep queue of acquirable buffers with a signalled sync handle. `Release…OutputBuffer` gives the buffer to the decoder to fill and puts it back in the queue at once, with a new sync handle that signals when a picture is in it; `Acquire…OutputBuffer` takes the oldest (description 3604 when the queue is empty). `Get…OutputBufferProperties` gives size, time and end of stream once the buffer is filled (3604 until then). `Submit…InputBuffer` takes a packet (3603 while the decoder cannot take more, the client retries after the submission-ready event). On the host, a submission queues its packet and each decoder decodes on its own thread ([below](#nncodec-packets-decoded-inside-the-submission)); pictures go into released buffers in release order, and up to 12 pictures and packets wait for one. The sync handles are the host's (16 bytes: magic, decoder, index, generation): `nn::gs::TryWaitNativeSync` answers them, and so do sync point events (`nn::syncpt::sdk`, whose library the graphics initialisation sets up, which the host replaces: `CreateSyncPointEvent` jumped through a null table). A sync point event holds a SystemEvent the host binds, linked into the client's MultiWait by the SDK's own `nn::os::InitializeMultiWaitHolder`, and is signalled when its buffer is filled. The decoder's submission-ready event lives in the decoder type and goes with the decoder: `FinalizeVp9Decoder` destroys it, as the SDK does. Kept, it stayed in the host's table of bound SystemEvents, so the next decoder made in the same memory, which the game had cleared, was not given an event, and `nn::os::ClearSystemEvent` / `TryWaitSystemEvent` aborted on the uninitialised one (Donkey Kong Bananza, about four minutes in, when its second movie starts: two `nn::diag` aborts with Result 0, from `main+0xe1a71c` and `main+0x18793a0`).
- **Check:** `make -C dm/host/tests GAME=<id> run` runs `t_codec_video` (AddressSanitizer and UBSan): the buffer queue and its results, sync handles and sync point events, NV12 / RGBA readback of a synthetic VP9 key frame, packets refused while 12 pictures and packets wait, end of stream, flush, a pool finalized while its buffer is still registered, a decoder finalized and made again in the same cleared memory, a VP8 key frame, a 64×64 H.264 baseline key frame decoded by VideoToolbox (ffmpeg's libx264, SEI removed), an unregistered buffer and AV1's refused Initialize.
- **Found on:** [Donkey Kong Bananza](../../dm/games/040074A01BF12000/README.md): 101 guide movies (`Movie/*.webm`, VP9 960×540, no sound); its player reads the pictures as NV12 (`ReadDecoderOutputImageNv12`, output format 3).
- **Open:** AV1 (no decoder on the host: Initialize fails with a red line, so a client takes its error path instead of reaching the service), encrypted input, native image handles (the GPU route of decoded pictures), alias pools and the readback of part of a picture are not served (each prints a red line once). No game here plays VP8 or H.264 through this API yet: those paths are checked by the test only. Donkey Kong Bananza's player imports all four decoders; its movies are VP9.

### H.264 parameter sets past the eighth argument

- **Symptom:** every movie stays black; `[RED] nn::codec H.264: VideoToolbox could not decode a packet`, the packets starting with an SEI NAL unit, not a parameter set.
- **Cause:** H.264's two `Initialize…Decoder` overloads end with the stream's parameter sets (SPS / PPS: a pointer and a size), the only place the game gives them. The host's shared entry points (`CV_FAMILY`) took the overloads without them, so VideoToolbox had no format. They are the ninth and tenth arguments of the longer overload: past the eight registers the guest's AAPCS64 gives each stack argument an 8-byte slot, where Apple's arm64 packs a 32-bit one into 4, so the argument before them had to be declared 64 bits wide.
- **Fix:** both overloads take the parameter sets for every decoder type and pass them on for H.264 only; the red line prints a refused packet's first bytes, which show how its NAL units are framed.
- **Found on:** [PRAGMATA](../../dm/games/0400C76024710000/README.md) (SDK 20.5.15): its opening movies.

### The submission-ready event counted as packets

- **Symptom:** a movie stays black with the game waiting behind it, the decoder made and no `first picture` line (`Submit…InputBuffer` never called). Or `[RED] nn::codec VP9: Corrupt frame detected`, then `Keyframe / intra-only frame required`, each time about 6 s into a movie or after skipping in one.
- **Cause:** [Mario Tennis Fever](../../dm/games/0400D4201C6EA000/README.md)'s player sends one packet per signal of the event it counts, and ignores `DecodeAsync`'s result (`nro0+0xd16130`), so each signal has to mean room for a packet. The host signalled at `Initialize`, which the player clears and drops while it is not started, and then only at releases. So nothing came after `Start`, and it never sent a packet. With a signal at `Start`, it then got one per release while a seek held back the samples, its count grew, and the host's refusal of a packet with 4 pictures waiting lost a frame for good: libvpx found everything after it corrupt until the next key frame.
- **Fix:** `Start…Decoder` signals (`cv_dec_start`). After that the event rises when a packet was taken or room came back, not at every release (`ready`), and goes down at 4 waiting, while packets are refused only at 12 (`CV_READY`, `CV_HOLD`), so a signal in flight still finds room. What waits is counted the same whether a packet has been decoded yet or not: the pictures without a released buffer, the packets queued and the one in decode (`cv_pending`).
- **A refusal is no fault:** the refusal returns nn::codec 3603, which `NintendoSDK_movie` expects: `detail::SubmitDecoderInputBuffer` turns exactly that result into its own retry code 0x7a, which `VideoDecoder::DecodeAsync` hands back to its player, and aborts on any other failure (Ocarina of Time's `subsdk0` +0x10fd20, +0x103840). It is logged as `nn::codec <codec>: busy (result 3603), a packet refused …`, no longer as a red line (Ocarina of Time AV1, Kirby Air Riders VP9: 4 a run, movies play). The host drops no picture: the decoder's thread waits for room before it holds another (`cv_room`).
- **Check:** `t_codec_video`.
- **Found on:** [Mario Tennis Fever](../../dm/games/0400D4201C6EA000/README.md), every movie.

### `nn::codec` packets decoded inside the submission

- **Symptom:** a movie stutters in small regular jerks (a picture stays for two or three frames, then the movie jumps on) while the frame rate reads a steady 60 and the frame times are flat: the game's frames are on time, the movie's pictures are not. Worst where the host decodes in software.
- **Cause:** the SDK's `Submit…InputBuffer` hands the packet to the decoder service and returns; the host decoded the packet inside the call, on the caller. A player that does all of a movie's work on one thread loses that thread for the length of each decode. Ocarina of Time's (in `main`) runs `DecodeAsync`, `AcquireVideoFrame` and `ReleaseVideoFrame` on its reader thread, and gives the shown picture's buffer back from there; its frame loop's `advance` (`main+0x3b61ea0`) keeps the picture on screen when no decoded frame is ready or the shown one has not been given back yet (the two failures at `main+0x3b61ed8`, `x8` zero or not). On an M2 Pro with dav1d, `DecodeAsync` took 1.9 ms at the median, 31 ms at the 90th percentile and 120 ms at the 99th, and `advance` failed in 546 and 569 of about 2,900 frames of the title (two runs; three in four of them or more with the picture not given back). The frame loop never waited, so the frame times showed nothing.
- **Fix:** each decoder decodes on a thread of its own (`cv_worker` in `hle_codec_video.c`), as `movie::Decoder` does ([Video decoded off the caller's thread](#video-decoded-off-the-callers-thread)). A submission copies its packet into the decoder's queue and returns (`cv_dec_submit`); the queue, the pictures waiting for a released buffer and the packet in decode count together for the submission-ready event and the refusal ([the event counted as packets](#the-submission-ready-event-counted-as-packets)), so a client sees the same event whatever the decoder's speed. The decoder's thread puts a picture into a released buffer and signals that buffer's sync point event itself (`hle_signal_system_event` is the host's, callable from any thread); with 12 pictures held it waits for a release before it decodes on. `Stop` and `Flush` drop the queued packets and the held pictures at once and number the stream anew, so the pictures of a packet still in decode are dropped when it ends; the codec is reset by its own thread before the next packet. Finalize joins the thread. Every guest call takes the decoder's lock as a host lock (`dm_host_mutex_lock`).
- **After:** `DecodeAsync` 0.10 / 0.20 / 1.1 ms, and 5 to 7 failed `advance` calls in about 2,900 frames with dav1d as it was; 0 or 1 with [dav1d on frame threads](#dav1d-decoded-one-frame-at-a-time).
- **Check:** `t_codec_video`: the event and the refusal are the same with the packets decoded or not, a submission signals its event itself and a sync point event comes from the decoder's thread, eight flushes with six undecoded packets each give only the key frame sent after them, and a decoder is finalized with six packets undecoded.
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md), its title movie (AV1, 1920x1080 at 60 fps) on an M2 Pro, headless, with `DM_PROBE=main:3b61ed8,main:3b61f80` counting the failed and the good `advance` calls. Again on an M4 Pro (48 GB, macOS 27.2) with dav1d forced (`DM_AV1_DAV1D=1`), 54 s of the title, one run a side: 361 failed of 3,041 before, 0 of 3,057 with both fixes, the frame times and red lines the same. [Mario Kart World](../../dm/games/0400C3F00006E000/README.md)'s cup opening movie (VP9, 2560x1440, libvpx) on the same Mac: all 1,423 pictures before and after, frame p95 18.8 to 19.3 ms in the movie on both, 3.24 ms a packet and 13.4 ms the longest, 2 pictures after their buffer.
- **Open:** Donkey Kong Bananza, Mario Tennis Fever, Kirby Air Riders and Fire Emblem Fortune's Weave (SDK 22's native images) were not run with it, nor an iPhone or iPad that decodes AV1 with dav1d.

### dav1d decoded one frame at a time

- **Symptom:** on a chip without a hardware AV1 decoder (M1, M2), a 1080p 60 fps AV1 movie shows a picture twice now and then in its heavy parts, with the decoder already on its own thread, and the log has `[codec] AV1 decoder 1: the decoder is behind the movie: of the last 600 packets decoded in … ms each, … ms the longest; … pictures came after their buffer was released`.
- **Cause:** dav1d was opened with two task threads and `max_frame_delay` 1: one frame at a time, so that a temporal unit's picture was there when its send returned. A hierarchical stream puts several frames in one temporal unit (the hidden ones the next units only show): Ocarina of Time's title movie took 80 to 122 ms for one unit in about 32 on an M2 Pro, and 15.4 ms a unit over its heaviest five seconds, at a budget of 16.7 ms. More task threads alone gave nothing (4 threads on one frame: the same times).
- **Fix:** dav1d decodes on 4 threads, 3 frames at once (`DAV1D_THREADS`, `DAV1D_FRAMES` in `hle_movie_av1.c`): 7.1 ms a unit over the same five seconds and 42 to 76 ms the longest (2 frames at once 8.8 ms; 6 threads on 3 frames no better). Its pictures now come out later than their units go in, so each unit's time rides with it (`Dav1dData.m.timestamp`, `mav1_picture.ts`) and the decoder's thread asks for more pictures after a unit (`mav1_next`). When no packet waits behind a unit, and at the end of the stream, it drains dav1d, so a picture is not kept back for a packet that may not come.
- **A drain goes to its end:** a `dav1d_get_picture` with no data sent before it is dav1d's drain (`drain_picture`, `lib.c`, 1.5.4), which steps over its empty frame contexts on the way to the oldest frame. Data sent between two such calls leaves `frame_thread.next` on a frame still in flight; that frame is then retired with `task_thread.first` one context past the oldest for good, and retiring a frame cancels the task rescan asked for `task_thread.first` (`reset_task_cur`), now a live frame's. Its task is never run: the workers sleep in `dav1d_worker_task` and `dav1d_submit_frame` waits for ever. With a drain that stopped as soon as a packet arrived, the title movie froze for good in 3 runs of 6 (`sample`: `dm codec video` in `dav1d_submit_frame`, the four workers parked). A drain that has given a picture is now called until it gives none (`mav1_next`'s contract; `decode_dav1d` ends one left open, with a red line).
- **Check:** `t_codec_video` sends AV1 key frames through the protocol and gets each with its own time, in order, the last with the end of stream; it drains mid-stream, and runs 600 rounds of units with drains at random places.
- **Found on:** [The Legend of Zelda: Ocarina of Time](../../dm/games/0400486024DEC000/README.md): eight runs of the title on an M2 Pro (54 s each, headless, the player's settings), 0 or 1 failed `advance` in 2,567 to 3,036 a run (none in six of them) and no frozen movie; before both fixes, 546 and 569 failed. In a window, over 88 s of the title movie: 5,273 packets decoded in 4.82 ms each, 87 ms the longest, 2 pictures after their buffer was released (the totals line at Finalize).
- **The log line:** the decoder's thread counts the pictures that found a released buffer already waiting for them (the client had run out of decoded pictures) and times each packet (`cv_stat`). A run of 600 packets with more such pictures than the decoder has buffers prints the line above; Finalize prints the totals. On the title movie: 0 to 3 in a run with frame threads, 29 and 18 in the two heaviest without, where `advance` failed 5 to 7 times.
- **Open:** measured on an M2 Pro only (8 performance cores). An M1 or M2 with 4 performance cores, and an M1 or M2 iPad, were not run; there the log line says whether the decoder keeps up.

### AV1 movies through SDK 22's decoder

- **Symptom:** a crash in `nn::codec::video::InitializeAv1DecoderByTransferMemory` when a movie starts.
- **Cause:** SDK 22 initializes its decoders by transfer or remote memory, with stack arguments, and the game's movies are AV1 decoded into native images: linear textures it makes over the output pool's memory with `nvnNativeImage*` and `nvnTextureInitializeFromNativeImage`.
- **Fix:** `hle_codec_video.c` takes the SDK 22 forms and the native images (16 KB aligned, 64-byte stride); `hle_movie_av1.c` decodes on VideoToolbox, building the av1C record from the stream's sequence header. VideoToolbox's AV1 decoder is hardware only (M3 and A17 Pro and later). Elsewhere, for a stream VideoToolbox refuses, or with `DM_AV1_DAV1D=1`, dav1d decodes ([`dm/third_party/dav1d`](../../dm/third_party/dav1d/README.md), 1.5.4 with its arm64 assembly, built by `dm/host/Makefile` into `gen/dav1d/libdav1d.a`). Its planar pictures are handed over as NV12, 10- and 12-bit streams shifted to 8 bits. A stream with no colour matrix is taken as BT.601 below 720 lines and BT.709 from there, as VideoToolbox does. Pictures cannot be left black instead: the game's movie module aborts on a decoder that does not initialize (`movie::detail::InitializeDecoderByTransferMemory`, `subsdk2+0xf9b4` in Fire Emblem Fortune's Weave).
- **Check:** `make -C dm/host/tests GAME=<id> run` runs `t_codec_video` (AddressSanitizer and UBSan), which covers the AV1 and SDK 22 forms, native images and the av1C record, and decodes a 64x64 AV1 key frame (SVT-AV1) on each backend the Mac has, read back as NV12.
- **Found on:** [Fire Emblem Fortune's Weave](../../dm/games/04007EF01F8AE000/README.md): 82 movies in WebM at 2560x1440; the opening's 3,301 pictures play. On #957's branch, VideoToolbox and dav1d gave the same pictures, bit for bit, over the opening's first 600 (about 450 pictures a second each on an M3 Pro), and a tester's M1 Pro played the first two movies through dav1d, smooth and in sync ([seedds/nxnative#927](https://github.com/seedds/nxnative/issues/927)).

### SDK 19's decoder forms

- **Symptom:** an abort in `nn::codec` (the SDK's own decoder client, which opens a service the host does not have) when a movie starts in a game built with SDK 19 for the Switch 2.
- **Cause:** SDK 19 names three calls differently from SDK 20 and 22. Initialize by remote memory takes two sizes before the picture size (`…DecoderEPNS0_14Vp9DecoderTypeEmmttj…`). The output buffer properties take a `nn::movie::ColorSpace*` before the time, nine arguments with the index in the first stack slot. The preference has a second form, `nn::codec::sdk::MakeDecodeImagePreferenceForCompat` (fewer output formats, `sdk+0x2c6040`), which the game chooses per decoder (`main+0x19e7860`).
- **Fix:** `hle_codec_video.c` takes them for VP9, VP8, AV1 and H.264 into the same host decoder. The colour space is written as 0: the 32-bit value the SDK fills in from the service. The game does not read it near either call (`main+0x19e63d4`, `+0x19e702c`), and it converts the NV12 pictures it reads itself.
- **Check:** `t_codec_video` initializes a VP9 decoder by SDK 19's remote memory form and reads a picture's properties through the nine-argument form.
- **Found on:** [Mario Kart World](../../dm/games/0400C3F00006E000/README.md)'s base game (SDK 19.3.3 Ounce; its movie player is in `main`), from its imports: the Mushroom Cup's opening (VP9, 2560x1440) decodes with them.

## Movie players

### Unity's video planes and checking the movie path

- Unity's `Hidden/VideoDecode` samples luma as `.w` of an `R8` texture with the swizzle `(0, 0, 0, R)` (Unity's Alpha8) and chroma from an `RG8` texture at half size; both are CPU-written block-linear textures ([nvn-objects.md](nvn-objects.md)).
- **Check:** to check the movie path apart from the decoder, fill the frames with a gradient in `hle_movie.c`; it should appear on screen.
- **Found on:** [Fire Emblem Engage](../../dm/games/0100A6301214E000/README.md).
