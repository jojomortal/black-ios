# iOS port notes

Target: iPhone 15 (A16), sideloaded. Branch `ios`. Android build must keep working.
SDK checkout: `../XboxXBE-Recomp` at `99078b1` (picked up by CMake before FetchContent).

## Phase 0: address space spike (`spike/memtest`)

`spike/memtest/build-ios.sh <none|eva|iml|both> [--sign]` builds a UIKit app that runs what
`kernel/memory.cpp` needs and prints/saves the report (`Documents/memtest-<variant>.txt`, also on screen).
`gen-ent.sh` writes the entitlements of a variant (Ad Hoc: app id, team id, get-task-allow=false + the capability keys); `run-device.sh` installs with devicectl and copies the report back; os_log subsystem `com.example.memtest`. macOS runs the same code (`cli.c`).

### Verified (macOS 26 arm64, 16 KB pages, no sandbox)

- 4 GB `PROT_NONE|MAP_NORESERVE` mmap works. The `0x100000000` hint is **ignored** (got `0x300000000`); nothing in
  Darwin code should depend on the fixed base (the Windows path only uses it for comparable dumps).
- All three aliasing methods give coherent views of one 64 MB region at `+0x80000000` and `+0xF0000000`:
  `vm_remap(copy=FALSE)` onto the reservation, `mach_make_memory_entry_64` + `vm_map(FIXED|OVERWRITE)`, `shm_open` + `mmap`.
- iOS SDK has no `mach_vm.h` (`#error`), so use the `vm_*` calls (`vm_allocate`, `vm_remap`, `vm_map`); `vm_address_t` is 64-bit on arm64.

### Verified on the iPhone 15 (iPhone15,4, 5687 MB, iOS 27.0.1), Ad Hoc build, all four variants

| variant | 4 GB PROT_NONE reserve | vm_remap | memory entry + vm_map | shm_open | available memory at start |
|---|---|---|---|---|---|
| none | ok | coherent | coherent | EPERM (sandbox) | 3067 MB |
| eva  | ok | coherent | coherent | EPERM | 3067 MB |
| iml  | ok | coherent | coherent | EPERM | 4091 MB |
| both | ok | coherent | coherent | EPERM | 4091 MB |

- The profile for `TEAMID.com.example.memtest` grants **both** capabilities and the signed entitlements of each
  variant matched (printed by `build-ios.sh --sign`). So the team can grant both.
- Reserving 4 GB of `PROT_NONE` works with **no** entitlement (reserved address space is not counted by jetsam). Extended
  Virtual Addressing changes nothing measurable here, so the real app does not need it; it only raises the limit on
  *accessible* address space, which the guest does not come near (64 MB RAM + commits).
- Increased Memory Limit raises the jetsam headroom from ~3.0 GB to ~4.0 GB (`os_proc_available_memory`). Use it.
- `shm_open` is denied by the sandbox: it can't be the aliasing method. `vm_remap` and a memory entry both work; the
  patch uses the memory entry (no source mapping to keep, port released after mapping).
- Base is ignored hint: `0x7000000000` on the phone.
- 16 KB host pages: `mprotect` of a 4 KB range not 16 KB aligned fails (EINVAL); `MADV_DONTNEED` does not zero.
- Launch trap, not memory related: iOS 27 kills an app without the UIScene lifecycle
  (`UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption`, SIGTRAP). The real app must adopt it
  (check what SDL 3.4.16 does on iOS in Phase 3).
- `devicectl device copy from --domain-type appDataContainer` works on the Ad Hoc build (Documents/ readback).

### SDK patch `patches/sdk/0001-memory-darwin.patch` (on 99078b1, `kernel/memory.cpp`, additions under `__APPLE__` only)

- RAM: one `mach_make_memory_entry_64` mapped at both windows with `vm_map(FIXED|OVERWRITE)`; the port is released after.
- `Commit`: `mprotect` rounded out to host pages (`sysconf(_SC_PAGESIZE)`).
- `Decommit`: clears the 4 KB bits, then releases only host pages with no committed guest page, by remapping them
  `MAP_FIXED|MAP_ANON|MAP_NORESERVE` `PROT_NONE` (zeroes). Neighbours stay.
- A reserve failure logs errno, the kern_return of the step that failed and the largest reservation that works.
- Windows and the generic Unix/Android path are unchanged. Tested on macOS with the real memory.cpp (aliasing, odd
  4 KB commits, neighbour survives, release faults, recommit reads zero). Not yet run on the phone inside the game.
