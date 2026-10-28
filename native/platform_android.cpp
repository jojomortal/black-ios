// The Android host's platform part: logcat, storage and the SD cards, the all files access, the activity's loose controller
// and update check
#include "platform.h"

#include <jni.h>

#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace fs = std::filesystem;

namespace platform {

namespace {

// the log's lines to logcat as well: stderr is read on a thread of its own
void LogToLogcat() {
    int fds[2];
    if (pipe(fds)) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0]] {
        char buf[1024];
        std::string line;
        for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) {
            line.append(buf, size_t(n));
            for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1))
                __android_log_write(ANDROID_LOG_INFO, "BLACK", line.substr(0, nl).c_str());
        }
    }).detach();
}

std::string Property(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return v;
}

// the shared storage's folder for the game, readable with all files access: the files or the image in it, or in its
// assets folder
constexpr const char* kShared = "/storage/emulated/0/Black";
constexpr const char* kVolumes = "/storage";

// the Black folders BlackActivity has from Android's storage volumes, which some phones don't show in /storage
std::vector<fs::path> ActivityCardFolders() {
    std::vector<fs::path> out;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return out;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "cardFolderPaths", "()[Ljava/lang/String;");
    auto paths = m ? static_cast<jobjectArray>(env->CallObjectMethod(activity, m)) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        paths = nullptr;
    }
    for (jsize i = 0, n = paths ? env->GetArrayLength(paths) : 0; i < n; i++) {
        auto s = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        if (!s) continue;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) {
            out.emplace_back(c);
            env->ReleaseStringUTFChars(s, c);
        }
        env->DeleteLocalRef(s);
    }
    if (paths) env->DeleteLocalRef(paths);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return out;
}

// the Black folder on each SD card or USB stick Android has mounted, /storage/<volume id>/Black; all files access
// reads them as it does the phone's own storage
std::vector<fs::path> RemovableFolders() {
    std::vector<fs::path> out = ActivityCardFolders();
    std::error_code ec, dir_ec;
    for (auto& e : fs::directory_iterator(kVolumes, ec)) {
        std::string name = e.path().filename().string();
        if (name == "emulated" || name == "self" || !e.is_directory(dir_ec)) continue;
        out.push_back(e.path() / "Black");
    }
    static bool logged = false;
    if (ec && !logged) {
        LOGW("game: %s cannot be listed: %s", kVolumes, ec.message().c_str());
        logged = true;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

void RedirectLog() { LogToLogcat(); }

void WindowSetup(SDL_WindowFlags& flags, int&, int&) { flags |= SDL_WINDOW_FULLSCREEN; }

void SetHints() { SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); }

void WatchLifecycle(void (*)(bool)) {}

// app-specific external storage: the player can reach the logs, saves and settings over USB
fs::path DataDir() {
    const char* p = nullptr;
    if (SDL_GetAndroidExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) p = SDL_GetAndroidExternalStoragePath();
    if (!p) p = SDL_GetAndroidInternalStoragePath();
    return fs::path(p ? p : ".");
}

std::vector<fs::path> GameFolders() {
    std::vector<fs::path> out = {fs::path(kShared), fs::path(kShared) / "assets"};
    for (const fs::path& card : RemovableFolders()) {
        out.push_back(card);
        out.push_back(card / "assets");
    }
    out.push_back(app::ExeDir() / "assets");
    return out;
}

void LogGameMissing() {
    std::error_code ec;
    fs::is_directory(kShared, ec);
    std::string cards;
    for (const fs::path& card : RemovableFolders()) cards += ", " + card.string();
    LOGE("game: no game files in %s%s or %s%s, waiting for them", kShared, cards.c_str(),
         (app::ExeDir() / "assets").c_str(), ec == std::errc::permission_denied ? ", no access to the first" : "");
}

void LogDevice() {
    LOGR("Device: %s %s, Android %s (API %d)", Property("ro.product.manufacturer").c_str(),
         Property("ro.product.model").c_str(), Property("ro.build.version.release").c_str(), SDL_GetAndroidSDKVersion());
    LOGR("CPU: %s (%d threads)", Property("ro.soc.model").c_str(), SDL_GetNumLogicalCPUCores());
}

bool CreateVulkanSurface(SDL_Window* window, VkInstance instance, VkSurfaceKHR* surface) {
    return SDL_Vulkan_CreateSurface(window, instance, nullptr, surface);
}

void DestroyVulkanSurface(VkInstance instance, VkSurfaceKHR surface) { vkDestroySurfaceKHR(instance, surface, nullptr); }

// Android has not handed the window back yet (resuming, or a surface change)
bool SurfaceReady(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr) != nullptr;
}

void SafeAreaPixels(SDL_Window* window, float& x, float& y, float& w, float& h) {
    int pw = 0, ph = 0;
    if (window) SDL_GetWindowSizeInPixels(window, &pw, &ph);
    x = y = 0;
    w = float(pw), h = float(ph);
}

void TouchMarginsPixels(SDL_Window*, float& top, float& bottom) { top = bottom = 0; }

bool TouchControls() { return true; }
bool AcceptsFinger(uint64_t) { return true; }

