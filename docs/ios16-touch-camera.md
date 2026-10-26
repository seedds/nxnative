# Touch camera after keyboard disconnection

The input defect was in touch ownership, not trackpad gesture recognition. Z-A's
profile starts with the Simple layout (`DM_GAME_PAD_LAYOUT=0`); after removing a
keyboard, Simple returned on screen but only the Full/3D layout assigned empty
right-side touches to the camera. Simple instead delivered those drags to the
game's touchscreen. The trackpad recognizers already excluded direct fingers.

Both visible control layouts now allow camera dragging in empty space in the
right half. The right stick remains invisible. Touch-down still checks locked
buttons, the layout selector, the face-button area, the D-pad and left stick
before claiming a camera touch. Its ownership then stays fixed until release or
cancellation. A camera finger crossing a face button does not press that skill;
a finger starting on ABXY remains a button finger. Left movement and right camera fingers keep their
separate existing stick slots and can operate simultaneously.

Releasing or cancelling the camera finger clears its slot and recomputes neutral
right-stick axes without releasing other fingers. Focus loss immediately
publishes neutral touch, keyboard, mouse and physical-controller state, then
clears UIKit ownership on the main queue. A focus gate also ignores subsequent
global controller/mouse events while the scene is inactive or in the background;
becoming active clears stale state before new input is used. This matters when
the game remains visible beside an active Stage Manager VM. Input-device notifications read their
inventory and change overlay ownership on the main queue; an unavailable
`keyboardInput` no longer counts as a usable keyboard solely because a coalesced
keyboard object remains. Reconnecting physical controls hides the overlay and
releases old touch ownership. Existing raw mouse, hover and scroll camera input
is preserved. Tap layout still sends its blank-area fingers to the game.

This changes the previously unused right-side blank area of Simple layout into
camera input. Touchscreen-focused games should use Tap layout for unrestricted
game touches. There is no new gesture recognizer and no visible right-stick UI.

Compilation checks use the actual UIKit/GameController source at iOS 16.1:

```sh
NX_COMPILE_GAME=0100F43008C44000 python3 scripts/check_ios17_compile.py DmGamepad.m
```

The existing owner-based touch handling supplies release/cancellation and
multi-touch behavior; no duplicate model test claims to validate UIKit delivery.
Device validation must cover keyboard detach/reconnect, holding left movement
while dragging the blank right half, a third ABXY/skill finger, dragging across
buttons without stealing them, releasing/cancelling one finger while keeping
another, focus loss and resumption, Tap passthrough, and preserved trackpad
motion with the overlay hidden. Cross-compilation does not establish those
on-device interaction results.

## Persistent virtual mice on the new host

On the physical M2 iPad, an independent GameController probe found usable mice
named `V-UC Automouse` and `V-Apple Internal Keyboard / Trackpad` after the user
detached physical inputs. A fresh game launch still received mouse connections,
so restarting the game did not restore the overlay. This is a real inventory
entry, not evidence of a connected physical keyboard cover.

The input policy now lets a direct finger select touch controls while only
mice remain connected. Controllers and usable keyboards retain priority. Mouse
movement selects pointer input again once all direct fingers have released;
it cannot steal a held movement, camera or skill finger. Raw motion, hover and
scroll use that same policy. Inventory notifications reset ownership only when
the overlay's eligibility actually changes. Queued raw events carry a focus
epoch and cannot cross a focus reset into a resumed scene.

`DM_TOUCH_DIAG=1` logs up to 256 actual UIKit ownership/axis transitions. It
records simultaneous left/right ownership and neutral release, and does not
replace a real two-finger acceptance test. The Mac-built v62 was installed on
the original M2 iPad without restarting SpringBoard or its running VM. Its
actual UIKit log captured simultaneous left/right owners with no face-button
bits, right-hand release with the left owner retained and right axes zero, full
release, and neutralization on focus loss. Longer scene, cancellation, keyboard
reconnection and GPU validation acceptance remain separate work.

## Explicit run shortcut

`DM_RUN_BUTTON=B` enables a labelled Run button above the left movement ring and
maps either keyboard Shift to native Npad B. This shortcut is independent of
the visible face letters and their skill-only positional conversion below. The option
is generic and off unless explicitly configured for a game. It sends the game's
ordinary button, preserving context-specific behavior (including B's skill
while targeting). It does not change game speed or synthesize B from stick tilt.

Run touch ownership is fixed at touch-down like other locked buttons. Crossing
it with a camera or movement finger does not press it. Both Shift states are
tracked separately from face keys, so releasing one alias cannot release another held
alias; keyboard disconnection and focus reset clear them. Touch cancellation,
layout changes and focus reset clear the Run owner with all other controls.
Compilation and on-device acceptance of this addition must be recorded before
claiming it works on hardware.

## Face-function restoration and accessible aiming (v65)

