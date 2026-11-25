# nxformat reader

From https://github.com/fab918/nxformat, revision
`8edddb86a98a879a2abc26778b16126f141de265` (1.0.4 with the reader pointer fix of #19), `dist/`.
MIT; see LICENSE and the reader header's notice.

Only the C++17 archive reader, its pinned, positional disk source and the
mapped views that source includes are included (reads stay positional). There is no writer, converter, Rust runtime or C API dependency.
`dm/tools/nx_reader.cpp` links the reader to Zstandard and supplies a bounded
read-only pipe protocol for `scripts/pack_nx.py` and the existing native NCA
reader. Blocks retain the upstream integrity checks and cache policy.

Local change: `Snapshot::icon_index` retains the already validated application
or metadata-history icon index, alongside the upstream `nacp_index`. This lets
an explicitly selected older version keep its own icon. There is no wire-format
or validation change.