// the APK's assets: SDL opens them by their relative name
fs::path AssetPath(const char* relative) { return relative; }

namespace {

using input::Pad;

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }
uint8_t Trigger(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

// A controller Android hands over without its device: some phones' game services (MIUI's on a Redmi Note 8 Pro)
// take a DualShock 4 and pass its buttons and sticks on as a virtual device, which SDL takes for no controller.
// BlackActivity sends those here as Android's key codes and axes; they play port 0.
struct Loose {
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, hx = 0, hy = 0;
    bool seen = false;
};
std::mutex g_loose_lock;
Loose g_loose;

// Android's key code as the pad's: a button bit (dpad, start, back, thumbs) or an analog button, -1 for neither
void LooseKey(int code, int& bit, int& analog) {
    bit = analog = -1;
    switch (code) {
        case 19: bit = 0; break;    // DPAD_UP
        case 20: bit = 1; break;    // DPAD_DOWN
        case 21: bit = 2; break;    // DPAD_LEFT
        case 22: bit = 3; break;    // DPAD_RIGHT
        case 108: bit = 4; break;   // BUTTON_START (Options)
        case 109: bit = 5; break;   // BUTTON_SELECT (Share)
        case 106: bit = 6; break;   // BUTTON_THUMBL
        case 107: bit = 7; break;   // BUTTON_THUMBR
        case 96: analog = 0; break;   // BUTTON_A (cross)
        case 97: analog = 1; break;   // BUTTON_B (circle)
        case 99: analog = 2; break;   // BUTTON_X (square)
        case 100: analog = 3; break;  // BUTTON_Y (triangle)
        case 103: analog = 4; break;  // BUTTON_R1: black
        case 102: analog = 5; break;  // BUTTON_L1: white
        case 104: analog = 6; break;  // BUTTON_L2
        case 105: analog = 7; break;  // BUTTON_R2
        default: break;
    }
}


// the loose controller's state on top of what SDL read: buttons added, the stick pushed further taken
void MergeLoose(Pad& p) {
    std::lock_guard lock(g_loose_lock);
    const Loose& l = g_loose;
    if (!l.seen) return;
    p.connected = true;
    p.buttons |= l.buttons;
    // the hat as the dpad: x left -1 right 1, y up -1 down 1
    if (l.hy < -0.5f) p.buttons |= 1 << 0;
    if (l.hy > 0.5f) p.buttons |= 1 << 1;
    if (l.hx < -0.5f) p.buttons |= 1 << 2;
    if (l.hx > 0.5f) p.buttons |= 1 << 3;
    for (int i = 0; i < 8; i++) p.analog[i] = std::max(p.analog[i], l.analog[i]);
    p.analog[6] = std::max(p.analog[6], Trigger(l.lt));
    p.analog[7] = std::max(p.analog[7], Trigger(l.rt));
    // Android's y is down, the pad's up
    auto further = [](int16_t& to, int16_t v) { if (std::abs(int(v)) > std::abs(int(to))) to = v; };
    further(p.lx, Axis(l.lx));
    further(p.ly, Axis(-l.ly));
    further(p.rx, Axis(l.rx));
    further(p.ry, Axis(-l.ry));
}

}  // namespace

void MergeLooseController(Pad& pad) { MergeLoose(pad); }

// BlackActivity: the loose controller's buttons, sticks and triggers, and its letting go when the app loses the focus
extern "C" JNIEXPORT void JNICALL Java_com_racas_black_BlackActivity_nativeLooseKey(JNIEnv*, jclass, jint code,
                                                                                     jboolean down) {
    int bit, analog;
    LooseKey(code, bit, analog);
    if (bit < 0 && analog < 0) return;
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    if (bit >= 0) g_loose.buttons = uint16_t(down ? g_loose.buttons | (1u << bit) : g_loose.buttons & ~(1u << bit));
    if (analog >= 0) g_loose.analog[analog] = down ? 0xFF : 0;
}

extern "C" JNIEXPORT void JNICALL Java_com_racas_black_BlackActivity_nativeLooseAxes(JNIEnv*, jclass, jfloat lx,
                                                                                      jfloat ly, jfloat rx, jfloat ry,
                                                                                      jfloat lt, jfloat rt, jfloat hx,
                                                                                      jfloat hy) {
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    g_loose.lx = lx, g_loose.ly = ly, g_loose.rx = rx, g_loose.ry = ry;
    g_loose.lt = lt, g_loose.rt = rt, g_loose.hx = hx, g_loose.hy = hy;
}

extern "C" JNIEXPORT void JNICALL Java_com_racas_black_BlackActivity_nativeLooseRelease(JNIEnv*, jclass) {
    std::lock_guard lock(g_loose_lock);
    bool seen = g_loose.seen;
    g_loose = {};
    g_loose.seen = seen;
}

}  // namespace platform

namespace host {

bool UpdatesSupported() { return true; }

// a no-argument void method of the activity; any thread, SDL attaches it to the VM
static void CallActivity(const char* name) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, name, "()V");
    if (m) env->CallVoidMethod(activity, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

void CheckForUpdates() { CallActivity("checkForUpdates"); }

}  // namespace host
