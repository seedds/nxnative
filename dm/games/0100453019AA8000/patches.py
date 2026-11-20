# Xenoblade Chronicles X DE: changes to the game's own module, by NSO build ID.
# Nintendo SDK changes belong to dm/tools/sdk_patches.py.
from nso_asm import ADRP, B, LDRX


CUTS = {
    # Monolith's allocator directly reserves 60 GB, then a 4 GB + 128 KB
    # first arena. Neither fits the iPhone layout. The SDK StandardAllocator
    # gate cannot adapt these nn::os calls, and disabling virtual memory has
    # no fallback: nninitStartup discards that setting's result.
    # Keep the bump, subtraction and stored capacity consistent when reducing
    # the first arena to 512 MB + 128 KB inside a 4 GB outer region.
    # This is a platform-limit workaround; later-game allocation peaks remain
    # open. All six cuts must apply together (the atomic matcher below).
    '3f2425864cf226844511065f77aa3e36': [
        (0x13f3cf0, 'e001c0d2', '2000c0d2', 'malloc address region: 60 GB to 4 GB'),
        (0xbabe70, 'e98340b2', 'e9ff9fd2', 'first arena bump size, low part: mov x9,#0xffff'),
        (0xbabe80, '4900a0f2', '4900a4f2', 'first arena bump size: 512 MB + 128 KB + 64 KB - 1'),
        (0xbabf04, 'ea7b5fb2', 'ea8361b2', 'first arena subtraction, sign extension'),
        (0xbabf0c, 'caffbff2', 'caffbbf2', 'first arena subtraction: -(512 MB + 128 KB)'),
        (0xbabf1c, '2800c0f2', '4800a4f2', 'first arena stored capacity: 512 MB + 128 KB'),
    ],
}

# Both complete functions and their callees match in base 1.0.0 and 1.0.1.
# The base is static enrollment evidence, not another supported build row.
# A forged 1.0.1 build ID takes this rule and rewrites byte-identically to CUTS.
MATCH_CUTS = {'main': [
    {'name': 'Monolith malloc region and first arena', 'functions': [
        {'size': 696, 'fingerprints': ['b00403a9284d46d15659fc765ce9c0d0474170eb6b9fe2369a4fb69f5458bba2'], 'cuts': [
            (0x10, 'e98340b2', 'e9ff9fd2', 'first arena bump size, low part: mov x9,#0xffff'),
            (0x20, '4900a0f2', '4900a4f2', 'first arena bump size: 512 MB + 128 KB + 64 KB - 1'),
            (0xa4, 'ea7b5fb2', 'ea8361b2', 'first arena subtraction, sign extension'),
            (0xac, 'caffbff2', 'caffbbf2', 'first arena subtraction: -(512 MB + 128 KB)'),
            (0xbc, '2800c0f2', '4800a4f2', 'first arena stored capacity: 512 MB + 128 KB'),
        ]},
        {'size': 16, 'fingerprints': ['df6920c64d84efc7c01f51f9b17e9bd252f9058ed5903bfe6fd5a7ff1a7f4ca3'], 'cuts': [
            (0, 'e001c0d2', '2000c0d2', 'malloc address region: 60 GB to 4 GB'),
        ]},
    ]},
]}


