# Taiko no Tatsujin: Rhythm Festival, US release (01006B0016AD6000): the rewriter's patches of its build. The screen shape code is the JP profile's (0100BCA0135A0000/patches.py:
# one module, a table of addresses per build, BUILDS); this file only registers the US 6.2.0 main (v2031616) with its table. The C blobs (top_lift, bg_cover) are position
# independent and shared. Every site checks its original word, so an address of the table that is wrong stops the rewrite instead of patching the wrong code.
import importlib.util, os

_spec = importlib.util.spec_from_file_location('patches_taiko_shared', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '0100BCA0135A0000', 'patches.py'))
_shared = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(_shared)

FRAME_STEP = {
    _shared.US: lambda ctx: _shared.screen_shape(ctx, _shared.BUILDS[_shared.US]),   # 6.2.0 (v2031616) main
}

BINDING = _shared.BINDING
