// The Apple hosts' platform part (macOS for building and testing, iOS): storage, the folders the game is looked for in,
// the system's log. BLACK_DATA_DIR and BLACK_GAME_DIR point a development build at a folder of its own.
#include "platform.h"

#include "platform_apple.h"

#include <TargetConditionals.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdlib>
#include <string>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

#if TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <mach/mach.h>
#include <os/proc.h>

#include <algorithm>
#include <chrono>
#include <thread>
#endif

namespace fs = std::filesystem;

namespace platform {

namespace {

std::string Sysctl(const char* name) {
    char v[256] = {};
    size_t n = sizeof(v) - 1;
    if (sysctlbyname(name, v, &n, nullptr, 0)) return "?";
    return v;
}

fs::path Env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
}

#if TARGET_OS_IPHONE
fs::path Documents() {
    const char* d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return d ? fs::path(d) : fs::path();
}
#endif

}  // namespace

#if TARGET_OS_IPHONE
// the id the app was signed with (scripts/sign-ios.sh sets it), for the log's subsystem
std::string BundleId() {
    char id[256] = "black";
    if (CFStringRef s = CFBundleGetIdentifier(CFBundleGetMainBundle())) CFStringGetCString(s, id, sizeof(id), kCFStringEncodingUTF8);
    return id;
}
#endif

// iOS shows no stderr: the log's lines go to os_log, read with the Console or devicectl. On the Mac stderr is the terminal
void RedirectLog() {
#if TARGET_OS_IPHONE
    int fds[2];
    if (pipe(fds)) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0]] {
        os_log_t log = os_log_create(BundleId().c_str(), "game");
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) {
            line.append(buf, size_t(n));
            for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1))
                os_log(log, "%{public}s", line.substr(0, nl).c_str());
        }
    }).detach();
    // the file log is only flushed on a warning: a log read off the phone while the game runs would lag behind
    std::thread([] {
        size_t peak = 0, last = 0;
        for (int tick = 1;; tick++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            logx::Flush();
            // what the app holds (the footprint iOS kills by) every 10 s once it moved 20 MB, and its peak
            task_vm_info_data_t vm;
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (tick % 10 || task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) != KERN_SUCCESS)
                continue;
            size_t mb = size_t(vm.phys_footprint >> 20);
            peak = std::max(peak, mb);
            if (mb + 20 < last || mb > last + 20) {
                last = mb;
                LOGI("memory: footprint %zu MB, peak %zu MB, %zu MB available", mb, peak, size_t(os_proc_available_memory() >> 20));
            }
        }
    }).detach();
#endif
}

void SetHints() {
#if TARGET_OS_IPHONE
    // the home indicator dim, the first swipe from the bottom shows it and only the second goes home: no game gesture there
    // ends in the home screen
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
#endif
}

#if TARGET_OS_IPHONE
namespace {

void (*g_set_foreground)(bool) = nullptr;

// report lines (flushed at once): the transitions a bug report about the GPU after a switch of apps needs, in the order
// iOS sends them
bool SDLCALL LifecycleWatch(void*, SDL_Event* e) {
    switch (e->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            LOGR("ios: will enter background");
            g_set_foreground(false);
            break;
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            LOGR("ios: did enter background");
            LogAudioSession("did enter background");
            break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND: LOGR("ios: will enter foreground"); break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            LOGR("ios: did enter foreground");
            LogAudioSession("did enter foreground");
            g_set_foreground(true);
            break;
        case SDL_EVENT_LOW_MEMORY: LOGR("ios: low memory warning, %zu MB available", size_t(os_proc_available_memory() >> 20)); break;
        default: break;
    }
    return true;
}

}  // namespace
#endif

// the one hook main.cpp calls once SDL is up: the Apple observers start here too
void WatchLifecycle(void (*set_foreground)(bool)) {
    StartAppleObservers();
#if TARGET_OS_IPHONE
    g_set_foreground = set_foreground;
    SDL_AddEventWatch(LifecycleWatch, nullptr);
#else
    (void)set_foreground;
#endif
}

// iOS: the screen. macOS: fullscreen, or a resizable window (its green button goes fullscreen) when display_mode is
// 'windowed' in the settings or BLACK_WINDOWED=1; BLACK_FULLSCREEN=1 wins over the setting
void WindowSetup(SDL_WindowFlags& flags, int& width, int& height) {
#if TARGET_OS_IPHONE
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && *v && *v != '0';
    };
    bool windowed = app::Get().display_mode == app::DisplayMode::Windowed;
    if (on("BLACK_WINDOWED")) windowed = true;
    if (on("BLACK_FULLSCREEN")) windowed = false;
    if (windowed) {
        flags |= SDL_WINDOW_RESIZABLE;
        width = 1280;
        height = 720;
    } else {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
#endif
}