- Side effect: the guest cannot have a guard smaller than 16 KB; `AllocStack`'s 64 KB guard is fine.
- Apply to a sibling checkout with `git -C ../XboxXBE-Recomp apply ../Black-Android/patches/sdk/0001-memory-darwin.patch`;
  CMake wiring for the FetchContent case comes in Phase 2.

## Phase 1: recompiler on macOS (done)

`scripts/build-apple.sh` (tools, xbe from `assets/` or `iso/`, recompiler with Apple clang, symbols, recompile) runs in
~5 s and writes the same 64 partitions; two runs hash identically. `tools/CMakeLists.txt` needed no change. Not compared
byte for byte with a Windows run.

## Phase 2: native macOS arm64 build (done, with caveats below)

Build: `scripts/fetch-deps.sh`, `scripts/apply-sdk-patches.sh`, then
`cmake -S . -B out/build/mac -G Ninja && ninja -C out/build/mac` (first build ~12 min, 64 partitions at -O2), run with
`scripts/run-mac.sh` (game from `../Black (USA)`, data in `out/run-data`; `BLACK_GAME_DIR`/`BLACK_DATA_DIR` override).
`-DBLACK_VERBOSE_LOG=ON` writes info lines (the SDK's logx follows NDEBUG); host sources only, so it rebuilds in seconds.
Needs `brew install molten-vk vulkan-headers` (Homebrew's MoltenVK is a macOS static lib).

- **Platform split**: `native/platform.h` (log redirect, hints, data dir, game folders, device lines, surface ready,
  asset path, loose controller, update check) with `platform_android.cpp` (the old code moved verbatim) and
  `platform_apple.cpp`. `main.cpp`, `game_files.cpp`, `input.cpp`, `touch.cpp` keep only shared logic. `update.cpp` is gone
  (moved into platform_android.cpp). The Android sources were syntax-checked with NDK 26 clang; the full Android build
  (NDK 28.2) was **not** run here: do a CI/Gradle build before merging.
- `europe.patch` is embedded by a CMake-generated byte array (was ELF `.incbin`), for Android too.
- SDL3 is static on Apple and finds MoltenVK through `dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")` (both Cocoa and UIKit
  do), so MoltenVK is linked in with `-Wl,-u,_vkGetInstanceProcAddr`; no loader and no dylib.
- `vk_portability.{h,cpp}`: portability enumeration and subset extensions, features chained, missing ones logged. No-ops
  off Apple. On the M1 Pro: **missing samplerMipLodBias and pointPolygons**; constant-alpha blend, comparison samplers,
  triangle fans, stencil ref, events etc. are all there. LOD bias is clamped to 0 without the feature (`vulkan.cpp`);
  constant-alpha for colour falls back to constant colour if a device lacks it (not exercised on the Mac).
  Not yet checked: point sprites need a written PointSize on Metal; the iPhone's subset may differ from the M1's.
- MoltenVK here reports Vulkan 1.3, depth **D32S8** (no D24S8 on Apple GPUs), no vsync / 3 swapchain images.
  The portability-enumeration extension is not listed when MoltenVK is linked directly; harmless.
- "Check for Updates" is hidden on Apple (`host::UpdatesSupported()`), Android keeps it.
- `BLACK_FORCE_NO_BC=1` forces the CPU DXT path (the iPhone has no BC textures). The decode time is logged every 5 s
  while it runs (`vulkan: DXT decoded on the CPU: N levels, X MB, T ms in all, U ms in the last S s`). At the title screen
  it was 0.2 ms over 10 s: **not representative**, measure in a mission before judging A16.

### Black window on the Mac: found and fixed

Cause: the Vulkan surface was made on the render thread. macOS makes the window's Metal view (SDL's
`SDL3_cocoametalview`) on the main thread only; from another thread everything succeeds (acquire, submit, present all
return success at 120 Hz, MoltenVK and the Metal validation layer say nothing) and the window stays black.
Fix: `platform::CreateVulkanSurface` runs `SDL_Vulkan_CreateSurface` on the main thread through `SDL_RunOnMainThread`
(Android calls SDL directly, unchanged); the swap chain and the presents stay on the render thread. The main thread's
event loop services the call, no deadlock at start up.

How it was found (all in `spike/vkpresent`, a 200 line SDL + MoltenVK presenter that clears to magenta; fullscreen runs,
magenta share of a screenshot): everything on main 97%, surface+swap chain+present on a worker **0%**, surface on main and
the rest on a worker 97%. The game's own `BLACK_DEBUG_CLEAR=1` (magenta, game drawing skipped) was black before the fix
and 96% magenta after. `BLACK_DEBUG_PRESENT=1` prints a once-a-second report of presents, acquire/present results,
swap chain extent and window points/pixels (x2 on Retina).
Steps not needed after that: the Vulkan validation layers were not run (not installed); Metal API validation
(`MTL_DEBUG_LAYER=1`) ran clean. For a Metal frame capture, if ever needed: run with `MTL_CAPTURE_ENABLED=1` from a
terminal, then Xcode > Debug > Attach to Process > black, and the camera button in the debug bar, or launch the binary
from an Xcode scheme and use Capture GPU Workload.
Windowed mode could not be checked reliably by screenshot (the window kept ending up where the capture did not see it,
also for the plain main-thread presenter): **please look at windowed mode** (`display_mode = 'windowed'` in
`out/run-data/black.toml`).

### Running on the Mac

From the repo root: `scripts/run-mac.sh` (fullscreen; game from `../Black (USA)`, settings/saves/logs in `out/run-data`).
`BLACK_WINDOWED=1 scripts/run-mac.sh` gives a resizable 1280x720 window (its green button goes native fullscreen);
`display_mode = 'windowed'` in `out/run-data/black.toml` does the same, `BLACK_FULLSCREEN=1` wins over the setting.
`BLACK_FORCE_NO_BC=1` forces the CPU DXT path. Extra logging: rebuild with `-DBLACK_VERBOSE_LOG=ON`, run with
`BLACK_DEBUG_PRESENT=1`. The touch controls show on macOS only while a direct touch device is present (the trackpad is
an indirect one and its fingers are ignored); always on Android and iOS.

### Verified on the Mac (M1 Pro, macOS 26.7.1)

- Boots from the ISO, SHA1 matches, no unresolved kernel imports, guest space reserved through the patched `memory.cpp`,
  reaches the title screen (`menu cursor: page PressStart` at 11 s), CoreAudio stream opens, attract-mode videos start, the
  run lasted the full 60 s with no guest trap or error line. Shader cache fills.
- The forced no-BC path runs and logs.

### NOT verified

- What is on screen: it was a black window, see above; after the fix screenshots show the Criterion and EA logos and the
  touch overlay (START and a gear: the Mac draws the touch controls too).
- Sound heard, a controller, touch/keyboard/mouse input, the options menu with the update row hidden, anything past the
  title screen, saves, dynamic resolution, the Europe disc path (`europe.patch` embed was only compiled).
- No `x86_trap` fired in 60 s of the title screen; deeper play is untested.

### Phase 3 needs

- **MoltenVK for iOS**: Homebrew's `molten-vk` is macOS-only. Phase 3 needs the iOS build as an xcframework (from the
  Vulkan SDK or MoltenVK's own `make ios` / release package), linked statically, with the same `-u _vkGetInstanceProcAddr`.
- The UIScene lifecycle, `UIFileSharingEnabled`, safe area, and an `.app` bundle target (the Mac target is a bare executable).
- `RedirectLog` has an os_log branch for iOS that is written but has never run.

## Phase 3: iOS (in progress)

Build (no signing needed to compile): `scripts/fetch-deps.sh && scripts/fetch-deps.sh ios`, then
`cmake -S . -B out/build/ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DBLACK_VERBOSE_LOG=ON`
and `cmake --build out/build/ios --config Release --target main` (first build ~15 min; configure ~6 min for the fetches).
Result `out/build/ios/Release-iphoneos/Black.app`: arm64, platform iOS, minos 16.0, no dylibs besides system ones,
`vkGetInstanceProcAddr` exported (SDL finds the linked-in MoltenVK with `dlsym`). Then
`scripts/sign-ios.sh` (needs `the profile named in ios/signing.local.env`, Ad Hoc, app id `TEAMID.com.example.black`;
signs increased-memory-limit only, `WITH_EVA=1` adds extended-virtual-addressing) and `scripts/install-ios.sh install|run|game <iso>|logs`.

- MoltenVK: the official v1.4.2 `MoltenVK-ios.tar` (sha256 pinned in fetch-deps.sh): a device-only `ios-arm64` static lib, no
  simulator slice, so the app only runs on the phone.
- UIScene: SDL 3.4.16 has `SDLUIKitSceneDelegate`; `ios/Info.plist.in` declares it in `UIApplicationSceneManifest`.
  Not yet seen working on the device.
- Info.plist: landscape only, status bar hidden, `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace`
  (Documents/Black in Files), `UIRequiredDeviceCapabilities` arm64 + metal.
- Home indicator: `SDL_HINT_IOS_HIDE_HOME_INDICATOR=2` (dim; the first swipe shows it, the second goes home).
- Lifecycle: on iOS the foreground flag is flipped from an SDL event watch (synchronously in the delegate callback), not
  only from the event loop. Nothing else changed for Android. **Untested on device**; whether MoltenVK survives a
  background/foreground cycle (command buffers committed while backgrounded fail on iOS) is an open question.
- Touch layout: shares are now of the safe area (`platform::SafeAreaPixels`, SDL_GetWindowSafeArea on iOS, the whole
  window on Android and the Mac). Untested on device.
- Game files: Documents/Black (then Documents), `scripts/install-ios.sh game <iso>` copies an ISO there with devicectl.
  Settings, saves, logs and the shader cache are in the app's Application Support (`SDL_GetPrefPath`); logs go to os_log
  (subsystem com.example.black) as well. Both unverified.

### iPhone 15 (A16, iOS 27.0.1) results so far

Verified from the device log (`scripts/install-ios.sh run` then `logs`; per-second lines need `BLACK_DEBUG_PRESENT=1`, which
`run` sets, as `BLACK_DEBUG_INPUT=1`):
- Boots from `Documents/Black/Black (USA).iso` (sha1 matches), title screen at 4.1 s, 4087 MB available to the app
  (Increased Memory Limit only, signed by `scripts/sign-ios.sh`). **The 4 GB guest reservation succeeded** at
  `0x7000000000`; Extended Virtual Addressing was not needed.
- A16 GPU through MoltenVK 1.4.2: Vulkan 1.3, D32S8 depth, FIFO present at 60 Hz (MAILBOX/IMMEDIATE not offered),
  missing portability features `samplerMipLodBias` and `pointPolygons`, BC textures decoded on the CPU.
- ~18 min of play: 1053 reported seconds, median 61 fps, none with a non-zero acquire/present result, no timeout, no
  device-lost, no guest trap, no low-memory warning. BC decode in all: 2172 levels, 32.3 MB, 15.9 ms.
- Background/foreground: every return resumed (`focus back: resumed`, new surface and swap chain, ~61 fps next second),
  for short interruptions (1.7 s and 2.8 s, no `did enter background`, Control Center or the app switcher) and for real
  backgrounds up to 108 s in an earlier run. SDL maps the resign-active step to `will enter background`.
- A real background of 231 s (home screen, `did enter background` at 1194.4 s, back at 1425.1 s): same process, no kill,
  `focus back: resumed`, new surface and swap chain, 60 then 61 fps, 34 reported seconds all with acquire/present 0.
- Player reports (not in the log): the picture, the DS4 controller and the sound during play were fine, and the sound
  was fine after returning from that background.

- Saves persist: saved, force-quit, relaunched, the save was there (player report).
- Edge swipes: Control Center pulled from the top and the home / app-switcher swipe from the bottom no longer act on the
  game (player report, after the fixes below). Cause: SDL defers every edge for `SDL_HINT_IOS_HIDE_HOME_INDICATOR=2`, so
  the first swipe of an edge reaches the app; and `SDL_EVENT_FINGER_CANCELED` was handled as a finger-up, which is a tap.
  Now: a cancelled finger is dropped without a tap, every finger is dropped on background/foreground/focus changes
  (`touch::ReleaseAll`), and touches that begin in a 20 pt top / 24 pt bottom margin (`platform::TouchMarginsPixels`, iOS
  only) are ignored unless they hit a control. A real tap in the bottom 24 pt on a menu is ignored too.

- App icon: `ios/Assets.xcassets/AppIcon.appiconset` (1024 px, cropped and scaled from the cover art in
  `app/src/main/res/drawable-nodpi/ic_launcher_art.png`, no alpha), compiled by actool (`Assets.car`); it looks right on
  the home screen (player report). The source art is 327 px wide, so a higher-resolution BLACK image would sharpen it;
  no dark or tinted iOS 18 variants.

Not yet verified: a background much longer than 4 minutes (iOS may kill a suspended app after a while or under memory
pressure), the safe-area layout, the Europe disc, the EasySMX M15 controller.
The audio session is `SoloAmbient` until SDL opens the device, then `Playback`.

## Decisions

- Full 4 GB guest layout (approved after Phase 0). No smaller layout needed.
- Real app bundle ID `com.example.black`. Its App ID gets Increased Memory Limit **and** Extended Virtual Addressing
  in the portal, with an Ad Hoc profile; the build signs **only** `increased-memory-limit`. EVA is the fallback to
  sign in if the reservation ever fails inside the full app (large binary + MoltenVK use address space the spike did not).
- iOS 27 requires the UIScene lifecycle (see above); the real app's Info.plist needs `UIApplicationSceneManifest`.
- Aliasing: memory entry + `vm_map`; `shm_open` is not available on iOS.
- Signing: the Apple Distribution identity of the developer account, manual, Ad Hoc. iPhone UDID `<device UDID>`.
- Game files for Mac testing: `iso/` symlink to `<the USA disc image>`; the USA
  `assets/default.xbe` has SHA1 `c5bfed25449c8212d7a976f15246e87ead57713f`. Nothing under iso/, assets/, generated/ is committed.

## Open questions

- Signing decided: the Apple Distribution identity of the developer account, manual, Ad Hoc profiles. iPhone UDID `<device UDID>` (devicectl: paired, available).
- Dev portal (done): one App ID `com.example.memtest` with both capabilities + one Ad Hoc profile; variants sign a subset of what the profile grants.

## HD textures (BLACK HD Reimagined)

Step 1 (key match, Mac). `native/hd_textures.cpp` computes the map's key (FNV-1a 64 over the top level's guest bytes as
64-bit words; P8/DXT1/DXT3/DXT5, swizzled, 2D, >= 16x16) when `BindGuestTexture` is about to upload a texture, and
logs hits and misses every 5 s. Reads the map from `BLACK_HD_MAP` or the bundle (`textures/`); each miss is logged.
- The SDK's format ids (`gpu/texture.cpp`: P8 0x0B, DXT1 0x0C, DXT3 0x0E, DXT5 0x0F) equal the map's format column; no conversion.
- Map: `textures/black-hd-reimagined.map` from portingpete/Black-2006-Recomp (MIT, licence copied beside it). Pack files are never committed.
- Result so far: title and menus only (DXT5 3/3 hit, DXT3 0/2); the first mission is not measured yet.

