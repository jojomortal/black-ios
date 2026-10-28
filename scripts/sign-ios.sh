#!/bin/sh
# usage: sign-ios.sh [path of Black.app] [WITH_EVA=1]
# Signs a copy of the built app (out/build/ios/Release-iphoneos/Black.app) into out/ios/Black.app with the profile and
# identity of ios/signing.local.env and the entitlement increased-memory-limit only (INCREASED_MEMORY_LIMIT=0 leaves it out). WITH_EVA=1 adds
# extended-virtual-addressing, the fallback if the 4 GB guest reservation ever fails in the full app. Prints what it signed.
set -e
cd "$(dirname "$0")/.."
APP_IN=${1:-out/build/ios/Release-iphoneos/Black.app}
CONF=ios/signing.local.env
[ -f "$CONF" ] || { echo "missing $CONF: copy ios/signing.local.env.example and fill it in" >&2; exit 1; }
WANT_IML=$INCREASED_MEMORY_LIMIT  # a value given on the command line wins over the file
. "./$CONF"
INCREASED_MEMORY_LIMIT=${WANT_IML:-$INCREASED_MEMORY_LIMIT}
: "${TEAM:?TEAM in $CONF}" "${BUNDLE_ID:?BUNDLE_ID in $CONF}" "${IDENTITY:?IDENTITY in $CONF}"
PROF=${PROFILE:-ios/profiles/black.mobileprovision}
IML=${INCREASED_MEMORY_LIMIT:-1}
OUT=out/ios
[ -d "$APP_IN" ] || { echo "no $APP_IN: cmake --build out/build/ios --config Release --target main" >&2; exit 1; }
[ -f "$PROF" ] || { echo "missing $PROF (provisioning profile of $BUNDLE_ID)" >&2; exit 1; }
mkdir -p $OUT && rm -rf $OUT/Black.app && cp -R "$APP_IN" $OUT/Black.app
security cms -D -i "$PROF" > $OUT/profile.plist
GOT=$(plutil -extract Entitlements.application-identifier raw -o - $OUT/profile.plist)
[ "$GOT" = "$TEAM.$BUNDLE_ID" ] || { echo "profile is for $GOT, expected $TEAM.$BUNDLE_ID" >&2; exit 1; }
IMLKEY=
if [ "$IML" = 1 ]; then
    grep -q "kernel.increased-memory-limit" $OUT/profile.plist || { echo "the profile does not grant increased-memory-limit (INCREASED_MEMORY_LIMIT=0 signs without it)" >&2; exit 1; }
    IMLKEY='<key>com.apple.developer.kernel.increased-memory-limit</key><true/>'
fi
EVA=
if [ "$WITH_EVA" = 1 ]; then
    grep -q "kernel.extended-virtual-addressing" $OUT/profile.plist || { echo "the profile does not grant extended-virtual-addressing" >&2; exit 1; }
    EVA='<key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>'
fi
cat > $OUT/entitlements.plist <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>application-identifier</key><string>$TEAM.$BUNDLE_ID</string>
<key>com.apple.developer.team-identifier</key><string>$TEAM</string>
<key>get-task-allow</key><false/>
$IMLKEY
$EVA
</dict></plist>
P
plutil -replace CFBundleIdentifier -string "$BUNDLE_ID" $OUT/Black.app/Info.plist
cp "$PROF" $OUT/Black.app/embedded.mobileprovision
codesign -f -s "$IDENTITY" --entitlements $OUT/entitlements.plist $OUT/Black.app
codesign --verify --strict $OUT/Black.app
echo "== signed $OUT/Black.app, entitlements =="
codesign -d --entitlements :- $OUT/Black.app 2>/dev/null
echo "== profile: devices =="
plutil -extract ProvisionedDevices json -o - $OUT/profile.plist; echo
