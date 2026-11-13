# Tetris Effect: Connected's own code, keyed by the NSO build ID (SDK cuts live in dm/tools/sdk_patches.py; the game note, README.md, says why each is here).
#
# 1.0.0 main: the host never loads the Epic Online Services SDK (rom:/nro/EOSSDK-Switch-Shipping.nro; dm/tools/nro_collect.py
# ONLINE_SDKS, hle_ro.c), so the game's NRO loader (main+0x8b2090) leaves its "loaded" flag clear and the Redpoint EOS plugin never
# creates its online subsystem: FOnlineSubsystemModule::GetOnlineSubsystem(FName("RedpointEOS")) (module vtable +0x58) returns
# nullptr. The game's own online layer (main+0x9e4418..0xac6978: FTPOnlineAsyncTask*EOS leaderboards, play logs, events, presence)
# asks for that subsystem by name 32 times and does not always check the result: main+0x9e44cc reads the vtable of the null
# subsystem (CRASH sig=11 addr=0 pc=main+0x9e44cc, about 3.5 s after boot, before the title). Workaround in the game's own code:
# each of those 32 adrp/add pairs that loads L"RedpointEOS" (main+0x56aed92) loads L"NULL" (main+0x56c07f8, Unreal's
# NULL_SUBSYSTEM name) instead, so the game layer gets OnlineSubsystemNull, which the engine already creates, and plays offline.
# The Redpoint plugin's own 101 uses of the name (main+0x213fcc..0x37ae0c) are left as they are.
# 2.0.2 only: the game layer's "logged in to EOS" check (main+0xb64e18; the debug readout "EOS LogIn: %d" calls it every frame) asks the subsystem above, now
# OnlineSubsystemNull, for local user 0's login status; nothing ever logs in through NULL, so it stays false and the 2.0.2 MULTIPLAYER screen waits for a login
# forever (black, audio on; 1.0.0 opens the menu without it). Answering true (mov w0, #1; ret) opens MULTIPLAYER: PLAY → RANKED / FRIEND / LOCAL.
why_login = 'game-layer "EOS logged in" check (2.0.2): always true, so MULTIPLAYER opens offline instead of waiting for a login that never comes'
why_eos = 'online layer asks for the RedpointEOS subsystem, which never exists without the EOS SDK: L"RedpointEOS" → L"NULL" (OnlineSubsystemNull)'