The user requires A/B/X/Y to retain their original functions in menus and normal
play, with positional selection applied only when choosing a combat skill.
V64 incorrectly converted every touch/keyboard face publication globally. That
approach was rejected and removed in v65: the published face bits retain their
letters. The on-device menu/normal controls must not be treated as interchangeable
with skill positions. A skill-only solution is still under investigation; holding
ZL alone is not a sufficient proof of skill context (capture and menus exist).
The retired conversion fixture establishes only the helper arithmetic, not the
correct behavior requested by the user.

The retired v65-v69 keyboard experiment used I/J/K/L as a positional diamond.
The current bindings are restored to J/K/U/I = A/B/X/Y. WASD moves, arrows and trackpad look, either Shift sends native Run B.
`DM_AIM_ZL=1` adds Aim beside Run. Tapping Aim latches ZL, tapping again releases
it. Either Command holds ZL until released; Z remains available. The view is
first responder and consumes Command combinations with game keys, including
Command-W. Cancelled Aim touches clear the latch. Focus, layout and input-device
resets clear held and latched input. Physical Command acceptance remains pending.

On this install, get_app_info reported the old data container but the process
actually used another container, confirmed by a temporary boot NSLog. Inspect
the process's boot log rather than trusting metadata for incremental deployment.
The old committed save was preserved on the Mac, the initially empty new save
and work directories were renamed as backups, and the committed save was copied
into the actual process directory. Configuration/control paths must follow that
actual directory. Runtime resources and all three shader companion directories
remain in the existing app bundle.

## Retired skill-input conversion (v69 validation build)

The input source now observes the actual Waza move layout's visibility, using
read-only OBS gates for main build `b1f12fd919eae86ab8a978317677e64b` only.
`DM_ZA_SKILL_DIRECTIONS=1` enables positional selection for new touch and keyboard
face presses while that layout is visible. A bottom selects the original native
B move slot, B right selects A, X left selects Y, and Y top selects X. The game
HUD and move assignments stay in their original positions. ZL alone is not the
condition. Unsupported builds and runs without the option retain native input.

Each source binds a press at touch/key down until release, so a visibility or Aim
transition cannot change a held skill into a new confirm/cancel action. Focus
reset clears those bindings. Run and Shift remain explicit native B shortcuts;
physical controllers keep their existing native mapping. The layout destructor
clears the observed context. No game register, HID implementation, or normal
menu mapping is globally changed by these observations.

The C acceptance fixture covers ordinary chords, four directions, entering and
leaving the selector with a held key, release, and independent touch/keyboard
sources. The full iOS Release build must pass; actual UIKit battle actions and
ordinary menu confirm/cancel still require device evidence. DM_CTL native-button
injection bypasses this frontend adaptation and cannot establish that evidence.

## Native face-function restoration (v70)

J/K/U/I again send native A/B/X/Y, exactly as before the positional keyboard
experiment. Touch A/B/X/Y also send their named native buttons in every context,
including battle. The frontend no longer calls the skill-input conversion helper;
changing a HUD layout must not change confirmation, cancellation or move bindings.
WASD, camera arrows, Run/Shift and Aim/Command retain their existing behavior.
Command-U is consumed alongside the other game combinations.

The keyboard regression compiles the actual frontend mapping function against
GameController, exercises the original four keys, shoulder shortcuts and Enter's
explicit diagnostic override, and checks native publication without a selector
remap. Battle HUD layout work is independent of these input semantics.

## Virtual battle geometry only (v71)

The original game's skill HUD, move assignments and ROM resources stay unchanged.
J/K/U/I remain native A/B/X/Y in every context. The same is true of the named
touch buttons; no selector state is consulted when publishing native input bits.

`DM_ZA_SKILL_LAYOUT=1` observes the exact-build Waza layout visibility and changes
only our virtual diamond's visible and hit-test geometry. Ordinary play keeps
A bottom, B right, X left and Y top. The visible skill selector uses A right,
B bottom, X top and Y left, matching the original game's move HUD. ZL alone
does not select this geometry. The old `DM_ZA_SKILL_DIRECTIONS` input conversion
option is ignored by the current host.

The existing main-run-loop timer observes geometry at 60 Hz. Drawing and hit
testing share `facePos:` and one production geometry helper. Every face-owned
finger freezes the diamond until release or cancellation, including a finger
which currently sits in a gap and presses no button. Pending visibility changes
use the latest selector state after release. Camera and movement ownership are
unchanged. The original keyboard regression and geometry/held-owner transition
tests pass; the Release iOS build passes. Actual battle interaction remains for
the user's verification. No battle-save fixture or autoplay is required.

An experimental same-slot ROM layout candidate was prepared locally before the
user clarified the intended frontend-only geometry. It was retired without
installation; neither the local original archive nor device ROM was modified.
