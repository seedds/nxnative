# Experimental internal-scene resolution, main 1.1.0 only. Derived from this build's instructions:
# 33170c packs the final scene dimensions into x21 AFTER both dynamic-resolution branches join;
# 3317e4 passes that size in the descriptor at sp+58 to 134850. Its fallback descriptor at 3317fc
# is 1600x900. Legacy choices retain display sizing and frame step; full-output
# choices separately change verified allocation/display constants. The 1800p
# and 2160p choices opt into the native-size scan-query bridge below.
# No original instruction changes here: runtime_patches.py can opt into a newly assembled stub.
from nso_asm import B, BL, BLR, ADRP, ADDI, SUBI, STP, LDP, STRX, LDRX

build_id = '8275fbe1ec5191c279f3a943ad8524f4'
scene_site = 0x33170c
scene_sizes = (('SCENE_1440', 2560, 1440), ('SCENE_2160', 3840, 2160), ('SCENE_1800', 3200, 1800), ('SCENE_D1792', 3200, 1792))   # runtime_patches.py's choices 1440p, 2160p, 1800p and diag1792
full_scene_sizes = (('SCENE_F1080', 1920, 1080), ('SCENE_F1440', 2560, 1440), ('SCENE_F2160', 3840, 2160), ('SCENE_F2880', 5120, 2880), ('SCENE_FNATIVE', 1600, 900),
                    ('SCENE_FNATIVE_W', 1600, 900), ('SCENE_F1080_W', 1920, 1080), ('SCENE_F1440_W', 2560, 1440), ('SCENE_F2160_W', 3840, 2160), ('SCENE_F2880_W', 5120, 2880))   # 16:10 crops the original square-pixel 16:9 canvas at presentation



def resolution_stub(c):
    c.site(scene_site, '358108aa')   # orr x21,x9,x8,lsl #32, common to adaptive and fixed-size paths
    for name, w, h in scene_sizes:   # one stub per runtime choice, each a branch target of its own manifest write
        start = c.here()
        c.emit(0xd2800000 | (w << 5) | 21)   # movz x21,#w
        c.emit(0xf2c00000 | (h << 5) | 21)   # movk x21,#h,lsl #32
        c.emit(B(c.here(), scene_site + 4))
        c.runtime_exports[name] = start


# This is a borrowed query view, used only from 336a8c through 336cfc. The
# original shared-pointer owner remains in x19/sp+c0 and is released normally.
scan_source_site = 0x336a8c
scan_source_original = '881740b9'
scan_bootstrap = 0x138df60
scan_extension = b'nxnativeMp4ScanSourceV1\0'
# Check every fixed-address ABI assumption against the legal original executable.
scan_source_abi = (
    (scan_source_site, scan_source_original),
    (scan_bootstrap, 'b02400f0115243f910821a9120021fd6'),
    (0x3369ac, '9ae22591'),  # x26 = renderer+978; [x26]+28 is the NVN command buffer
    (0x1d6664, '6a544129'),  # view target/format at +8/+c
    (0x1d6710, '89ab4229'),  # view dimensions at +14/+18
    (0x1d6780, '991f40f9'),  # pointer to NVNtexture at +38
    (0x1d678c, '97230191'),  # inline NVNtextureView at +48 (16 bytes)
    (0x336cfc, '8087422d'),  # last use of the borrowed prefix
    (0x336d00, 'fce75ef9'),  # x28 replaced; the original owner is still released
    (0x13c1b88, '35000000'),  # original engine-format table: 0x12 -> NVN D24S8
    (0x13c1b90, '34000000'),  # 0x13 -> NVN D32F
    (0x13c1bf0, '0a000000'),  # 0x1f -> NVN R32F
)