Step 2 (render). `hd::Find` keys a texture, `hd::Load` prepares the pack file, `VulkanBackend::UploadHdTexture` swaps it in at the
game texture's binding (the game's coordinates are normalised, so a different size fits).
- Setting: `textures` in black.toml, options menu Graphics > Quality > TEXTURES (ORIGINAL / HD), default ORIGINAL. It takes
  effect at the next bind of each texture, no restart: the setting is part of a texture's staleness signature. `BLACK_TEXTURES=hd|original`
  overrides it for a run. Original is the old path untouched (the HD branch is only taken when a map entry matches and the pack is there).
- Pack folder: `BLACK_HD_PACK`, else `hd/` in the game folder (Documents/Black/hd on iOS); any layout, files found by name.
- DDS (DXT1/3/5) is uploaded as it is when the GPU has BC (the pack's own mips); without BC each level is decoded to BGRA on the CPU.
  PNG: SDL_LoadPNG, box-filtered mip chain, BGRA. PS2 alpha (biggest 32x32 cell average of alpha <= 130): BC3 end points doubled, PNG alpha doubled, as the reference does.
- No-BC devices keep the memory sane by leaving out levels bigger than 512 px (`BLACK_HD_MAX=<n>`, 0 for none; it is a preview, step 3's budget replaces it).
  19 map entries are single-level files over 512 px: with the limit they fall back to the original texture (logged as "cannot use").
- `BLACK_HD_SELFTEST=<max side>` loads every map entry once at start. Mac, no limit: 1161 of 1161 load, 6.7 s, 4555 MB of image data in all
  (it counts files shared by several game textures every time). Limit 512: 1142 load, 422 MB.
- The build version in the log is written by `cmake/build_info.cmake` before every build (`-dirty` when tracked files differ from HEAD).
- Not yet seen: the first mission with HD on (needs a play-through); pack alpha on glass/effects is the thing to look at.

## Signing configuration
Nothing account-specific is committed. `scripts/sign-ios.sh`, `install-ios.sh` and `spike/memtest/*` read `ios/signing.local.env`
(gitignored; `ios/signing.local.env.example` lists the keys: TEAM, BUNDLE_ID, IDENTITY, PROFILE, DEVICE, INCREASED_MEMORY_LIMIT).
The build carries a placeholder bundle id (`org.black.ios`); the sign script sets the real one in Info.plist, and the log's os_log
subsystem follows it. The data folder name (`Library/Application Support/com.muhammad/Black`, SDL's pref path) is unchanged so existing saves stay.
