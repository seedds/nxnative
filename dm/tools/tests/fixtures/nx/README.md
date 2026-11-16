# Synthetic NX inputs

These eight NX 0.2 archives contain fabricated NSO/NPDM bytes, repeated data,
empty files/directories and synthetic NCA/CNMT/Control structures. They contain
no executable game code, copyrighted game assets, keys or decrypted game data.

Each platform has RAW and Zstandard materialized ExeFS/RomFS trees, plus an
archive of native decrypted NCAs. Each archive contains base version 0,
update 65536 and add-on `0100000000011001`. Materialized data crosses 64 KiB
block boundaries; the update replaces the base tree. Native versions have
independent IVFC filesystems and exercise the existing patch composition path
with a complete replacement NCA. They do not fabricate BKTR relocation tables
or establish compatibility with real games. Two further archives omit the exact
base of a native update and must be rejected. Native DLC requires application
version 65536.

Regenerate with `python3 dm/tools/tests/fixtures/nx/generate.py` (Git, Rust
and network access required). Normal tests need only the committed inputs.
The generator pins nxformat and completes its synthetic RomFS lookup tables
and CNMT tails for LibHac, without changing any installed nxformat checkout.
