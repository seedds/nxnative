# cJSON

- What: cJSON, a JSON parser in one C file (`cJSON.c`, `cJSON.h`), by Dave Gamble and the cJSON contributors.
- Version: 1.7.19, release tag `v1.7.19` (published 2025-09-09), commit `c859b25da02955fef659d658b8f324b5cde87be3` of https://github.com/DaveGamble/cJSON.
- License: MIT, in `LICENSE` beside this file (the upstream file, unchanged).
- Changes: none. `cJSON.c`, `cJSON.h` and `LICENSE` are the files of the release archive, byte for byte (SHA-256: `cJSON.c` 298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d, `cJSON.h` 25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee, `LICENSE` a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c).
- Downloaded with:

  ```sh
  curl -sL -o cjson.tar.gz https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz   # SHA-256 7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
  tar xzf cjson.tar.gz
  cp cJSON-1.7.19/cJSON.c cJSON-1.7.19/cJSON.h cJSON-1.7.19/LICENSE dm/third_party/cjson/
  ```

- Use: the shader reflection parser in plain C, `dm/gfx/nvn/nvn_refl.c` (`nvn_refl_from_json_text`), which reads the reflection JSON the shader chain writes (`dm/gfx/tools/dmsh_build.py`) on every host. `dm/host/Makefile` and `dm/gfx/tests/Makefile` compile `cJSON.c` beside the NVN sources; `dm/host/CMakeLists.txt` builds it into `t_refl_c` for Linux and Windows.
