# bcdec

A single-header decoder for the BC1-BC7 block-compressed texture formats (BC6H and BC7 included): https://github.com/iOrange/bcdec, commit 80859ed (version 0.985), unmodified. License: `LICENSE` (MIT or the Unlicense, your choice).

Used by `dm/gfx/tex/nvn_bcdec.c`: on a GPU without BC texture support the replay layer decodes BC textures on the CPU.
