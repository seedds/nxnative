# Super Mario Bros. Wonder (010015100B514000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
# The gate number is dm/host/dm.h's OBS_GRPC_ERROR (27), written as a number as patch_nso.py writes its own (a table name is the only upper-case name a patches.py may have)
OBS = {
  # 1.2.1 (v327680) main, also Nextendo's signed patch of it (an IPS of three instructions in gRPC's TLS code that keeps the build ID): gRPC's grpc_error_create
  # (const char *file, int line, const grpc_slice &desc, ...), called from 509 sites. Nintendo's gRPC has its logging compiled out, so an online call that fails before
  # its first socket (2321-4992, UNAVAILABLE) leaves no line; the host logs each error's source file, line and text once (dm/host/hle_grpc.c). The entry instruction is
  # sub sp, sp, #0x60
  'ff773e90972d544eb79406eaa65396d5': [(0xb57b40, 27, 'd10183ff', "gRPC's grpc_error_create entry: the source file, line and text of each internal error (hle_grpc.c)")],
}
BINDING = [
    {'what': "gRPC's internal errors logged (grpc_error_create observation gate)", 'now': 'build', 'goal': 'engine', 'purpose': 'runtime',
     'kept': 'a read-only diagnostic for bringing up online play on one build; a gRPC-wide locator (a fingerprint of grpc_error_create) is worth it only when another game needs it',
     'why': "gRPC is middleware: its grpc_error_create could be found by a fingerprint in any game that links the same gRPC; it is placed by offset in the one build "
            "brought up online so far"},
]
