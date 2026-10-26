# Native tvOS development target

This target reuses the host and locally supplied rewritten modules from a
prepared iOS project. It has its own UIKit/Metal/GameController front end and
does not change the iPad touch controls. It accepts a Siri Remote, the system
Apple TV Remote on an iPhone/iPad, or an extended gamepad.

## Remote controls

With the game visible, swipe/hold the touch surface or use the directional ring
to move, click to press A, and press Back/Menu to press B. Play/Pause opens the
full controls panel. Choose Camera stick or D-pad to change the directional
input; Resume / Move restores the left stick. The current mode is shown along
the bottom and is remembered between launches.

The panel provides ABXY, L/R/ZL/ZR, +/− and both stick-click buttons. A normal
button sends one short press. Hold L/R/ZL/ZR switches retain a shoulder button
while other controls are used; select the switch again or Release held buttons
to release it. Back or Play/Pause closes the panel. All inputs, including held
shoulders, release when the app loses focus. The TV/Home button retains its
system behavior. This Remote scheme covers the full gamepad through modes,
although a dual-stick gamepad is faster for simultaneous camera/movement.

The iPhone/iPad Control Center Apple TV Remote uses the UIKit directional and
Select/Back/Play-Pause events. Connect it to the same Apple TV and use the
same controls panel; no extra controller app or web server is required. Its
directional input is digital. Physical touch-surface pan gestures also provide
proportional stick input. The frontend does not claim touch-surface pan support
from every phone Remote version. The production handler has portable state and
tvOS simulator UIKit/focus regression tests. Physical phone/Remote input still
requires verification on the target TV; installation alone does not prove it.

`prepare_project.py` derives the tvOS layered icon and top-shelf assets from the
prepared iOS icon. The original artwork is fitted whole on a dark TV canvas; it
is not replaced by a generic icon or cropped through its title.

## Static signed module layout

The linker places each module in contiguous 16 KiB-aligned RX/RW/RX segments.
The middle segment includes the original read-only data, writable data, BSS and
key-page contents. Module-relative addresses and all original bytes are retained.
The loader only writes already-writable data and reduces the key page to read
protection. It does not make signed TEXT writable, remap executable code, or use
JIT. Native host TEXT, unwind information and Objective-C metadata keep their
normal linker layout. Multiple RX segments are intended for local development;
this is not an App Store packaging target.

`TARGET_OS_TV` selects this layout in `dm/host/loader.c`. Other platforms keep
their existing loader. tvOS lacks `sigaltstack`, so crash diagnostics use the
ordinary signal stack there.

## Build and validate

The existing iOS project must have current `Gen/src`, `Gen/main`, `Gen/dm`, and
`Gen/embed.S` inputs. The module paths in its assembly must still exist. For
example, from the repository root:

`prepare_project.py` deliberately removes `Gen/dm/env.txt` from the copied
inputs. That file comes from an iOS-only profile; tvOS settings belong in the
app container's writable `env.txt` or in tvOS frontend defaults.

```sh
python3 dm/tvos/tools/prepare_project.py \
  --ios-project /path/to/ZA-xcode --output /path/to/ZA-TV-xcode \
  --team YOUR_TEAM --shader-archive-sha256 YOUR_RESOURCE_ARCHIVE_SHA256
xcodebuild -project /path/to/ZA-TV-xcode/DmTV.xcodeproj -scheme DmTV \
  -configuration Release -sdk appletvos -derivedDataPath /path/to/ZA-TV-derived \
  CODE_SIGNING_ALLOWED=NO build
python3 dm/tvos/tools/verify_segments.py \
  /path/to/ZA-TV-derived/Build/Products/Release-appletvos/DmTV.app/DmTV \
  /path/to/ZA-TV-xcode/Gen
python3 dm/tvos/tests/test_resources.py
python3 dm/tvos/tests/test_romfs_patch.py
clang -std=c11 -I dm/host dm/tvos/tests/test_remote.c -o /tmp/test-tv-remote
/tmp/test-tv-remote
python3 dm/tvos/tests/test_remote_ui.py --output /path/to/remote-ui-qa
```

Normal Xcode development signing uses the configured team and paired Apple TV.
Only install when the user has authorized device deployment. The verifier checks
the native tvOS platform, nonoverlapping segment ranges, RX/RW protections,
module-relative addresses, alignment, and exact original input bytes.

For a loading-time exit, set `DM_TV_MEMORY_DIAG=1` for that launch to log the
process footprint and remaining dirty-memory allowance every 250 ms, including
UIKit memory warnings. It is disabled by default. Metal's allocation total is
not the process footprint. The existing `DM_MEMSTAT=<absolute TSV path>` tool
adds a one-second VM-class breakdown; `DM_MEMSTAT_DUMP=<second>` also writes a
region snapshot. Capture the official termination signal or system report
before attributing an exit to memory pressure.

