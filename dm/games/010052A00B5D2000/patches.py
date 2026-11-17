# LEGO Harry Potter Collection (010052A00B5D2000): the rewriter's patches of this game's own modules. dm/tools/patch_nso.py
# applies those of a module's NSO build ID — CUTS, OBS, FRAME_STEP and STUBS as described there ("Registered patches").
from nso_asm import B

# libharry2.nro (Years 5-7, build 7e19fa8490f09d9d2a71f74f5903211d): the pass table's publish, moved to record time.
#
# The recorder nro0+0xd2ff0 writes one pass state block per call into the renderer's *live* table (singleton+0x9d0, indexed by the slot it returns, 0xd3044) and the
# begin-frame routine nro0+0x15f420 copies those 24 pointers into the *published* table (singleton+0xa90, 0x15f4b8..0x15f57c) that every replay reads: begin_pass
# nro0+0xd39c0 and the eleven other sites (0x128668 0x12926c 0x12a234 0x12a93c 0x153f80 0x15446c 0x154dec 0x155648 0x155ac4 0x1562ec 0x156a5c 0x15710c) index it by the
# pass id the recorder returned, and they replay in the same frame the record is made. The two run on different threads under two locks (0x11541a8 and 0x11541c0) that
# do not order them: in the first failing frame the worker held the frame lock and sat in the pass loop while the main thread was still at 0x15f458, the lock acquire
# at the top of 0x15f420, so the replay read a table that had never been published — NULL at 0xd3a2c, and one site further on a record whose memory the frame's bump
# allocator (x26) had already reused (the two 1.0f at 0xd3adc). The records live only for their frame, so the publish cannot wait for a routine that runs a whole
# frame phase later: it has to happen when the record is made.
#
# The site is the recorder's own increment, "add w8, w0, #1" at 0xd3138: x19 is the singleton, w0 the slot the recorder is about to return (loaded from
# singleton+0x9c8 at 0xd3134), and x8, x9 and x10 are dead there — the epilogue that follows restores x19-x30 from the stack and w0 is the result. The stub stores
# live[slot] into published[slot] before it increments the slot count, then returns to 0xd3140 (the epilogue), so the caller still gets the original slot id. The whole
# double buffer is kept: live stays the recorder's target and published stays what the replays read; only the one-frame lag the copy imposed is removed, and a
# slot that is not recorded in a frame keeps the published pointer it had.
def publish_record(ctx):
    here, emit, site, put = ctx.here, ctx.emit, ctx.site, ctx.put
    site(0xd3138, '08040011')                        # add w8, w0, #1
    start = here()
    for w in (0x11000408,   # add w8, w0, #1                  the original increment
              0x91274269,   # add x9, x19, #0x9d0             &live[slot]
              0xf860d92a,   # ldr x10, [x9, w0, sxtw #3]      live[slot], stored at 0xd3044
              0x912a4269,   # add x9, x19, #0xa90             &published[slot]
              0xf820d92a,   # str x10, [x9, w0, sxtw #3]      published[slot] = live[slot]
              0xb909ca68):  # str w8, [x19, #0x9c8]           ++slot count (the original 0xd313c)
        emit(w)
    emit(B(here(), 0xd3140))
    put(0xd3138, [B(0xd3138, start)],
        'pass-state recorder: publish live[slot] into published[slot] at record time, before the slot count is incremented (the replay reads the record in the same '
        'frame, and the records die with the frame; without it the published table is only filled by 0x15f420, which the worker\'s pass loop can outrun)')

STUBS = {'7e19fa8490f09d9d2a71f74f5903211d': publish_record}
