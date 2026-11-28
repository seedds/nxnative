# FINAL FANTASY TACTICS - The Ivalice Chronicles (010038B015560000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_STUBS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import B, LDRX, STRX

# The job system wakes one worker of a pool through the pool's single auto-clear event. A job goes into worker k's own queue (main+0x154c0), and if k's state reads asleep the
# caller signals the event (main+0x12554 on 1.0.0 / 1.4.0 / 1.5.1, from the per-worker dispatch loops, the single-job submits and the shutdown). The SDK event wakes one condition
# waiter, by priority then FIFO (matching the host and Horizon); it cannot target k's queue. The woken worker runs only its own queue (worker loop main+0x15ce0) and sleeps again,
# so with two workers in one pool (threads 0x1006 / 0x1007, later pinned to core 2) the signal went to the wrong one: the main thread waited forever on a dispatch at boot in 4 of
# 6 Mac runs, and in a run that got through, a job sat in worker 0's queue for 12 s.
#
# The wait wrapper (main+0x12500, {event, pending}) already signals the event once more when it atomically clears a nonzero pending word at [x0, #8] after waking, and nothing in
# main sets it. The stub replaces only the signal wrapper's first instruction (ldr x0, [x0]) with a trampoline that stores x0 (nonzero) to [x0, #8], runs the original load, and
# returns to site + 4 (the build's own `b SignalEvent`, left untouched so PLT shifts across updates do not matter).
#
# The 8-byte signal wrapper (`ldr x0, [x0]; b SignalEvent`) is byte-identical to an adjacent 8-byte wrapper (main+0x1254c), so `called_by` identifies it through the single-job
# submit caller (main+0x14790, bl at +0x94) and `others` checks that the wait wrapper (main+0x12500) still consumes [x0, #8]. None of the matched functions contains an ADRP/ADR.


def job_pool_wake(ctx):
    if ctx.others.get('wait wrapper') is None:
        return
    site = ctx.base
    ctx.site(site, '000040f9')  # ldr x0, [x0]
    start = ctx.here()
    ctx.emit(STRX(0, 0, 1))             # str x0, [x0, #8] (pending = nonzero: the woken worker signals once more)
    ctx.emit(LDRX(0, 0))                # ldr x0, [x0] (original instruction)
    ctx.emit(B(ctx.here(), site + 4))   # back to the build's own `b SignalEvent`
    ctx.put(site, [B(site, start)], 'job pool wake: set pending = nonzero before SignalEvent')


MATCH_STUBS = {'main': [
    {
        'name': 'job pool wake-up',
        'size': 0x8,
        'fingerprints': ['eff9c2620407adacdf5a814b5843c4b8cd68aa9039fc209d0ab4c017a2b55fa8'],
        'called_by': {
            'size': 0xb0,
            'fingerprints': ['60e6830d5585508df333a854f20038d83aea9267eb3bc702832c7217fc90d889'],
            'call': 0x94,
        },
        'others': {
            'wait wrapper': {
                'size': 0x4c,
                'fingerprints': ['dcddea6d045afbecac6aeed6a68b478ee3fa8438c073dccb2dd6cb4ea51cf910'],
            },
        },
        'stub': job_pool_wake,
    },
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'job pool wake-up', 'now': 'game', 'goal': 'game', 'purpose': 'race',
     'checked': '1.0.0 (v0) and 1.4.0 (v393216) mains: complete signal wrapper, submit caller, queue push and wait wrapper verified by disassembly, rewrite and instruction replay (2026-11-23)',
     'why': "a lost wake-up in the game's own job pool, not a host or SDK difference; function-matched (MATCH_STUBS) across updates via its submit caller and wait wrapper"},
]
