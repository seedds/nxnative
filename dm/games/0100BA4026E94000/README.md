# Castlevania: Belmont's Curse (0100BA4026E94000)

Provisional profile, brought up 2026-10-29 and confirmed running on macOS 26.6.

- Base 1.1.0 + update v65536 + DLC "Bonus Contents Pack". Unity / IL2CPP, SDK 21, NVN.
- **The retail NSP is a repack**: the NCA partition signature fails
  (`ResultFsPartitionSignatureVerificationFailed`) and the code section fails its hash check.
  Both are tolerated now: `nx_extract` retries the code section with `IntegrityCheckLevel.IgnoreOnInvalid`
  and `--allow-damaged` keeps going, but the extraction still aborts on the signature. Workaround used
  for the bring-up: the ExeFS was extracted by a small standalone NCA reader and the RomFS came from a
  previously prepared image, then placed in `extract/v65536/` (ExeFS/main + RomFS.img + control.nacp),
  which the build reuses.
- ExeFS build IDs: main `0f1169aa7d0fa4529c861f96adec306e`, sdk `8af286d7bfb124125a15e96a39cb1ab7`,
  subsdk0 `8205ead9e1f93d8b2337feaddefac330`.
- Shaders: 19 162 from the Unity bundles, all translated and compiled (about 15 min per target).
- Tested on: macOS 26.6 — `nnMain` reached, IL2CPP bootstraps, 3 121 RomFS entries mounted,
  Unity worker threads (`AssetGarbageCollectorHelper`) running.