CUTS = {
    '02e64f62153c1f009b699806197e82db': [
        (0x9e4418, '416602d021483691', 'e166029021e01f91', why_eos),
        (0x9e4728, '416602d021483691', 'e166029021e01f91', why_eos),
        (0x9e4894, '416602d021483691', 'e166029021e01f91', why_eos),
        (0x9e4b80, '416602d021483691', 'e166029021e01f91', why_eos),
        (0x9e83d4, '216602d021483691', 'c166029021e01f91', why_eos),
        (0x9e8b08, '216602d021483691', 'c166029021e01f91', why_eos),
        (0x9ec108, '016602d021483691', 'a166029021e01f91', why_eos),
        (0x9ec7e8, '016602d021483691', 'a166029021e01f91', why_eos),
        (0x9f0be0, 'e16502d021483691', '8166029021e01f91', why_eos),
        (0x9f1118, 'e16502b021483691', '616602f021e01f91', why_eos),
        (0x9f2348, 'e165029021483691', '616602d021e01f91', why_eos),
        (0x9f29a8, 'e165029021483691', '616602d021e01f91', why_eos),
        (0x9f7728, 'a16502f021483691', '416602b021e01f91', why_eos),
        (0x9f7d98, 'a16502f021483691', '416602b021e01f91', why_eos),
        (0x9fc778, '816502d021483691', '2166029021e01f91', why_eos),
        (0xa00b68, '616502d021483691', '0166029021e01f91', why_eos),
        (0xa01b04, '616502b021483691', 'e16502f021e01f91', why_eos),
        (0xa05f88, '416502b021483691', 'c16502f021e01f91', why_eos),
        (0xa07424, '216502f021483691', 'c16502b021e01f91', why_eos),
        (0xa0d868, '016502b021483691', '816502f021e01f91', why_eos),
        (0xa16438, 'c164029021483691', '416502d021e01f91', why_eos),
        (0xa171a8, 'a16402f021483691', '416502b021e01f91', why_eos),
        (0xa1b43c, '816402f021483691', '216502b021e01f91', why_eos),
        (0xa1b888, '816402f021483691', '216502b021e01f91', why_eos),
        (0xa1c274, '816402d021483691', '2165029021e01f91', why_eos),
        (0xa21f24, '616402b021483691', 'e16402f021e01f91', why_eos),
        (0xa278b0, '216402f021483691', 'c16402b021e01f91', why_eos),
        (0xa7bf54, '816102f021483691', '216202b021e01f91', why_eos),
        (0xabfb34, '615f02f021483691', '016002b021e01f91', why_eos),
        (0xac0100, '615f02d021483691', '0160029021e01f91', why_eos),
        (0xac64b8, '415f029021483691', 'c15f02d021e01f91', why_eos),
        (0xac6978, '415f029021483691', 'c15f02d021e01f91', why_eos),
    ],
    # 2.0.2 (v1310720) main: the same lookups, 34 of them (main+0xb64b40..0xc55824; crash at main+0xb64bec without them); L"RedpointEOS" at main+0x5abf044,
    # L"NULL" at main+0x5ad0e10; the plugin's own 124 uses (main+0x2086fc..0x466a44) are left as they are
    '8838095f4f659f63cb45a00e62dfb887': [
        (0xb64e18, 'ff0301d1fd7b01a9', '20008052c0035fd6', why_login),
        (0xb64b40, 'c17a02f021100191', '617b029021403891', why_eos),
        (0xb64e30, 'c17a02f021100191', '617b029021403891', why_eos),
        (0xb64f94, 'c17a02f021100191', '617b029021403891', why_eos),
        (0xb65290, 'c17a02d021100191', '417b02f021403891', why_eos),
        (0xb68aac, 'a17a02f021100191', '417b029021403891', why_eos),
        (0xb69198, 'a17a02d021100191', '217b02f021403891', why_eos),
        (0xb6cbc8, '817a02f021100191', '217b029021403891', why_eos),
        (0xb6d2ac, '817a02d021100191', '017b02f021403891', why_eos),
        (0xb71464, '617a02d021100191', 'e17a02f021403891', why_eos),
        (0xb7198c, '617a02d021100191', 'e17a02f021403891', why_eos),
        (0xb72b18, '617a02b021100191', 'e17a02d021403891', why_eos),
        (0xb73180, '617a029021100191', 'e17a02b021403891', why_eos),
        (0xb77ef0, '417a029021100191', 'c17a02b021403891', why_eos),
        (0xb78564, '217a02f021100191', 'c17a029021403891', why_eos),
        (0xb7d2d8, '017a02d021100191', '817a02f021403891', why_eos),
        (0xb81674, 'e17902d021100191', '617a02f021403891', why_eos),
        (0xb825c8, 'e17902b021100191', '617a02d021403891', why_eos),
        (0xb86968, 'c17902b021100191', '417a02d021403891', why_eos),
        (0xb87df4, 'c179029021100191', '417a02b021403891', why_eos),
        (0xb8e540, '817902b021100191', '017a02d021403891', why_eos),
        (0xb97380, '4179029021100191', 'c17902b021403891', why_eos),
        (0xb980e8, '217902f021100191', 'c179029021403891', why_eos),
        (0xb9c424, '017902f021100191', 'a179029021403891', why_eos),
        (0xb9c834, '017902f021100191', 'a179029021403891', why_eos),
        (0xb9cfcc, '017902f021100191', 'a179029021403891', why_eos),
        (0xba3898, 'e178029021100191', '617902b021403891', why_eos),
        (0xba9618, 'a17802d021100191', '217902f021403891', why_eos),
        (0xbfcb58, '017602f021100191', 'a176029021403891', why_eos),
        (0xbfd33c, '017602d021100191', '817602f021403891', why_eos),
        (0xc05664, 'c17502d021100191', '417602f021403891', why_eos),
        (0xc4e550, '817302b021100191', '017402d021403891', why_eos),
        (0xc4eb18, '817302b021100191', '017402d021403891', why_eos),
        (0xc55370, '417302d021100191', 'c17302f021403891', why_eos),
        (0xc55824, '417302d021100191', 'c17302f021403891', why_eos),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': "online layer's RedpointEOS subsystem lookups → OnlineSubsystemNull", 'now': 'build', 'goal': 'game', 'purpose': 'runtime',
     'why': 'the subsystem name is a string literal the game layer loads at each site; a function-matched rule could carry the sites over updates, but each build has its own count (32 in 1.0.0, 34 in 2.0.2)',
     'checked': 'main 1.0.0: 32 adrp/add pairs to main+0x56aed92 in main+0x9e0000..0xac8000; main 2.0.2: 34 pairs to main+0x5abf044 in main+0xb60000..0xc58000; both boot past the old crash on the Mac, 2.0.2 plays on the iPhone'},
    {'what': 'game-layer "EOS logged in" check → true', 'now': 'build', 'goal': 'build', 'purpose': 'runtime',
     'why': 'one function of the game layer (2.0.2 only; 1.0.0 opens MULTIPLAYER without it) asks the NULL subsystem for a login nothing makes; it exists because of the cut above',
     'checked': 'main 2.0.2: MULTIPLAYER → PLAY → RANKED / FRIEND / LOCAL opens on the Mac (2026-11-06)'},
]
