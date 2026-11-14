# Donkey Kong Bananza (040074A01BF12000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # Donkey Kong Bananza v458752 main: the frame-rate controller keeps present interval 1 (60 fps).
  # The controller (main+0x204a18, called from main+0x203718 with the display object's [+0x218] settings) turns the frame's measured time into vsyncs (ticks / frequency * 60 + margin),
  # takes that plus one as its candidate interval, clamps it to the settings' minimum [+0] and maximum [+4] (1 and 3 here), and steps the present interval [+0xe0] up after 2 late frames
  # and down after 6 under. It measures from the frame's start to the later of its submission and its GPU end: on the Mac the host's translation and the GPU run one after the other there
  # (about 27 ms in the close-ups of the second world), so it chose 2 (30 fps) whenever a scene got busy and stayed there, although the Mac shows such frames at 50-60 fps.
  # The cut makes the candidate the minimum: csinc w9,w11,w9,ge → mov w9,w8, so the controller only ever steps down to 1. A late frame is shown when it is ready (55 fps, not 30).
  # The fixed-interval path (main+0x204a50, the settings' [+0x50] when [+0x58] is set) is not touched
  'e25c90c1c136638885659fd55bf8a03b': [
    (0x204abc, '69a5891a', 'e903082a', 'frame-rate controller: candidate interval = the settings\' minimum (csinc w9,w11,w9,ge → mov w9,w8): present interval 1 in busy scenes'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'frame-rate controller held at present interval 1', 'now': 'build', 'goal': 'unreviewed', 'purpose': 'frame rate',
     'why': "the controller is the game's own code (EPD's engine); whether other EPD Switch 2 games share it is not checked"},
]
