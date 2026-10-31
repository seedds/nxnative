#!/bin/sh
# The differential test of dm/host/aot_fp.c (the host's floating-point and AES helpers, in C) against dm/tools/aot/src/lift/runtime.cpp (nxaot's, over
# dynarmic's common/fp and common/crypto): builds t_aot_fp_diff.cpp with both and runs it. Needs a C++20 compiler and dynarmic's sources (its common/fp
# needs no Boost); run in the nxnative-aot image (docker/Dockerfile.aot), as .github/workflows/aot.yml does:
#   sh dm/host/tests/aot_fp_diff.sh <out folder> [random cases per operation] [seed]
# CC and CXX choose the compilers (cc and c++ by default). Exit status: the test's (1 on a mismatch), 2 on a usage or build error
set -eu

if [ $# -lt 1 ]; then
    echo "usage: aot_fp_diff.sh <out folder> [random cases per operation] [seed]" >&2
    exit 2
fi
out=$1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
dyn=$repo/dm/third_party/dynarmic
common=$dyn/src/dynarmic/common
mkdir -p "$out"
inc="-I$dyn/src -I$dyn/externals/mcl/include -I$dyn/externals/fmt/include -I$repo/dm/tools/aot/include -I$repo/dm/tools/aot/src"
"${CC:-cc}" -std=gnu17 -O2 -DDM_AOT_FP_TEST -c "$repo/dm/host/aot_fp.c" -o "$out/aot_fp.o" || exit 2
# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++20 -O2 -DFMT_HEADER_ONLY $inc -o "$out/t_aot_fp_diff" "$repo/dm/host/tests/t_aot_fp_diff.cpp" "$repo/dm/tools/aot/src/lift/runtime.cpp" \
    "$common/fp/fused.cpp" "$common/fp/process_exception.cpp" "$common/fp/process_nan.cpp" "$common/fp/unpacked.cpp" "$common/fp/op/FPCompare.cpp" \
    "$common/fp/op/FPConvert.cpp" "$common/fp/op/FPMulAdd.cpp" "$common/fp/op/FPRSqrtEstimate.cpp" "$common/fp/op/FPRSqrtStepFused.cpp" \
    "$common/fp/op/FPRecipEstimate.cpp" "$common/fp/op/FPRecipExponent.cpp" "$common/fp/op/FPRecipStepFused.cpp" "$common/fp/op/FPRoundInt.cpp" \
    "$common/fp/op/FPToFixed.cpp" "$common/math_util.cpp" "$common/u128.cpp" "$common/crypto/aes.cpp" "$common/crypto/sm4.cpp" \
    "$dyn/externals/mcl/src/assert.cpp" "$out/aot_fp.o" -lpthread || exit 2
"$out/t_aot_fp_diff" "${2:-300000}" "${3:-1}"
