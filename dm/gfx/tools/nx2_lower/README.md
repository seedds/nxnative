# nx2_lower

Switch 2 (SM86) shader code to Maxwell code, for `nx2_split.py`. `shader_recompiler/frontend/ampere/` holds `lower_to_maxwell.h` and `program_capture.h` unchanged from [Eglantine](https://github.com/djihads80/eglantine) (`src/shader_recompiler/frontend/ampere/`, GPL-3.0-or-later; see the headers), so a host or tool built with them is under the GPL, not this repository's MIT licence. `common/common_types.h` stands in for Eden's.

```
clang++ -std=c++20 -O2 -w -I. nx2_lower.cpp -o nx2_lower
```
