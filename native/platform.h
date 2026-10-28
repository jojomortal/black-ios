#pragma once
// What differs between the hosts the parts of native/ run on: platform_android.cpp and platform_apple.cpp (macOS, iOS)
#include <vulkan/vulkan.h>

#include <filesystem>
#include <vector>

struct SDL_Window;
typedef uint64_t SDL_WindowFlags;

namespace input {
struct Pad;
}

namespace platform {

// the log's lines (stderr) to the system's log as well: logcat, os_log
void RedirectLog();
// SDL hints this host sets, before SDL_Init
void SetHints();
// iOS gives an app no GPU work once it is told it goes to the background, and says so in a callback that must act at
// once, which the main loop may not reach in time: this calls set_foreground from that callback. Nothing elsewhere
void WatchLifecycle(void (*set_foreground)(bool));
// app-specific storage: the settings, saves, logs and the shader cache
std::filesystem::path DataDir();
// where the game files or a disc image are looked for, in this order
std::vector<std::filesystem::path> GameFolders();
// the window's flags and size (0 for the display's): the phones' window is the screen, a Mac's can be a window
void WindowSetup(SDL_WindowFlags& flags, int& width, int& height);
// the log line for no game found, once; the place to look and what the player does about it
void LogGameMissing();
// the device, CPU and RAM lines of the log's header
void LogDevice();
// the Vulkan surface of the window, from any thread: macOS makes the window's Metal view on the main thread only, where
// a surface made on another shows nothing (black, no error anywhere)
bool CreateVulkanSurface(SDL_Window* window, VkInstance instance, VkSurfaceKHR* surface);
void DestroyVulkanSurface(VkInstance instance, VkSurfaceKHR surface);
// SDL may make the Vulkan surface now (Android hands the window over late and takes it away again)
bool SurfaceReady(SDL_Window* window);
// a file shipped with the app (the touch controls' skin), as a path SDL can open
std::filesystem::path AssetPath(const char* relative);
// the part of the window the controls may sit in, in pixels: the notch's and the home indicator's insets are left out on iOS,
// elsewhere it is the whole window
void SafeAreaPixels(SDL_Window* window, float& x, float& y, float& w, float& h);
// pixels at the top and the bottom of the screen where a touch is the system's, not the game's (iOS: Control Center and
// the notification shade come from the top, the home screen and the app switcher from the bottom, and the first swipe of a
// deferred edge reaches the app); 0 elsewhere
void TouchMarginsPixels(SDL_Window* window, float& top, float& bottom);
// the touch controls are for a touch screen: always on a phone, on a Mac only while a direct touch device is present
bool TouchControls();
// a finger event of this touch device (SDL_TouchID) is the player's: not a Mac trackpad's
bool AcceptsFinger(uint64_t touch_id);
// a controller the OS hands over without a device, on top of what SDL read of port 0 (Android's game services)
void MergeLooseController(input::Pad& pad);

}  // namespace platform

namespace host {
// the options' update check: a dialog says what GitHub has; false where the platform has none
bool UpdatesSupported();
void CheckForUpdates();
}  // namespace host