`DM_MAP_HEADROOM_MB` is a diagnostic switch that makes a new
`MapPhysicalMemory` request return `ResultLimitReached` after the process's
available dirty-memory allowance falls below the selected reserve. Mapping only
changes protection on reserved virtual pages, so the map's full virtual length
is not subtracted from the allowance. It defaults to `0` (disabled) on every
platform: a living-room b18 experiment showed that refusing small runtime maps
at 640 MiB propagated the error into the game instead of freeing enough cache.

## Resources and saves

The thin app contains code, module tables and font resources. Copy the owned
RomFS directory into its app-data container at
`Library/Caches/nxnative/romfs`. Compile shader libraries for tvOS;
iOS or macOS metallibs are not substitutes. Put a USTAR/gzip archive with only
ordinary `msl/` files/directories at
`Library/Caches/nxnative/msl.tar.gz`. Its SHA-256 must match the
build's `DMShaderArchiveSHA256`. The app imports it on a background thread after
its window appears, verifies SHA-256 and gzip CRC, rejects links and unsafe paths,
and publishes the complete `msl` directory by rename. The input archive remains
on failure and is removed only after success. A previous resource directory is
retained as `.msl-before` during an update.

A complete, independently verified direct directory transfer can publish
`msl/.archive-sha256` last, containing the build's archive SHA-256. A matching
marker makes startup use that directory while retaining a pending archive;
never publish the marker before all resource files have been verified. The
archive importer also writes this marker after successfully activating files.

The same cache directory holds `env.txt`, `log.txt`, `save`, and `temp`.
On the tested tvOS 27.2 device, Foundation returned an Application Support path,
but opening a staged resource there failed with `EPERM`. Caches is readable and
writable by the app. CoreDevice listing a Support file is not evidence that the
application sandbox can read it. A resource bundle is also accepted.
`DM_MSL_DIR` and other explicit
environment options take precedence. Pipeline cache is disabled initially and
the original game present interval is retained. Saves currently live only in
the local cache, with no automatic backup or cross-device synchronization.
tvOS can purge cache data while the app is not running or storage is low, so
these files are not persistent saves and must be backed up separately. See
[Apple's archived local-storage guidance](https://developer.apple.com/library/archive/documentation/General/Conceptual/AppleTV_PG/index.html#//apple_ref/doc/uid/TP40015241-CH12-SW7).

An offline relocated RomFS patch is opt-in. Put its verified schema-4
`virtual-pack-layout-before-root` bundle in the writable cache and set
`DM_ROMFS_PATCH` in `env.txt` to that directory (a relative path is resolved
below `Library/Caches/nxnative`; an absolute path is accepted for development).
The app prepares it on a background thread before starting guest code. It
verifies the complete source archive, descriptor, source root tail, compact
payload, and `redirects.tsv` on first use, then creates an APFS clone overlay
with the physical layout `source prefix + inserted replacement-pack payload +
shifted root tail`. The header points to a virtual root, and the shifted root's
hash-sorted offset vector contains a 16-byte-aligned virtual prefix sum of all
descriptor sizes. The redirect table maps that complete virtual archive to
unchanged source packs, inserted replacements, and the shifted physical root.

The materializer updates only the manifest's exact header, shifted-index, and
descriptor ranges, verifies the inserted payload, shifted root, redirect table,
and complete outputs, writes and syncs `READY.json` last, then publishes all
three files with one directory rename. Startup then sets `DM_ROMFS_OVERLAY` and
`DM_ROMFS_REDIRECTS` together. The file layer validates continuous virtual
coverage and physical bounds, supports partial and cross-row reads, and reports
the virtual size for `arc/data.trpfs`; malformed metadata or a short mapped read
fails closed. Earlier bundle schemas are rejected. The original RomFS stays
read-only and remains the rollback copy. A failed or interrupted preparation
never activates either environment variable and never starts the guest; the
next launch discards an unpublished `.partial` directory. Later launches use a
bounded READY check (sizes, replacement ranges, inserted-payload SHA-256,
final-root SHA-256, descriptor SHA-256, and redirect-table SHA-256) rather than
hashing the unchanged multi-gigabyte archive again. Without the explicit
environment variable this mechanism does nothing.

An explicit launch environment `DM_TV_VALIDATE_LOADER=1` exercises the real
module loader, relocations, key pages, and the original guest `memcmp` code
without requiring game assets. A build/install or linker verification alone
does not establish this device test, gameplay, performance, or memory stability.
An `env.txt` setting also enables this diagnostic. Remove the variable and
terminate the diagnostic app process before a normal launch; an empty value
still enables it because the entry point checks whether the variable exists.