def scan_source_words(start):
    """Preserve live integer/SIMD registers across a private host call. x28 is
    the sole changed pointer; the displaced LDR still executes before rejoining.
    No platform register x18, guest TLS, SVC or new shader translation is needed.
    """
    words = []
    def emit(w):
        words.append(w)
    def here():
        return start + 4 * len(words)
    emit(SUBI(31, 31, 0x2a0))
    for n in range(0, 18, 2):
        emit(STP(n, n + 1, 31, n * 8))
    emit(STRX(30, 31, 18))
    # Q stores, unsigned immediate scaled by 16. Preserve all 128 bits, including
    # the upper halves of callee-saved v8-v15 (not preserved by the C ABI).
    for n in range(32):
        emit(0x3d800000 | ((10 + n) << 10) | (31 << 5) | n)
    string_adrp = len(words); emit(0); string_add = len(words); emit(0)
    emit(BL(here(), scan_bootstrap))
    no_extension = len(words); emit(0)
    emit(0xaa0003f0)  # mov x16,x0 (extension function)
    emit(LDRX(0, 26))
    emit(ADDI(0, 0, 0x28))
    emit(0xaa1c03e1)  # mov x1,x28 (engine query view)
    emit(BLR(16))
    emit(0xaa0003fc)  # mov x28,x0 (borrowed view, or original on refusal)
    restore = here()
    for n in range(32):
        emit(0x3dc00000 | ((10 + n) << 10) | (31 << 5) | n)
    emit(LDRX(30, 31, 18))
    for n in range(0, 18, 2):
        emit(LDP(n, n + 1, 31, n * 8))
    emit(ADDI(31, 31, 0x2a0))
    emit(int.from_bytes(bytes.fromhex(scan_source_original), 'little'))
    emit(B(here(), scan_source_site + 4))
    string = here()
    literal = scan_extension + bytes((-len(scan_extension)) % 4)
    for off in range(0, len(literal), 4):
        emit(int.from_bytes(literal[off:off + 4], 'little'))
    words[string_adrp] = ADRP(0, start + 4 * string_adrp, string)
    words[string_add] = ADDI(0, 0, string & 0xfff)
    # cbz x0,restore: absent extension retains the original size rejection.
    words[no_extension] = 0xb4000000 | (((restore - (start + 4 * no_extension)) // 4) << 5)
    return words


scene_control_extension = b'nxnativeMp4SceneSizeV1\0'
scene_control_abi = ((0x33147c, 'ed330129'),)  # stp w13,w12,[sp,#8]: input H,W before native DR


def scene_control_words(start, width, height):
    """A scene allocation choice with the native controller independently selectable.
    Keep every register/flag except the displaced ORR's x21 result. The host
    receives native dimensions, selected maximum and the native input extent.
    """
    words = []
    def emit(w): words.append(w)
    def here(): return start + 4 * len(words)
    emit(0xaa088135)  # original orr x21,x9,x8,lsl#32
    emit(SUBI(31, 31, 0x2b0))
    for n in range(0, 18, 2): emit(STP(n, n + 1, 31, n * 8))
    emit(STRX(30, 31, 18)); emit(STRX(21, 31, 20))
    emit(0xd53b4211); emit(STRX(17, 31, 19))  # NZCV in spare scalar spill
    for n in range(32): emit(0x3d800000 | ((11 + n) << 10) | (31 << 5) | n)
    emit(0xd2800000 | width << 5 | 21); emit(0xf2c00000 | height << 5 | 21)
    adr = len(words); emit(0); add = len(words); emit(0)
    emit(BL(here(), scan_bootstrap)); missing = len(words); emit(0)
    emit(0xaa0003f0)  # mov x16,x0
    emit(LDRX(0, 31, 20)); emit(0xaa1503e1)  # native packed; selected packed
    emit(0xb9400000 | ((0x2b0 + 12) // 4) << 10 | 31 << 5 | 2)
    emit(0xb9400000 | ((0x2b0 + 8) // 4) << 10 | 31 << 5 | 3)
    emit(0xaa038042)  # orr x2,x2,x3,lsl#32 (native full-resolution input)
    emit(BLR(16)); emit(0xaa0003f5)  # mov x21,x0
    restore = here()
    for n in range(32): emit(0x3dc00000 | ((11 + n) << 10) | (31 << 5) | n)
    emit(LDRX(30, 31, 18)); emit(LDRX(17, 31, 19)); emit(0xd51b4211)
    for n in range(0, 18, 2): emit(LDP(n, n + 1, 31, n * 8))
    emit(ADDI(31, 31, 0x2b0)); emit(B(here(), scene_site + 4))
    literal = here(); data = scene_control_extension + bytes((-len(scene_control_extension)) % 4)
    for off in range(0, len(data), 4): emit(int.from_bytes(data[off:off+4], 'little'))
    words[adr] = ADRP(0, start + 4 * adr, literal); words[add] = ADDI(0, 0, literal & 0xfff)
    words[missing] = 0xb4000000 | (((restore - (start + 4 * missing)) // 4) << 5)
    return words


# The window rectangle has its own sizing path, independent of scene/canvas allocations.
# a1d280 is called only at a1d1e8. It returns packed visible/backing extents in x0/x1;
# a1d1ec stores both, a1d1fc sizes the window and sets its crop, a1d214 publishes
# the visible extent at 1826938. Leaving it native retained a 1920x1080 viewport/crop.
window_extent_site = 0xa1d2d4
window_extent_original = '008109aa'  # orr x0,x8,x9,lsl#32
window_extent_abi = ((0xa1d1e8, '26000094'), (0xa1d1ec, 'e00700a9'),
                     (0xa1d1fc, '91000094'), (0xa1d214, '0b000094'),
                     (0xa1d2d8, 'c0035fd6'), (0xa1d268, '28fd9fc8'))


def window_extent_words(start, width, height):
    return [0xd2800000 | width << 5, 0xf2c00000 | height << 5,
            0xaa0003e1, B(start + 12, window_extent_site + 4)]


def window_extent_stub(c):
    c.site(window_extent_site, window_extent_original)
    for off, original in window_extent_abi: c.site(off, original)
    for name, w, h in full_scene_sizes:
        start = c.here()
        for word in window_extent_words(start, w, h): c.emit(word)
        c.runtime_exports['WINDOW_' + name] = start


# UICamera (type b35b0375) builds an orthographic descriptor at 916c48.
# Its only two direct callers are the component setup and the UI manager's
# default camera. This is not the world/scan perspective camera or a viewport.
# Expand both UI axes about their existing centre by 10/9 before the descriptor
# is copied to the component/manager and their inverse matrices are calculated.
# The final 16:10 crop then retains the complete 16:9 UI, with square pixels.
# Native near/far, camera transforms, world projection and letterbox settings
# stay untouched. Opted in only by the build-validated *_full_1610 manifests.
ui_camera_site = 0x916c70
ui_camera_original = 'c0035fd6'  # original builder RET
ui_camera_abi = (
    (0x916c48, '09f0a752000d00bd011100bd090900b949008052021500bd090100f9031900bd041d00bd052100bd'),
    (0x916bf8, '0005412d0309422d0415432d'),  # bounds s0-s3, near/far s4/s5
    (0x916c08, '10000094'),  # UICamera component -> descriptor builder
    (0x9d49f4, '9508fd97'),  # default UI camera -> same builder
    (0x916cf0, '34e02994'),  # descriptor -> manager camera+90
    (0x916b1c, '7c9fe397'),  # adjusted descriptor -> inverse projection
    (0xfc2ef4, 'a06e8052606bb672c0035fd6'),  # independently identified UICamera type
)


def ui_camera_words(start):
    """Postprocess only the four orthographic bounds; preserve all live SIMD
    bits, SP, NZCV and the builder's integer result (x9=2). Position independent.
    Paired bounds may have a nonzero centre: expand the span, not the origin.
    """
    return [
        SUBI(31, 31, 48), 0xad0007e0, 0x3d800be2,  # save q0/q1/q2
        0x3cc0c100, 0x4ea00801, 0x4e21d401,  # bounds; swap pairs; pair sums
        0x1e2c1002, 0x4f829021, 0x4ea1d400,  # .5; pair centres; relative bounds
        0x52871c89, 0x72a7f1c9, 0x1e270122,  # float32(10/9) in s2
        0x4f829000, 0x4e21d400, 0x3c80c100,  # scale; add centres; store bounds
        0x3dc00be2, 0xad4007e0, 0x52800049,  # restore q2/q0/q1; native x9=2
        ADDI(31, 31, 48), 0xd65f03c0,
    ]


def ui_camera_stub(c):
    c.site(ui_camera_site, ui_camera_original)
    for off, original in ui_camera_abi: c.site(off, original)
    start = c.here()
    for word in ui_camera_words(start): c.emit(word)
    c.runtime_exports['UI_CAMERA_FIT_1610'] = start


# Optional in-game HUD children, separate from the orthographic UI/menu camera.
# Entity factory 231100 builds a 0x4c-byte transform record from typed properties;
# 231240 passes it to 23128c. Preserve template roots and every non-HUD entity.
hud_layout_site = 0x231240
hud_layout_original = 'e2130091'  # add x2,sp,#4: native transform record
hud_layout_extension = b'nxnativeMp4HudLayoutV1\0'
hud_layout_abi = (
    (0x23111c, '142c40f9'),  # typed properties [initialization descriptor+58]
    (0x2311f4, '839e432d'),  # position properties +1c/+20
    (0x2311b4, '884243f8'),  # independent object scale at properties+34
    (0x231244, 'e00314aae10313aa10000094'),  # object/descriptor/transform -> ctor
    (0x231330, 'a4764594'),  # copy the complete 0x4c-byte native record
    (0x2313d0, '7c764594883a40b9891a40f9c89a00b9'),  # local matrix then separate scale
    (0x11f0e0, '88c200f8894201f8'),  # serialized template GUID -> properties+c/+14
    (0x11f2c4, '28d1288be92b41a9092900a9'),  # 16-byte component GUID vector
    (0x1823eb8, '0011230000000000'),  # registered Entity factory
)


def hud_layout_words(start):
    """Borrow a stack-local entity transform, preserving all registers/flags.
    Missing extension retains native construction. No shader or asset rewrite.
    """
    words = [int.from_bytes(bytes.fromhex(hud_layout_original), 'little')]
    def emit(w): words.append(w)
    def here(): return start + 4*len(words)
    emit(SUBI(31,31,0x2b0))
    for n in range(0,18,2): emit(STP(n,n+1,31,n*8))
    emit(STRX(30,31,18));emit(0xd53b4211);emit(STRX(17,31,19))
    for n in range(32): emit(0x3d800000 | (11+n)<<10 | 31<<5 | n)
    adr=len(words);emit(0);add=len(words);emit(0)
    emit(BL(here(),scan_bootstrap));missing=len(words);emit(0)
    emit(0xaa0003f0)  # mov x16,x0
    emit(LDRX(0,19,11))  # properties = [native initialization descriptor+58]
    emit(LDRX(1,31,2))  # saved x2: caller's original stack-local record
    emit(BLR(16))
    restore=here()
    for n in range(32): emit(0x3dc00000 | (11+n)<<10 | 31<<5 | n)
    emit(LDRX(30,31,18));emit(LDRX(17,31,19));emit(0xd51b4211)
    for n in range(0,18,2): emit(LDP(n,n+1,31,n*8))
    emit(ADDI(31,31,0x2b0));emit(B(here(),hud_layout_site+4))
    literal=here();data=hud_layout_extension+bytes((-len(hud_layout_extension))%4)
    for off in range(0,len(data),4): emit(int.from_bytes(data[off:off+4],'little'))
    words[adr]=ADRP(0,start+4*adr,literal);words[add]=ADDI(0,0,literal&0xfff)
    words[missing]=0xb4000000 | (((restore-(start+4*missing))//4)<<5)
    return words


def hud_layout_stub(c):
    c.site(hud_layout_site,hud_layout_original)
    for off,original in hud_layout_abi: c.site(off,original)
    start=c.here()
    for word in hud_layout_words(start): c.emit(word)
    c.runtime_exports['HUD_LAYOUT_V1']=start


# Pause-menu camera only. The orthographic UICamera and the perspective Camera
# component are separate native contracts, on the same original owner template.
# The Camera initializer copies near/far/FOV/aspect to a stack-local parameter
# record, then both constructor paths copy it to the new camera at +1e4.
pause_ui_sites = ((0x916c0c, '60320291'), (0x3fd4c, 'ffb30c39'))
pause_ui_abi = (
    (0x916bf0, '681a40f9'), (0x916bf8, '0005412d0309422d0415432d'),
    (0x916c08, '10000094'), (0x916c18, '6ae02994'),
    (0x916a9c, '601240f9'),  # component+20 owner
    (0x231318, '292c40f9091800f9'),  # Entity+30 properties
    (0x3fab0, 'fd830091'), (0x3fb1c, 'f50300aa'),  # original FP / Camera component x21
    (0x3fd04, 'bb1a40f9'), (0x3fd38, '6a2f41a9'),  # typed Camera properties+10
    (0x3fd48, 'aaaf31a9'), (0x3fd50, 'a8c31238'),  # parameter copy before borrowed record
    (0x3ffcc, 'a3a303d1e5e30b91e6630591e7230091e00317aae40318aa9ad81594'),
    (0x4007c, 'a3a303d1e5e30b91e6630591e7230091e00317aae40318aa3cd91594'),
    (0x5b66bc, '889207910b1340b9092b40a98bf601b9092900a9081740b9'),
    (0xfbe3b8, '20d18e52a0fca972c0035fd6'),  # Camera type 4fe57689, not UICamera b35b0375
)
# V2 borrows a constructor record, so it must never resolve an old frame-packet ABI.
pause_panel_site = (0x101fc14, '68ee4b39')
pause_panel_abi = ((0x101fba8, '691240f9686240f9291d40f9080140f9'),
                   (0x101fbfc, '03110c2d05190d2d07410e2d11490f2d0005102d0251112d'),
                   (0x101fe64, '205e95528015a772c0035fd6'))
pause_ui_extension = b'nxnativeMp4PauseUiV2\0'


def pause_ui_words(start, kind):
    """Borrow the 2D descriptor or perspective constructor parameter record.
    Every register/flag survives the host helper; missing extension is native.
    """
    assert kind in ('bounds', 'fov', 'panel')
    words=[]
    def emit(w): words.append(w)
    def here(): return start+4*len(words)
    if kind == 'fov': emit(0x390cb3ff)  # original STRB wzr,[sp,#32c], before changing SP
    emit(SUBI(31,31,0x2b0))
    for n in range(0,18,2): emit(STP(n,n+1,31,n*8))
    emit(STRX(30,31,18));emit(0xd53b4211);emit(STRX(17,31,19))
    for n in range(32): emit(0x3d800000 | (11+n)<<10 | 31<<5 | n)
    adr=len(words);emit(0);add=len(words);emit(0)
    emit(BL(here(),scan_bootstrap));missing=len(words);emit(0)
    emit(0xaa0003f0);emit(0xaa1503e0 if kind=='fov' else 0xaa1303e0)  # x16=helper, x0=component
    emit(SUBI(1,29,0xe8) if kind=='fov' else LDRX(1,31,8))  # original FP local / saved x8 descriptor
    emit(0x52800042 if kind=='panel' else 0x52800022 if kind=='fov' else 0x52800002)  # event=2 read-only panel / 1 perspective constructor / 0 bounds
    emit(BLR(16));restore=here()
    for n in range(32): emit(0x3dc00000 | (11+n)<<10 | 31<<5 | n)
    emit(LDRX(30,31,18));emit(LDRX(17,31,19));emit(0xd51b4211)
    for n in range(0,18,2): emit(LDP(n,n+1,31,n*8))
    emit(ADDI(31,31,0x2b0))
    if kind=='panel':
        emit(0x394bee68);emit(B(here(),0x101fc18))  # original TextPane load, x16 is dead
    elif kind=='fov':
        emit(B(here(),0x3fd50))
    else:
        emit(0x91023260);emit(B(here(),0x916c10))
    literal=here();data=pause_ui_extension+bytes((-len(pause_ui_extension))%4)
    for off in range(0,len(data),4):emit(int.from_bytes(data[off:off+4],'little'))
    words[adr]=ADRP(0,start+4*adr,literal);words[add]=ADDI(0,0,literal&0xfff)
    words[missing]=0xb4000000 | (((restore-(start+4*missing))//4)<<5)
    return words


def pause_ui_stub(c):
    for off,original in pause_ui_sites+pause_ui_abi+(pause_panel_site,)+pause_panel_abi: c.site(off,original)
    for kind in ('bounds','fov','panel'):
        start=c.here()
        for word in pause_ui_words(start,kind):c.emit(word)
        c.runtime_exports['PAUSE_UI_'+kind.upper()]=start


# All v131072 GUI visual serializers using the same base 3x4 packet contract.
# Camera nodes use a different +4 / +34 contract and are deliberately absent.
menu_visual_copies = (0x81b88,0x2317f4,0x306bd0,0xba2bac,0xcca4bc,
                     0xd17714,0xdb969c,0xe120a4,0xf541d4,0xfd5eb8,
                     0x101d940,0x115466c,0x12f3850)
menu_visual_sites = tuple((off+4,'685a4239') for off in menu_visual_copies)
menu_visual_destroy = (0xc2c3a4,'fd7bbea9')
# Bracket native TextPane / Model creation. The calls themselves stay native.
# Every GUI base constructor/destructor registers/releases node provenance.
menu_visual_scope_sites = (
    (0x18b688,'e80300b9',2,'component'), (0x18b690,'686240f9',3,'component'),
    (0x100747c,'080d40f9',2,'component'), (0x1007484,'60a640f9',3,'component'),
    (0x2b6384,'23610091',2,'component'), (0x2b638c,'60a640f9',3,'component'),
    (0x1c099c,'081400f9',4,'node'), (0x15577c,'fd7bbea9',5,'original'),
)
# The same visual-base contract is also copied inline (three LDP/STP pairs).
# Omitting these copies leaves model bars/icons at native scale beside fitted text.
menu_visual_inline = (
    (0xb87970,'695a4239',0,'inline'),(0xb8b9b8,'695a4239',0,'inline'),
    (0xc08364,'695a4239',0,'inline'),(0xcc9cb8,'695a4239',0,'inline'),
    (0xdb9acc,'09584239',0,'inline_original'),
)
# Lazy nine-slice creation also runs outside TextPane/Model initialization.
# Both callers retain the original Model component: x4 / x1 respectively.
# 155824 unlinks GUI nodes even when their derived destructor inlines the base.
menu_visual_lazy_sites = (
    (0x155824,'fd7bbea9',5,'original'),
    (0x1217d8,'e213052d',2,'saved4'), (0x1217e0,'f50300aa',3,'original'),
    (0x69a844,'75420391',2,'saved1'), (0x69a860,'a0031ff8',3,'original'),
    # Map-only orthographic compass models copy a literal projection, bypassing
    # UICamera. Borrow each fresh 64-byte projection before native model drawing.
    (0x544ec,'e00318aa',6,'compass'), (0x54694,'e00318aa',6,'compass'),
)
menu_visual_extension = b'nxnativeMp4MenuVisualV2\0'
def menu_visual_hooks():
    return tuple((off,h,0,'node') for off,h in menu_visual_sites) + ((menu_visual_destroy[0],menu_visual_destroy[1],1,'original'),) + menu_visual_scope_sites + menu_visual_inline + menu_visual_lazy_sites
def menu_visual_abi():
    return tuple((off-16,'742a40f9618201910206805280220091'+
                  BL(off,0x138edc0).to_bytes(4,'little').hex()+'685a4239')
                 for off in menu_visual_copies) + tuple(
        (off-28,('082840f9092846a909a900a9092847a90b3048a90bb102a909a901a909584239' if argument=='inline_original' else
                  '682a40f9692a46a909a900a9692a47a96b3248a90bb102a909a901a9695a4239'))
        for off,_,_,argument in menu_visual_inline) + (
        (0x1c0998,'280040f9081400f9280440f9081800f9'), # node frame/control reference
        (0x18b5f4,'681240f9fb0315aa084940f915410291'), # TextPane owner room+90 frame
        (0x916ccc,'02068052011d40f9089040f900810191'), # UICamera manager node transform
        (0xc2c3a4,'fd7bbea9f30b00f9fd030091f30300aa'), # camera destruction before owner release
        (0x18b68c,'5e010094686240f9'), # TextPane call and continuation
        (0x18b7c8,'880a40b91f05007120f1ff54'), # shared GUI context branch
        (0x1007450,'681a40f903610091085540b91f050071'), # Model projection property +54
        (0x1007474,'e10314aae20313aa080d40f900013fd660a640f9'), # Model component x19 / native virtual call
        (0x2b6374,'691a40f9e20313aae40317aa081140f92361009100013fd660a640f9'), # Model init virtual call
        (0x15577c,'fd7bbea9f30b00f9fd03009168b100f008c10191'), # common GUI base destruction
        (0x676328,'080540b9a80202b9'), # projection enum to TextPane properties+200
        (0x13d3444,'85001390010000006d766d98020000007bb5eef600000000'),
        (0x231318,'292c40f9091800f9'), # Entity properties pointer
        (0x121588,'e80300aa2a0040f9e40303aa090d4ff8e30302aa'), # native Model component x4
        (0x12163c,'881040f94b114139'), # Model+20 owner / style properties
        (0x1217d8,'e213052d9e000094f50300aa'), # nine-slice native call
        (0x69a83c,'201040f9e8030091754203915c000094e2030091e00315aae10314aae3031f2a7e1cea97a0031ff8'),
        (0x155824,'fd7bbea9f30b00f9fd030091f30300aa001440f9600200b4'), # unlink native GUI frame
        (0x54438,'619d00f021501c91'), (0x545e0,'619d00f021501c91'), # literal compass projection
        (0x544d8,'02088052ec0700f9fa0301aaec1300f936ea4c94e00318aa'),
        (0x54680,'02088052ec0700f9fa0301aaec1300f9cce94c94e00318aa'),
        (0x1403714,'0000003e00000000'
                   '0000000000000000'
                   '00000000398e633e'
                   '0000000000000080'
                   '0000000000000000'
                   '000080b900000080'
                   '0000000000000000'
                   '000000000000803f'),


    )

def menu_visual_words(start,index):
    off,original,event,argument=menu_visual_hooks()[index]
    words=[]
    def emit(w):words.append(w)
    def here():return start+len(words)*4
    emit(SUBI(31,31,0x2b0))
    for n in range(0,18,2):emit(STP(n,n+1,31,n*8))
    emit(STRX(30,31,18));emit(0xd53b4211);emit(STRX(17,31,19))
    for n in range(32):emit(0x3d800000 | (11+n)<<10 | 31<<5 | n)
    adr=len(words);emit(0);add=len(words);emit(0)
    emit(BL(here(),scan_bootstrap));missing=len(words);emit(0)
    emit(0xaa0003f0)
    emit(0xaa1703e0 if argument=='compass' else LDRX(0,31,int(argument[5:])) if argument.startswith('saved') else LDRX(0,31,0) if argument in ('original','inline_original') else 0xaa1303e0) # original x0 / live x19
    emit(SUBI(1,24,0x40) if event==6 else (LDRX(1,31,8) if argument.startswith('inline') else 0xaa1403e1) if event==0 else 0xaa1f03e1) # saved x8 inline packet / x20 memcpy packet / null
    emit(0x52800002 | event<<5)
    emit(BLR(16));restore=here()
    for n in range(32):emit(0x3dc00000 | (11+n)<<10 | 31<<5 | n)
    emit(LDRX(30,31,18));emit(LDRX(17,31,19));emit(0xd51b4211)
    for n in range(0,18,2):emit(LDP(n,n+1,31,n*8))
    emit(ADDI(31,31,0x2b0));emit(int.from_bytes(bytes.fromhex(original),'little'))
    emit(B(here(),off+4))
    literal=here();data=menu_visual_extension+bytes((-len(menu_visual_extension))%4)
    for i in range(0,len(data),4):emit(int.from_bytes(data[i:i+4],'little'))
    words[adr]=ADRP(0,start+4*adr,literal);words[add]=ADDI(0,0,literal&0xfff)
    words[missing]=0xb4000000 | (((restore-(start+4*missing))//4)<<5)
    return words

def menu_visual_stubs(c):
    for off,h in tuple((a,h) for a,h,_,_ in menu_visual_hooks())+menu_visual_abi():c.site(off,h)
    for index in range(len(menu_visual_hooks())):
        start=c.here()
        for word in menu_visual_words(start,index):c.emit(word)
        c.runtime_exports['MENU_VISUAL_'+str(index)]=start


def scan_source_stub(c):
    resolution_stub(c)
    for off, original in scene_control_abi: c.site(off, original)
    for name, w, h in full_scene_sizes:
        start = c.here()
        for word in scene_control_words(start, w, h): c.emit(word)
        c.runtime_exports[name] = start
    for off, original in scan_source_abi:
        c.site(off, original)
    start = c.here()
    for word in scan_source_words(start):
        c.emit(word)
    c.runtime_exports['SCAN_SOURCE_V1'] = start
    window_extent_stub(c)
    ui_camera_stub(c)
    hud_layout_stub(c)
    pause_ui_stub(c)
    menu_visual_stubs(c)
    shadow_stub(c)
    lod_distance_stub(c)
    fog_scale_stub(c)


# Shadow resolution (runtime_patches.py shadow_writes, DM_SHADOW_RESOLUTION=2x). The frame's shadow code (main+0x137a63c) gives each shadow-casting
# light a square tile of 1 << floor(log2(its requested size)) (w28 = 1 at 0x137a6f0, lsl at 0x137a768), packs the tiles into a 2048 atlas (0x137a82c,
# 0x137a840, 0x137a850), sets each tile as the viewport (0x137ad1c, found with DM_NVN_SHADOW_TRACE) and turns pixel rects into texture coordinates with
# the reciprocal of a (2048.0, 1.0) pair loaded at 0x137abf4. This stub loads (4096.0, 1.0) instead; x8 is free there (0x137abf8 sets it again).
shadow_uv_site = 0x137abf4


def shadow_uv_words(start, size_hi16=0x4580):
    """(size, 1.0) in d0: size_hi16 is the float's upper half, 0x4580 for 4096.0, 0x4600 for 8192.0."""
    return [0xd2a00000 | size_hi16 << 5 | 8,   # movz x8,#size_hi16,lsl #16
            0xf2e7f008,   # movk x8,#0x3f80,lsl #48  (1.0f in the upper half)
            0x9e670100,   # fmov d0,x8
            B(start + 12, shadow_uv_site + 4)]


shadow_uv_stubs = (('SHADOW_UV4096', 0x4580), ('SHADOW_UV8192', 0x4600))   # runtime_patches.py's shadow choices 2x and 4x


def shadow_stub(c):
    c.site(shadow_uv_site, '009942fd')   # ldr d0,[x8,#0x530]
    for name, hi in shadow_uv_stubs:
        start = c.here()
        for word in shadow_uv_words(start, hi): c.emit(word)
        c.runtime_exports[name] = start


# LOD distance (runtime_patches.py lod_writes, DM_MODEL_LOD=x2 / x4). The visibility job (main+0x13a24c) scales each instance's offset from the view's camera
# by the view's factor [view+0x3c8] (loaded at 0x13a268) and compares the result with the model's level thresholds; an offset past all of them takes the
# far branch (0x13a4d4). These stubs multiply that factor by 0.5 or 0.25, so every threshold lies 2 or 4 times as far away. s2 is free there: every path
# writes it before reading it (0x13a38c, 0x13a3a8, 0x13a3c4, 0x13a440).
lod_distance_site = 0x13a268
lod_distance_stubs = (('LOD_DISTANCE_X2', 0x1e2c1002), ('LOD_DISTANCE_X4', 0x1e2a1002))   # fmov s2,#0.5 / #0.25


def lod_distance_words(start, fmov):
    return [0xbd43c920,   # ldr s0,[x9,#0x3c8]  (the displaced load)
            fmov,
            0x1e220800,   # fmul s0,s0,s2
            B(start + 12, lod_distance_site + 4)]


def lod_distance_stub(c):
    c.site(lod_distance_site, '20c943bd')
    for name, fmov in lod_distance_stubs:
        start = c.here()
        for word in lod_distance_words(start, fmov): c.emit(word)
        c.runtime_exports[name] = start


# Volumetric fog density (runtime_patches.py fog_writes, DM_FOG_QUALITY=high). The fog effect's node (method at main+0xf54498) passes its grid scale
# [node+0x1a0] (loaded at 0xf54720) to the sizing function 0x185428: a froxel grid of round(scale * ceil(w / 16)) (aligned to 4) by
# round(scale * ceil(h / 16)) tiles and 64 * scale depth slices, and a target of 8 pixels per tile (1600x904 at 1800p with scale 1, lsl #3 at 0x185574).
# This stub doubles the scale: tiles of 8 pixels, 128 slices, the target at the scene's full size. s1 is free there: the call at 0xf5473c takes s0
# only, and the callee writes s1 before reading it (0x1854ec).
fog_scale_site = 0xf54720
fog_scale_stubs = (('FOG_SCALE_X2', 0x1e201001),)   # fmov s1,#2.0


def fog_scale_words(start, fmov):
    return [0xbd41a280,   # ldr s0,[x20,#0x1a0]  (the displaced load)
            fmov,
            0x1e210800,   # fmul s0,s0,s1
            B(start + 12, fog_scale_site + 4)]


def fog_scale_stub(c):
    c.site(fog_scale_site, '80a241bd')
    for name, fmov in fog_scale_stubs:
        start = c.here()
        for word in fog_scale_words(start, fmov): c.emit(word)
        c.runtime_exports[name] = start


STUBS = {build_id: scan_source_stub}

BINDING = [
    {'what': 'optional pause-room GUI render-copy fit with native creation provenance and lifetime', 'now': 'build', 'goal': 'build', 'purpose': 'rendering',
     'why': 'deliberate adjustment of this authored 16:9 UI for the requested 16:10 crop; exact native serializers, room-frame ABI and pause template are build checked',
     'checked': 'original v131072 enum/creation disassembly, isolated provenance policy and native ARM64 bridges; user-confirmed inventory, compass and background fit in the shown 16:10 menus'},
    {'what': '1800p/2160p native-size scan-query bridge (inactive by default)', 'now': 'build', 'goal': 'build', 'purpose': 'rendering',
     'why': 'the borrowed engine-view ABI and call site belong to main 1.1.0; the full original build and every ABI instruction are checked',
     'checked': 'original v1.1.0, live 3200x1800 size-guard rejection and synthetic GPU resampling/readback; 1800p scanning user-confirmed; 2160p gameplay confirmation pending'},
    {'what': 'scene stubs and experimental 1080p-to-5K full-output choices (inactive by default)', 'now': 'build', 'goal': 'unreviewed', 'purpose': 'rendering',
     'why': 'main 1.1.0 immediate scene/canvas/display dimensions require validated runtime writes; full-output gameplay and other builds remain open'},
]

# Read-only scan diagnostics, independently located through the native script bindings in the original NSO.
# Full NSO identity and the instruction at each point are checked by scan_diag.py, including in the rewritten main.
# No PC-relative instruction; x16 is dead at these entries / after calls, and on the short paths to the next call.
# label, instruction offset, original bytes, @ register (None = every call), field offset, optional scalar register.
scan_build_id = '8275fbe1ec5191c279f3a943ad8524f4bab087a3000000000000000000000000'
scan_diagnostics = {
    scan_build_id: (
        ('target-handle', 0xcb2ca4, '886641b9', 20, 0x164, None),
        ('target-resolved', 0xcb2cb8, 'f44f42a9', 0, 0x2b0, None),
        ('target-range', 0xcb2de0, '08b04579', 0, 0x2d8, None),
        ('scanning-target-state', 0x1213350, '08b042b9', 0, 0x2b0, None),
        ('scanning-script-gate', 0x1213358, '68d243b9', 19, 0x3d0, None),
        ('begin-request', 0x5d9450, 'ff4302d1', None, 0, None),
        ('begin-target', 0x5d9560, 'e10313aa', 0, 0x2d8, None),
        ('begin-range-gate', 0x5d97f0, '68b20579', 19, 0x2d8, None),
        ('progress-call', 0x699d4, 'ff0302d1', 0, 0x2b0, 0),
        ('progress-manager', 0x69a10, '0041201e', 20, 0x2b0, 8),
        ('progress-return', 0x69a64, '88724039', 20, 0x2b0, 8),
    ),
}

# Acquisition upstream of BeginScan. Same read-only probe ABI as scan_diagnostics.
# @/^ polls are limited to one printed observation per 30 presented frames; every BeginScan entry is kept.
scan_acquisition_diagnostics = {
    scan_build_id: (
        *[p for p in scan_diagnostics[scan_build_id] if p[0] in
          ('target-handle', 'target-resolved', 'begin-request', 'begin-target', 'progress-call')],
        ('acquire-input', 0x402924, '0a8040b9', 0, 0x80, None),
        ('acquire-context-state', 0x402934, '09d14439', 8, 0x130, None),
        ('acquire-context-pointer', 0x402938, '08c940f9', 8, 0x190, None),
        ('acquire-eligible', 0x40294c, '08b840b9', 0, 0x80, None),
        ('spatial-candidates', 0x402b50, 'e0031baa', 22, 0x80, None),
        ('project-depth', 0x40342c, '4020281e', 28, 0x18, None),
        ('project-bounds', 0x4034a0, '40212c1e', 28, 0x18, None),
        ('project-accepted', 0x4034c4, 'e93f40f9', 28, 0x18, None),
        ('raw-candidates', 0x221ed8, '97de43f9', 20, 0x218, None),
        ('filtered-candidates', 0x21ea8c, 'e93340f9', 19, 0x218, None),
        ('selected-candidate', 0x222b2c, 'ff0304d1', 0, 0x120, None),
    ),
}

# Renderer query size and CPU counter lifecycle, original v1.1.0 only.
# x16 is dead at these non-PC-relative sites, between calls/after the ring-copy loop.
scan_query_diagnostics = {
    scan_build_id: (
        *[p for p in scan_acquisition_diagnostics[scan_build_id] if p[0] in
          ('target-handle', 'begin-request', 'progress-call', 'raw-candidates', 'filtered-candidates')],
        ('counter-binding-slot', 0xd325e8, '097992b9', 8, 0x1270, None),
        ('counter-binding-resource', 0xd33144, '1f0b0031', 8, 0x0, None),
        # Before the runtime branch: probes relocate only non-PC-relative instructions.
        ('query-source-size', 0x336a6c, 'e8e35ef9', 28, 0x14, None),
        ('query-size-accepted', 0x336ab0, 'e0364bbd', 28, 0x14, None),
        ('query-copy-request', 0x336c5c, '7f0a00b9', 28, 0x14, None),
        ('counter-copy-slot0', 0xf747f0, '88be8052', 1, 0x0, None),
        ('counter-copy-slot1', 0xf748f4, '88be8052', 1, 0x0, None),
        ('counter-copy-slot2', 0xf749f8, '88be8052', 1, 0x0, None),
        ('counter-before-clear', 0xf74b68, 'e1031f2a', 0, 0x0, None),
        ('counter-after-clear', 0xf74b74, 'e81340f9', 0, 0x0, None),
        ('counter-consumed', 0x11fdd6c, '8c421491', 11, 0x4f0, None),
    ),
}

# Read-only vibration observations, independently identified in the legal v1.1.0 main.
# No guest instruction is changed in the packaged module; the optional Mac probe
# trampolines validate the full build, rewritten layout and original words first.
# label, site, original instruction, optional memory-base register, field offset.
rumble_diagnostics = {
    scan_build_id: (
        ('graph-handle-count', 0xaab7ac, '1f040071', None, 0),
        ('source-enable-flags', 0x2815dc, '08ad4f39', 8, 0x3e8),
        ('source-curve-left', 0x281878, 'e10313aa', 4, 0),
        ('request-left', 0xa4e00, '3f200071', 2, 0),
        ('logical-pad-map', 0xa4e08, 'fd7bbea9', 0, 0x28),
        ('effect-file-request', 0xd350b8, '3f200071', None, 0),
    ),
}


# Read-only HUD layout observations; all sites are non-PC-relative and x16 is
# dead at entry or after the preceding native call. Native UICamera uses an
# orthographic manager only for context >1 (6bb8a0/916c74); its game HUD callback
# fc2e20 instead copies its transform and the active perspective FOV in degrees.
# Capture both paths before choosing a per-widget top/bottom anchor transform.
hud_layout_diagnostics = {
    scan_build_id: (
        ('hud-view-before-copy', 0xfc2e20, '08704039', 2, 0xa0, None),
        ('hud-fov-context', 0xfc2e74, '603201bd', 20, 8, 0),
        ('orthographic-ui-bounds', 0x916c0c, '60320291', 8, 0xc, None),
        ('composition-camera', 0x12df9c, '42630091', 26, 0x48, None),
        ('composition-ortho-bounds', 0x12dfa4, '413740bd', 26, 0x54, None),
    ),
}
