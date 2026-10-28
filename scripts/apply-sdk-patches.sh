#!/bin/sh
# ../XboxXBE-Recomp (cloned if missing) at the pinned commit with patches/sdk/*.patch applied (what the Apple builds need)
set -e
cd "$(dirname "$0")/.."
SDK=../XboxXBE-Recomp
PIN=99078b119b5d5e0d1d6afb1390e29437c0c984c0
[ -d $SDK/.git ] || git clone -q https://github.com/TGP482/XboxXBE-Recomp.git $SDK
git -C $SDK cat-file -e $PIN || git -C $SDK fetch -q origin
# the SDK's main has moved on (game code built into a DLL); the port is for the pinned commit
if [ "$(git -C $SDK rev-parse HEAD)" != "$PIN" ]; then
    git -C $SDK checkout -q $PIN || { echo "$SDK is not at $PIN and cannot be moved there (local changes?)" >&2; exit 1; }
    echo "$SDK at $PIN"
fi
for p in patches/sdk/*.patch; do
    if git -C $SDK apply --check "$PWD/$p" 2>/dev/null; then git -C $SDK apply "$PWD/$p" && echo "applied $p"
    elif git -C $SDK apply --check -R "$PWD/$p" 2>/dev/null; then echo "already applied: $p"
    else echo "does not apply: $p (is $SDK at $PIN?)" >&2; exit 1; fi
done
