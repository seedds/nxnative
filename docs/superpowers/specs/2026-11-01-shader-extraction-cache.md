# Shader extraction reuse across chain updates

The user requested extraction caching for macOS and iOS, followed by a LegendsZA rebuild benchmark with an unchanged NSP. Both targets already call the same `split_shaders` stage. Preserve the existing archive-reuse change and publish the extraction change on its own branch, with the benchmark and platform validation details.

The split currently fingerprints every graphics Python tool, every profile file, and the whole Oodle host executable. Translation-only changes and the compiled chain label therefore invalidate extraction. Use the extractor's transitive repository Python dependencies, the profile settings and tables extraction actually consumes/checks, and the existing game-data identities. Track explicit capture directories and auxiliary modules read by the extractor too.

For the managed Oodle host, record a decoder identity covering all existing host build inputs while normalizing only the shader-chain label. Bind that record to the actual executable bytes. A missing/invalid record falls back to executable identity; modified binaries cannot inherit trusted provenance. Host/decoder source, compiler, SDK, profile-header and module changes remain conservative misses. Do not narrow the host's runtime dependencies further without evidence.

Keep output integrity, registered-table validation, DLC/compute promotion proofs, provisional-profile behavior, and damaged-input handling. Old extraction caches warm once. A valid extraction result can serve either target; translation and target-specific Metal artifacts retain their own keys.

Benchmark a warmed unchanged LegendsZA build before and after a temporary comment-only chain update. Compare the legacy and new split on the same game inputs, record a complete Mac app rebuild and a real shared-stage iOS reuse check, and restore the temporary chain edit/header. Distinguish stage savings from total-build measurements. Keep game data/logs beside the NSP. No iOS signing, installation, or device claim.
