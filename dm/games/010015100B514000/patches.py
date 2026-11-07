# Super Mario Bros. Wonder (010015100B514000): the rewriter's patches of this game's own builds, keyed by NSO build ID
# (dm/tools/patch_nso.py, "Registered patches"). SDK cuts live in dm/tools/sdk_patches.py.
CUTS = {
  # Nintendo Switch 2 Edition, Program 1 (040015100B514001) main, display version 1.2.1.
  # Resource heap slack, as the Switch 1 builds once carried (README, Memory, "Removed workaround"; issue #1580).
  # The resource loader (main+0x130460) sizes each resource's sead::ExpHeap as the resource-size-table value
  # + max(alignment, 0x20) + 8 (main+0x1304e8..0x130514) and passes it to the heap's create (main+0x131e00).
  # On the host, Model/NpcCommonerA.bfres (heap 0x8d03c8) runs short: the model's combined MatBlock + DisplayList
  # request 0x5424 (main+0x1ff04c / +0x1ff068) rounds to a 0x6000 GPU chunk (main+0x287048), leaving 0x3a8 for
  # a 0x418 descriptor, and the game then initializes a buffer on a null memory pool (crash in
  # nvnMemoryPoolGetFlags). The native NVN2 command sizes that would keep the request at or below 0x5000 are
  # not known (#1580). Every resource heap gets 16 KB more instead: adds w0,w8,#8 -> adds w0,w8,#4,lsl #12
  # (+0x4000 in place of +8). The b.eq that follows still sees the flags of the same adds.
  'ec47e6f8846d2a7b6892e2682845c725': [
    (0x130514, '00210031', '00114031', 'resource heap size: adds w0,w8,#8 -> adds w0,w8,#0x4000 (ExpHeap size +16 KB - 8)'),
  ],
}

BINDING = [
    {'what': 'resource heap size +16 KB (ExpHeap create size, main+0x130514)', 'now': 'build', 'goal': 'host', 'purpose': 'memory',
     'why': "the shortfall comes from the host's NVN2 command-memory accounting (#1580); a correct host representation would make the cut unnecessary for every build",
     'blocked': 'the native NVN2 sizes of a recorded indexed draw are not established (#1580)'},
]