fs::path DataDir() {
    if (fs::path p = Env("BLACK_DATA_DIR"); !p.empty()) {
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
    // Application Support, in the app's sandbox on iOS
    const char* p = SDL_GetPrefPath("com.muhammad", "Black");
    return fs::path(p ? p : ".");
}

std::vector<fs::path> GameFolders() {
    std::vector<fs::path> out;
    if (fs::path p = Env("BLACK_GAME_DIR"); !p.empty()) {
        out.push_back(p);
        out.push_back(p / "assets");
    }
#if TARGET_OS_IPHONE
    // Documents/Black in the Files app (UIFileSharingEnabled)
    if (fs::path d = Documents(); !d.empty()) {
        out.push_back(d / "Black");
        out.push_back(d);
    }
#else
    // a checkout's own folders
    out.push_back(fs::current_path() / "assets");
    out.push_back(fs::current_path() / "iso");
#endif
    out.push_back(app::ExeDir() / "assets");
    return out;
}

void LogGameMissing() {
    std::string where;
    for (const fs::path& p : GameFolders()) where += (where.empty() ? "" : ", ") + p.string();
    LOGE("game: no game files in %s, waiting for them (default.xbe of the USA release, or its .iso)", where.c_str());
}

void LogDevice() {
#if TARGET_OS_IPHONE
    LOGR("Device: %s, iOS %s", Sysctl("hw.machine").c_str(), Sysctl("kern.osproductversion").c_str());
#else
    LOGR("Device: %s, macOS %s", Sysctl("hw.model").c_str(), Sysctl("kern.osproductversion").c_str());
#endif
#if TARGET_OS_IPHONE
    LOGR("CPU: %d cores, %zu MB available to the app", SDL_GetNumLogicalCPUCores(), size_t(os_proc_available_memory() >> 20));
#else
    LOGR("CPU: %s (%d threads)", Sysctl("machdep.cpu.brand_string").c_str(), SDL_GetNumLogicalCPUCores());
#endif
}

namespace {

struct SurfaceJob {
    SDL_Window* window;
    VkInstance instance;
    VkSurfaceKHR* surface;
    bool ok;
};

void SDLCALL MakeSurface(void* p) {
    auto* j = static_cast<SurfaceJob*>(p);
    j->ok = SDL_Vulkan_CreateSurface(j->window, j->instance, nullptr, j->surface);
}

struct DestroyJob {
    VkInstance instance;
    VkSurfaceKHR surface;
};

void SDLCALL DropSurface(void* p) {
    auto* j = static_cast<DestroyJob*>(p);
    vkDestroySurfaceKHR(j->instance, j->surface, nullptr);
}

}  // namespace

// the main thread's event loop runs the job; the caller waits. Measured with spike/vkpresent: from another thread the
// window stays black
bool CreateVulkanSurface(SDL_Window* window, VkInstance instance, VkSurfaceKHR* surface) {
    SurfaceJob job{window, instance, surface, false};
    if (SDL_IsMainThread()) MakeSurface(&job);
    else if (!SDL_RunOnMainThread(MakeSurface, &job, true)) return false;
    return job.ok;
}

void DestroyVulkanSurface(VkInstance instance, VkSurfaceKHR surface) {
    DestroyJob job{instance, surface};
    if (SDL_IsMainThread()) DropSurface(&job);
    else SDL_RunOnMainThread(DropSurface, &job, true);
}

bool SurfaceReady(SDL_Window* window) { return window != nullptr; }

// next to the executable, or in the bundle's resources
fs::path AssetPath(const char* relative) {
    const char* base = SDL_GetBasePath();
    return (base ? fs::path(base) : fs::path()) / relative;
}

void SafeAreaPixels(SDL_Window* window, float& x, float& y, float& w, float& h) {
    int pw = 0, ph = 0;
    if (window) SDL_GetWindowSizeInPixels(window, &pw, &ph);
    x = y = 0;
    w = float(pw), h = float(ph);
#if TARGET_OS_IPHONE
    // SDL's rectangle is in points: the notch side and the home indicator are outside it
    SDL_Rect r;
    int lw = 0, lh = 0;
    if (window && SDL_GetWindowSafeArea(window, &r) && SDL_GetWindowSize(window, &lw, &lh) && lw > 0 && lh > 0) {
        float kx = float(pw) / float(lw), ky = float(ph) / float(lh);
        x = float(r.x) * kx, y = float(r.y) * ky;
        w = float(r.w) * kx, h = float(r.h) * ky;
    }
#endif
}

void TouchMarginsPixels(SDL_Window* window, float& top, float& bottom) {
    top = bottom = 0;
#if TARGET_OS_IPHONE
    // about 20 points at the top (the strip Control Center is pulled from) and 24 at the bottom (the home indicator's)
    if (!window) return;
    float density = SDL_GetWindowPixelDensity(window);
    top = 20.0f * density;
    bottom = 24.0f * density;
#else
    (void)window;
#endif
}

// a Mac has no touch screen, but its trackpad reports fingers to SDL as an indirect touch device
bool TouchControls() {
#if TARGET_OS_IPHONE
    return true;
#else
    int n = 0;
    bool direct = false;
    if (SDL_TouchID* ids = SDL_GetTouchDevices(&n)) {
        for (int i = 0; i < n && !direct; i++) direct = SDL_GetTouchDeviceType(ids[i]) == SDL_TOUCH_DEVICE_DIRECT;
        SDL_free(ids);
    }
    return direct;
#endif
}

bool AcceptsFinger(uint64_t touch_id) {
#if TARGET_OS_IPHONE
    return true;
#else
    return SDL_GetTouchDeviceType(touch_id) == SDL_TOUCH_DEVICE_DIRECT;
#endif
}

void MergeLooseController(input::Pad&) {}

}  // namespace platform

namespace host {

// no release check outside Android's: the options hide the row
bool UpdatesSupported() { return false; }
void CheckForUpdates() {}

}  // namespace host
