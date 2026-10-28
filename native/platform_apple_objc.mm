// The Apple hosts' Objective-C++ part: what C++ cannot call. GameController (the system gestures of a controller's
// buttons), the audio session's interruptions, and the controller log through SDL's event watch.
#include "platform_apple.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <AVFAudio/AVFAudio.h>
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include "core/log.h"

namespace platform {

namespace {

bool Env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

const char* TypeName(SDL_GamepadType t) {
    switch (t) {
        case SDL_GAMEPAD_TYPE_STANDARD: return "standard";
        case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
        case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One";
        case SDL_GAMEPAD_TYPE_PS3: return "PS3";
        case SDL_GAMEPAD_TYPE_PS4: return "PS4";
        case SDL_GAMEPAD_TYPE_PS5: return "PS5";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
        default: return "other";
    }
}

// what SDL knows of a controller, once each: the mapping string and the buttons, axes, sensors and extras it has
std::mutex g_seen_lock;
std::set<SDL_JoystickID> g_seen;

void Describe(SDL_JoystickID id) {
    {
        std::lock_guard lock(g_seen_lock);
        if (!g_seen.insert(id).second) return;
    }
    char guid[40] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    char* mapping = SDL_GetGamepadMappingForID(id);
    LOGR("pad: id %u \"%s\" type %s, vendor %04X product %04X version %04X, guid %s", unsigned(id),
         SDL_GetGamepadNameForID(id) ? SDL_GetGamepadNameForID(id) : "?", TypeName(SDL_GetGamepadTypeForID(id)),
         unsigned(SDL_GetGamepadVendorForID(id)), unsigned(SDL_GetGamepadProductForID(id)),
         unsigned(SDL_GetGamepadProductVersionForID(id)), guid);
    LOGR("pad: mapping %s", mapping ? mapping : "(none)");
    SDL_free(mapping);
    // the controller itself: opened a second time (SDL counts the opens), only to ask
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) return;
    std::string buttons, axes;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GamepadHasButton(pad, SDL_GamepadButton(b))) buttons += std::string(buttons.empty() ? "" : " ") + SDL_GetGamepadStringForButton(SDL_GamepadButton(b));
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
        if (SDL_GamepadHasAxis(pad, SDL_GamepadAxis(a))) axes += std::string(axes.empty() ? "" : " ") + SDL_GetGamepadStringForAxis(SDL_GamepadAxis(a));
    LOGR("pad: buttons: %s", buttons.c_str());
    LOGR("pad: axes: %s", axes.c_str());
    SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    int percent = -1;
    SDL_PowerState power = SDL_GetGamepadPowerInfo(pad, &percent);
    LOGR("pad: touchpads %d, rumble %d, trigger rumble %d, rgb led %d, player led %d, player %d, power state %d (%d%%)",
         SDL_GetNumGamepadTouchpads(pad), int(SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)),
         int(SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false)),
         int(SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false)),
         int(SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_PLAYER_LED_BOOLEAN, false)), SDL_GetGamepadPlayerIndex(pad),
         int(power), percent);
    for (SDL_SensorType s : {SDL_SENSOR_GYRO, SDL_SENSOR_ACCEL})
        LOGR("pad: %s sensor: %s, %.0f Hz, enabled now: %d (the game turns the gyro on only while Motion Controls is not OFF)",
             s == SDL_SENSOR_GYRO ? "gyro" : "accel", SDL_GamepadHasSensor(pad, s) ? "present" : "none",
             double(SDL_GetGamepadSensorDataRate(pad, s)), int(SDL_GamepadSensorEnabled(pad, s)));
    SDL_CloseGamepad(pad);
}

// ---- raw input, with BLACK_DEBUG_INPUT=1

struct GyroSum {
    double sum[3] = {};
    float peak = 0;
    int n = 0;
    uint64_t since = 0;
};
GyroSum g_gyro;
int16_t g_last_axis[SDL_GAMEPAD_AXIS_COUNT] = {};
bool g_debug_input = false;

