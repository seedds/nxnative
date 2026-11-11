#!/bin/sh
# sqlite-zstd v0.3.5 (https://github.com/phiresky/sqlite-zstd), an optional build-time SQLite extension.
# Requires Xcode and Rust through rustup, with both targets: rustup target add aarch64-apple-darwin x86_64-apple-darwin.
# Usage: build.sh <scratch folder on the game drive>. No translator/chain rebuild is needed.
# Upstream LICENSE is LGPLv3; Cargo.toml instead says LGPL-2.0-or-later. Retain upstream's license unchanged.
set -eu
TAG=v0.3.5
COMMIT=ebc5d418dd2057099c9d0d36df155e8637a04748
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:?usage: build.sh <scratch folder>}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
command -v rustup >/dev/null || { echo '[RED] building sqlite-zstd needs Rust/Cargo through rustup; running it does not' >&2; exit 1; }
CARGO=$(rustup which cargo)
export RUSTC="$(rustup which rustc)"
if [ ! -d "$WORK/src/.git" ]; then git clone --quiet --depth 1 --branch "$TAG" https://github.com/phiresky/sqlite-zstd.git "$WORK/src"; fi
got=$(git -C "$WORK/src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "[RED] sqlite-zstd source is $got, expected $COMMIT" >&2; exit 1; }
export CARGO_TARGET_DIR="$WORK/target" CC=/usr/bin/clang CXX=/usr/bin/clang++ MACOSX_DEPLOYMENT_TARGET=13.0
export CARGO_PROFILE_RELEASE_STRIP=none
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER=/usr/bin/clang CARGO_TARGET_X86_64_APPLE_DARWIN_LINKER=/usr/bin/clang
for target in aarch64-apple-darwin x86_64-apple-darwin; do
    # Rust 1.96's stripping can leave a LINKEDIT string pool macOS refuses to load.
    "$CARGO" rustc --manifest-path "$WORK/src/Cargo.toml" --release --locked --features build_extension --target "$target" -- -C strip=none -C link-arg=-Wl,-install_name,@rpath/libsqlite_zstd.dylib > "$WORK/$target.log" 2>&1 \
        || { tail -20 "$WORK/$target.log" >&2; echo "[RED] sqlite-zstd did not build for $target; check Rust targets and Xcode (log: $WORK/$target.log)" >&2; exit 1; }
done
lipo -create "$WORK/target/aarch64-apple-darwin/release/libsqlite_zstd.dylib" "$WORK/target/x86_64-apple-darwin/release/libsqlite_zstd.dylib" -output "$HERE/libsqlite_zstd.dylib"
cp "$WORK/src/LICENSE" "$HERE/LICENSE"
echo "sqlite-zstd $TAG ($COMMIT): sha256 $(shasum -a 256 "$HERE/libsqlite_zstd.dylib" | cut -c1-64)"
