# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own builds (dm/tools/patch_nso.py, "Registered patches").
#
# Ultrawide (opt-in at build time): DM_ULTRAWIDE=<width>x<height> (the screen, e.g. 3440x1440 or 2560x1080) makes the game render that shape natively, with a wider view,
# not a stretch. The game sizes everything from its 1440-high docked output, so the widths become 1440 x the screen's ratio (3440 for 21:9, a multiple of 16); the 3D view
# takes its aspect from the render size, so the field of view widens and nothing is distorted; the HUD stays at its 16:9 place in the middle. The host shows the game's
# crop (WindowCrop) scaled to the window, so with DM_FULLSCREEN=1 (and DM_SCREEN=<name> on a second screen) it fills the screen. Found on a 3440x1440 screen (Mac, M4 Pro):
#   main+0x1202ee0  the graphics setup config's output size (stur at [x21+0x14], read back as the swap chain size at main+0x346c24c): 2560 x 1440
#   main+0x31f39f4  the render size config, docked 2560 x 1440 and handheld (main+0x31f3a00) 1920 x 1080
#   main+0xecbc80   2560.0f beside 1440.0f in the same object's init (its float copy of the output size)
#   main+0x3f58540  the output resolution table (640x480 ... 3840x2160, read at main+0x112c600 by the display mode): each width from its height
# Without it the swap chain is 2560x1440 and the game's crop 16:9 (2048x1152 at its usual 80 % dynamic resolution); with 3440x1440 the swap chain is 3440x1440 and the crop
# 2752x1152 (2.389:1). Movies and the opening title stay 16:9 in the middle of the wider picture.
import os, struct

CUTS = {}
_uw = os.environ.get('DM_ULTRAWIDE', '')
if _uw:
    _w, _h = (int(v) for v in _uw.lower().split('x'))
    _ratio = _w / _h
    def _width(h): return int(round(h * _ratio / 16)) * 16
    def _movz(imm, rd, x64): return struct.pack('<I', (0xD2800000 if x64 else 0x52800000) | imm << 5 | rd).hex()
    _table = [640, 480, 720, 480, 1280, 720, 1600, 900, 1920, 1080, 2560, 1440, 3840, 2160]
    _wide = [v if i % 2 else _width(_table[i + 1]) for i, v in enumerate(_table)]
    _f = struct.unpack('<I', struct.pack('<f', float(_width(1440))))[0] >> 16
    CUTS['e0f3ccfaf50065ae4ea01eb5aeb13b1a'] = [
        (0x1202ee0, _movz(2560, 8, True), _movz(_width(1440), 8, True), 'ultrawide: graphics setup output width 2560 → %d (DM_ULTRAWIDE)' % _width(1440)),
        (0x31f39f4, _movz(2560, 8, True), _movz(_width(1440), 8, True), 'ultrawide: docked render width 2560 → %d' % _width(1440)),
        (0x31f3a00, _movz(1920, 8, True), _movz(_width(1080), 8, True), 'ultrawide: handheld render width 1920 → %d' % _width(1080)),
        (0xecbc80, struct.pack('<I', 0x52A8A409).hex(), struct.pack('<I', 0x52A00009 | _f << 5).hex(), 'ultrawide: output width 2560.0f → %d.0f' % _width(1440)),
        (0x3f58540, struct.pack('<14I', *_table).hex(), struct.pack('<14I', *_wide).hex(), 'ultrawide: output resolution table widths from their heights'),
    ]

BINDING = [
    {'what': 'ultrawide output and render sizes (opt-in, DM_ULTRAWIDE)', 'now': 'build', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own size constants; a function-matched rule could carry them over the game's updates"},
]
