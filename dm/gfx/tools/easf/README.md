# EA Shader archives

Adapted from ohxiaoguang/Touchline23 (MIT). `shader_split.py` recognizes BIG4,
EASF, chunkzip and NXSHADER inputs and sends their shader records through its
normal key, package, compute-query and single-stage pairing paths. Directory and
DMROMFS1 inputs use the same `romfs` IO as the other readers. Outer BIG4 tables
are read separately; only archive/program members are loaded.

The default EASF key source is that version's own ExeFS `main`. The shared
`dm/tools/nso.py` loader restores and validates its segments. An eight-byte key
ID names a registry record (`id[8], key[16]`); a candidate must reproduce the
EASF header's SHA-256 of the first 32 plaintext bytes. This is prefix verification,
not whole-file authentication. No fixed address, build ID or network lookup is
used. Logs contain fingerprints and build IDs, never keys.

`DM_EASF_KEY_FILE` optionally names a private JSON map of IDs to hex keys; a
wrong candidate falls through to module discovery. `DM_EASF_KEY_SCAN=1` enables
the bounded read-only-segment fallback. These are distinct from the player's
`prod.keys`, which opens Switch containers. EASF needs PyCryptodome; NSO's LZ4
support uses the normal tool dependency. No actual keys or archive samples ship.

The supported layouts have been checked on FIFA 23; other EA games need their
own validation. The chunkzip reader retains Touchline23's bounded 96-byte stream
search between aligned blocks: it validates decoded sizes, but unknown block
record variants are not claimed supported.

Synthetic tests: `python3 -m unittest dm.tools.tests.test_easf`.

Source reference: [Touchline23 at 229be41](https://github.com/ohxiaoguang/touchline23/tree/229be41d5270d07da1eca12bf6f1e862840b0d8e/dm/gfx/tools/easf).
The [source MIT license](https://github.com/ohxiaoguang/touchline23/blob/229be41d5270d07da1eca12bf6f1e862840b0d8e/LICENSE)
and this repository's root license retain Copyright (c) 2026 seedds. The
adaptation's ohxiaoguang/Touchline23 attribution is preserved in the source headers.
PyCryptodome is an installed dependency, not vendored in this reader.
