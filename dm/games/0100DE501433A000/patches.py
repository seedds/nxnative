# Tetris Effect: Connected (0100DE501433A000): the rewriter's patches of this game's own builds.
# dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID; STUBS and CUTS as described there.
from nso_asm import B

# Workaround in the game's own code: the host refuses nro/EOSSDK-Switch-Shipping.nro (hle_ro.c / nro_collect.py), so EOS_Platform_Create stays a stub
# returning 0, OnlineSubsystemRedpointEOS does not register "RedpointEOS", and IOnlineSubsystem::Get("RedpointEOS") returns nullptr.
# 1. The Actor's BeginPlay at main+0xb64b18 (called from AActor::DispatchBeginPlay at main+0x2742aec) calls GetOnlineSubsystem("RedpointEOS") at main+0xb64b88
#    and dereferences the result in x20 at main+0xb64bec without checking for null. When x0 == 0, skip constructing/passing the delegate and branch to the
#    epilogue at main+0xb64c3c.
# 2. On the Multiplayer screen, Blueprint/UMG polls ATPOnlineManagerEOS::IsEOSLoggedIn() (main+0xc90004 -> main+0xb64e18) every frame and blocks input
#    until it returns 1; returning 1 from main+0xb64e18 unblocks the Multiplayer screen and allows backing out to the Main Menu.
def eos_null_guard(ctx):
    here, emit = ctx.here, ctx.emit
    ctx.site(0xb64b8c, 'f40300aa')   # mov x20, x0
    start = here()
    emit(0xb4000060)                 # cbz x0, start+12 (0xb64c3c)
    emit(0xaa0003f4)                 # mov x20, x0
    emit(B(here(), 0xb64b90))
    emit(B(here(), 0xb64c3c))
    ctx.put(0xb64b8c, [B(0xb64b8c, start)], 'RedpointEOS null check at main+0xb64b8c')


STUBS = {'8838095f4f659f63cb45a00e62dfb887': eos_null_guard}   # 2.0.2.2 (v1310720) main

CUTS = {
    '8838095f4f659f63cb45a00e62dfb887': [
        # sub sp, sp, #0x40; stp x29, x30, [sp, #0x10]  ->  mov w0, #1; ret
        (0xb64e18, 'ff0301d1fd7b01a9', '20008052c0035fd6', 'ATPOnlineManagerEOS::IsEOSLoggedIn() -> 1 so the Multiplayer screen does not block input'),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': 'RedpointEOS null check at main+0xb64b8c', 'now': 'build', 'goal': 'unreviewed', 'purpose': 'runtime',
     'why': "the game's own BeginPlay dereferences IOnlineSubsystem::Get(\"RedpointEOS\") without a null check when EOSSDK-Switch-Shipping.nro is refused and RedpointEOS is not registered",
     'checked': 'verified on 2.0.2.2 (v1310720) Mac app: boots past the spinning logo and single player is playable'},
    {'what': 'ATPOnlineManagerEOS::IsEOSLoggedIn() returns 1', 'now': 'build', 'goal': 'unreviewed', 'purpose': 'runtime',
     'why': 'the Multiplayer screen Blueprint polls main+0xb64e18 every frame and blocks controller input while it returns 0',
     'checked': 'verified on 2.0.2.2 (v1310720) Mac app: unblocks the Multiplayer screen dialogs (Cross-Platform Play, Welcome Guardian) and allows backing out with B'},
]
