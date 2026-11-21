# Mina the Hollower (0100DCD01F306000)

Update 1.0.6 (v327680), SDK 21.4.3, Yacht Club Games engine, NVN.

## Startup reservations

The compact iPhone layout aborted before NVN initialization. The return address
at the first allocation was `main+0x87cbac`, with the abort at `sdk+0x141b9c`.
The game requests 16 GiB for StandardAllocator and 4 GiB each for the Default
Mem and Render Mem slabs. The three build-guarded cuts reduce those requests
to 1.5 GiB, 768 MiB and 768 MiB. The profile supplies a 4.5625 GiB alias
window within the 5 GiB guest reservation. Startup mapped about 1.51 GiB of
that alias window in the tested run. Long-session allocator headroom is open.

## Validation and shader inputs

On iPhone Duo, the earlier local bring-up at `7ed6223a61bb` booted with the
compact reservations and loaded a 110-entry shader cache prepared from a local
dump. The local validation series reached gameplay and rendered portrait name
entry, including the keyboard and entered glyph. The portrait trace recorded
four draws per frame on three sampled frames.

This profile change provides the startup configuration and allocator patches.
The earlier runs used separate shader inventory tooling and locally generated
shader tables. Rebuilding that library from the same game revision and replaying
portrait name entry from current upstream remains open.
