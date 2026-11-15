# Ori and the Will of the Wisps (01008DD013200000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO
# build ID; STUBS as described there ("Stubs"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B, BL, CBNZW, LDP, LDP_POST, LDRX, STP, STP_PRE, STRX

# Unity 2018.4.24's GfxDevice keeps one table of constant-buffer layouts (UnityPerDraw, UnityPerMaterial, VGlobals, ...), a vector sorted by name hash | size << 32 of 40-byte entries,
# each holding a std::map of its members (device +0x3068), with their values at +0x3080. The shared device (thread index 0 at +0x2f88) uses it with no lock; the graphics-job devices
# (1-3) take the registry's mutex ([main+0x7e48960], recursive) before they add to it (main+0x2df18). Two threads use the shared device at once: the main thread queues "set this
# constant buffer" to the render thread (GfxDeviceClient slot 0x208, main+0xab7d0, command 0x2737), then registers the layout itself on the real device (slot 0x210, main+0x34f80:
# look it up, insert it, fill its member map) and looks members up (main+0x353c0), while UnityGfxDeviceWorker runs the commands and the draws, which look layouts up, insert the
# missing ones and write their values. Both inserted the same new layout within 0.1 ms, and a vector shift copying an entry's map while the other thread changed it crashed in the
# map copy (main+0x3e528, a node pointer 0x7e600004755): 3 of 7 Mac boots in the first second of the shader loading. The game's own race (it also runs without graphics jobs),
# which the Switch's timing hides. Probes of 45 s of loading: the main thread reaches the shared table only through the register and the member lookup; the render thread through
# the program-parameter apply, the sets and the value writes below. The stubs run Unity's own mutex around every function of the table's API when it works on the shared device,
# so each thread's lookup, insert and use of an entry is whole
_lock, _unlock = 0xcbebd0, 0xcbebe0   # Unity's Mutex::Lock / Unlock (an nn::os mutex at +8, created recursive)
_mutex = 0x2df18                      # adrp + ldr of the registry mutex's pointer, where the job devices lock it
_wrapped = (   # (function, its first instruction, the register holding the device, what it does)
    (0x2d640, 'ff4302d1', 0, 'insert a layout'),
    (0x34f80, 'ffc301d1', 0, 'register a layout and its members (main thread)'),
    (0x353c0, 'f60f1df8', 0, 'look up a member (main thread)'),
    (0x24550, 'ff0302d1', 1, "apply a program's parameters (render thread, per draw)"),
    (0x24de0, 'ffc301d1', 0, 'set a constant buffer (render thread)'),
    (0x2d200, 'ff0302d1', 0, 'set several constant buffers (render thread)'),
    (0x35510, 'ff0301d1', 0, "write a buffer's values (render thread)"),
    (0x25110, 'fc6fbaa9', 0, "move a layout's bound slots (render thread)"),
    (0x29330, '083858f9', 0, "reset the layouts' slots"),
    (0x29120, 'f40f1ef8', 1, 'copy the table into a graphics-job device'),
)


def registry_lock(ctx):
    here, emit = ctx.here, ctx.emit
    slot = ctx.address(_mutex)
    def mutex_call(fn):
        emit(ADRP(0, here(), slot)); emit(LDRX(0, 0, (slot & 0xfff) // 8)); emit(BL(here(), fn))
    for fn, first, dev, what in _wrapped:
        ctx.site(fn, first)
        start = here(); orig = start + 4 * 23   # the original first instruction, after the 23 words below
        emit(0xb96f8810 | dev << 5)        # ldr w16, [x<dev>, #0x2f88]   device thread index (x16 is dead at a call)
        emit(CBNZW(16, here(), orig))      # a job device: as before (its own table is its thread's; it locks where it adds to the shared one)
        emit(STP_PRE(29, 30, 31, -0x60)); emit(0x910003fd)   # stp x29, x30, [sp, #-0x60]!; mov x29, sp
        for i in range(4): emit(STP(2 * i, 2 * i + 1, 31, 0x10 + 0x10 * i))   # the arguments x0..x7 (none of these functions takes more)
        mutex_call(_lock)
        for i in range(4): emit(LDP(2 * i, 2 * i + 1, 31, 0x10 + 0x10 * i))
        emit(BL(here(), orig))
        emit(STRX(0, 31, 2))               # str x0, [sp, #0x10]   the result (the insert's entry)
        mutex_call(_unlock)
        emit(LDRX(0, 31, 2)); emit(LDP_POST(29, 30, 31, 0x60)); emit(0xd65f03c0)   # ret
        assert here() == orig
        emit(int.from_bytes(bytes.fromhex(first), 'little')); emit(B(here(), fn + 4))
        ctx.put(fn, [B(fn, start)], f"Unity's constant-buffer table: {what} on the shared device under the registry mutex")


STUBS = {'11c33c7c2dbd3c5491841f67495c7ee8': registry_lock}   # 1.2.1 (v196608) main

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open; checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': "Unity's constant-buffer table locked on the shared device", 'now': 'build', 'goal': 'engine', 'purpose': 'race',
     'why': "Unity 2018.4's threaded GfxDevice: the main thread registers layouts on the real device while the render thread uses the same table; any Unity 2018.4 build with this code has the race",
     'blocked': 'the stubs have no function-matched form yet, and no other Unity 2018.4 main on the drive shares these functions (Ori and the Blind Forest 1.0.2: no fingerprint match)',
     'checked': "probes of both threads inserting the same key and of the table API's callers by thread; Blind Forest main compared with nso_fnmap"},
]
