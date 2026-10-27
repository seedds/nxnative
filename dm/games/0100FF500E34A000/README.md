# Xenoblade Chronicles: Definitive Edition (`0100FF500E34A000`)

This note records the rendering finding contributed in [PR #365](https://github.com/seedds/nxnative/pull/365). The game profile is a separate, pending contribution; this note does not establish playable-game status.

## Rendering

- **Resolved: scenery darkened during the first Colony 9 bridge reply by Shulk.** In version 1.1.2 on the Mac, the depth-of-field initialization draw takes a Float3 position through a uint4 shader declaration. Its missing `w` caused the draw to be clipped, leaving a black depth-of-field buffer that a later composite blended over textured scenery. The general fix and synthetic regression are linked below.

## Found on this game

- [Float32 vertex padding through integer inputs](../../../docs/reference/rendering.md#float32-vertex-padding-through-integer-inputs).

## Open

- Contribute the game profile and document its requirements, remaining lighting fixes and gameplay validation. These are outside the scope of PR #365.
