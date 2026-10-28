#!/bin/sh
# usage: make-ipa.sh [path of Black.app]
# An unsigned IPA of the built app (out/build/ios/Release-iphoneos/Black.app) in out/ios/Black-unsigned.ipa, for SideStore,
# AltStore or Sideloadly, which sign it with the user's own Apple ID. No account details go in; the bundle id is the build's
# placeholder (org.black.ios), which those tools replace. Such a signature has no Increased Memory Limit.
set -e
cd "$(dirname "$0")/.."
APP_IN=${1:-out/build/ios/Release-iphoneos/Black.app}
OUT=out/ios
[ -d "$APP_IN" ] || { echo "no $APP_IN: ./scripts/build-apple.sh ios, or cmake --build out/build/ios --config Release --target main" >&2; exit 1; }
rm -rf $OUT/ipa && mkdir -p $OUT/ipa/Payload
cp -R "$APP_IN" $OUT/ipa/Payload/Black.app
rm -rf $OUT/ipa/Payload/Black.app/_CodeSignature $OUT/ipa/Payload/Black.app/embedded.mobileprovision
codesign --remove-signature $OUT/ipa/Payload/Black.app 2>/dev/null || true
(cd $OUT/ipa && rm -f ../Black-unsigned.ipa && zip -qry ../Black-unsigned.ipa Payload)
rm -rf $OUT/ipa
echo "== $OUT/Black-unsigned.ipa ($(du -h $OUT/Black-unsigned.ipa | cut -f1)) =="
unzip -l $OUT/Black-unsigned.ipa | grep -c "Payload/Black.app/" | sed 's/$/ files/'