def camera_step(ctx):
    """Integrate stick-driven camera rotation with the game's elapsed frame time.

    The orbit controller curves each stick axis, then multiplies both angles by
    its configured common gain. That gain was applied once per update at the
    original 30 fps. Scale the loaded gain, keeping the input curve, saved
    sensitivities and parameter table unchanged. The task's own timing routine
    clamps elapsed seconds to [1/fps, 2/fps], with fps = 60/(interval or 2).
    """
    other = ctx.others
    if any(other.get(name) is None for name in ('interval setter', 'elapsed seconds', 'clamped task step')):
        return
    # The setter interleaves two ADRPs with their two loads. Its first pair
    # addresses the GOT slot pointing to the live interval, not a fixed global.
    interval = ctx.address(other['interval setter'], other['interval setter'] + 8)
    elapsed = ctx.address(other['elapsed seconds'])
    assert interval % 8 == elapsed % 8 == 0
    site = ctx.base + 0x1ec
    ctx.site(site, 'a8b640bd')  # ldr s8,[x21,#0xb4]: common angular gain
    start = ctx.here()
    ctx.emit(0xbd40b6a8)  # the original gain load
    ctx.emit(ADRP(8, ctx.here(), interval))
    ctx.emit(LDRX(8, 8, (interval & 0xfff) // 8))  # GOT -> interval pointer
    for word in (
        0xb9400108,  # ldr w8,[x8]
        0x7100011f,  # cmp w8,#0
        0x52800049,  # mov w9,#2
        0x1a880128,  # csel w8,w9,w8,eq
        0x52800789,  # mov w9,#60
        0x1ac80d28,  # sdiv w8,w9,w8: fps
        0x1e220101,  # scvtf s1,w8
        0x1e2e1002,  # fmov s2,#1
        0x1e211841,  # fdiv s1,s2,s1: nominal seconds
    ):
        ctx.emit(word)
    ctx.emit(ADRP(8, ctx.here(), elapsed))
    ctx.emit(LDRX(8, 8, (elapsed & 0xfff) // 8))  # frame object pointer
    for word in (
        0x1e204022,  # fmov s2,s1: absent frame object clamps to nominal
        0xb4000048,  # cbz x8,+8
        0xbd626d02,  # ldr s2,[x8,#0x226c]: elapsed seconds
        0x1e212825,  # fadd s5,s1,s1: upper bound
        0x1e252040,  # fcmp s2,s5
        0x1e22cca5,  # fcsel s5,s5,s2,gt
        0x1e212040,  # fcmp s2,s1
        0x1e254c22,  # fcsel s2,s1,s5,mi
        0x1e27d001,  # fmov s1,#30
        0x1e210842,  # fmul s2,s2,s1: 30 Hz frame ratio
        0x1e220908,  # fmul s8,s8,s2: integrate angular speed
    ):
        ctx.emit(word)
    ctx.emit(B(ctx.here(), site + 4))
    # x8/x9 and s1/s2/s5 are overwritten before use after this site in both
    # reviewed bodies; the next FCMP replaces flags before a conditional use.
    ctx.put(site, [B(site, start)], 'camera angular integration: use the game\'s clamped elapsed frame time')
    ctx.fps_ready = True


# Both complete orbit functions have the same code and site; the four remaining
# fingerprint differences are delayed GOT/SIMD-table address loads. Their callees
# and the timing assumptions below were independently checked in base 1.0.0 and
# update 1.0.1. The base remains static evidence, not a supported profile build.
MATCH_STUBS = {'main': [
    {'name': 'camera angular integration uses elapsed time', 'size': 2780,
     'fingerprints': ['42a7fb59ce3cc9292102bfb463f9e4a56886e88a998ad8f14999767331b9fb72',
                      '5d789f70bf12105dd2ae10c6822b8d840055330e9db6893271e4b56349d14c83'],
     'stub': camera_step, 'fps_required': True,
     'others': {
         'interval setter': {'size': 32, 'fingerprints': ['5b3276a0309456b63d7a5ebf4477d5b4249b49b769e9033d540beb29823345ce']},
         'elapsed seconds': {'size': 32, 'fingerprints': ['4aa168a4c43f8e25311f1983d975a58457ab41b00497892fc6cf433e1a54bd97']},
         'clamped task step': {'size': 224, 'fingerprints': ['8480c574e5a54f7e9f79b72a4e903daade2896eaa663a517666edb968d013f03']},
     }},
]}

BINDING = [
    {'what': 'malloc region 60 GB → 4 GB and its first arena', 'now': 'game', 'goal': 'game', 'purpose': 'address space',
     'why': "Monolith's allocator calls nn::os directly; its sizes span two functions matched atomically. The 512 MB first arena is sized for this game; an engine rule needs shared-layout proof and dynamic sizing",
     'checked': 'both complete functions and their callees in base 1.0.0 and update 1.0.1; forged-build rewrite byte-identical; 1.0.1 opening-field gameplay with the iPhone alias size, first arena 95 MB committed'},
    {'what': 'camera angular integration uses elapsed time', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's orbit camera integrates configured angular speed once per update; multiply the loaded gain by its own 30 Hz elapsed-time ratio. Engine input is amplitude state and must not be scaled",
     'checked': 'complete orbit bodies, scratch-register liveness, unique interval setter, elapsed getter and clamped task-step functions in base 1.0.0 and update 1.0.1'},
]