bool SDLCALL InputWatch(void*, SDL_Event* e) {
    switch (e->type) {
        case SDL_EVENT_GAMEPAD_ADDED: Describe(e->gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: LOGR("pad: id %u removed", unsigned(e->gdevice.which)); break;
        default: break;
    }
    if (!g_debug_input) return true;
    switch (e->type) {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            LOGR("pad: button %s %s", SDL_GetGamepadStringForButton(SDL_GamepadButton(e->gbutton.button)), e->gbutton.down ? "down" : "up");
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            // a line when the value moved a good way, reached a rest or an end
            int a = e->gaxis.axis;
            int16_t v = e->gaxis.value;
            bool rest = v == 0 && g_last_axis[a] != 0;
            bool end = (v == 32767 || v == -32768) && v != g_last_axis[a];
            if (std::abs(int(v) - int(g_last_axis[a])) >= 3000 || rest || end) {
                LOGR("pad: axis %s %d", SDL_GetGamepadStringForAxis(SDL_GamepadAxis(a)), int(v));
                g_last_axis[a] = v;
            }
            break;
        }
        // the buttons and hats the controller has beyond SDL's mapping: Share, PS, the touchpad's click
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            LOGR("pad: raw joystick button %d %s", int(e->jbutton.button), e->jbutton.down ? "down" : "up");
            break;
        case SDL_EVENT_JOYSTICK_HAT_MOTION: LOGR("pad: raw joystick hat %d %d", int(e->jhat.hat), int(e->jhat.value)); break;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
            LOGR("pad: touchpad %d finger %d %s at %.2f %.2f", int(e->gtouchpad.touchpad), int(e->gtouchpad.finger),
                 e->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN ? "down" : "up", double(e->gtouchpad.x), double(e->gtouchpad.y));
            break;
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            if (e->gsensor.sensor != SDL_SENSOR_GYRO) break;
            // the average over half a second (a controller laid still reads its bias there) and the peak turn rate
            if (!g_gyro.since) g_gyro.since = e->gsensor.timestamp;
            for (int i = 0; i < 3; i++) g_gyro.sum[i] += e->gsensor.data[i];
            g_gyro.peak = std::max({g_gyro.peak, std::fabs(e->gsensor.data[0]), std::fabs(e->gsensor.data[1]), std::fabs(e->gsensor.data[2])});
            g_gyro.n++;
            if (e->gsensor.timestamp - g_gyro.since >= 500'000'000ull) {
                LOGR("pad: gyro rad/s, mean (%.4f %.4f %.4f) peak %.3f, %d samples in 0.5 s", g_gyro.sum[0] / g_gyro.n,
                     g_gyro.sum[1] / g_gyro.n, g_gyro.sum[2] / g_gyro.n, double(g_gyro.peak), g_gyro.n);
                g_gyro = {};
            }
            break;
        default: break;
    }
    return true;
}

// ---- GameController: the buttons the system keeps for itself

NSString* Name(GCControllerButtonInput* b) {
    NSString* n = b.localizedName ?: b.unmappedLocalizedName;
    if (!n) n = b.aliases.anyObject;
    return n ?: @"?";
}

// the system's gestures on a controller's buttons (Share records the screen, the PS button the dashboard) swallow the
// button: the game is told to have them (BLACK_GC_KEEP_SYSTEM_GESTURES=1 leaves them as they are)
void FreeGestures(GCController* c) {
    if (@available(iOS 14.0, macOS 11.0, *)) {
        GCPhysicalInputProfile* in = c.physicalInputProfile;
        if (!in) return;
        bool keep = Env("BLACK_GC_KEEP_SYSTEM_GESTURES");
        LOGR("gc: \"%s\" category %s, motion: %s", c.vendorName ? c.vendorName.UTF8String : "?",
             c.productCategory ? c.productCategory.UTF8String : "?",
             c.motion ? (c.motion.hasAttitudeAndRotationRate ? "rotation rate" : "no rotation rate") : "none");
        for (GCControllerButtonInput* b in in.allButtons) {
            if (!b.isBoundToSystemGesture) continue;
            GCSystemGestureState before = b.preferredSystemGestureState;
            if (!keep) b.preferredSystemGestureState = GCSystemGestureStateDisabled;
            LOGR("gc: button \"%s\" is bound to a system gesture (preferred state %d -> %d, still bound: %d)", Name(b).UTF8String,
                 int(before), int(b.preferredSystemGestureState), int(b.isBoundToSystemGesture));
        }
    }
}

void WatchControllers() {
    for (GCController* c in GCController.controllers) FreeGestures(c);
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
                                                  usingBlock:^(NSNotification* n) { FreeGestures(n.object); }];
}

#if TARGET_OS_IPHONE
// CoreAudio stops for a call, Siri or another app taking the session; SDL restarts its device when told it may
void WatchAudioSession() {
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        NSUInteger opts = [n.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
        if (type == AVAudioSessionInterruptionTypeBegan) LOGR("audio: interruption began");
        else LOGR("audio: interruption ended, should resume: %d", int((opts & AVAudioSessionInterruptionOptionShouldResume) != 0));
    }];
    [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
        LOGR("audio: route changed (reason %lu), now %s", (unsigned long)[n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue],
             AVAudioSession.sharedInstance.currentRoute.outputs.firstObject.portType.UTF8String ?: "?");
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        LOGR("audio: media services were reset");
    }];
}
#endif

}  // namespace

void LogAudioSession(const char* when) {
#if TARGET_OS_IPHONE
    AVAudioSession* s = AVAudioSession.sharedInstance;
    LOGR("audio: at %s: session category %s, %.0f Hz, other audio playing %d, output %s", when, s.category.UTF8String,
         s.sampleRate, int(s.otherAudioPlaying), s.currentRoute.outputs.firstObject.portType.UTF8String ?: "none");
#else
    (void)when;
#endif
}

void StartAppleObservers() {
    g_debug_input = Env("BLACK_DEBUG_INPUT");
    SDL_AddEventWatch(InputWatch, nullptr);
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; i++) Describe(ids[i]);
        SDL_free(ids);
    }
    WatchControllers();
#if TARGET_OS_IPHONE
    WatchAudioSession();
#endif
}

}  // namespace platform
