#!/bin/sh
# usage: build-ios.sh <none|eva|iml|both> [--sign]
# unsigned by default (compile check). --sign uses profiles/memtest.mobileprovision (one App ID, both capabilities)
# and signs with the entitlements of the variant only, then prints them for checking
set -e
cd "$(dirname "$0")"
V=${1:-none}
. ../../ios/signing.local.env
BUNDLE_ID=$BUNDLE_ID.memtest
ID=$IDENTITY
PROF=profiles/memtest.mobileprovision
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
APP=build/$V/Memtest.app
rm -rf "$APP"; mkdir -p "$APP"
CF="-arch arm64 -isysroot $SDK -miphoneos-version-min=16.0 -O1"
xcrun clang++ $CF -std=c++17 -c spike.cpp -o build/$V/spike.o
xcrun clang $CF -fobjc-arc -c main.m -o build/$V/main.o
xcrun clang++ $CF build/$V/spike.o build/$V/main.o -framework UIKit -framework Foundation -o "$APP/Memtest"
cat > "$APP/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Memtest</string>
<key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
<key>CFBundleName</key><string>Memtest</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1</string>
<key>MemtestVariant</key><string>$V</string>
<key>UIFileSharingEnabled</key><true/>
<key>LSSupportsOpeningDocumentsInPlace</key><true/>
<key>UILaunchScreen</key><dict/>
<key>UIApplicationSceneManifest</key><dict>
<key>UIApplicationSupportsMultipleScenes</key><false/>
<key>UISceneConfigurations</key><dict><key>UIWindowSceneSessionRoleApplication</key><array><dict>
<key>UISceneConfigurationName</key><string>Default</string>
<key>UISceneDelegateClassName</key><string>SceneDelegate</string>
</dict></array></dict></dict>
<key>MinimumOSVersion</key><string>16.0</string>
<key>UIDeviceFamily</key><array><integer>1</integer></array>
</dict></plist>
P
if [ "$2" != "--sign" ]; then echo "built unsigned $APP"; exit 0; fi
[ -f "$PROF" ] || { echo "missing $PROF" >&2; exit 1; }
security cms -D -i "$PROF" > build/$V/profile.plist
GOT=$(plutil -extract Entitlements.application-identifier raw -o - build/$V/profile.plist)
[ "$GOT" = "$TEAM.$BUNDLE_ID" ] || { echo "profile is for $GOT, expected $TEAM.$BUNDLE_ID" >&2; exit 1; }
cp "$PROF" "$APP/embedded.mobileprovision"
./gen-ent.sh $V $TEAM $BUNDLE_ID > build/$V/ent.plist
codesign -f -s "$ID" --entitlements build/$V/ent.plist "$APP"
echo "== entitlements of $APP (variant $V) =="
codesign -d --entitlements :- "$APP" 2>/dev/null
echo "== profile: app id, device count, entitlements it grants =="
plutil -extract Entitlements xml1 -o - build/$V/profile.plist | grep -E "application-identifier|kernel|get-task-allow" || true
plutil -extract ProvisionedDevices raw -o - build/$V/profile.plist
