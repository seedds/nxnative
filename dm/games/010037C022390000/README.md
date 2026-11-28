# Hitman: Absolution

## Frame rate

The [build defaults](feral_defaults.tsv) set Feral's unconditional `IdleFPS` and `ResponsiveFPS` values in `resources/specifications.xml` to 60. New saves take their values from that specification. The game's hardware presets and specification UUID stay as shipped. Saves that have already applied that UUID keep their preferences.

The NSP packer applies the defaults even when the title uses a provisional profile. It matches the XML registry path and names across versions. If either entry is absent or ambiguous, the packer logs it and uses the original image.

On an M4 Pro with build 1.4_107768 (v131072), the opening sequence went from 30.0 to 59.8 fps at NVN interval 1. Standing still at the first playable checkpoint averaged 60.0 fps.
