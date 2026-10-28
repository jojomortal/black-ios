# Black for iOS

An unofficial iPhone port of the original Xbox version of *Black*, built from the same statically recompiled game as the
Android port. The game's x86 code is recompiled to C++ with [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp)
(TGP482), then built for arm64. Drawing goes through Vulkan on [MoltenVK](https://github.com/KhronosGroup/MoltenVK) (Metal),
windowing, input and audio through SDL3. No emulator is involved. The iOS port is by Mian Muhammad
([mianmuhammadse](https://github.com/mianmuhammadse)); it builds on [RanggaCasper](https://github.com/RanggaCasper)'s
Android port.

> [!NOTE]
> The repository has no game assets. You need the files of your own copy of the game: the **USA** release (an ISO, or its
> `default.xbe` and files). The Europe disc is patched at start-up but has not been tested on iOS.

Tested on an iPhone 15 (A16, 6 GB) with iOS 27, at 60 fps with a DualShock 4, with the touch controls, and with saves.
Other devices have not been tried.

## What you need

- A Mac with Apple Silicon and Xcode (command line tools and the iOS SDK), `brew install cmake ninja molten-vk vulkan-headers python3`.
- The USA disc image of *Black*, and `git` with network access: the builds download SDL3, MoltenVK, glslang, toml++, Dear ImGui and
  the SDK from GitHub (a few hundred MB, the first configure takes several minutes).
- A way to sign the app (see below), and an iPhone running iOS 16 or later.

## Build

```sh
git clone https://github.com/mianmuhammadse/Black-Android.git && cd Black-Android
git checkout ios
mkdir -p iso && cp "/path/to/Black (USA).iso" iso/   # your disc image; iso/ and assets/ are gitignored, the game's files never leave your machine
./scripts/build-apple.sh                   # recompiles default.xbe from your ISO into generated/game (about 5 s once the tools are built)
./scripts/fetch-deps.sh ios                # SDL3 and MoltenVK for iOS
./scripts/apply-sdk-patches.sh             # clones the SDK to ../XboxXBE-Recomp (beside the repo) at the pinned commit and patches it
cmake -S . -B out/build/ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DBLACK_VERBOSE_LOG=ON
cmake --build out/build/ios --config Release --target main     # the first build takes 15 minutes or more
```

(`assets/` works as well as `iso/`, or a `default.xbe` in `assets/`.) `PORTING_IOS.md` has the details and what was decided on the way. For a build on the Mac itself: `scripts/run-mac.sh`.

## Signing and installing

The build carries a placeholder bundle id (`org.black.ios`); the signing step sets yours. Pick one:

**1. A paid Apple Developer account (best).** Create an App ID with the *Increased Memory Limit* capability and an Ad Hoc or
Development profile for your device. Copy `ios/signing.local.env.example` to `ios/signing.local.env`, fill it in (team id,
bundle id, identity, profile, device), then:

```sh
./scripts/sign-ios.sh          # prints the entitlements it signed
./scripts/install-ios.sh install
```

**2. A free Apple ID with SideStore, AltStore or Sideloadly.** Make an unsigned IPA and let the tool sign it:

```sh
./scripts/make-ipa.sh          # out/ios/Black-unsigned.ipa
```

A free account cannot grant Increased Memory Limit, so the app gets the default memory limit of iOS (about 3 GB on a
6 GB iPhone instead of about 4 GB). That is enough: **tested on level_01 for 10 minutes with the HD textures on at 1024 px: the
footprint peaked at about 740 MB (about 410 MB with the 512 px cap) and the frame rate held 61 fps**, with a short hitch when a
batch of new HD textures loads; see *Known limits*. Free profiles also expire after 7 days.

**3. Xcode.** `ios/signing.local.env` is only for the scripts; you can instead open the generated project
(`out/build/ios/black_android.xcodeproj`), set your team and bundle id on the `main` target and run it.

**Bundle id.** Set `BUNDLE_ID` in `ios/signing.local.env`: an App ID belongs to one team, so you cannot sign with
the example's default (`com.muhammad.black`, the author's); register your own (for example `com.yourname.black`) and put it
in the profile and in the file. The sign script writes it into the app. The folder name inside the app where it keeps
saves, settings and logs is fixed (`Library/Application Support/com.muhammad/Black`, set in the code, not taken from the bundle
id). That does not carry saves between ids: a new bundle id is a new app with its own container, so it starts with no game
files and no saves; copy the game in again (`install-ios.sh game` or the Files app). Keeping one id across builds keeps everything.

## Game files

The game goes into the app's `Documents/Black` folder. In the **Files** app that is: **Browse, On My iPhone, Black** (the app's
own folder, named after the app), and inside it a folder named **Black** (make it if it is not there). Put the ISO (or
`default.xbe` with its files) in that inner **Black** folder: *On My iPhone / Black / Black / Black (USA).iso*. If the app finds
no game it lists the folders it looked in, in its log. From the Mac: `./scripts/install-ios.sh game "Black (USA).iso"`.
Saves and settings stay in the app's folder when you install a new build over the old one, with the same bundle id.

## HD textures (optional)

The game can use **BLACK HD Reimagined** by **HD REIMAGINED**, a texture pack made for the PS2 version. It is their work and
is not part of this repository. Download it from
[ModDB](https://www.moddb.com/mods/black-hd-reimagined/addons/black-hd-reimagined-texture-pack) and put the pack's files
(any folder layout, the `.dds` and `.png` files) in an `hd` folder next to the ISO: *On My iPhone / Black / Black / hd*. The pairing of pack files with game
textures is [portingpete](https://github.com/portingpete)'s map from
[Black-2006-Recomp](https://github.com/portingpete/Black-2006-Recomp) (MIT), kept in `textures/`.

Then choose **Options, Graphics, Quality, Textures: HD**. It applies at once. The iPhone has no BC texture hardware, so the
pack's files are decoded on the CPU and levels bigger than 1024 px are left out, and a file with one big level is scaled down to that (`BLACK_HD_MAX=<px>` changes the cap, 0 removes it).

## Known limits

- Tested on one device. Long play (an hour and a half, HD on) held with Increased Memory Limit; a free Apple ID's signature
  has not been played for as long.
- HD textures: about half of the game's textures have a counterpart in the pack. 1024 px at most on the iPhone for now.
- The Android code in this branch has only been syntax-checked, not built. **Android users should use
  [upstream](https://github.com/RanggaCasper/Black-Android).**
- Not tried: the Europe disc, the iPad, the PlayStation button prompts (controllers work, the prompts show Xbox names).
- iOS 27 requires a UIScene lifecycle, which is in; iOS versions before 16 are not supported.

## Credits

[RanggaCasper](https://github.com/RanggaCasper) (the Android port this builds on), [TGP482](https://github.com/TGP482)
(XboxXBE-Recomp), [portingpete](https://github.com/portingpete) (the texture map), HD REIMAGINED (the HD texture pack),
SDL3, MoltenVK, glslang, toml++ and Dear ImGui.
