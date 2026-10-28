#!/bin/sh
# usage: fetch-deps.sh [ios]
# SDL3 into third_party/SDL at the tag the Android build uses (app/build.gradle's fetchSDL does the same for Gradle)
set -e
cd "$(dirname "$0")/.."
TAG=$(sed -n 's/.*sdlTag *= *"\(.*\)".*/\1/p' app/build.gradle | head -1)
[ -n "$TAG" ] || TAG=release-3.4.16
[ -f third_party/SDL/CMakeLists.txt ] || git clone --depth 1 --branch "$TAG" https://github.com/libsdl-org/SDL.git third_party/SDL
echo "SDL $TAG in third_party/SDL"

# MoltenVK for iOS: the official prebuilt package (Homebrew's is macOS-only), the version Homebrew's has (1.4.2), checked by sha256
if [ "$1" = "ios" ]; then
    MVK=v1.4.2
    MVK_SHA256=b5d947b1660e6e9fed40b9cd2387e160aaab9e80b775c0cef7e14059405178c1
    DIR=third_party/MoltenVK-ios
    if [ ! -f $DIR/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a ]; then
        mkdir -p $DIR
        curl -fsSL -o $DIR/MoltenVK-ios.tar https://github.com/KhronosGroup/MoltenVK/releases/download/$MVK/MoltenVK-ios.tar
        [ "$(shasum -a 256 $DIR/MoltenVK-ios.tar | cut -d' ' -f1)" = "$MVK_SHA256" ] || { echo "MoltenVK-ios.tar: sha256 differs" >&2; exit 1; }
        tar -xf $DIR/MoltenVK-ios.tar -C $DIR
    fi
    echo "MoltenVK $MVK for iOS in $DIR"
fi
