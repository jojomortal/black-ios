#!/bin/sh
# What build.bat does up to the recompile, on macOS: tools, the xbe, the recompiler, the symbols, generated/game.
# The Android, macOS and iOS builds then take generated/game as it is.
set -e
cd "$(dirname "$0")/.."
GAME_XBE=default.xbe
TOOLS=out/build/apple-tools
# default.xbe of the Europe release: the recompiler takes the USA one
EUROPE_SHA1=796ad8369354e48aa15644d4707dd70bd3bc3b4c

fail() { echo; echo "  BUILD FAILED. $*" >&2; exit 1; }

echo "[1/4] Tools"
for t in cmake clang clang++ python3 shasum; do
    command -v $t >/dev/null 2>&1 || fail "$t is missing (xcode-select --install, brew install cmake)"
done
GENERATOR=
command -v ninja >/dev/null 2>&1 && GENERATOR="-G Ninja"
JOBS=$(sysctl -n hw.ncpu)

echo "[2/4] $GAME_XBE"
if [ ! -f assets/$GAME_XBE ]; then
    ISO=
    for f in assets/*.iso iso/*.iso; do
        [ -f "$f" ] && { ISO=$f; break; }
    done
    [ -n "$ISO" ] || fail "Put $GAME_XBE from the USA release, or its disc image (.iso), in $PWD/assets"
    echo "      taking it from $ISO"
    mkdir -p assets
    python3 -I tools/xdvdfs.py "$ISO" $GAME_XBE assets/$GAME_XBE || fail "could not read $ISO"
fi
SHA=$(shasum assets/$GAME_XBE | cut -d' ' -f1)
echo "      sha1 $SHA"
[ "$SHA" != "$EUROPE_SHA1" ] || fail "assets/$GAME_XBE is the Europe release: recompile the USA one (the Europe xbe is converted at run time)"

echo "[3/4] Recompiler"
cmake -Wno-dev -Wno-deprecated -S tools -B $TOOLS $GENERATOR -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null || fail "cmake"
cmake --build $TOOLS -j "$JOBS" >/dev/null || fail "the recompiler did not build"
SDK=$(cat $TOOLS/sdk_dir.txt)
grep -q '^\[symbols\]' config/game.toml || "$TOOLS/xbe_symbols" assets/$GAME_XBE "$SDK/xdk" --out config/game.toml || fail "xbe_symbols"

echo "[4/4] Recompiling $GAME_XBE into generated/game"
"$TOOLS/xbe_recomp" config/game.toml --xdk "$SDK/xdk" >/dev/null || fail "xbe_recomp"
echo
echo "  generated/game is ready."
